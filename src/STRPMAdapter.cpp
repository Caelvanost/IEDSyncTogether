#include "PCH.h"
#include "STRPMAdapter.h"

#include <Windows.h>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr char kChannel[] = "strpm.iedsynctogether.state.v1";
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
            const auto result = _api->getLocalConnectionID(&_localConnectionID);
            if (result != STRPM::Result::kOk) {
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
        SKSE::log::info(
            "STRPM adapter started: channel={} player=\"{}\" connectionID={}",
            kChannel,
            GetLocalPlayerName(),
            _localConnectionID);
        return true;
    }

    void STRPMAdapter::Stop()
    {
        if (!_running.exchange(false)) {
            return;
        }
        if (_api && _api->unregisterChannel && _listener.value != 0) {
            (void)_api->unregisterChannel(_listener);
        }
        _listener = {};
        _localConnectionID = 0;
        _api = nullptr;
        SKSE::log::info("STRPM adapter stopped");
    }

    void STRPMAdapter::Publish(const LocalIEDState& state, std::string_view payload)
    {
        if (!_running.load() || !_api || !_api->send || payload.empty()) {
            return;
        }
        if (payload.size() > STRPM::kMaxPayloadBytes) {
            SKSE::log::warn("STRPM IED state TX dropped: payload too large ({} bytes)", payload.size());
            return;
        }

        const STRPM::Target target{
            STRPM::TargetKind::kAllPlayers,
            0,
            nullptr
        };

        std::scoped_lock lock(_sendMutex);
        const auto result = _api->send(
            kChannel,
            target,
            payload.data(),
            payload.size(),
            STRPM::kMessageReliable | STRPM::kMessageOrdered);

        if (result != STRPM::Result::kOk) {
            SKSE::log::warn("STRPM IED state TX failed: {}", STRPM::ResultToString(result));
            return;
        }

        const auto customCount = std::ranges::count_if(
            state.objects,
            [](const CapturedIEDObject& object) { return object.kind == IEDObjectKind::kCustom; });
        SKSE::log::info(
            "STRPM IED STATE TX: bytes={} objects={} customObjects={}",
            payload.size(),
            state.objects.size(),
            customCount);
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
        if (message->sender.connectionID != 0 && self->_localConnectionID != 0 &&
            message->sender.connectionID == self->_localConnectionID) {
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
