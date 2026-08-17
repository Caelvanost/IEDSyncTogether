#include "PCH.h"

#include "IEDBridge.h"
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

        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded:
            // All IED integration goes through the public Papyrus API shipped
            // by official Immersive Equipment Displays 1.7.4. v0.3.0 uses
            // IED's own global NPC-slot suppression as the development
            // baseline instead of intercepting any private IED runtime code.
            SKSE::log::info(
                "IED integration mode: official Papyrus Custom Item API; no IED runtime patch installed");
            SKSE::log::warn(
                "IED v0.3.0 baseline: enable \"Disable NPC equipment displays\" in IED on every STR client to prevent stock NPC slot duplication while remote Custom Item restitution is developed");
            service.Start();
            break;

        case SKSE::MessagingInterface::kPreLoadGame:
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
