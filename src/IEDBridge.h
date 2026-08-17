#pragma once

#include "PCH.h"
#include "FormIdentity.h"

namespace IEDSyncTogether
{
    class IEDBridge
    {
    public:
        using CaptureCallback = std::function<void(SlotState)>;

        static IEDBridge& GetSingleton();

        bool IsInstalled() const;
        bool CapturePlayerSlots(CaptureCallback callback);

        // Kept under the historical name because SyncService already uses this
        // compatibility hook. In v0.2.0 it no longer blocks IED. A true value
        // registers the STR proxy for authoritative Custom Item rendering; false
        // unregisters it and removes IEDSyncTogether-owned Custom Items.
        bool SetActorBlocked(RE::Actor* actor, bool blocked) const;
        void ResetRemoteRendering() const;

    private:
        IEDBridge() = default;
    };
}
