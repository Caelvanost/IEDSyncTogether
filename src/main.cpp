#include "PCH.h"

#include "IEDBridge.h"
#include "IEDDiagnostics.h"
#include "IEDSyncTogether/Interface.h"
#include "SyncService.h"

namespace
{
    void InitializeLogging()
    {
        auto directory = SKSE::log::log_directory();
        if (!directory) {
            return;
        }
        *directory /= "IEDSyncTogether.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            directory->string(),
            true);
        auto logger = std::make_shared<spdlog::logger>("IEDSyncTogether", std::move(sink));
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_level(spdlog::level::trace);
        spdlog::flush_on(spdlog::level::trace);
    }

    void OnSKSEMessage(SKSE::MessagingInterface::Message* message)
    {
        using namespace IEDSyncTogether;
        auto& service = SyncService::GetSingleton();
        auto& diagnostics = IEDDiagnostics::GetSingleton();

        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            // v0.3.2 removes the v0.3.1 ActorBlock experiment completely.
            // Rendering stays on IED's official Papyrus Custom Item API while
            // read-only diagnostics verify the setting and proxy classification
            // assumptions behind IED's native disable_npc_slots path.
            SKSE::log::info(
                "IED integration mode: official Papyrus Custom Item API + read-only diagnostics; no ActorBlock probe and no IED runtime patch installed");
            SKSE::log::info(
                "IED v0.3.2 diagnostic: checking Data\\SKSE\\Plugins\\IED\\Settings.json disable_npc_slots and exact STR proxy vs PlayerCharacter identity");
            service.Start();
            diagnostics.Start();
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
            diagnostics.Reset();
            // IED custom entries are save-persistent. Clear every proxy still
            // owned by this runtime before the VM swaps to another save.
            IEDBridge::GetSingleton().ResetRemoteRendering();
            service.SetGameLoaded(false);
            service.Reset();
            break;

        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            service.SetGameLoaded(true);
            break;

        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    InitializeLogging();
    SKSE::Init(skse);
    SKSE::log::info("IEDSyncTogether v{} loading", IEDST_VERSION_STRING);

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging) {
        SKSE::log::critical("SKSE messaging interface is unavailable");
        return false;
    }
    messaging->RegisterListener(OnSKSEMessage);
    return true;
}

extern "C" std::uint32_t IEDST_GetInterfaceVersion() noexcept
{
    return IEDST::kInterfaceVersion;
}

extern "C" std::uint32_t IEDST_QuerySlotOverride(
    std::uint32_t actorFormID,
    std::uint32_t slotIndex,
    std::uint32_t* outFormID) noexcept
{
    if (!outFormID) {
        return static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kNotRemote);
    }

    RE::FormID resolved = 0;
    const auto result = IEDSyncTogether::SyncService::GetSingleton().QueryRemoteSlot(
        actorFormID,
        slotIndex,
        resolved);
    *outFormID = resolved;
    return result;
}
