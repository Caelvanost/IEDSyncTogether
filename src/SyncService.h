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

    private:
        struct RemoteSnapshot
        {
            SlotState slots{};
            std::uint64_t revision{ 0 };
            RE::FormID lastProxy{ 0 };
        };

        SyncService() = default;
        ~SyncService();
        SyncService(const SyncService&) = delete;
        SyncService& operator=(const SyncService&) = delete;

        void TimerLoop(std::stop_token token);
        void Tick();
        void OnLocalCapture(SlotState slots);
        void UpdateSTRSessionState(const std::vector<RE::Actor*>& proxies);
        void SuspendSTRSession();
        void RefreshProxyMitigation(const std::vector<RE::Actor*>& proxies);
        void LogRemoteResolution(std::string_view sender, RemoteSnapshot& snapshot);

        static std::optional<std::string> ReadField(
            std::string_view packet,
            std::string_view key);
        static std::string NormalizeName(std::string_view name);

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
        mutable std::mutex _snapshotMutex;
        std::unordered_map<std::string, RemoteSnapshot> _remoteSnapshots;
        std::unordered_set<RE::FormID> _blockedProxies;
    };
}
