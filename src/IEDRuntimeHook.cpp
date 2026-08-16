#include "PCH.h"
#include "IEDRuntimeHook.h"

#include <cstddef>
#include <cstring>
#include <limits>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr std::uintptr_t kSelectSlotItemCallRva = 0xE3806;
        constexpr std::uintptr_t kSelectSlotItemRva = 0x146360;

        // CommonLib searches a +/-2 GiB window around the address passed to
        // Trampoline::create(). Bias the search 1 GiB above the IED call-site,
        // so its low-to-high allocator starts comfortably inside rel32 range.
        constexpr std::uintptr_t kTrampolineSearchBias = 0x40000000ull;

        constexpr std::array<std::uint8_t, 5> kExpectedCall{
            0xE8, 0x55, 0x2B, 0x06, 0x00
        };

        constexpr std::array<std::uint8_t, 16> kExpectedSelectSlotItemPrologue{
            0x4C, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x30
        };

#pragma pack(push, 1)
        // CALL site -> this relay -> JMP stock IED SelectSlotItem.
        //
        // This deliberately does not enter a C++ hook. The original CALL has
        // already pushed ProcessSlots' return address; the relay preserves all
        // argument registers and stack contents and tail-jumps to the official
        // IED function. SelectSlotItem's RET therefore returns directly to the
        // instruction after the original CALL, exactly as it did before patching.
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
    }

    bool IEDRuntimeHook::Install()
    {
        if (g_installed) {
            return true;
        }

        const auto module = GetModuleHandleW(L"ImmersiveEquipmentDisplays.dll");
        if (!module) {
            SKSE::log::critical("IED transparent shim: ImmersiveEquipmentDisplays.dll is not loaded");
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto callSite = base + kSelectSlotItemCallRva;
        const auto selectSlotItem = base + kSelectSlotItemRva;

        if (!MatchBytes(callSite, kExpectedCall)) {
            SKSE::log::critical(
                "IED transparent shim: unsupported IED build (SelectSlotItem call signature mismatch at RVA 0x{:X})",
                kSelectSlotItemCallRva);
            return false;
        }

        if (!MatchBytes(selectSlotItem, kExpectedSelectSlotItemPrologue)) {
            SKSE::log::critical(
                "IED transparent shim: unsupported IED build (SelectSlotItem signature mismatch at RVA 0x{:X})",
                kSelectSlotItemRva);
            return false;
        }

        std::int32_t originalDisplacement = 0;
        std::memcpy(
            &originalDisplacement,
            reinterpret_cast<const void*>(callSite + 1),
            sizeof(originalDisplacement));
        const auto originalTarget = callSite + sizeof(RelativeCall) + originalDisplacement;
        if (originalTarget != selectSlotItem) {
            SKSE::log::critical(
                "IED transparent shim: call target mismatch (expected RVA 0x{:X})",
                kSelectSlotItemRva);
            return false;
        }

        // A 5-byte x64 CALL can only reach +/-2 GiB. Allocate a tiny relay near
        // IED, then make that relay tail-jump straight to the official function.
        // No private IED ABI is re-declared or interpreted in this version.
        const auto searchCenter = callSite + kTrampolineSearchBias;
        g_iedTrampoline.create(
            64,
            reinterpret_cast<void*>(searchCenter));

        auto* relay = g_iedTrampoline.allocate<AbsoluteJumpRelay>();
        *relay = AbsoluteJumpRelay{};
        relay->target = static_cast<std::uint64_t>(selectSlotItem);

        std::int32_t relayDisplacement = 0;
        const auto relayAddress = reinterpret_cast<std::uintptr_t>(relay);
        if (!GetRelative32(callSite + sizeof(RelativeCall), relayAddress, relayDisplacement)) {
            SKSE::log::critical(
                "IED transparent shim: allocated relay is outside rel32 range (call=0x{:X}, relay=0x{:X})",
                callSite,
                relayAddress);
            return false;
        }

        RelativeCall patchedCall{};
        patchedCall.displacement = relayDisplacement;
        REL::safe_write(callSite, &patchedCall, sizeof(patchedCall));
        g_installed = true;

        SKSE::log::info(
            "IED 1.7.4 transparent shim installed: call RVA=0x{:X} -> relay=0x{:X} -> stock SelectSlotItem RVA=0x{:X}; no C++ ABI hook active",
            kSelectSlotItemCallRva,
            relayAddress,
            kSelectSlotItemRva);
        return true;
    }

    bool IEDRuntimeHook::IsInstalled() noexcept
    {
        return g_installed;
    }
}
