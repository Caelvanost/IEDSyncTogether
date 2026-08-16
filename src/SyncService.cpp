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
            "Service started dormant: LAN peer id is persistent for the session; STR proxy FormIDs are treated as ephemeral; capture={}ms suppressRemoteNpcDisplays={}",
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
            SKSE::log::info("Game unloaded; LAN/IED synchronization suspended");
            return;
        }

        _lastReadySend = {};
        SKSE::log::info(
            "Game loaded; waiting for LAN peers and matching dynamic STR proxies");
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

    void SyncService::UpdateSTRSessionState(const std::vector<PeerBinding>& bindings)
    {
        const auto boundCount = static_cast<std::size_t>(std::count_if(
            bindings.begin(),
            bindings.end(),
            [](const auto& binding) { return binding.proxyFormID != 0; }));

        const bool hasRemotePlayers = boundCount != 0;
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
                "LAN peer binding active: {} remote proxy/proxies currently resolved; enabling IED synchronization",
                boundCount);
        } else {
            _capturePending.store(false);
            SKSE::log::info(
                "All remote proxies are temporarily unresolved; retaining LAN peer identities and remote snapshots");
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
        return std::any_of(
            _remoteSnapshots.begin(),
            _remoteSnapshots.end(),
            [](const auto& entry) { return entry.second.lastProxy != 0; });
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

        bool firstSeen = false;
        bool characterChanged = false;
        {
            std::scoped_lock lock(_peerStateMutex);
            auto& peer = _knownPeers[*instanceID];
            firstSeen = peer.name.empty();
            characterChanged = !peer.name.empty() && peer.name != *playerName;
            if (peer.name != *playerName) {
                peer.name = *playerName;
                peer.currentProxy = 0;
            }
            peer.lastSeen = std::chrono::steady_clock::now();
        }

        if (characterChanged) {
            std::scoped_lock lock(_snapshotMutex);
            _remoteSnapshots.erase(*instanceID);
        }

        if (firstSeen || characterChanged) {
            SKSE::log::info(
                "LAN peer ready: id={} character=\"{}\"{}",
                *instanceID,
                *playerName,
                characterChanged ? " (character changed; old snapshot discarded)" : "");
        }
    }

    void SyncService::ExpireKnownPeers()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto timeout = std::chrono::milliseconds(_config.peerTimeoutMs);
        std::vector<std::string> expired;

        {
            std::scoped_lock lock(_peerStateMutex);
            for (auto iterator = _knownPeers.begin(); iterator != _knownPeers.end();) {
                if (now - iterator->second.lastSeen > timeout) {
                    expired.push_back(iterator->first);
                    iterator = _knownPeers.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        }

        if (!expired.empty()) {
            std::scoped_lock lock(_snapshotMutex);
            for (const auto& peerID : expired) {
                _remoteSnapshots.erase(peerID);
                SKSE::log::info("LAN peer expired: id={}; remote snapshot discarded", peerID);
            }
        }
    }

    void SyncService::ClearKnownPeers()
    {
        std::scoped_lock lock(_peerStateMutex);
        _knownPeers.clear();
    }

    std::vector<SyncService::PeerBinding> SyncService::RefreshPeerBindings()
    {
        std::vector<PeerBinding> bindings;
        {
            std::scoped_lock lock(_peerStateMutex);
            bindings.reserve(_knownPeers.size());
            for (const auto& [peerID, peer] : _knownPeers) {
                bindings.push_back(PeerBinding{
                    .id = peerID,
                    .name = peer.name,
                    .proxyFormID = 0
                });
            }
        }

        std::unordered_set<RE::FormID> claimedProxies;
        for (auto& binding : bindings) {
            auto* proxy = binding.name.empty() ? nullptr : FindRemotePlayerProxy(binding.name);
            binding.proxyFormID = proxy ? proxy->GetFormID() : 0;
            if (binding.proxyFormID != 0 &&
                !claimedProxies.insert(binding.proxyFormID).second) {
                binding.proxyFormID = 0;
            }

            RE::FormID previousProxy = 0;
            {
                std::scoped_lock lock(_peerStateMutex);
                const auto iterator = _knownPeers.find(binding.id);
                if (iterator == _knownPeers.end()) {
                    binding.proxyFormID = 0;
                    continue;
                }
                previousProxy = iterator->second.currentProxy;
                iterator->second.currentProxy = binding.proxyFormID;
            }

            if (previousProxy != binding.proxyFormID) {
                if (previousProxy == 0 && binding.proxyFormID != 0) {
                    SKSE::log::info(
                        "LAN peer bound: id={} character=\"{}\" proxy={:08X}",
                        binding.id,
                        binding.name,
                        binding.proxyFormID);
                } else if (previousProxy != 0 && binding.proxyFormID == 0) {
                    SKSE::log::info(
                        "LAN peer proxy disappeared: id={} character=\"{}\" oldProxy={:08X}; retaining peer state",
                        binding.id,
                        binding.name,
                        previousProxy);
                } else {
                    SKSE::log::info(
                        "LAN peer rebound after proxy recreation: id={} character=\"{}\" {:08X}->{:08X}",
                        binding.id,
                        binding.name,
                        previousProxy,
                        binding.proxyFormID);
                }
            }

            {
                std::scoped_lock lock(_snapshotMutex);
                if (const auto iterator = _remoteSnapshots.find(binding.id);
                    iterator != _remoteSnapshots.end()) {
                    iterator->second.lastProxy = binding.proxyFormID;
                }
            }
        }

        return bindings;
    }

    void SyncService::Tick()
    {
        if (!_running.load() || !_gameLoaded.load()) {
            return;
        }

        SendReadyHeartbeat();
        ExpireKnownPeers();

        const auto bindings = RefreshPeerBindings();
        UpdateSTRSessionState(bindings);
        RefreshProxyMitigation(bindings);

        if (!_strConnected.load() || !_remotePlayersAvailable.load()) {
            return;
        }

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

        if (packet.find("|STATE|") == std::string::npos) {
            return;
        }

        const auto instanceID = ReadField(packet, "id");
        const auto encodedSender = ReadField(packet, "from");
        const auto revisionText = ReadField(packet, "rev");
        const auto slotsText = ReadField(packet, "slots");
        if (!instanceID || !encodedSender || !revisionText || !slotsText) {
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

        RE::FormID currentProxy = 0;
        bool characterChanged = false;
        {
            std::scoped_lock lock(_peerStateMutex);
            auto& peer = _knownPeers[*instanceID];
            characterChanged = !peer.name.empty() && peer.name != *sender;
            if (peer.name != *sender) {
                peer.name = *sender;
                peer.currentProxy = 0;
            }
            peer.lastSeen = std::chrono::steady_clock::now();
            currentProxy = peer.currentProxy;
        }

        {
            std::scoped_lock lock(_snapshotMutex);
            auto& snapshot = _remoteSnapshots[*instanceID];
            if (characterChanged || snapshot.characterName != *sender) {
                snapshot = RemoteSnapshot{};
                snapshot.characterName = *sender;
            }

            if (revision >= snapshot.revision) {
                snapshot.slots = *slots;
                snapshot.revision = revision;
                snapshot.lastProxy = currentProxy;
            } else {
                return;
            }
        }

        const auto visible = std::count_if(
            slots->begin(),
            slots->end(),
            [](const auto& slot) { return slot.has_value(); });
        SKSE::log::info(
            "Remote IED state stored: peer={} character=\"{}\" proxy={} revision={} slots={}; snapshot survives proxy recreation",
            *instanceID,
            *sender,
            currentProxy ? fmt::format("{:08X}", currentProxy) : std::string("unresolved"),
            revision,
            visible);
    }

    void SyncService::RefreshProxyMitigation(const std::vector<PeerBinding>& bindings)
    {
        std::unordered_set<RE::FormID> current;
        for (const auto& binding : bindings) {
            if (binding.proxyFormID == 0) {
                continue;
            }

            current.insert(binding.proxyFormID);
            if (_config.suppressRemoteNpcDisplays &&
                _blockedProxies.insert(binding.proxyFormID).second) {
                auto* form = RE::TESForm::LookupByID(binding.proxyFormID);
                auto* actor = form ? form->As<RE::Actor>() : nullptr;
                if (actor) {
                    IEDBridge::GetSingleton().SetActorBlocked(actor, true);
                    SKSE::log::info(
                        "Suppressed IED NPC display: peer={} character=\"{}\" proxy={:08X}",
                        binding.id,
                        binding.name,
                        binding.proxyFormID);
                }
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

        std::optional<FormIdentity> identity;
        bool matched = false;
        {
            std::scoped_lock lock(_snapshotMutex);
            for (const auto& [peerID, snapshot] : _remoteSnapshots) {
                (void)peerID;
                if (snapshot.lastProxy != actorFormID) {
                    continue;
                }
                identity = snapshot.slots[slotIndex];
                matched = true;
                break;
            }
        }

        if (!matched) {
            return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kNotRemote);
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
