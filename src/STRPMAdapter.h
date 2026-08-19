#pragma once

#include "PCH.h"
#include "LocalIEDState.h"
#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

namespace IEDSyncTogether
{
    class STRPMAdapter
    {
    public:
        static STRPMAdapter& GetSingleton();
        ~STRPMAdapter();

        bool Start();
        void Stop();
        void Publish(const LocalIEDState& state, std::string_view payload);

    private:
        static void STRPM_CALL OnMessage(const STRPM::Message* message, void* userData);
        bool LoadApi();
        std::string GetLocalPlayerName() const;

        std::atomic_bool _running{ false };
        const STRPM::Interface* _api{ nullptr };
        STRPM::ListenerHandle _listener{};
        STRPM::ConnectionID _localConnectionID{ 0 };
        std::mutex _sendMutex;
    };
}
