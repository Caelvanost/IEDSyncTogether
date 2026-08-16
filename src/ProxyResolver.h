#pragma once

#include "PCH.h"

namespace IEDSyncTogether
{
    bool EqualsInsensitive(std::string_view left, std::string_view right);
    [[nodiscard]] bool IsDynamicActorCandidate(RE::Actor* actor);
    std::vector<RE::Actor*> FindRemotePlayerProxies(
        const std::vector<std::string>& playerNames);
    RE::Actor* FindRemotePlayerProxy(std::string_view playerName);
}
