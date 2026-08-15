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

        struct CaptureRequest
        {
            CaptureRequest(
                IEDBridge::CaptureCallback value,
                bool diagnosticValue) :
                callback(std::move(value)),
                diagnostic(diagnosticValue)
            {}

            void Complete(std::size_t slot, RE::FormID formID)
            {
                IEDBridge::CaptureCallback finalCallback;
                SlotState finalSlots{};

                {
                    std::scoped_lock lock(mutex);
                    if (completed[slot]) {
                        return;
                    }
                    completed[slot] = true;

                    if (diagnostic) {
                        SKSE::log::debug(
                            "First IED capture: completing slot={} form={:08X} remainingBefore={}",
                            slot,
                            formID,
                            remaining);
                    }

                    if (formID != 0) {
                        slots[slot] = MakeFormIdentity(RE::TESForm::LookupByID(formID));
                    }

                    if (--remaining == 0) {
                        finalSlots = slots;
                        finalCallback = std::move(callback);
                    }
                }

                if (finalCallback) {
                    if (diagnostic) {
                        SKSE::log::info("First IED capture: all 19 slot callbacks completed");
                    }
                    finalCallback(std::move(finalSlots));
                }
            }

            std::mutex mutex;
            SlotState slots{};
            std::array<bool, 19> completed{};
            std::size_t remaining{ 19 };
            IEDBridge::CaptureCallback callback;
            bool diagnostic{ false };
        };

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
                if (_request->diagnostic) {
                    SKSE::log::debug("First IED capture: callback entered slot={}", _slot);
                }

                RE::FormID formID = 0;
                if (result.IsObject() && !result.IsNoneObject()) {
                    if (auto* form = result.Unpack<RE::TESForm*>()) {
                        formID = form->GetFormID();
                    }
                }

                if (_request->diagnostic) {
                    SKSE::log::debug(
                        "First IED capture: callback decoded slot={} form={:08X}",
                        _slot,
                        formID);
                }

                auto request = _request;
                const auto slot = _slot;
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([request = std::move(request), slot, formID]() {
                        request->Complete(slot, formID);
                    });
                } else {
                    request->Complete(slot, 0);
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

        RE::BSScript::IVirtualMachine* GetVM()
        {
            auto* skyrimVM = RE::SkyrimVM::GetSingleton();
            return skyrimVM && skyrimVM->impl ? skyrimVM->impl.get() : nullptr;
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
                "First IED capture: dispatching 19 GetSlottedForm calls player={:08X}",
                player->GetFormID());
        }

        auto request = std::make_shared<CaptureRequest>(
            std::move(callback),
            diagnostic);

        for (std::size_t slot = 0; slot < request->slots.size(); ++slot) {
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result(
                new SlotResultCallback(request, slot));

            if (diagnostic) {
                SKSE::log::debug("First IED capture: dispatch begin slot={}", slot);
            }

            const bool dispatched = vm->DispatchStaticCall(
                RE::BSFixedString(kPapyrusClass.data()),
                RE::BSFixedString("GetSlottedForm"),
                RE::MakeFunctionArguments(
                    static_cast<RE::Actor*>(player),
                    static_cast<std::int32_t>(slot)),
                result);

            if (diagnostic) {
                SKSE::log::debug(
                    "First IED capture: dispatch end slot={} dispatched={}",
                    slot,
                    dispatched ? 1 : 0);
            }

            if (!dispatched) {
                request->Complete(slot, 0);
            }
        }

        if (diagnostic) {
            SKSE::log::info("First IED capture: all 19 calls dispatched");
        }
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
