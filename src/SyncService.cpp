#include "PCH.h"
#include "SyncService.h"

#include "IEDBridge.h"
#include "IEDSyncTogether/Interface.h"
#include "ProxyResolver.h"
#include "UdpTransport.h"

namespace IEDSyncTogether
{
    namespace
    {
        constexpr std::string_view kGameplayPrefix = "IEDST|v1|";
        std::atomic_bool g_loggedFirstActiveTick{ false };
    }

    SyncService& SyncService::GetSingleton()
    {
        static SyncService instance;
        return instance;
    }

    SyncService::~SyncService()
    {
        Stop();
    }

    std::optional<std::string> SyncService::ReadField(
        std::string_view packet,
        std::string_view key)
    {
        const auto needle = fmt::format("{}=", key);
        auto position = packet.find(needle);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        position += needle.size();
        auto end = packet.find('|', position);
        if (end == std::string_view::npos) {
            end = packet.size();
        }
        return std::string(packet.substr(position, end - position));
    }

    std::string SyncService::NormalizeName(std::string_view name)
    {
        std::string result(name);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return result;
    }

    void SyncService::Start()
    {
        if (_running.exchange(true)) {
            return;
        }

        _gameReady.store(false);
        g_loggedFirstActiveTick.store(false);
        _config = Config::Load();
        if (!IEDBridge::GetSingleton().IsInstalled()) {
            SKSE::log::critical("Immersive Equipment Displays is not loaded");
            _running.store(false);
            return;
        }

        if (_config.networkEnabled) {
            UdpTransport::GetSingleton().Start(
                _config,
                [](std::string packet) {
                    if (auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([packet = std::move(packet)]() mutable {
                            SyncService::GetSingleton().HandlePacket(std::move(packet));
                        });
                    }
                });
        }

        _timer = std::jthread([this](std::stop_token token) { TimerLoop(token); });
        SKSE::log::info(
            "Service started suspended: capture={}ms suppressRemoteNpcDisplays={}",
            _config.captureIntervalMs,
            _config.suppressRemoteNpcDisplays ? 1 : 0);
    }

    void SyncService::Stop()
    {
        _gameReady.store(false);
        if (!_running.exchange(false)) {
            return;
        }
        if (_timer.joinable()) {
            _timer.request_stop();
            _timer.join();
        }
        UdpTransport::GetSingleton().Stop();
    }

    void SyncService::Reset()
    {
        // Reset is called while Skyrim is tearing down the old save. Do not
        // invoke IED/Papyrus on actors that may already be in destruction.
        _gameReady.store(false);
        g_loggedFirstActiveTick.store(false);
        _blockedProxies.clear();
        {
            std::scoped_lock lock(_snapshotMutex);
            _remoteSnapshots.clear();
        }
        _hasLocalSlots = false;
        _capturePending.store(false);
    }

    void SyncService::SetGameReady(bool ready) noexcept
    {
        _gameReady.store(ready);
        if (!ready) {
            _capturePending.store(false);
            g_loggedFirstActiveTick.store(false);
        }
        SKSE::log::info("Game state {} for IED synchronization", ready ? "ready" : "suspended");
    }

    bool SyncService::CanApplyRuntimeOverrides() const
    {
        if (!_running.load() || !_gameReady.load()) {
            return false;
        }

        std::scoped_lock lock(_snapshotMutex);
        return !_remoteSnapshots.empty();
    }

    void SyncService::TimerLoop(std::stop_token token)
    {
        // Never enqueue game-thread work while a save is loading. The task queue
        // can stop draining during loading; continuously adding Tick tasks would
        // otherwise create a burst immediately after PostLoadGame.
        while (!token.stop_requested() && _running.load()) {
            if (!_gameReady.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([]() { SyncService::GetSingleton().Tick(); });
            }

            const auto interval = std::chrono::milliseconds(_config.captureIntervalMs);
            auto elapsed = std::chrono::milliseconds(0);
            while (elapsed < interval && !token.stop_requested() && _running.load()) {
                constexpr auto slice = std::chrono::milliseconds(100);
                std::this_thread::sleep_for(slice);
                elapsed += slice;
            }
        }
    }

