#pragma once

#include "PCH.h"
#include "Config.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace IEDSyncTogether
{
    class UdpTransport
    {
    public:
        using PacketHandler = std::function<void(std::string)>;

        static UdpTransport& GetSingleton();

        bool Start(const Config& config, PacketHandler handler);
        void Stop();
        void Send(std::string_view payload);

        [[nodiscard]] bool IsRunning() const noexcept
        {
            return _running.load();
        }

        [[nodiscard]] std::string GetLocalPlayerName() const;
        [[nodiscard]] std::vector<std::string> GetPeerNames();

    private:
        struct Peer
        {
            sockaddr_in address{};
            std::string name;
            std::chrono::steady_clock::time_point lastSeen{};
        };

        UdpTransport() = default;
        ~UdpTransport();
        UdpTransport(const UdpTransport&) = delete;
        UdpTransport& operator=(const UdpTransport&) = delete;

        void ReceiverLoop();
        void DiscoveryLoop(std::stop_token token);
        void SendHello();
        void SendHelloTo(const sockaddr_in& destination);
        bool HandleDiscovery(std::string_view packet, const sockaddr_in& source);
        void TouchGameplayPeer(std::string_view packet, const sockaddr_in& source);
        void ExpirePeers();
        std::vector<sockaddr_in> SnapshotPeers();

        static std::optional<std::string> ReadField(
            std::string_view packet,
            std::string_view key);
        static std::string AddressToString(const sockaddr_in& address);

        Config _config{};
        PacketHandler _handler;
        SOCKET _socket{ INVALID_SOCKET };
        sockaddr_in _broadcast{};
        sockaddr_in _manualPeer{};
        bool _hasManualPeer{ false };
        std::string _instanceID;

        std::jthread _receiver;
        std::jthread _discovery;
        std::atomic_bool _running{ false };
        std::mutex _sendMutex;
        std::mutex _peerMutex;
        std::unordered_map<std::string, Peer> _peers;
    };
}
