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
        constexpr std::ptrdiff_t kObjectEntrySlotIdOffset = 0x24;

        // CommonLib searches a +/-2 GiB window around the address passed to
        // Trampoline::create(). Bias the search 1 GiB above the IED call-site,
        // so its low-to-high allocator starts at callSite-1 GiB instead of the
        // fragile exact -2 GiB rel32 boundary.
        constexpr std::uintptr_t kTrampolineSearchBias = 0x40000000ull;

        constexpr std::array<std::uint8_t, 5> kExpectedCall{
            0xE8, 0x55, 0x2B, 0x06, 0x00
        };

        constexpr std::array<std::uint8_t, 16> kExpectedSelectSlotItemPrologue{
            0x4C, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x30
        };

#pragma pack(push, 1)
        struct AbsoluteJumpRelay
        {
            std::uint8_t jmp{ 0xFF };
            std::uint8_t modrm{ 0x25 };
            std::int32_t displacement{ 0 };
            std::uint64_t target{ 0 };
        };
        static_assert(sizeof(AbsoluteJumpRelay) == 0xE);

        struct RelativeCall
        {
            std::uint8_t opcode{ 0xE8 };
            std::int32_t displacement{ 0 };
        };
        static_assert(sizeof(RelativeCall) == 0x5);
#pragma pack(pop)

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

        bool GetRelative32(
            std::uintptr_t nextInstruction,
            std::uintptr_t target,
            std::int32_t& out) noexcept
        {
            const auto displacement =
                static_cast<std::int64_t>(target) -
                static_cast<std::int64_t>(nextInstruction);

            if (displacement < (std::numeric_limits<std::int32_t>::min)() ||
                displacement > (std::numeric_limits<std::int32_t>::max)()) {
                return false;
            }

            out = static_cast<std::int32_t>(displacement);
            return true;
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

            // Save loading and the pre-network state must be indistinguishable
            // from stock IED. This also prevents us from reading private IED
            // structure offsets until an actual remote snapshot exists.
            auto& service = SyncService::GetSingleton();
            if (!service.CanApplyRuntimeOverrides()) {
                return g_originalSelectSlotItem(
                    result,
                    processParams,
                    configSlot,
                    candidates,
                    objectEntrySlot);
            }

            // Verified against the official IED 1.7.4 binary:
            // ProcessParams::CommonParams::actor is read at +0x38 in ProcessSlots.
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

            // Verified at the SelectSlotItem call-site: ObjectEntrySlot::slotid
            // is read from +0x24 immediately after the call returns.
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
            const auto overrideResult = service.QueryRemoteSlot(
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

        std::int32_t originalDisplacement = 0;
        std::memcpy(
            &originalDisplacement,
            reinterpret_cast<const void*>(callSite + 1),
            sizeof(originalDisplacement));
        const auto originalTarget = callSite + 5 + originalDisplacement;
        if (originalTarget != selectSlotItem) {
            SKSE::log::critical(
                "IED runtime hook: call target mismatch (expected RVA 0x{:X})",
                kSelectSlotItemRva);
            return false;
        }

        // A 5-byte x64 CALL can only reach +/-2 GiB. IEDSyncTogether.dll may be
        // loaded farther away than that, so place an absolute-jump relay near
        // IED and patch the original CALL to target the relay.
        const auto searchCenter = callSite + kTrampolineSearchBias;
        g_iedTrampoline.create(
            64,
            reinterpret_cast<void*>(searchCenter));

        auto* relay = g_iedTrampoline.allocate<AbsoluteJumpRelay>();
        relay->target = reinterpret_cast<std::uint64_t>(&SelectSlotItemHook);

        std::int32_t relayDisplacement = 0;
        const auto relayAddress = reinterpret_cast<std::uintptr_t>(relay);
        if (!GetRelative32(callSite + sizeof(RelativeCall), relayAddress, relayDisplacement)) {
            SKSE::log::critical(
                "IED runtime hook: allocated relay is outside rel32 range (call=0x{:X}, relay=0x{:X})",
                callSite,
                relayAddress);
            return false;
        }

        RelativeCall patchedCall;
        patchedCall.displacement = relayDisplacement;
        REL::safe_write(callSite, &patchedCall, sizeof(patchedCall));

        g_originalSelectSlotItem = reinterpret_cast<SelectSlotItemFn>(selectSlotItem);
        g_installed = true;

        SKSE::log::info(
            "IED 1.7.4 runtime hook installed: call RVA=0x{:X} SelectSlotItem RVA=0x{:X} relay=0x{:X}",
            kSelectSlotItemCallRva,
            kSelectSlotItemRva,
            relayAddress);
        return true;
    }

    bool IEDRuntimeHook::IsInstalled() noexcept
    {
        return g_installed;
    }
}
