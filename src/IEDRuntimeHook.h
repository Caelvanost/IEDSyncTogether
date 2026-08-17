#pragma once

#include "PCH.h"

namespace IEDSyncTogether
{
    class IEDRuntimeHook
    {
    public:
        static bool Install();
        static bool IsInstalled() noexcept;

        // Registers the transient Skyrim reference handle of a resolved STR
        // proxy. The ProcessSlots hook reads only the source-verified first
        // 32-bit handle field of IED's ProcessParams and never dereferences or
        // reconstructs any other private IED structure.
        static void TrackRemoteProxy(
            RE::FormID formID,
            RE::Actor* actor,
            bool tracked) noexcept;

        static void ClearTrackedProxies() noexcept;
    };
}
