#pragma once

#include "PCH.h"

namespace IEDSyncTogether
{
    class IEDRuntimeHook
    {
    public:
        static bool Install();
        static bool IsInstalled() noexcept;

        // Registers only the transient STR proxy address. The runtime shim uses
        // pointer equality and never interprets or dereferences IED private
        // ProcessParams fields.
        static void TrackRemoteProxy(RE::FormID formID, RE::Actor* actor, bool tracked) noexcept;
        static void ClearTrackedProxies() noexcept;
    };
}
