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
        constexpr std::uintptr_t kTrampolineSearchBias = 0x40000000ull;

        constexpr std::array<std::uint8_t, 5> kExpectedCall{
            0xE8, 0x55, 0x2B, 0x06, 0x00
        };

        constexpr std::array<std::uint8_t, 16> kExpectedSelectSlotItemPrologue{
            0x4C, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x30
        };

        // ProcessParams is an IED-private type. We deliberately do not declare
        // its layout. The source-verified structure is much larger than this
        // prefix and its CommonParams base stores the Actor* in the prefix. We
        // only compare machine words against already-known STR proxy addresses;
        // no candidate pointer is dereferenced.
        constexpr std::size_t kProcessParamsProbeBytes = 0xA0;
        constexpr std::size_t kMaxTrackedProxies = 16;

#pragma pack(push, 1)
        struct RelativeCall
        {
            std::uint8_t opcode{ 0xE8 };
            std::int32_t displacement{ 0 };
        };
        static_assert(sizeof(RelativeCall) == 0x5);
#pragma pack(pop)

        struct TrackedProxy
        {
            std::atomic<std::uintptr_t> actor{ 0 };
            std::atomic<RE::FormID> formID{ 0 };
            std::atomic_bool logged{ false };
        };

        struct RelayStorage
        {
            std::array<std::uint8_t, 128> code{};
        };

        SKSE::Trampoline g_iedTrampoline{ "IEDSyncTogether.IED" };
        std::array<TrackedProxy, kMaxTrackedProxies> g_trackedProxies{};
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

        bool ShouldSuppressStockSlots(const void* processParams) noexcept
        {
            if (!processParams) {
                return false;
            }

            const auto* bytes = static_cast<const std::byte*>(processParams);
            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <= kProcessParamsProbeBytes;
                 offset += sizeof(std::uintptr_t)) {
                std::uintptr_t candidate = 0;
                std::memcpy(&candidate, bytes + offset, sizeof(candidate));
                if (!candidate) {
                    continue;
                }

                for (auto& tracked : g_trackedProxies) {
                    const auto actor = tracked.actor.load(std::memory_order_acquire);
                    if (!actor || candidate != actor) {
                        continue;
                    }

                    if (!tracked.logged.exchange(true, std::memory_order_acq_rel)) {
                        SKSE::log::info(
                            "IED stock-slot suppression active: proxy={:08X} actor=0x{:X}",
                            tracked.formID.load(std::memory_order_relaxed),
                            actor);
                    }
                    return true;
                }
            }

            return false;
        }

        std::size_t BuildRelay(
            std::uint8_t* code,
            std::uintptr_t helper,
            std::uintptr_t stockTarget)
        {
            std::size_t offset = 0;

            auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
                for (const auto byte : bytes) {
                    code[offset++] = byte;
                }
            };
            auto emit64 = [&](std::uint64_t value) {
                std::memcpy(code + offset, &value, sizeof(value));
                offset += sizeof(value);
            };

            // Preserve the original SelectSlotItem argument registers and RAX.
            // Seven pushes move RSP from 8 mod 16 to 0 mod 16; the helper call
            // then gets the required 32-byte Windows x64 shadow space.
            emit({ 0x50 });
            emit({ 0x51 });
            emit({ 0x52 });
            emit({ 0x41, 0x50 });
            emit({ 0x41, 0x51 });
            emit({ 0x41, 0x52 });
            emit({ 0x41, 0x53 });
            emit({ 0x48, 0x83, 0xEC, 0x20 });

            emit({ 0x48, 0x89, 0xD1 }); // mov rcx, rdx (ProcessParams*)
            emit({ 0x48, 0xB8 });
            emit64(helper);
            emit({ 0xFF, 0xD0 });
            emit({ 0x48, 0x83, 0xC4, 0x20 });
            emit({ 0x84, 0xC0 });

            emit({ 0x75, 0x00 });
            const auto suppressJumpDisp = offset - 1;

            // Passthrough: restore everything and tail-jump to the exact stock
            // IED function. The RIP-indirect jump does not consume a register.
            emit({ 0x41, 0x5B });
            emit({ 0x41, 0x5A });
            emit({ 0x41, 0x59 });
            emit({ 0x41, 0x58 });
            emit({ 0x5A });
            emit({ 0x59 });
            emit({ 0x58 });
            emit({ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 });
            emit64(stockTarget);

            const auto suppressOffset = offset;
            const auto displacement = static_cast<std::ptrdiff_t>(suppressOffset) -
                                      static_cast<std::ptrdiff_t>(suppressJumpDisp + 1);
            if (displacement < (std::numeric_limits<std::int8_t>::min)() ||
                displacement > (std::numeric_limits<std::int8_t>::max)()) {
                return 0;
            }
            code[suppressJumpDisp] = static_cast<std::uint8_t>(
                static_cast<std::int8_t>(displacement));

            // Suppressed: restore the caller state and return nullptr. The
            // surrounding ProcessSlots removes its stock entry; ProcessCustom
            // is executed afterwards by IED and remains completely untouched.
            emit({ 0x41, 0x5B });
            emit({ 0x41, 0x5A });
            emit({ 0x41, 0x59 });
            emit({ 0x41, 0x58 });
            emit({ 0x5A });
            emit({ 0x59 });
            emit({ 0x58 });
            emit({ 0x31, 0xC0 });
            emit({ 0xC3 });

            return offset;
        }
    }

    bool IEDRuntimeHook::Install()
    {
        if (g_installed) {
            return true;
        }

        const auto module = GetModuleHandleW(L"ImmersiveEquipmentDisplays.dll");
        if (!module) {
            SKSE::log::critical("IED stock-slot shim: ImmersiveEquipmentDisplays.dll is not loaded");
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto callSite = base + kSelectSlotItemCallRva;
        const auto selectSlotItem = base + kSelectSlotItemRva;

        if (!MatchBytes(callSite, kExpectedCall)) {
            SKSE::log::critical(
                "IED stock-slot shim: unsupported IED build (SelectSlotItem call signature mismatch at RVA 0x{:X})",
                kSelectSlotItemCallRva);
            return false;
        }

        if (!MatchBytes(selectSlotItem, kExpectedSelectSlotItemPrologue)) {
            SKSE::log::critical(
                "IED stock-slot shim: unsupported IED build (SelectSlotItem signature mismatch at RVA 0x{:X})",
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
                "IED stock-slot shim: call target mismatch (expected RVA 0x{:X})",
                kSelectSlotItemRva);
            return false;
        }

        const auto searchCenter = callSite + kTrampolineSearchBias;
        g_iedTrampoline.create(
            256,
            reinterpret_cast<void*>(searchCenter));

        auto* storage = g_iedTrampoline.allocate<RelayStorage>();
        const auto relaySize = BuildRelay(
            storage->code.data(),
            reinterpret_cast<std::uintptr_t>(&ShouldSuppressStockSlots),
            selectSlotItem);
        if (relaySize == 0 || relaySize > storage->code.size()) {
            SKSE::log::critical("IED stock-slot shim: failed to build relay");
            return false;
        }

        std::int32_t relayDisplacement = 0;
        const auto relayAddress = reinterpret_cast<std::uintptr_t>(storage->code.data());
        if (!GetRelative32(callSite + sizeof(RelativeCall), relayAddress, relayDisplacement)) {
            SKSE::log::critical(
                "IED stock-slot shim: allocated relay is outside rel32 range (call=0x{:X}, relay=0x{:X})",
                callSite,
                relayAddress);
            return false;
        }

        RelativeCall patchedCall{};
        patchedCall.displacement = relayDisplacement;
        REL::safe_write(callSite, &patchedCall, sizeof(patchedCall));
        g_installed = true;

        SKSE::log::info(
            "IED 1.7.4 proxy stock-slot shim installed: call RVA=0x{:X} relay=0x{:X} stock SelectSlotItem RVA=0x{:X}; official DLL unchanged on disk",
            kSelectSlotItemCallRva,
            relayAddress,
            kSelectSlotItemRva);
        return true;
    }

    bool IEDRuntimeHook::IsInstalled() noexcept
    {
        return g_installed;
    }

    void IEDRuntimeHook::TrackRemoteProxy(
        RE::FormID formID,
        RE::Actor* actor,
        bool tracked) noexcept
    {
        const auto address = reinterpret_cast<std::uintptr_t>(actor);

        if (!tracked) {
            for (auto& entry : g_trackedProxies) {
                const auto storedFormID = entry.formID.load(std::memory_order_acquire);
                const auto storedActor = entry.actor.load(std::memory_order_acquire);
                if ((formID != 0 && storedFormID == formID) ||
                    (address != 0 && storedActor == address)) {
                    entry.actor.store(0, std::memory_order_release);
                    entry.formID.store(0, std::memory_order_release);
                    entry.logged.store(false, std::memory_order_release);
                }
            }
            return;
        }

        if (!actor || formID == 0) {
            return;
        }

        for (auto& entry : g_trackedProxies) {
            if (entry.formID.load(std::memory_order_acquire) == formID) {
                entry.logged.store(false, std::memory_order_release);
                entry.actor.store(address, std::memory_order_release);
                return;
            }
        }

        for (auto& entry : g_trackedProxies) {
            std::uintptr_t expected = 0;
            if (entry.actor.compare_exchange_strong(
                    expected,
                    address,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                entry.formID.store(formID, std::memory_order_release);
                entry.logged.store(false, std::memory_order_release);
                SKSE::log::debug(
                    "IED stock-slot suppression armed: proxy={:08X} actor=0x{:X}",
                    formID,
                    address);
                return;
            }
        }

        SKSE::log::error(
            "IED stock-slot suppression table full; proxy {:08X} will keep stock IED slots",
            formID);
    }

    void IEDRuntimeHook::ClearTrackedProxies() noexcept
    {
        for (auto& entry : g_trackedProxies) {
            entry.actor.store(0, std::memory_order_release);
            entry.formID.store(0, std::memory_order_release);
            entry.logged.store(false, std::memory_order_release);
        }
    }
}
