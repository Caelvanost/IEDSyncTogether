#include "PCH.h"
#include "SyncService.h"

#include "IEDSyncTogether/Interface.h"

namespace IEDSyncTogether
{
    bool SyncService::QueryRemoteSlotIdentity(
        RE::FormID actorFormID,
        std::uint32_t slotIndex,
        FormIdentity& outIdentity) const
    {
        if (!_running.load() ||
            !_gameLoaded.load() ||
            !_strConnected.load() ||
            !_remotePlayersAvailable.load() ||
            slotIndex >= IEDST::kSlotCount) {
            return false;
        }

        std::scoped_lock lock(_snapshotMutex);
        for (const auto& [peerID, snapshot] : _remoteSnapshots) {
            (void)peerID;
            if (snapshot.lastProxy != actorFormID) continue;
            const auto& identity = snapshot.slots[slotIndex];
            if (!identity) return false;
            outIdentity = *identity;
            return true;
        }
        return false;
    }
}