    void SyncService::Tick()
    {
        if (!_running.load() || !_gameReady.load()) {
            return;
        }

        const bool firstActiveTick = !g_loggedFirstActiveTick.exchange(true);
        if (firstActiveTick) {
            SKSE::log::info("First active synchronization tick started");
        }

        bool hasRemoteSnapshots = false;
        {
            std::scoped_lock lock(_snapshotMutex);
            hasRemoteSnapshots = !_remoteSnapshots.empty();
        }

        // Proxy discovery is unnecessary before we either have remote state to
        // match or explicitly need to suppress IED displays on STR proxies.
        if (_config.suppressRemoteNpcDisplays || hasRemoteSnapshots || !_blockedProxies.empty()) {
            if (firstActiveTick) {
                SKSE::log::debug("First active tick: refreshing STR proxy state");
            }
            RefreshProxyMitigation();
            if (firstActiveTick) {
                SKSE::log::debug("First active tick: STR proxy refresh complete");
            }
        } else if (firstActiveTick) {
            SKSE::log::debug("First active tick: STR proxy refresh skipped (no remote state)");
        }

        bool expected = false;
        if (!_capturePending.compare_exchange_strong(expected, true)) {
            if (firstActiveTick) {
                SKSE::log::debug("First active tick: capture already pending");
            }
            return;
        }

        if (firstActiveTick) {
            SKSE::log::info("First active tick: starting local IED slot capture");
        }

        const bool requested = IEDBridge::GetSingleton().CapturePlayerSlots(
            [this](SlotState slots) {
                _capturePending.store(false);
                if (!_gameReady.load()) {
                    return;
                }
                OnLocalCapture(std::move(slots));
            });

        if (firstActiveTick) {
            SKSE::log::info(
                "First active tick: local IED capture dispatch returned requested={}",
                requested ? 1 : 0);
        }

        if (!requested) {
            _capturePending.store(false);
        }
    }

