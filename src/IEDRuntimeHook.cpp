#include "PCH.h"
#include "IEDRuntimeHook.h"

#include <Windows.h>

#include <cstddef>
#include <cstring>
#include <limits>

namespace IEDSyncTogether
{
    namespace
    {
        // Source/call-stack aligned with official IED 1.7.4 (ied-dev commit
        // 3f014c3e8574ef0e88b2ec0b7cdf58b86c9737b0). The hook never changes
        // SelectSlotItem's return value. It skips the complete void ProcessSlots
        // call only after the current ProcessParams actor has been identified as
        // an exact tracked STR proxy.
        constexpr std::uintptr_t kProcessSlotsCallRva = 0xDF58C;
        constexpr std::uintptr_t kProcessSlotsReturnRva = 0xDF591;
        constexpr std::uintptr_t kObservedProcessSlotsBodyRva = 0xE5C70;

        // Known official 1.7.4 signatures retained only as a build fingerprint.
        // These locations are never patched by v0.2.5.
        constexpr std::uintptr_t kFingerprintCallRva = 0xE3806;
        constexpr std::uintptr_t kFingerprintFunctionRva = 0x146360;
        constexpr std::array<std::uint8_t, 5> kExpectedFingerprintCall{
            0xE8, 0x55, 0x2B, 0x06, 0x00
        };
        constexpr std::array<std::uint8_t, 16> kExpectedFingerprintFunction{
            0x4C, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x56, 0x57,
            0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x30
        };

        constexpr std::size_t kMaxTrackedProxies = 16;
        constexpr std::size_t kActorCalibrationScanBytes = 0x200;
        constexpr std::size_t kUnknownActorOffset =
            (std::numeric_limits<std::size_t>::max)();
        constexpr std::uint32_t kRequiredCalibrationHits = 2;

        struct TrackedProxy
        {
            std::atomic<std::uintptr_t> actor{ 0 };
            std::atomic<RE::FormID> formID{ 0 };
        };

        using ProcessSlotsFn = void (*)(void*, void*) noexcept;

        SKSE::Trampoline g_iedTrampoline{ "IEDSyncTogether.IED.ProcessSlots" };
        std::array<TrackedProxy, kMaxTrackedProxies> g_trackedProxies{};
        ProcessSlotsFn g_originalProcessSlots{ nullptr };
        std::atomic<std::uintptr_t> g_playerActor{ 0 };
        std::atomic<std::size_t> g_actorOffset{ kUnknownActorOffset };
        std::atomic<std::size_t> g_candidateActorOffset{ kUnknownActorOffset };
        std::atomic<std::uint32_t> g_candidateActorOffsetHits{ 0 };
        bool g_installed{ false };

        bool IsRangeInsideImage(
            std::uintptr_t rva,
            std::size_t length,
            std::uint32_t imageSize) noexcept
        {
            return rva <= imageSize &&
                   length <= imageSize - static_cast<std::size_t>(rva);
        }

        const IMAGE_NT_HEADERS64* GetNtHeaders(std::uintptr_t base) noexcept
        {
            if (!base) {
                return nullptr;
            }

            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
                return nullptr;
            }

            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                base + static_cast<std::uintptr_t>(dos->e_lfanew));
            if (nt->Signature != IMAGE_NT_SIGNATURE ||
                nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
                return nullptr;
            }

