#pragma once

#include "PCH.h"

namespace IEDSyncTogether
{
    using RemoteProxyScanCallback = std::function<void(std::vector<RE::FormID>)>;

    bool EqualsInsensitive(std::string_view left, std::string_view right);

    // Ask STR itself whether the dynamic actors currently present are remote
    // players. SkyrimTogetherUtils.IsRemotePlayer is an official STR 1.8.0
    // native Papyrus function and is the authority for this classification.
    bool RequestRemotePlayerProxyScan(RemoteProxyScanCallback callback);
    void ClearRemotePlayerProxyCache();

    [[nodiscard]] bool IsVerifiedRemotePlayerProxy(RE::Actor* actor);
    std::vector<RE::Actor*> FindRemotePlayerProxies();
    RE::Actor* FindRemotePlayerProxy(std::string_view playerName);
}
