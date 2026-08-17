#include "PCH.h"
#include "IEDRuntimeHook.h"

#include <Windows.h>

#include <cstring>

namespace IEDSyncTogether
{
    namespace
    {
        // Source/call-stack aligned with official IED 1.7.4 (ied-dev commit
        // 3f014c3e8574ef0e88b2ec0b7cdf58b86c9737b0). Unlike v0.2.2, this
        // hook never changes SelectSlotItem's return value. It skips the
        // complete void ProcessSlots call for a tracked STR proxy, matching
        // IED's own disableNPCSlots control flow while leaving ProcessCustom
        // to execute immediately afterwards.
        constexpr std::uintptr_t kProcessSlotsCallRva = 0xDF58C;
        constexpr std::uintptr_t kProcessSlotsReturnRva = 0xDF591;
        constexpr std::uintptr_t kObservedProcessSlotsBodyRva = 0xE5C70;

        // Known official 1.7.4 signatures retained only as a build fingerprint.
        // These locations are never patched by v0.2.4.
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

        struct TrackedProxy
        {
            std::atomic<std::uint32_t> nativeHandle{ 0 };
            std::atomic<RE::FormID> formID{ 0 };
        };

        using ProcessSlotsFn = void (*)(void*, void*) noexcept;

        SKSE::Trampoline g_iedTrampoline{ "IEDSyncTogether.IED.ProcessSlots" };
        std::array<TrackedProxy, kMaxTrackedProxies> g_trackedProxies{};
        ProcessSlotsFn g_originalProcessSlots{ nullptr };
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

        bool ShouldSuppressProcessSlots(const void* processParams) noexcept
        {
            if (!processParams) {
                return false;
            }

            // IED 1.7.4 ProcessParams derives from ProcessParamsData, whose
            // first member is const Game::ObjectRefHandle handle. Skyrim's
            // reference handle is a 32-bit native value. Reading only this
            // source-verified prefix avoids interpreting any other IED-private
            // layout.
            std::uint32_t nativeHandle = 0;
            std::memcpy(
                std::addressof(nativeHandle),
                processParams,
                sizeof(nativeHandle));
            if (!nativeHandle) {
                return false;
            }

            for (const auto& entry : g_trackedProxies) {
                if (entry.nativeHandle.load(std::memory_order_acquire) == nativeHandle) {
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

        // CommonLib's 5-byte call trampoline allocates an in-range relay and
        // preserves the original call target. We deliberately patch only this
        // void ProcessSlots invocation; SelectSlotItem remains untouched.
        g_originalProcessSlots =
            reinterpret_cast<ProcessSlotsFn>(originalTarget);
        g_iedTrampoline.create(64, reinterpret_cast<void*>(callSite));
        g_iedTrampoline.write_call<5>(callSite, &ProcessSlotsHook);

        g_installed = true;
        SKSE::log::info(
            "IED 1.7.4 proxy ProcessSlots suppression installed: call RVA=0x{:X} target RVA=0x{:X}; SelectSlotItem untouched; official DLL unchanged on disk",
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

        std::uint32_t nativeHandle = 0;
        if (actor) {
            RE::ObjectRefHandle handle(actor);
            nativeHandle = handle.native_handle();
        }

        if (!tracked) {
            for (auto& entry : g_trackedProxies) {
                const auto storedFormID = entry.formID.load(std::memory_order_acquire);
                const auto storedHandle = entry.nativeHandle.load(std::memory_order_acquire);
                if ((formID != 0 && storedFormID == formID) ||
                    (nativeHandle != 0 && storedHandle == nativeHandle)) {
                    entry.nativeHandle.store(0, std::memory_order_release);
                    entry.formID.store(0, std::memory_order_release);
                }
            }
            return;
        }

        if (!actor || formID == 0 || nativeHandle == 0) {
            SKSE::log::warn(
                "IED ProcessSlots suppression could not track proxy {:08X}: no live native reference handle",
                formID);
            return;
        }

        for (auto& entry : g_trackedProxies) {
            if (entry.formID.load(std::memory_order_acquire) == formID ||
                entry.nativeHandle.load(std::memory_order_acquire) == nativeHandle) {
                entry.formID.store(formID, std::memory_order_release);
                entry.nativeHandle.store(nativeHandle, std::memory_order_release);
                return;
            }
        }

        for (auto& entry : g_trackedProxies) {
            std::uint32_t expected = 0;
            if (entry.nativeHandle.compare_exchange_strong(
                    expected,
                    nativeHandle,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                entry.formID.store(formID, std::memory_order_release);
                SKSE::log::info(
                    "IED ProcessSlots suppression armed: proxy={:08X} handle={:08X}",
                    formID,
                    nativeHandle);
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
            entry.nativeHandle.store(0, std::memory_order_release);
            entry.formID.store(0, std::memory_order_release);
        }
    }
}
