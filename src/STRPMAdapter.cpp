#include "PCH.h"
#include "STRPMAdapter.h"

#include <Windows.h>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr char kChannel[] = "strpm.iedsynctogether.state.v1";
        constexpr auto kRetryInterval = std::chrono::seconds(1);
        constexpr auto kHeartbeatInterval = std::chrono::seconds(10);
    }

    STRPMAdapter& STRPMAdapter::GetSingleton()
    {
        static STRPMAdapter instance;
        return instance;
    }

    STRPMAdapter::~STRPMAdapter()
    {
        Stop();
    }

    std::string STRPMAdapter::GetLocalPlayerName() const
    {
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (const auto* name = player->GetName(); name && *name) {
                return name;
            }
        }
        return "Player";
    }

    bool STRPMAdapter::LoadApi()
    {
        auto module = GetModuleHandleW(L"STRPluginMessagingAPI.dll");
        if (!module) {
            module = LoadLibraryW(L"STRPluginMessagingAPI.dll");
        }
        if (!module) {
            module = LoadLibraryW(L"Data\\SKSE\\Plugins\\STRPluginMessagingAPI.dll");
        }
        if (!module) {
            SKSE::log::warn("STRPM adapter unavailable: STRPluginMessagingAPI.dll not loaded");
            return false;
        }

        const auto rawQuery = GetProcAddress(module, STRPM::kQueryInterfaceExportName);
        if (!rawQuery) {
            SKSE::log::warn("STRPM adapter unavailable: query export missing");
            return false;
        }

        const auto query = reinterpret_cast<STRPM::QueryInterfaceFn>(rawQuery);
        const STRPM::Interface* api = nullptr;
        const auto result = query(STRPM::kInterfaceVersion, &api);
        if (result != STRPM::Result::kOk || !api || api->version != STRPM::kInterfaceVersion) {
            SKSE::log::warn(
                "STRPM adapter unavailable: interface query failed ({})",
                STRPM::ResultToString(result));
            return false;
        }

        _api = api;
        return true;
    }

    bool STRPMAdapter::Start()
    {
        if (_running.load()) {
            return true;
        }
        if (!LoadApi()) {
            return false;
        }

        if (_api->setLocalDisplayName) {
            const auto name = GetLocalPlayerName();
            const auto result = _api->setLocalDisplayName(name.c_str());
            if (result != STRPM::Result::kOk) {
                SKSE::log::warn("STRPM setLocalDisplayName failed: {}", STRPM::ResultToString(result));
            }
        }

        if (_api->getLocalConnectionID) {
            STRPM::ConnectionID localID = 0;
            const auto result = _api->getLocalConnectionID(&localID);
            if (result == STRPM::Result::kOk) {
                _localConnectionID.store(localID);
            } else {
                SKSE::log::warn("STRPM getLocalConnectionID failed: {}", STRPM::ResultToString(result));
            }
        }

        const auto result = _api->registerChannel(
            kChannel,
            &STRPMAdapter::OnMessage,
            this,
            &_listener);
        if (result != STRPM::Result::kOk) {
            SKSE::log::warn("STRPM registerChannel failed: {}", STRPM::ResultToString(result));
            _api = nullptr;
            _listener = {};
            return false;
        }

        _running.store(true);
        _retryTimer = std::jthread([this](std::stop_token token) { RetryLoop(token); });
        SKSE::log::info(
            "STRPM adapter started: channel={} player=\"{}\" connectionID={} retry={}s heartbeat={}s",
            kChannel,
            GetLocalPlayerName(),
            _localConnectionID.load(),
            kRetryInterval.count(),
            kHeartbeatInterval.count());
        return true;
    }

    void STRPMAdapter::Stop()
    {
        if (!_running.exchange(false)) {
            return;
        }

        if (_retryTimer.joinable()) {
            _retryTimer.request_stop();
            _retryTimer.join();
        }

        if (_api && _api->unregisterChannel && _listener.value != 0) {
            (void)_api->unregisterChannel(_listener);
        }

        Reset();
        _listener = {};
        _localConnectionID.store(0);
        _api = nullptr;
        SKSE::log::info("STRPM adapter stopped");
    }

    void STRPMAdapter::Reset()
    {
        std::scoped_lock lock(_stateMutex);
        _latestState = {};
        _latestPayload.clear();
        _pending = false;
        _haveSuccessfulSend = false;
        _lastFailure.reset();
        _lastSuccessfulSend = {};
    }

    void STRPMAdapter::Publish(const LocalIEDState& state, std::string_view payload)
    {
        if (payload.empty()) {
            return;
        }
        if (payload.size() > STRPM::kMaxPayloadBytes) {
            SKSE::log::warn("STRPM IED state TX dropped: payload too large ({} bytes)", payload.size());
            return;
        }

        {
            std::scoped_lock lock(_stateMutex);
            _latestState = state;
            _latestPayload.assign(payload);
            _pending = true;
        }

        (void)TrySend(state, payload, "change");
    }

    bool STRPMAdapter::TrySend(
        const LocalIEDState& state,
        std::string_view payload,
        std::string_view reason)
    {
        if (!_running.load() || !_api || !_api->send || payload.empty()) {
            return false;
        }
        if (payload.size() > STRPM::kMaxPayloadBytes) {
            return false;
        }

        if (_api->getLocalConnectionID) {
            STRPM::ConnectionID currentID = 0;
            if (_api->getLocalConnectionID(&currentID) == STRPM::Result::kOk && currentID != 0) {
                _localConnectionID.store(currentID);
            }
        }

        if (_api->setLocalDisplayName) {
            const auto name = GetLocalPlayerName();
            (void)_api->setLocalDisplayName(name.c_str());
        }

        const STRPM::Target target{
            STRPM::TargetKind::kAllPlayers,
            0,
            nullptr
        };

        STRPM::Result result = STRPM::Result::kTransportError;
        {
            std::scoped_lock lock(_sendMutex);
            result = _api->send(
                kChannel,
                target,
                payload.data(),
                payload.size(),
                STRPM::kMessageReliable | STRPM::kMessageOrdered);
        }

        if (result != STRPM::Result::kOk) {
            bool shouldLog = false;
            {
                std::scoped_lock lock(_stateMutex);
                if (_latestPayload == payload) {
                    _pending = true;
                }
                shouldLog = !_lastFailure || *_lastFailure != result;
                _lastFailure = result;
            }
            if (shouldLog) {
                SKSE::log::warn(
                    "STRPM IED STATE TX deferred: reason={} result={} bytes={}",
                    reason,
                    STRPM::ResultToString(result),
                    payload.size());
            }
            return false;
        }

        bool recovered = false;
        {
            std::scoped_lock lock(_stateMutex);
            if (_latestPayload == payload) {
                _pending = false;
            }
            recovered = _lastFailure.has_value();
            _lastFailure.reset();
            _haveSuccessfulSend = true;
            _lastSuccessfulSend = std::chrono::steady_clock::now();
        }

        if (recovered) {
            SKSE::log::info("STRPM IED transport resumed; latest pending state delivered");
        }

        const auto customCount = std::ranges::count_if(
            state.objects,
            [](const CapturedIEDObject& object) { return object.kind == IEDObjectKind::kCustom; });

        if (reason == "heartbeat") {
            SKSE::log::debug(
                "STRPM IED STATE TX heartbeat: bytes={} objects={} customObjects={}",
                payload.size(),
                state.objects.size(),
                customCount);
        } else {
            SKSE::log::info(
                "STRPM IED STATE TX: reason={} bytes={} objects={} customObjects={}",
                reason,
                payload.size(),
                state.objects.size(),
                customCount);
        }
        return true;
    }

    void STRPMAdapter::RetryLoop(std::stop_token token)
    {
        while (!token.stop_requested() && _running.load()) {
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([]() { STRPMAdapter::GetSingleton().RetryTick(); });
            }

            auto elapsed = std::chrono::milliseconds(0);
            while (elapsed < kRetryInterval && !token.stop_requested() && _running.load()) {
                constexpr auto slice = std::chrono::milliseconds(100);
                std::this_thread::sleep_for(slice);
                elapsed += slice;
            }
        }
    }

    void STRPMAdapter::RetryTick()
    {
        if (!_running.load()) {
            return;
        }

        LocalIEDState state;
        std::string payload;
        bool pending = false;
        bool haveSuccessfulSend = false;
        std::chrono::steady_clock::time_point lastSuccessfulSend{};
        {
            std::scoped_lock lock(_stateMutex);
            if (_latestPayload.empty()) {
                return;
            }
            state = _latestState;
            payload = _latestPayload;
            pending = _pending;
            haveSuccessfulSend = _haveSuccessfulSend;
            lastSuccessfulSend = _lastSuccessfulSend;
        }

        if (pending) {
            (void)TrySend(state, payload, "pending");
            return;
        }

        if (haveSuccessfulSend &&
            std::chrono::steady_clock::now() - lastSuccessfulSend >= kHeartbeatInterval) {
            (void)TrySend(state, payload, "heartbeat");
        }
    }

    void STRPM_CALL STRPMAdapter::OnMessage(const STRPM::Message* message, void* userData)
    {
        auto* self = static_cast<STRPMAdapter*>(userData);
        if (!self || !self->_running.load() || !message || !message->data || message->size == 0) {
            return;
        }
        if (message->size > STRPM::kMaxPayloadBytes) {
            SKSE::log::warn("STRPM IED state RX dropped: payload too large ({} bytes)", message->size);
            return;
        }
        const auto localConnectionID = self->_localConnectionID.load();
        if (message->sender.connectionID != 0 && localConnectionID != 0 &&
            message->sender.connectionID == localConnectionID) {
            return;
        }

        const std::string_view payload{
            static_cast<const char*>(message->data),
            message->size
        };
        auto state = DecodeLocalIEDState(payload);
        if (!state) {
            SKSE::log::warn(
                "STRPM IED state RX decode failed: sender={} name=\"{}\" bytes={}",
                message->sender.connectionID,
                message->sender.displayName ? message->sender.displayName : "",
                message->size);
            return;
        }

        const auto customCount = std::ranges::count_if(
            state->objects,
            [](const CapturedIEDObject& object) { return object.kind == IEDObjectKind::kCustom; });
        std::size_t slotCount = 0;
        for (const auto& slot : state->slots) {
            slotCount += slot.has_value() ? 1u : 0u;
        }

        SKSE::log::info(
            "STRPM IED STATE RX: sender={} name=\"{}\" sequence={} bytes={} slots={} objects={} customObjects={} (remote rendering disabled)",
            message->sender.connectionID,
            message->sender.displayName ? message->sender.displayName : "",
            message->sequence,
            message->size,
            slotCount,
            state->objects.size(),
            customCount);
    }
}
