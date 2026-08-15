#include "PCH.h"
#include "IEDRuntimeHook.h"

#include "IEDSyncTogether/Interface.h"
#include "SyncService.h"

#include <cstddef>
#include <cstring>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr std::uintptr_t kSelectSlotItemCallRva = 0xE3806;
        constexpr std::uintptr_t kSelectSlotItemRva = 0x146360;
        constexpr std::ptrdiff_t kProcessParamsActorOffset = 0x38;
        constexpr std::ptrdiff_t kObjectEntrySlotIdOffset = 0x20;

        constexpr std::array<std::uint8_t, 5> kExpectedCall{
            0xE8, 0x55, 0x2B, 0x06, 0x00
        };

        constexpr std::array<std::uint8_t, 16> kExpectedSelectSlotItemPrologue{
            0x4C, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x30
        };

        struct ItemDataABI
        {
            RE::TESBoundObject* form;
        };

        struct SlotCandidateABI
        {
            const ItemDataABI* item;
            std::uint32_t extra;
            std::uint32_t rating;
        };
        static_assert(sizeof(SlotCandidateABI) == 0x10);

        struct SlotItemCandidatesABI
        {
            SlotCandidateABI* begin;
            SlotCandidateABI* end;
            SlotCandidateABI* capacity;
        };
        static_assert(sizeof(SlotItemCandidatesABI) == 0x18);

        struct SelectedItemABI
        {
            SlotCandidateABI* iterator;
            bool hasValue;
            std::byte padding[7];
        };
        static_assert(sizeof(SelectedItemABI) == 0x10);

        using SelectSlotItemFn = SelectedItemABI* (*)(
            SelectedItemABI* result,
            void* processParams,
            const void* configSlot,
            SlotItemCandidatesABI* candidates,
            const void* objectEntrySlot) noexcept;

        SelectSlotItemFn g_originalSelectSlotItem{ nullptr };
        SKSE::Trampoline g_iedTrampoline{ "IEDSyncTogether.IED" };
        bool g_installed{ false };

        bool MatchBytes(std::uintptr_t address, const auto& expected) noexcept
        {
            return std::memcmp(
                reinterpret_cast<const void*>(address),
                expected.data(),
                expected.size()) == 0;
        }

        SelectedItemABI* MakeEmpty(SelectedItemABI* result) noexcept
        {
            if (result) {
                result->iterator = nullptr;
                result->hasValue = false;
            }
            return result;
        }

        SelectedItemABI* SelectSlotItemHook(
            SelectedItemABI* result,
            void* processParams,
            const void* configSlot,
            SlotItemCandidatesABI* candidates,
            const void* objectEntrySlot) noexcept
        {
            if (!g_originalSelectSlotItem ||
                !result ||
                !processParams ||
                !candidates ||
                !objectEntrySlot) {
                return g_originalSelectSlotItem ?
                    g_originalSelectSlotItem(result, processParams, configSlot, candidates, objectEntrySlot) :
                    MakeEmpty(result);
            }

            const auto paramsBytes = static_cast<const std::byte*>(processParams);
            auto* actor = *reinterpret_cast<RE::Actor* const*>(
                paramsBytes + kProcessParamsActorOffset);
            if (!actor) {
                return g_originalSelectSlotItem(
                    result,
                    processParams,
                    configSlot,
                    candidates,
                    objectEntrySlot);
            }

            const auto slotBytes = static_cast<const std::byte*>(objectEntrySlot);
            const auto slotIndex = *reinterpret_cast<const std::uint32_t*>(
                slotBytes + kObjectEntrySlotIdOffset);
            if (slotIndex >= IEDST::kSlotCount) {
                return g_originalSelectSlotItem(
                    result,
                    processParams,
                    configSlot,
                    candidates,
                    objectEntrySlot);
            }

            RE::FormID desiredForm = 0;
            const auto overrideResult = SyncService::GetSingleton().QueryRemoteSlot(
                actor->GetFormID(),
                slotIndex,
                desiredForm);

            switch (static_cast<IEDST::SlotOverrideResult>(overrideResult)) {
            case IEDST::SlotOverrideResult::kNotRemote:
                return g_originalSelectSlotItem(
                    result,
                    processParams,
                    configSlot,
                    candidates,
                    objectEntrySlot);

            case IEDST::SlotOverrideResult::kEmpty:
                return MakeEmpty(result);

            case IEDST::SlotOverrideResult::kForm:
                break;

            default:
                return g_originalSelectSlotItem(
                    result,
                    processParams,
                    configSlot,
                    candidates,
                    objectEntrySlot);
            }

            if (!desiredForm || !candidates->begin || !candidates->end) {
                return MakeEmpty(result);
            }

            for (auto* candidate = candidates->begin;
                 candidate != candidates->end;
                 ++candidate) {
                if (!candidate->item || !candidate->item->form) {
                    continue;
                }

                if (candidate->item->form->GetFormID() == desiredForm) {
                    result->iterator = candidate;
                    result->hasValue = true;
                    return result;
                }
            }

            // The remote state is authoritative. If its form is unavailable in
            // IED's locally generated candidate set, keep this display slot empty.
            return MakeEmpty(result);
        }
    }

    bool IEDRuntimeHook::Install()
    {
        if (g_installed) {
            return true;
        }

        const auto module = GetModuleHandleW(L"ImmersiveEquipmentDisplays.dll");
        if (!module) {
            SKSE::log::critical("IED runtime hook: ImmersiveEquipmentDisplays.dll is not loaded");
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto callSite = base + kSelectSlotItemCallRva;
        const auto selectSlotItem = base + kSelectSlotItemRva;

        if (!MatchBytes(callSite, kExpectedCall)) {
            SKSE::log::critical(
                "IED runtime hook: unsupported IED build (SelectSlotItem call signature mismatch at RVA 0x{:X})",
                kSelectSlotItemCallRva);
            return false;
        }

        if (!MatchBytes(selectSlotItem, kExpectedSelectSlotItemPrologue)) {
            SKSE::log::critical(
                "IED runtime hook: unsupported IED build (SelectSlotItem signature mismatch at RVA 0x{:X})",
                kSelectSlotItemRva);
            return false;
        }

        std::int32_t displacement = 0;
        std::memcpy(
            &displacement,
            reinterpret_cast<const void*>(callSite + 1),
            sizeof(displacement));
        const auto originalTarget = callSite + 5 + displacement;
        if (originalTarget != selectSlotItem) {
            SKSE::log::critical(
                "IED runtime hook: call target mismatch (expected RVA 0x{:X})",
                kSelectSlotItemRva);
            return false;
        }

        g_iedTrampoline.create(64, module);
        const auto original = g_iedTrampoline.write_call<5>(
            callSite,
            SelectSlotItemHook);
        g_originalSelectSlotItem = reinterpret_cast<SelectSlotItemFn>(original);

        if (reinterpret_cast<std::uintptr_t>(g_originalSelectSlotItem) != selectSlotItem) {
            SKSE::log::critical("IED runtime hook: trampoline returned an unexpected original target");
            return false;
        }

        g_installed = true;
        SKSE::log::info(
            "IED 1.7.4 runtime hook installed: call RVA=0x{:X} SelectSlotItem RVA=0x{:X}",
            kSelectSlotItemCallRva,
            kSelectSlotItemRva);
        return true;
    }

    bool IEDRuntimeHook::IsInstalled() noexcept
    {
        return g_installed;
    }
}
