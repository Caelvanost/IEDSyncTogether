#include "PCH.h"
#include "ProxyRegistry.h"

namespace IEDSyncTogether
{
    ProxyRegistry& ProxyRegistry::GetSingleton()
    {
        static ProxyRegistry instance;
        return instance;
    }

    void ProxyRegistry::Register(STRPM::ProxyFormID proxyFormID)
    {
        if (proxyFormID == STRPM::kInvalidProxyFormID) {
            return;
        }

        std::scoped_lock lock(_mutex);
        _remoteProxyFormIDs.insert(proxyFormID);
    }

    void ProxyRegistry::Unregister(STRPM::ProxyFormID proxyFormID)
    {
        if (proxyFormID == STRPM::kInvalidProxyFormID) {
            return;
        }

        std::scoped_lock lock(_mutex);
        _remoteProxyFormIDs.erase(proxyFormID);
    }

    void ProxyRegistry::Clear()
    {
        std::scoped_lock lock(_mutex);
        _remoteProxyFormIDs.clear();
    }

    bool ProxyRegistry::Contains(RE::FormID formID) const
    {
        if (formID == 0) {
            return false;
        }

        std::scoped_lock lock(_mutex);
        return _remoteProxyFormIDs.contains(formID);
    }

    std::size_t ProxyRegistry::Size() const
    {
        std::scoped_lock lock(_mutex);
        return _remoteProxyFormIDs.size();
    }
}
