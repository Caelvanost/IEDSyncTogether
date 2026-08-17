#include "PCH.h"
#include "IEDBridge.h"

#include "IEDSyncTogether/Interface.h"
#include "SyncService.h"

#include <RE/F/FunctionArguments.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/P/PackUnpack.h>
#include <RE/S/SkyrimVM.h>

#include <cmath>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr std::string_view kPapyrusClass = "IED";
        constexpr std::string_view kPluginKey = "IEDSyncTogether.esp";
        constexpr float kRadiansToDegrees = 57.29577951308232f;

        // Capture anchors corresponding to IED Data::ObjectSlot 0..18.
        // These are only entry points into the owner's live scene graph. v0.4.0
        // walks through MOV/CME helper nodes and serializes the resulting
        // transform relative to the first stable skeleton node.
        constexpr std::array<std::string_view, 19> kGearNodes{
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
            "WeaponCrossbow",
            "ShieldBack",
            "WeaponTorch",
            "QUIVER"
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

        struct RemoteRenderSlot
        {
            RE::FormID formID{ 0 };
            std::optional<PlacementTransform> placement;
            auto operator<=>(const RemoteRenderSlot&) const = default;
        };
        using RemoteRenderState = std::array<RemoteRenderSlot, 19>;

        std::atomic_bool g_firstCaptureStarted{ false };
        std::mutex g_remoteMutex;
        std::unordered_set<RE::FormID> g_trackedRemoteProxies;
        std::unordered_map<RE::FormID, RemoteRenderState> g_appliedRemoteStates;

        float Quantize(float value)
        {
            return std::round(value * 10000.0f) / 10000.0f;
        }

        bool IsIEDPlacementHelper(const RE::NiAVObject* node)
        {
            if (!node) {
                return false;
            }
            const auto* value = node->name.c_str();
            if (!value || !*value) {
                return false;
            }
            const std::string_view name(value);
            return name.starts_with("MOV ") || name.starts_with("CME ");
        }

        std::optional<PlacementTransform> CapturePlacement(
            RE::PlayerCharacter* player,
            std::size_t slot)
        {
            if (!player || slot >= kGearNodes.size()) {
                return std::nullopt;
            }

            auto* root = player->Get3D1(false);
            if (!root) {
                return std::nullopt;
            }

            auto* gearNode = root->GetObjectByName(RE::BSFixedString(kGearNodes[slot].data()));
            if (!gearNode || !gearNode->parent) {
                return std::nullopt;
            }

            // Skyrim computes world = parent.world * local. Reproduce that same
            // multiplication while walking IED/XPMSSE MOV/CME helper nodes so
            // offsets stored on the helpers are part of the transmitted state.
            RE::NiTransform relative = gearNode->local;
            RE::NiAVObject* anchor = gearNode->parent;
            std::size_t helperDepth = 0;
            while (anchor && IsIEDPlacementHelper(anchor)) {
                relative = anchor->local * relative;
                anchor = anchor->parent;
                ++helperDepth;
            }

            if (!anchor) {
                return std::nullopt;
            }
            const auto* anchorName = anchor->name.c_str();
            if (!anchorName || !*anchorName) {
                return std::nullopt;
            }

            RE::NiPoint3 euler{};
            relative.rotate.ToEulerAnglesXYZ(euler);

            PlacementTransform result;
            result.node = anchorName;
            result.position = {
                Quantize(relative.translate.x),
                Quantize(relative.translate.y),
                Quantize(relative.translate.z)
            };
            result.rotation = {
                Quantize(euler.x * kRadiansToDegrees),
                Quantize(euler.y * kRadiansToDegrees),
                Quantize(euler.z * kRadiansToDegrees)
            };
            result.scale = Quantize(std::clamp(relative.scale, 0.01f, 100.0f));

            SKSE::log::trace(
                "IED placement chain: slot={} gearNode=\"{}\" helperDepth={} anchor=\"{}\"",
                slot,
                kGearNodes[slot],
                helperDepth,
                result.node);
            return result;
        }

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
                RE::MakeFunctionArguments(
                    std::decay_t<Args>(std::forward<Args>(args))...),
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

        bool ApplyTransform(
            RE::Actor* actor,
            const std::string& name,
            bool female,
            const PlacementTransform& placement)
        {
            std::vector<float> position{
                placement.position[0],
                placement.position[1],
                placement.position[2]
            };
            std::vector<float> rotation{
                placement.rotation[0],
                placement.rotation[1],
                placement.rotation[2]
            };

            bool dispatched = true;
            dispatched &= DispatchNoResult(
                "SetItemPositionActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                name,
                female,
                position);
            dispatched &= DispatchNoResult(
                "SetItemRotationActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                name,
                female,
                rotation);
            dispatched &= DispatchNoResult(
                "SetItemScaleActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                name,
                female,
                placement.scale);
            return dispatched;
        }

        bool ApplyRemoteCustomItems(
            RE::Actor* actor,
            const RemoteRenderState& state)
        {
            if (!actor || !GetVM()) {
                return false;
            }

            bool dispatched = true;
            std::size_t visible = 0;
            std::size_t placed = 0;

            dispatched &= DispatchNoResult(
                "RemoveActorBlock",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey));
            dispatched &= DispatchNoResult(
                "DeleteAllActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey));

            for (std::size_t slot = 0; slot < state.size(); ++slot) {
                const auto& remote = state[slot];
                if (remote.formID == 0) {
                    continue;
                }

                auto* form = RE::TESForm::LookupByID(remote.formID);
                if (!form) {
                    SKSE::log::warn(
                        "IED custom render skipped unresolved form: proxy={:08X} slot={} form={:08X}",
                        actor->GetFormID(),
                        slot,
                        remote.formID);
                    continue;
                }

                ++visible;
                const auto name = fmt::format("remote-slot-{:02}", slot);
                const auto node = remote.placement && !remote.placement->node.empty() ?
                    remote.placement->node :
                    std::string(kGearNodes[slot]);

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

                for (const bool female : { false, true }) {
                    dispatched &= DispatchNoResult(
                        "SetItemNodeActor",
                        static_cast<RE::Actor*>(actor),
                        std::string(kPluginKey),
                        name,
                        female,
                        node);

                    if (remote.placement) {
                        dispatched &= ApplyTransform(actor, name, female, *remote.placement);
                    }

                    if (kLeftWeaponSlots[slot]) {
                        dispatched &= DispatchNoResult(
                            "SetItemLeftWeaponActor",
                            static_cast<RE::Actor*>(actor),
                            std::string(kPluginKey),
                            name,
                            female,
                            true);
                    }

                    dispatched &= DispatchNoResult(
                        "SetItemEnabledActor",
                        static_cast<RE::Actor*>(actor),
                        std::string(kPluginKey),
                        name,
                        female,
                        true);
                }

                if (remote.placement) {
                    ++placed;
                    SKSE::log::debug(
                        "IED placement applied: proxy={:08X} slot={} anchor=\"{}\" pos=({:.2f},{:.2f},{:.2f}) rot=({:.2f},{:.2f},{:.2f}) scale={:.3f}",
                        actor->GetFormID(),
                        slot,
                        remote.placement->node,
                        remote.placement->position[0],
                        remote.placement->position[1],
                        remote.placement->position[2],
                        remote.placement->rotation[0],
                        remote.placement->rotation[1],
                        remote.placement->rotation[2],
                        remote.placement->scale);
                }
            }

            dispatched &= DispatchNoResult(
                "Evaluate",
                static_cast<RE::Actor*>(actor));

            SKSE::log::info(
                "IED Custom Item render queued: proxy={:08X} slots={} placements={} dispatchAccepted={}",
                actor->GetFormID(),
                visible,
                placed,
                dispatched ? 1 : 0);
            return dispatched;
        }

        void RefreshTrackedRemoteProxies()
        {
            std::vector<RE::FormID> proxies;
            {
                std::scoped_lock lock(g_remoteMutex);
                proxies.assign(
                    g_trackedRemoteProxies.begin(),
                    g_trackedRemoteProxies.end());
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

                RemoteRenderState desired{};
                bool matched = false;
                for (std::size_t slot = 0; slot < desired.size(); ++slot) {
                    RE::FormID remoteFormID = 0;
                    const auto result = SyncService::GetSingleton().QueryRemoteSlot(
                        proxyFormID,
                        static_cast<std::uint32_t>(slot),
                        remoteFormID);

                    if (result == static_cast<std::uint32_t>(IEDST::SlotOverrideResult::kForm)) {
                        desired[slot].formID = remoteFormID;
                        FormIdentity identity;
                        if (SyncService::GetSingleton().QueryRemoteSlotIdentity(
                                proxyFormID,
                                static_cast<std::uint32_t>(slot),
                                identity)) {
                            desired[slot].placement = identity.placement;
                        }
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
                    changed = iterator == g_appliedRemoteStates.end() ||
                        iterator->second != desired;
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
                request->slots[slot] = MakeFormIdentity(
                    RE::TESForm::LookupByID(formID));

                if (request->slots[slot]) {
                    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                        request->slots[slot]->placement = CapturePlacement(player, slot);

                        if (request->slots[slot]->placement) {
                            const auto& p = *request->slots[slot]->placement;
                            SKSE::log::debug(
                                "IED placement captured: slot={} gearNode=\"{}\" anchor=\"{}\" pos=({:.2f},{:.2f},{:.2f}) rot=({:.2f},{:.2f},{:.2f}) scale={:.3f}",
                                slot,
                                kGearNodes[slot],
                                p.node,
                                p.position[0],
                                p.position[1],
                                p.position[2],
                                p.rotation[0],
                                p.rotation[1],
                                p.rotation[2],
                                p.scale);
                        } else {
                            SKSE::log::warn(
                                "IED placement capture unavailable: slot={} gearNode=\"{}\"; remote renderer will use fallback node",
                                slot,
                                kGearNodes[slot]);
                        }
                    }
                }
            }

            const auto nextSlot = slot + 1;
            if (nextSlot < request->slots.size()) {
                DispatchCaptureSlot(request, nextSlot);
                return;
            }

            if (request->diagnostic) {
                SKSE::log::info(
                    "First IED capture: all 19 slots completed sequentially with scene-graph placement capture");
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
                SKSE::log::debug(
                    "First IED capture: sequential dispatch begin slot={}",
                    slot);
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
                "First IED capture: Papyrus slot API + composed scene-graph Node Override capture active; player={:08X}",
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
            proxies.assign(
                g_trackedRemoteProxies.begin(),
                g_trackedRemoteProxies.end());
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
            SKSE::log::info(
                "IED Custom Item renderer reset: cleared {} tracked proxy/proxies",
                proxies.size());
        }
    }
}