            return nt;
        }

        template <class T>
        bool MatchBytes(
            std::uintptr_t base,
            std::uintptr_t rva,
            const T& expected,
            std::uint32_t imageSize) noexcept
        {
            if (!IsRangeInsideImage(rva, expected.size(), imageSize)) {
                return false;
            }

            return std::memcmp(
                reinterpret_cast<const void*>(base + rva),
                expected.data(),
                expected.size()) == 0;
        }

        const RUNTIME_FUNCTION* FindRuntimeFunction(
            std::uintptr_t base,
            const IMAGE_NT_HEADERS64* nt,
            std::uint32_t rva) noexcept
        {
            if (!base || !nt) {
                return nullptr;
            }

            const auto imageSize = nt->OptionalHeader.SizeOfImage;
            const auto& directory =
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            if (!directory.VirtualAddress ||
                directory.Size < sizeof(RUNTIME_FUNCTION) ||
                !IsRangeInsideImage(directory.VirtualAddress, directory.Size, imageSize)) {
                return nullptr;
            }

            const auto* table = reinterpret_cast<const RUNTIME_FUNCTION*>(
                base + directory.VirtualAddress);
            const auto count = directory.Size / sizeof(RUNTIME_FUNCTION);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& entry = table[i];
                if (rva >= entry.BeginAddress && rva < entry.EndAddress) {
                    return std::addressof(entry);
                }
            }

            return nullptr;
        }

        bool SameRuntimeFunction(
            const RUNTIME_FUNCTION* lhs,
            const RUNTIME_FUNCTION* rhs) noexcept
        {
            return lhs && rhs &&
                   lhs->BeginAddress == rhs->BeginAddress &&
                   lhs->EndAddress == rhs->EndAddress;
        }

        bool DecodeRelativeCall(
            std::uintptr_t callSite,
            std::uintptr_t base,
            std::uint32_t imageSize,
            std::uintptr_t& outTarget,
            std::uint32_t& outTargetRva) noexcept
        {
            if (!IsRangeInsideImage(callSite - base, 5, imageSize) ||
                *reinterpret_cast<const std::uint8_t*>(callSite) != 0xE8) {
                return false;
            }

            std::int32_t displacement = 0;
            std::memcpy(
                std::addressof(displacement),
                reinterpret_cast<const void*>(callSite + 1),
                sizeof(displacement));

            const auto targetSigned =
                static_cast<std::int64_t>(callSite + 5) + displacement;
            if (targetSigned < static_cast<std::int64_t>(base) ||
                targetSigned >= static_cast<std::int64_t>(base + imageSize)) {
                return false;
            }

            outTarget = static_cast<std::uintptr_t>(targetSigned);
            outTargetRva = static_cast<std::uint32_t>(outTarget - base);
            return true;
        }

        std::size_t GetReadableScanBytes(const void* address) noexcept
        {
            if (!address) {
                return 0;
            }

            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(address, std::addressof(mbi), sizeof(mbi)) != sizeof(mbi) ||
                mbi.State != MEM_COMMIT ||
                (mbi.Protect & PAGE_GUARD) != 0 ||
                (mbi.Protect & 0xFFu) == PAGE_NOACCESS) {
                return 0;
            }

            const auto start = reinterpret_cast<std::uintptr_t>(address);
            const auto regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto regionEnd = regionStart + mbi.RegionSize;
            if (start < regionStart || start >= regionEnd) {
                return 0;
            }

            return (std::min)(
                kActorCalibrationScanBytes,
                static_cast<std::size_t>(regionEnd - start));
        }

        bool FindUniquePointerOffset(
            const void* processParams,
            std::uintptr_t target,
            std::size_t& outOffset) noexcept
        {
            if (!processParams || !target) {
                return false;
            }

            const auto readable = GetReadableScanBytes(processParams);
            if (readable < sizeof(std::uintptr_t)) {
                return false;
            }

            const auto* bytes = static_cast<const std::byte*>(processParams);
            std::size_t found = kUnknownActorOffset;
            std::uint32_t matches = 0;

            for (std::size_t offset = 0;
                 offset + sizeof(std::uintptr_t) <= readable;
                 offset += alignof(std::uintptr_t)) {
                std::uintptr_t candidate = 0;
                std::memcpy(
                    std::addressof(candidate),
                    bytes + offset,
                    sizeof(candidate));

                if (candidate != target) {
                    continue;
                }

                found = offset;
                ++matches;
                if (matches > 1) {
                    return false;
                }
            }

            if (matches != 1 || found == kUnknownActorOffset) {
                return false;
            }

            outOffset = found;
            return true;
        }

        void TryCalibrateActorOffset(const void* processParams) noexcept
        {
            if (!processParams ||
                g_actorOffset.load(std::memory_order_acquire) != kUnknownActorOffset) {
                return;
            }

            auto playerAddress = g_playerActor.load(std::memory_order_acquire);
            if (!playerAddress) {
                if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    playerAddress = reinterpret_cast<std::uintptr_t>(player);
                    g_playerActor.store(playerAddress, std::memory_order_release);
                }
            }
            if (!playerAddress) {
                return;
            }

            std::size_t foundOffset = kUnknownActorOffset;
            if (!FindUniquePointerOffset(processParams, playerAddress, foundOffset)) {
                return;
            }

            const auto candidate =
                g_candidateActorOffset.load(std::memory_order_acquire);
            if (candidate != foundOffset) {
                g_candidateActorOffset.store(foundOffset, std::memory_order_release);
                g_candidateActorOffsetHits.store(1, std::memory_order_release);
                return;
            }

            const auto hits =
                g_candidateActorOffsetHits.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (hits < kRequiredCalibrationHits) {
                return;
            }

            std::size_t expected = kUnknownActorOffset;
            g_actorOffset.compare_exchange_strong(
                expected,
                foundOffset,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
        }

        bool ReadActorAtCalibratedOffset(
            const void* processParams,
            std::uintptr_t& outActor) noexcept
        {
            const auto offset = g_actorOffset.load(std::memory_order_acquire);
            if (offset == kUnknownActorOffset ||
                offset + sizeof(std::uintptr_t) > GetReadableScanBytes(processParams)) {
                return false;
            }

            std::memcpy(
                std::addressof(outActor),
                static_cast<const std::byte*>(processParams) + offset,
                sizeof(outActor));
            return outActor != 0;
        }

        bool ShouldSuppressProcessSlots(const void* processParams) noexcept
        {
            if (!processParams) {
                return false;
            }

            // Fail open until we have observed the exact PlayerCharacter pointer
            // at one unique, repeatable offset in IED's ProcessParams. This makes
            // the local player the runtime calibration source instead of assuming
            // that IED's Game::ObjectRefHandle matches CommonLibSSE's handle ABI.
            TryCalibrateActorOffset(processParams);

            std::uintptr_t evaluatedActor = 0;
            if (!ReadActorAtCalibratedOffset(processParams, evaluatedActor)) {
                return false;
            }

            const auto playerAddress = g_playerActor.load(std::memory_order_acquire);
            if (playerAddress && evaluatedActor == playerAddress) {
                return false;
            }

            for (const auto& entry : g_trackedProxies) {
                const auto trackedActor = entry.actor.load(std::memory_order_acquire);
                if (trackedActor && evaluatedActor == trackedActor) {
                    return true;
                }
            }

            return false;
        }

        void ProcessSlotsHook(void* controller, void* processParams) noexcept
        {
            if (ShouldSuppressProcessSlots(processParams)) {
                return;
            }

            if (const auto original = g_originalProcessSlots) {
                original(controller, processParams);
            }
        }
    }

    bool IEDRuntimeHook::Install()
    {
        if (g_installed) {
            return true;
        }

        const auto module = GetModuleHandleW(L"ImmersiveEquipmentDisplays.dll");
        if (!module) {
            SKSE::log::warn(
                "IED ProcessSlots suppression unavailable: ImmersiveEquipmentDisplays.dll is not loaded");
            return false;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto* nt = GetNtHeaders(base);
        if (!nt) {
            SKSE::log::warn(
                "IED ProcessSlots suppression unavailable: invalid IED PE headers");
            return false;
        }

        const auto imageSize = nt->OptionalHeader.SizeOfImage;
        if (!MatchBytes(
                base,
                kFingerprintCallRva,
                kExpectedFingerprintCall,
                imageSize) ||
            !MatchBytes(
                base,
                kFingerprintFunctionRva,
                kExpectedFingerprintFunction,
                imageSize)) {
            SKSE::log::warn(
                "IED ProcessSlots suppression unavailable: official IED 1.7.4 fingerprint mismatch; no memory was modified");
            return false;
        }

        if (!IsRangeInsideImage(kProcessSlotsCallRva, 5, imageSize) ||
            kProcessSlotsReturnRva != kProcessSlotsCallRva + 5) {
            SKSE::log::warn(
                "IED ProcessSlots suppression unavailable: invalid validated call-site range");
            return false;
        }

        const auto callSite = base + kProcessSlotsCallRva;
        std::uintptr_t originalTarget = 0;
        std::uint32_t originalTargetRva = 0;
        if (!DecodeRelativeCall(
                callSite,
                base,
                imageSize,
                originalTarget,
                originalTargetRva)) {
            SKSE::log::warn(
                "IED ProcessSlots suppression unavailable: expected CALL rel32 missing at RVA 0x{:X}; no memory was modified",
                kProcessSlotsCallRva);
            return false;
        }

        const auto* callerFunction =
            FindRuntimeFunction(base, nt, static_cast<std::uint32_t>(kProcessSlotsCallRva));
        const auto* returnFunction =
            FindRuntimeFunction(base, nt, static_cast<std::uint32_t>(kProcessSlotsReturnRva));
        const auto* targetFunction =
            FindRuntimeFunction(base, nt, originalTargetRva);
        const auto* observedBodyFunction =
            FindRuntimeFunction(base, nt, static_cast<std::uint32_t>(kObservedProcessSlotsBodyRva));

        if (!SameRuntimeFunction(callerFunction, returnFunction) ||
            !SameRuntimeFunction(targetFunction, observedBodyFunction) ||
            !targetFunction ||
            originalTargetRva != targetFunction->BeginAddress) {
            SKSE::log::warn(
                "IED ProcessSlots suppression unavailable: PE unwind validation failed at call RVA 0x{:X} target RVA 0x{:X}; no memory was modified",
                kProcessSlotsCallRva,
                originalTargetRva);
            return false;
        }

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            g_playerActor.store(
                reinterpret_cast<std::uintptr_t>(player),
                std::memory_order_release);
        }

        g_originalProcessSlots = reinterpret_cast<ProcessSlotsFn>(originalTarget);
        g_iedTrampoline.create(64, reinterpret_cast<void*>(callSite));
        g_iedTrampoline.write_call<5>(callSite, &ProcessSlotsHook);

        g_installed = true;
        SKSE::log::info(
            "IED 1.7.4 proxy ProcessSlots suppression installed: call RVA=0x{:X} target RVA=0x{:X}; actor identity will be runtime-calibrated from PlayerCharacter; SelectSlotItem untouched; official DLL unchanged on disk",
            kProcessSlotsCallRva,
            originalTargetRva);
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
        if (!g_installed) {
            return;
        }

        const auto address = reinterpret_cast<std::uintptr_t>(actor);
        const auto playerAddress = g_playerActor.load(std::memory_order_acquire);

        if (!tracked) {
            for (auto& entry : g_trackedProxies) {
                const auto storedFormID = entry.formID.load(std::memory_order_acquire);
                const auto storedActor = entry.actor.load(std::memory_order_acquire);
                if ((formID != 0 && storedFormID == formID) ||
                    (address != 0 && storedActor == address)) {
                    entry.actor.store(0, std::memory_order_release);
                    entry.formID.store(0, std::memory_order_release);
                }
            }
            return;
        }

        if (!actor || formID == 0 || address == 0) {
            SKSE::log::warn(
                "IED ProcessSlots suppression could not track proxy {:08X}: no live Actor*",
                formID);
            return;
        }

        if (playerAddress && address == playerAddress) {
            SKSE::log::critical(
                "IED ProcessSlots suppression refused to track local PlayerCharacter {:08X}",
                formID);
            return;
        }

        for (auto& entry : g_trackedProxies) {
            if (entry.formID.load(std::memory_order_acquire) == formID ||
                entry.actor.load(std::memory_order_acquire) == address) {
                entry.formID.store(formID, std::memory_order_release);
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

                const auto actorOffset = g_actorOffset.load(std::memory_order_acquire);
                if (actorOffset == kUnknownActorOffset) {
                    SKSE::log::info(
                        "IED ProcessSlots suppression armed: proxy={:08X} actor=0x{:X}; actor-field calibration pending, suppression remains fail-open until calibrated",
                        formID,
                        address);
                } else {
                    SKSE::log::info(
                        "IED ProcessSlots suppression armed: proxy={:08X} actor=0x{:X} actorOffset=0x{:X}",
                        formID,
                        address,
                        actorOffset);
                }
                return;
            }
        }

        SKSE::log::error(
            "IED ProcessSlots suppression table full; proxy {:08X} will keep normal IED NPC slots",
            formID);
    }

    void IEDRuntimeHook::ClearTrackedProxies() noexcept
    {
        for (auto& entry : g_trackedProxies) {
            entry.actor.store(0, std::memory_order_release);
            entry.formID.store(0, std::memory_order_release);
        }
    }
}
