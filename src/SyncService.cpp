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

        _gameLoaded.store(false);
        _strConnected.store(false);
        _remotePlayersAvailable.store(false);
        _capturePending.store(false);
        _lastReadySend = {};
        ClearKnownPeers();
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
            "Service started dormant: remote proxies are matched by IED peer character name; no STR Papyrus calls; capture={}ms suppressRemoteNpcDisplays={}",
            _config.captureIntervalMs,
            _config.suppressRemoteNpcDisplays ? 1 : 0);
    }

    void SyncService::Stop()
    {
        _gameLoaded.store(false);
        SuspendSTRSession();
        ClearKnownPeers();
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
        _gameLoaded.store(false);
        SuspendSTRSession();
        ClearKnownPeers();
    }

    void SyncService::SetGameLoaded(bool loaded) noexcept
    {
        _gameLoaded.store(loaded);
        if (!loaded) {
            SuspendSTRSession();
            ClearKnownPeers();
            SKSE::log::info("Game unloaded; STR/IED synchronization suspended");
            return;
        }

        _lastReadySend = {};
        SKSE::log::info(
            "Game loaded; waiting for an IED peer name to match a dynamic STR proxy before enabling synchronization");
    }

    void SyncService::SuspendSTRSession()
    {
        _strConnected.store(false);
        _remotePlayersAvailable.store(false);
        _capturePending.store(false);
        _blockedProxies.clear();
        {
            std::scoped_lock lock(_snapshotMutex);
            _remoteSnapshots.clear();
        }
        _localSlots = {};
        _hasLocalSlots = false;
        _lastSend = {};
    }

    void SyncService::UpdateSTRSessionState(const std::vector<RE::Actor*>& proxies)
    {
        const bool hasRemotePlayers = !proxies.empty();
        _remotePlayersAvailable.store(hasRemotePlayers);

        const bool wasConnected = _strConnected.exchange(hasRemotePlayers);
        if (hasRemotePlayers == wasConnected) {
            return;
        }

        if (hasRemotePlayers) {
            _capturePending.store(false);
            _hasLocalSlots = false;
            _lastSend = {};
            SKSE::log::info(
                "STR proxy matched safely by IED peer name: {} remote player proxy/proxies; enabling IED synchronization",
                proxies.size());
        } else {
            SKSE::log::info(
                "No dynamic actor matches an active IED peer name; suspending IED synchronization");
            SuspendSTRSession();
        }
    }

    bool SyncService::CanApplyRuntimeOverrides() const
    {
        if (!_running.load() ||
            !_gameLoaded.load() ||
            !_strConnected.load() ||
            !_remotePlayersAvailable.load()) {
            return false;
        }

        std::scoped_lock lock(_snapshotMutex);
        return !_remoteSnapshots.empty();
    }

    void SyncService::TimerLoop(std::stop_token token)
    {
        while (!token.stop_requested() && _running.load()) {
            if (!_gameLoaded.load()) {
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

    void SyncService::SendReadyHeartbeat()
    {
        if (!_config.networkEnabled) {
            return;
        }

        auto& transport = UdpTransport::GetSingleton();
        if (!transport.IsRunning()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto interval = std::chrono::milliseconds(
            std::max<std::uint32_t>(_config.discoveryIntervalMs, 500));
        if (_lastReadySend != std::chrono::steady_clock::time_point{} &&
            now - _lastReadySend < interval) {
            return;
        }

        const auto playerName = transport.GetLocalPlayerName();
        transport.Send(fmt::format(
            "READY|from={}",
            HexEncode(playerName)));
        _lastReadySend = now;
    }

    void SyncService::HandleReadyPacket(std::string_view packet)
    {
        const auto instanceID = ReadField(packet, "id");
        const auto encodedName = ReadField(packet, "from");
        const auto playerName = encodedName ? HexDecode(*encodedName) : std::nullopt;
        if (!instanceID || !playerName || playerName->empty()) {
            return;
        }

        bool changed = false;
        {
            std::scoped_lock lock(_peerStateMutex);
            auto& peer = _knownPeers[*instanceID];
            changed = peer.name != *playerName;
            peer.name = *playerName;
            peer.lastSeen = std::chrono::steady_clock::now();
        }

        if (changed) {
            SKSE::log::info(
                "IED peer ready: player=\"{}\"; waiting for a dynamic actor with the same name",
                *playerName);
        }
    }

    void SyncService::ExpireKnownPeers()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::milliseconds(_config.peerTimeoutMs);
        std::scoped_lock lock(_peerStateMutex);
        for (auto iterator = _knownPeers.begin(); iterator != _knownPeers.end();) {
            if (now - iterator->second.lastSeen > timeout) {
                iterator = _knownPeers.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void SyncService::ClearKnownPeers()
    {
        std::scoped_lock lock(_peerStateMutex);
        _knownPeers.clear();
    }

    std::vector<std::string> SyncService::SnapshotKnownPeerNames() const
    {
        std::vector<std::string> result;
        std::unordered_set<std::string> seen;
        std::scoped_lock lock(_peerStateMutex);
        for (const auto& [instanceID, peer] : _knownPeers) {
            (void)instanceID;
            if (peer.name.empty()) {
                continue;
            }
            const auto normalized = NormalizeName(peer.name);
            if (seen.insert(normalized).second) {
                result.push_back(peer.name);
            }
        }
        return result;
    }

    void SyncService::Tick()
    {
        if (!_running.load() || !_gameLoaded.load()) {
            return;
        }

        SendReadyHeartbeat();
        ExpireKnownPeers();

        const auto peerNames = SnapshotKnownPeerNames();
        const auto proxies = FindRemotePlayerProxies(peerNames);
        UpdateSTRSessionState(proxies);
        if (!_strConnected.load() || !_remotePlayersAvailable.load()) {
            return;
        }

        RefreshProxyMitigation(proxies);

        bool expected = false;
        if (!_capturePending.compare_exchange_strong(expected, true)) {
            return;
        }

        const bool requested = IEDBridge::GetSingleton().CapturePlayerSlots(
            [this](SlotState slots) {
                _capturePending.store(false);
                if (!_gameLoaded.load() ||
                    !_strConnected.load() ||
                    !_remotePlayersAvailable.load()) {
                    return;
                }
                OnLocalCapture(std::move(slots));
            });

        if (!requested) {
            _capturePending.store(false);
        }
    }

    void SyncService::OnLocalCapture(SlotState slots)
    {
        if (!_running.load() ||
            !_gameLoaded.load() ||
            !_strConnected.load() ||
            !_remotePlayersAvailable.load()) {
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
        if (!_gameLoaded.load() || !packet.starts_with(kGameplayPrefix)) {
            return;
        }

        if (packet.find("|READY|") != std::string::npos) {
            HandleReadyPacket(packet);
            return;
        }

        if (!_strConnected.load() || !_remotePlayersAvailable.load()) {
            return;
        }

        if (packet.find("|STATE|") == std::string::npos) {
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
            snapshot.lastProxy = 0;
            SKSE::log::info(
                "Remote IED state waiting for name-matched proxy: player=\"{}\" revision={}",
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
            "Remote IED state matched: player=\"{}\" proxy={:08X} revision={} slots={}/{} adapter=peer-name",
            sender,
            proxy->GetFormID(),
            snapshot.revision,
            resolved,
            visible);
    }

    void SyncService::RefreshProxyMitigation(const std::vector<RE::Actor*>& proxies)
    {
        std::unordered_set<RE::FormID> current;
        for (auto* proxy : proxies) {
            if (!proxy) {
                continue;
            }

            current.insert(proxy->GetFormID());
            if (_config.suppressRemoteNpcDisplays &&
                _blockedProxies.insert(proxy->GetFormID()).second) {
                IEDBridge::GetSingleton().SetActorBlocked(proxy, true);
                const auto* proxyName = proxy->GetName();
                SKSE::log::info(
                    "Suppressed IED NPC display on name-matched proxy {:08X} \"{}\"",
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
        if (!_running.load() ||
            !_gameLoaded.load() ||
            !_strConnected.load() ||
            !_remotePlayersAvailable.load() ||
            slotIndex >= IEDST::kSlotCount) {
            return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kNotRemote);
        }

        auto* form = RE::TESForm::LookupByID(actorFormID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!IsDynamicActorCandidate(actor)) {
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
            if (iterator == _remoteSnapshots.end() ||
                iterator->second.lastProxy != actorFormID) {
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