    void SyncService::OnLocalCapture(SlotState slots)
    {
        if (!_running.load() || !_gameReady.load()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const bool changed = !_hasLocalSlots || slots != _localSlots;
        const bool resendDue = !_hasLocalSlots ||
            now - _lastSend >= std::chrono::milliseconds(_config.resendIntervalMs);
        if (!changed && !resendDue) {
            return;
        }

        if (changed) {
            _localSlots = std::move(slots);
            _hasLocalSlots = true;
            ++_revision;
        }

        const auto& transport = UdpTransport::GetSingleton();
        const auto payload = fmt::format(
            "STATE|from={}|rev={}|slots={}",
            HexEncode(transport.GetLocalPlayerName()),
            _revision,
            EncodeSlots(_localSlots));
        UdpTransport::GetSingleton().Send(payload);
        _lastSend = now;

        const auto visible = std::count_if(
            _localSlots.begin(),
            _localSlots.end(),
            [](const auto& slot) { return slot.has_value(); });
        SKSE::log::info(
            "Local IED state: revision={} displayedSlots={} changed={}",
            _revision,
            visible,
            changed ? 1 : 0);
    }

    void SyncService::HandlePacket(std::string packet)
    {
        if (!_gameReady.load()) {
            return;
        }

        if (!packet.starts_with(kGameplayPrefix) || packet.find("|STATE|") == std::string::npos) {
            return;
        }

        const auto encodedSender = ReadField(packet, "from");
        const auto revisionText = ReadField(packet, "rev");
        const auto slotsText = ReadField(packet, "slots");
        if (!encodedSender || !revisionText || !slotsText) {
            return;
        }

        const auto sender = HexDecode(*encodedSender);
        const auto slots = DecodeSlots(*slotsText);
        if (!sender || sender->empty() || !slots) {
            SKSE::log::warn("Rejected malformed remote IED state");
            return;
        }

        std::uint64_t revision = 0;
        try {
            revision = std::stoull(*revisionText);
        } catch (...) {
            return;
        }

        std::scoped_lock lock(_snapshotMutex);
        auto& snapshot = _remoteSnapshots[NormalizeName(*sender)];
        if (revision >= snapshot.revision) {
            snapshot.slots = *slots;
            snapshot.revision = revision;
            LogRemoteResolution(*sender, snapshot);
        }
    }

    void SyncService::LogRemoteResolution(
        std::string_view sender,
        RemoteSnapshot& snapshot)
    {
        auto* proxy = FindRemotePlayerProxy(sender);
        if (!proxy) {
            SKSE::log::info(
                "Remote IED state waiting for proxy: player=\"{}\" revision={}",
                sender,
                snapshot.revision);
            return;
        }

        snapshot.lastProxy = proxy->GetFormID();
        std::size_t visible = 0;
        std::size_t resolved = 0;
        for (const auto& slot : snapshot.slots) {
            if (!slot) {
                continue;
            }
            ++visible;
            if (ResolveFormIdentity(*slot)) {
                ++resolved;
            }
        }

        SKSE::log::info(
            "Remote IED state matched: player=\"{}\" proxy={:08X} revision={} slots={}/{} adapter=diagnostic",
            sender,
            proxy->GetFormID(),
            snapshot.revision,
            resolved,
            visible);
    }

    void SyncService::RefreshProxyMitigation()
    {
        std::unordered_set<RE::FormID> current;
        for (auto* proxy : FindRemotePlayerProxies()) {
            current.insert(proxy->GetFormID());
            if (_config.suppressRemoteNpcDisplays &&
                _blockedProxies.insert(proxy->GetFormID()).second) {
                IEDBridge::GetSingleton().SetActorBlocked(proxy, true);
                const auto* proxyName = proxy->GetName();
                SKSE::log::info(
                    "Suppressed IED NPC display on proxy {:08X} \"{}\"",
                    proxy->GetFormID(),
                    proxyName ? proxyName : "");
            }
        }

        for (auto iterator = _blockedProxies.begin(); iterator != _blockedProxies.end();) {
            if (!_config.suppressRemoteNpcDisplays || !current.contains(*iterator)) {
                if (auto* form = RE::TESForm::LookupByID(*iterator)) {
                    if (auto* actor = form->As<RE::Actor>()) {
                        IEDBridge::GetSingleton().SetActorBlocked(actor, false);
                    }
                }
                iterator = _blockedProxies.erase(iterator);
            } else {
                ++iterator;
            }
        }

        std::scoped_lock lock(_snapshotMutex);
        for (auto& [name, snapshot] : _remoteSnapshots) {
            if (snapshot.lastProxy == 0 || !current.contains(snapshot.lastProxy)) {
                LogRemoteResolution(name, snapshot);
            }
        }
    }

    std::uint32_t SyncService::QueryRemoteSlot(
        RE::FormID actorFormID,
        std::uint32_t slotIndex,
        RE::FormID& outFormID) const
    {
        outFormID = 0;
        if (!_running.load() || !_gameReady.load() || slotIndex >= IEDST::kSlotCount) {
            return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kNotRemote);
        }

        {
            std::scoped_lock lock(_snapshotMutex);
            if (_remoteSnapshots.empty()) {
                return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kNotRemote);
            }
        }

        auto* form = RE::TESForm::LookupByID(actorFormID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!IsLikelyRemotePlayerProxy(actor)) {
            return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kNotRemote);
        }

        const auto* name = actor->GetName();
        if (!name || !*name) {
            return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kNotRemote);
        }

        std::optional<FormIdentity> identity;
        {
            std::scoped_lock lock(_snapshotMutex);
            const auto iterator = _remoteSnapshots.find(NormalizeName(name));
            if (iterator == _remoteSnapshots.end()) {
                return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kNotRemote);
            }
            identity = iterator->second.slots[slotIndex];
        }

        if (!identity) {
            return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kEmpty);
        }
        if (auto* resolved = ResolveFormIdentity(*identity)) {
            outFormID = resolved->GetFormID();
            return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kForm);
        }
        return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kEmpty);
    }
}
