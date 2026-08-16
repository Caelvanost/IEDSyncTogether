#include "PCH.h"
#include "IEDBridge.h"

#include <RE/F/FunctionArguments.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/P/PackUnpack.h>
#include <RE/S/SkyrimVM.h>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr std::string_view kPapyrusClass = "IED";
        constexpr std::string_view kPluginKey = "IEDSyncTogether.esp";
        std::atomic_bool g_firstCaptureStarted{ false };

        using GetSourceBridgeVersionFn = std::uint32_t(__cdecl*)() noexcept;

        struct CaptureRequest
        {
            CaptureRequest(
                IEDBridge::CaptureCallback value,
                bool diagnosticValue) :
                callback(std::move(value)),
                diagnostic(diagnosticValue)
            {}

            SlotState slots{};
            IEDBridge::CaptureCallback callback;
            bool diagnostic{ false };
        };

        RE::BSScript::IVirtualMachine* GetVM()
        {
            auto* skyrimVM = RE::SkyrimVM::GetSingleton();
            return skyrimVM && skyrimVM->impl ? skyrimVM->impl.get() : nullptr;
        }

        void DispatchCaptureSlot(
            const std::shared_ptr<CaptureRequest>& request,
            std::size_t slot);

        void CompleteCaptureSlot(
            const std::shared_ptr<CaptureRequest>& request,
            std::size_t slot,
            RE::FormID formID)
        {
            if (request->diagnostic) {
                SKSE::log::debug(
                    "First IED capture: completed slot={} form={:08X}",
                    slot,
                    formID);
            }

            if (formID != 0) {
                request->slots[slot] = MakeFormIdentity(RE::TESForm::LookupByID(formID));
            }

            const auto nextSlot = slot + 1;
            if (nextSlot < request->slots.size()) {
                DispatchCaptureSlot(request, nextSlot);
                return;
            }

            if (request->diagnostic) {
                SKSE::log::info("First IED capture: all 19 slots completed sequentially");
            }

            auto callback = std::move(request->callback);
            if (callback) {
                callback(std::move(request->slots));
            }
        }

        class SlotResultCallback final :
            public RE::BSScript::IStackCallbackFunctor
        {
        public:
            SlotResultCallback(
                std::shared_ptr<CaptureRequest> request,
                std::size_t slot) :
                _request(std::move(request)),
                _slot(slot)
            {}

            void operator()(RE::BSScript::Variable result) override
            {
                RE::FormID formID = 0;
                if (result.IsObject() && !result.IsNoneObject()) {
                    if (auto* form = result.Unpack<RE::TESForm*>()) {
                        formID = form->GetFormID();
                    }
                }

                auto request = _request;
                const auto slot = _slot;
                auto complete = [request = std::move(request), slot, formID]() {
                    CompleteCaptureSlot(request, slot, formID);
                };

                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask(std::move(complete));
                } else {
                    complete();
                }
            }

            void SetObject(
                const RE::BSTSmartPointer<RE::BSScript::Object>& object) override
            {
                _object = object;
            }

        private:
            std::shared_ptr<CaptureRequest> _request;
            std::size_t _slot;
            RE::BSTSmartPointer<RE::BSScript::Object> _object;
        };

        void DispatchCaptureSlot(
            const std::shared_ptr<CaptureRequest>& request,
            std::size_t slot)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* vm = GetVM();
            if (!player || !vm || slot >= request->slots.size()) {
                CompleteCaptureSlot(request, slot, 0);
                return;
            }

            if (request->diagnostic) {
                SKSE::log::debug("First IED capture: sequential dispatch begin slot={}", slot);
            }

            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result(
                new SlotResultCallback(request, slot));

            const bool dispatched = vm->DispatchStaticCall(
                RE::BSFixedString(kPapyrusClass.data()),
                RE::BSFixedString("GetSlottedForm"),
                RE::MakeFunctionArguments(
                    static_cast<RE::Actor*>(player),
                    static_cast<std::int32_t>(slot)),
                result);

            if (request->diagnostic) {
                SKSE::log::debug(
                    "First IED capture: sequential dispatch end slot={} dispatched={}",
                    slot,
                    dispatched ? 1 : 0);
            }

            if (!dispatched) {
                auto complete = [request, slot]() {
                    CompleteCaptureSlot(request, slot, 0);
                };
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask(std::move(complete));
                } else {
                    complete();
                }
            }
        }
    }

    IEDBridge& IEDBridge::GetSingleton()
    {
        static IEDBridge instance;
        return instance;
    }

    bool IEDBridge::IsInstalled() const
    {
        return GetModuleHandleW(L"ImmersiveEquipmentDisplays.dll") != nullptr;
    }

    std::uint32_t IEDBridge::GetSourceBridgeVersion() const
    {
        const auto module = GetModuleHandleW(L"ImmersiveEquipmentDisplays.dll");
        if (!module) {
            return 0;
        }

        const auto function = reinterpret_cast<GetSourceBridgeVersionFn>(
            GetProcAddress(module, "IEDST_GetSourceBridgeVersion"));
        return function ? function() : 0;
    }

    bool IEDBridge::HasSourceBridge() const
    {
        return GetSourceBridgeVersion() != 0;
    }

    bool IEDBridge::CapturePlayerSlots(CaptureCallback callback)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* vm = GetVM();
        if (!player || !vm || !callback || !IsInstalled()) {
            return false;
        }

        const bool diagnostic = !g_firstCaptureStarted.exchange(true);
        if (diagnostic) {
            SKSE::log::info(
                "First IED capture: source bridge version={} ; starting sequential 19-slot capture player={:08X}",
                GetSourceBridgeVersion(),
                player->GetFormID());
        }

        auto request = std::make_shared<CaptureRequest>(
            std::move(callback),
            diagnostic);
        DispatchCaptureSlot(request, 0);
        return true;
    }

    bool IEDBridge::SetActorBlocked(RE::Actor* actor, bool blocked) const
    {
        auto* vm = GetVM();
        if (!actor || !vm || !IsInstalled()) {
            return false;
        }

        RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
        return vm->DispatchStaticCall(
            RE::BSFixedString(kPapyrusClass.data()),
            RE::BSFixedString(blocked ? "AddActorBlock" : "RemoveActorBlock"),
            RE::MakeFunctionArguments(
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey)),
            callback);
    }
}
