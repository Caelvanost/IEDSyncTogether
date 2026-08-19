#pragma once

#include "PCH.h"
#include "LocalIEDState.h"
#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

namespace IEDSyncTogether
{
    class RemoteIEDRenderer
    {
    public:
        static RemoteIEDRenderer& GetSingleton();

        bool Apply(
            STRPM::ConnectionID connectionID,
            STRPM::ProxyFormID proxyFormID,
            std::string_view displayName,
            const LocalIEDState& state,
            bool force = false);

        void ClearProxy(STRPM::ProxyFormID proxyFormID);
        void Reset();

    private:
        RemoteIEDRenderer() = default;

        std::unordered_map<STRPM::ProxyFormID, std::string> _appliedSignatures;
    };
}
