#pragma once

#include "PCH.h"
#include "Config.h"
#include "FormIdentity.h"

namespace IEDSyncTogether
{
    class SyncService
    {
    public:
        static SyncService& GetSingleton();

        void Start();
        void Stop();
        void Reset();
        void SetGameLoaded(bool loaded) noexcept;
        [[nodiscard]] bool CanApplyRuntimeOverrides() const;
        void HandlePacket(std::string packet);
        std::uint32_t QueryRemoteSlot(
            RE::FormID actorFormID,
            std::uint32_t slotIndex,
            RE::FormID& outFormID) const;
        bool QueryRemoteSlotIdentity(
            RE::FormID actorFormID,
            std::uint32_t slotIndex,
            FormIdentity& outIdentity) const;

        [[nodiscard]] bool IsRemoteNpcSuppressionEnabled() const noexcept
        {
            return _config.suppressRemoteNpcDisplays;
        }

        [[nodiscard]] std::vector<RE::FormID> GetResolvedRemoteProxies() const
        {
            std::vector<RE::FormID> result;
            std::scoped_lock lock(_peerStateMutex);
            result.reserve(_knownPeers.size());
            for (const auto& [peerID, peer] : _knownPeers) {
                (void)peerID;
                if (peer.currentProxy != 0) result.push_back(peer.currentProxy);
            }
            return result;
        }

    private:
        struct RemoteSnapshot
        {
            std::string characterName;
            SlotState slots{};
            std::uint64_t revision{ 0 };
            RE::FormID lastProxy{ 0 };
        };

        struct KnownPeer
        {
            std::string name;
            RE::FormID currentProxy{ 0 };
            std::chrono::steady_clock::time_point lastSeen{};
        };

        struct PeerBinding
        {
            std::string id;
            std::string name;
            RE::FormID proxyFormID{ 0 };
        };

        SyncService() = default;
        ~SyncService();
        SyncService(const SyncService&) = delete;
        SyncService& operator=(const SyncService&) = delete;

        void TimerLoop(std::stop_token token);
        void Tick();
        void OnLocalCapture(SlotState slots);
        void UpdateSTRSessionState(const std::vector<PeerBinding>& bindings);
        void SuspendSTRSession();
        void RefreshProxyMitigation(const std::vector<PeerBinding>& bindings);
        void SendReadyHeartbeat();
        void HandleReadyPacket(std::string_view packet);
        void ExpireKnownPeers();
        void ClearKnownPeers();
        std::vector<PeerBinding> RefreshPeerBindings();

        static std::optional<std::string> ReadField(
            std::string_view packet,
            std::string_view key);

        Config _config{};
        std::jthread _timer;
        std::atomic_bool _running{ false };
        std::atomic_bool _gameLoaded{ false };
        std::atomic_bool _strConnected{ false };
        std::atomic_bool _remotePlayersAvailable{ false };
        std::atomic_bool _capturePending{ false };
        SlotState _localSlots{};
        bool _hasLocalSlots{ false };
        std::uint64_t _revision{ 0 };
        std::chrono::steady_clock::time_point _lastSend{};
        std::chrono::steady_clock::time_point _lastReadySend{};
        mutable std::mutex _snapshotMutex;
        std::unordered_map<std::string, RemoteSnapshot> _remoteSnapshots;
        std::unordered_set<RE::FormID> _blockedProxies;
        mutable std::mutex _peerStateMutex;
        std::unordered_map<std::string, KnownPeer> _knownPeers;
    };
}
