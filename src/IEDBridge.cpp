#include "PCH.h"
#include "IEDBridge.h"

#include "IEDSyncTogether/Interface.h"
#include "SyncService.h"

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

        constexpr std::array<std::string_view, 19> kSlotNodes{
            "WeaponSword",
            "WeaponSwordLeft",
            "WeaponAxe",
            "WeaponAxeLeft",
            "WeaponBack",
            "WeaponBackLeft",
            "WeaponBackAxeMace",
            "WeaponBackAxeMaceLeft",
            "WeaponDagger",
            "WeaponDaggerLeft",
            "WeaponMace",
            "WeaponMaceLeft",
            "WeaponStaff",
            "WeaponStaffLeft",
            "WeaponBow",
            "WeaponCrossBow",
            "Shield",
            "Shield",
            "Quiver"
        };

        constexpr std::array<bool, 19> kLeftWeaponSlots{
            false, true,
            false, true,
            false, true,
            false, true,
            false, true,
            false, true,
            false, true,
            false, false,
            false, false, false
        };

        using RemoteFormState = std::array<RE::FormID, 19>;

        std::atomic_bool g_firstCaptureStarted{ false };
        std::mutex g_remoteMutex;
        std::unordered_set<RE::FormID> g_trackedRemoteProxies;
        std::unordered_map<RE::FormID, RemoteFormState> g_appliedRemoteStates;

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

        template <class... Args>
        bool DispatchNoResult(std::string_view functionName, Args&&... args)
        {
            auto* vm = GetVM();
            if (!vm) {
                return false;
            }

            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback;
            return vm->DispatchStaticCall(
                RE::BSFixedString(kPapyrusClass.data()),
                RE::BSFixedString(functionName.data()),
                RE::MakeFunctionArguments(std::forward<Args>(args)...),
                callback);
        }

        void ClearRemoteCustomItems(RE::Actor* actor)
        {
            if (!actor) {
                return;
            }

            DispatchNoResult(
                "RemoveActorBlock",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey));
            DispatchNoResult(
                "DeleteAllActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey));
            DispatchNoResult(
                "Evaluate",
                static_cast<RE::Actor*>(actor));
        }

        bool ApplyRemoteCustomItems(
            RE::Actor* actor,
            const RemoteFormState& forms)
        {
            if (!actor || !GetVM()) {
                return false;
            }

            bool dispatched = true;
            std::size_t visible = 0;

            dispatched &= DispatchNoResult(
                "RemoveActorBlock",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey));
            dispatched &= DispatchNoResult(
                "DeleteAllActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey));

            for (std::size_t slot = 0; slot < forms.size(); ++slot) {
                const auto formID = forms[slot];
                if (formID == 0) {
                    continue;
                }

                auto* form = RE::TESForm::LookupByID(formID);
                if (!form) {
                    SKSE::log::warn(
                        "IED custom render skipped unresolved form: proxy={:08X} slot={} form={:08X}",
                        actor->GetFormID(),
                        slot,
                        formID);
                    continue;
                }

                ++visible;
                const auto name = fmt::format("remote-slot-{:02}", slot);
                const auto node = std::string(kSlotNodes[slot]);

                dispatched &= DispatchNoResult(
                    "CreateItemActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    name,
                    false,
                    form,
                    false,
                    node);

                dispatched &= DispatchNoResult(
                    "SetItemFormActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    name,
                    true,
                    form);

                // CreateItemActor initializes the requested sex variant only.
                // Mirror the managed node explicitly as well as the form so a
                // female STR proxy is rendered on the same IED equipment node.
                dispatched &= DispatchNoResult(
                    "SetItemNodeActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    name,
                    false,
                    node);
                dispatched &= DispatchNoResult(
                    "SetItemNodeActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    name,
                    true,
                    node);

                if (kLeftWeaponSlots[slot]) {
                    dispatched &= DispatchNoResult(
                        "SetItemLeftWeaponActor",
                        static_cast<RE::Actor*>(actor),
                        std::string(kPluginKey),
                        name,
                        false,
                        true);
                    dispatched &= DispatchNoResult(
                        "SetItemLeftWeaponActor",
                        static_cast<RE::Actor*>(actor),
                        std::string(kPluginKey),
                        name,
                        true,
                        true);
                }

                dispatched &= DispatchNoResult(
                    "SetItemEnabledActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    name,
                    false,
                    true);
                dispatched &= DispatchNoResult(
                    "SetItemEnabledActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    name,
                    true,
                    true);
            }

            dispatched &= DispatchNoResult(
                "Evaluate",
                static_cast<RE::Actor*>(actor));

            SKSE::log::info(
                "IED Custom Item render queued: proxy={:08X} slots={} dispatchAccepted={}",
                actor->GetFormID(),
                visible,
                dispatched ? 1 : 0);
            return dispatched;
        }

        void RefreshTrackedRemoteProxies()
        {
            std::vector<RE::FormID> proxies;
            {
                std::scoped_lock lock(g_remoteMutex);
                proxies.assign(g_trackedRemoteProxies.begin(), g_trackedRemoteProxies.end());
            }

            for (const auto proxyFormID : proxies) {
                auto* form = RE::TESForm::LookupByID(proxyFormID);
                auto* actor = form ? form->As<RE::Actor>() : nullptr;
                if (!actor) {
                    std::scoped_lock lock(g_remoteMutex);
                    g_trackedRemoteProxies.erase(proxyFormID);
                    g_appliedRemoteStates.erase(proxyFormID);
                    continue;
                }

                RemoteFormState desired{};
                bool matched = false;
                for (std::size_t slot = 0; slot < desired.size(); ++slot) {
                    RE::FormID remoteFormID = 0;
                    const auto result = SyncService::GetSingleton().QueryRemoteSlot(
                        proxyFormID,
                        static_cast<std::uint32_t>(slot),
                        remoteFormID);

                    if (result == static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kForm)) {
                        desired[slot] = remoteFormID;
                        matched = true;
                    } else if (result == static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kEmpty)) {
                        matched = true;
                    }
                }

                if (!matched) {
                    continue;
                }

                bool changed = true;
                {
                    std::scoped_lock lock(g_remoteMutex);
                    const auto iterator = g_appliedRemoteStates.find(proxyFormID);
                    changed = iterator == g_appliedRemoteStates.end() || iterator->second != desired;
                }
                if (!changed) {
                    continue;
                }

                if (ApplyRemoteCustomItems(actor, desired)) {
                    std::scoped_lock lock(g_remoteMutex);
                    g_appliedRemoteStates[proxyFormID] = desired;
                }
            }
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

    bool IEDBridge::CapturePlayerSlots(CaptureCallback callback)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* vm = GetVM();
        if (!player || !vm || !callback || !IsInstalled()) {
            return false;
        }

        RefreshTrackedRemoteProxies();

        const bool diagnostic = !g_firstCaptureStarted.exchange(true);
        if (diagnostic) {
            SKSE::log::info(
                "First IED capture: official Papyrus API active; starting sequential 19-slot capture player={:08X}",
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
        if (!actor || !GetVM() || !IsInstalled()) {
            return false;
        }

        const auto formID = actor->GetFormID();
        if (blocked) {
            ClearRemoteCustomItems(actor);
            {
                std::scoped_lock lock(g_remoteMutex);
                g_trackedRemoteProxies.insert(formID);
                g_appliedRemoteStates.erase(formID);
            }
            RefreshTrackedRemoteProxies();
            return true;
        }

        {
            std::scoped_lock lock(g_remoteMutex);
            g_trackedRemoteProxies.erase(formID);
            g_appliedRemoteStates.erase(formID);
        }
        ClearRemoteCustomItems(actor);
        return true;
    }

    void IEDBridge::ResetRemoteRendering() const
    {
        std::vector<RE::FormID> proxies;
        {
            std::scoped_lock lock(g_remoteMutex);
            proxies.assign(g_trackedRemoteProxies.begin(), g_trackedRemoteProxies.end());
            g_trackedRemoteProxies.clear();
            g_appliedRemoteStates.clear();
        }

        for (const auto formID : proxies) {
            if (auto* form = RE::TESForm::LookupByID(formID)) {
                if (auto* actor = form->As<RE::Actor>()) {
                    ClearRemoteCustomItems(actor);
                }
            }
        }

        if (!proxies.empty()) {
            SKSE::log::info("IED Custom Item renderer reset: cleared {} tracked proxy/proxies", proxies.size());
        }
    }
}
