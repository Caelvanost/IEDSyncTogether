#pragma once

#include "PCH.h"

namespace IEDSyncTogether
{
    class IEDRuntimeHook
    {
    public:
        static bool Install();
        static bool IsInstalled() noexcept;

        // Registers the exact transient Actor* of a resolved STR proxy.
        // v0.2.5 does not assume IED's private Game::ObjectRefHandle ABI.
        // The ProcessSlots wrapper first calibrates the Actor* field location
        // from repeated PlayerCharacter evaluations, then suppresses only an
        // exact tracked proxy pointer. The local PlayerCharacter is explicitly
        // excluded from suppression.
        static void TrackRemoteProxy(
            RE::FormID formID,
            RE::Actor* actor,
            bool tracked) noexcept;

        static void ClearTrackedProxies() noexcept;
    };
}
