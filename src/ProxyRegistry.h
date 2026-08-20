#pragma once

#include "PCH.h"
#include "STRPluginMessagingAPI/STRPluginMessagingAPI.h"

namespace IEDSyncTogether
{
    class ProxyRegistry
    {
    public:
        static ProxyRegistry& GetSingleton();

        void Register(STRPM::ProxyFormID proxyFormID);
        void Unregister(STRPM::ProxyFormID proxyFormID);
        void Clear();
        [[nodiscard]] bool Contains(RE::FormID formID) const;
        [[nodiscard]] std::size_t Size() const;

    private:
        mutable std::mutex _mutex;
        std::unordered_set<RE::FormID> _remoteProxyFormIDs;
    };
}
