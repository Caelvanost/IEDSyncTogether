#include "PCH.h"
#include "RemoteIEDRenderer.h"

#include "FormIdentity.h"

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
        constexpr float kRadiansToDegrees = 57.29577951308232f;

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

        struct NodeSelection
        {
            std::string node;
            std::string_view source;
        };

        NodeSelection SlotNode(const CapturedIEDObject& object, std::size_t slot)
        {
            if (!object.anchorNode.empty() &&
                (object.anchorNode.starts_with("MOV ") || object.anchorNode.starts_with("CME "))) {
                return { object.anchorNode, "captured-anchor" };
            }

            constexpr std::string_view kReferencePrefix = "OBJECT R ";
            constexpr std::string_view kParentPrefix = "OBJECT P ";

            if (object.attachmentNode.starts_with(kReferencePrefix)) {
                const auto suffix = std::string_view(object.attachmentNode).substr(kReferencePrefix.size());
                if (!suffix.empty()) {
                    return { std::string(suffix), "captured-attachment" };
                }
            }
            if (object.attachmentNode.starts_with(kParentPrefix)) {
                const auto suffix = std::string_view(object.attachmentNode).substr(kParentPrefix.size());
                if (!suffix.empty()) {
                    return { std::string(suffix), "captured-attachment" };
                }
            }
            return { std::string(kGearNodes[slot]), "slot-fallback" };
        }

        std::array<float, 3> RotationDegrees(const CapturedIEDObject& object)
        {
            RE::NiMatrix3 matrix{};
            std::size_t index = 0;
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t col = 0; col < 3; ++col) {
                    matrix.entry[row][col] = object.rotationMatrix[index++];
                }
            }

            RE::NiPoint3 euler{};
            matrix.ToEulerAnglesXYZ(euler);
            return {
                euler.x * kRadiansToDegrees,
                euler.y * kRadiansToDegrees,
                euler.z * kRadiansToDegrees
            };
        }

        bool ApplyCapturedTransform(
            RE::Actor* actor,
            const std::string& itemName,
            bool female,
            const CapturedIEDObject& object,
            const std::array<float, 3>& rotationDegrees)
        {
            std::vector<float> position{
                object.position[0],
                object.position[1],
                object.position[2]
            };
            std::vector<float> rotation{
                rotationDegrees[0],
                rotationDegrees[1],
                rotationDegrees[2]
            };

            bool dispatched = true;
            dispatched &= DispatchNoResult(
                "SetItemPositionActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                itemName,
                female,
                position);
            dispatched &= DispatchNoResult(
                "SetItemRotationActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                itemName,
                female,
                rotation);
            dispatched &= DispatchNoResult(
                "SetItemScaleActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                itemName,
                female,
                std::clamp(object.scale, 0.01f, 100.0f));
            return dispatched;
        }

        void ClearOwnedItems(RE::Actor* actor)
        {
            if (!actor || !GetVM()) {
                return;
            }

            (void)DispatchNoResult(
                "RemoveActorBlock",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey));
            (void)DispatchNoResult(
                "DeleteAllActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey));
            (void)DispatchNoResult(
                "Evaluate",
                static_cast<RE::Actor*>(actor));
        }
    }

    RemoteIEDRenderer& RemoteIEDRenderer::GetSingleton()
    {
        static RemoteIEDRenderer instance;
        return instance;
    }

    bool RemoteIEDRenderer::Apply(
        STRPM::ConnectionID connectionID,
        STRPM::ProxyFormID proxyFormID,
        std::string_view displayName,
        const LocalIEDState& state,
        bool force)
    {
        auto* form = RE::TESForm::LookupByID(proxyFormID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (!actor || actor == RE::PlayerCharacter::GetSingleton() || !GetVM()) {
            SKSE::log::warn(
                "REMOTE IED RENDER deferred: connection={} name=\"{}\" proxy={:08X} actorReady=0",
                connectionID,
                displayName,
                proxyFormID);
            return false;
        }

        const auto signature = EncodeLocalIEDState(state);
        if (!force) {
            const auto it = _appliedSignatures.find(proxyFormID);
            if (it != _appliedSignatures.end() && it->second == signature) {
                return true;
            }
        }

        std::array<const CapturedIEDObject*, 19> visibleSlots{};
        for (const auto& object : state.objects) {
            if (object.kind != IEDObjectKind::kSlot || !object.slot || !object.visible) {
                continue;
            }
            const auto slot = static_cast<std::size_t>(*object.slot);
            if (slot < visibleSlots.size() && !visibleSlots[slot]) {
                visibleSlots[slot] = &object;
            }
        }

        bool dispatched = true;
        dispatched &= DispatchNoResult(
            "RemoveActorBlock",
            static_cast<RE::Actor*>(actor),
            std::string(kPluginKey));
        dispatched &= DispatchNoResult(
            "DeleteAllActor",
            static_cast<RE::Actor*>(actor),
            std::string(kPluginKey));

        std::size_t rendered = 0;
        for (std::size_t slot = 0; slot < visibleSlots.size(); ++slot) {
            const auto* object = visibleSlots[slot];
            if (!object) {
                continue;
            }

            auto* remoteForm = ResolveFormIdentity(object->form);
            if (!remoteForm) {
                SKSE::log::warn(
                    "REMOTE IED SLOT skipped unresolved form: connection={} proxy={:08X} slot={} plugin=\"{}\" localForm={:X}",
                    connectionID,
                    proxyFormID,
                    slot,
                    object->form.plugin,
                    object->form.localFormID);
                continue;
            }

            const auto itemName = fmt::format("remote-slot-{:02}", slot);
            const auto nodeSelection = SlotNode(*object, slot);
            const auto rotationDegrees = RotationDegrees(*object);

            dispatched &= DispatchNoResult(
                "CreateItemActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                itemName,
                false,
                remoteForm,
                false,
                nodeSelection.node);

            dispatched &= DispatchNoResult(
                "SetItemFormActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                itemName,
                true,
                remoteForm);

            for (const bool female : { false, true }) {
                dispatched &= DispatchNoResult(
                    "SetItemNodeActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    itemName,
                    female,
                    nodeSelection.node);

                dispatched &= ApplyCapturedTransform(
                    actor,
                    itemName,
                    female,
                    *object,
                    rotationDegrees);

                if (kLeftWeaponSlots[slot]) {
                    dispatched &= DispatchNoResult(
                        "SetItemLeftWeaponActor",
                        static_cast<RE::Actor*>(actor),
                        std::string(kPluginKey),
                        itemName,
                        female,
                        true);
                }

                dispatched &= DispatchNoResult(
                    "SetItemEnabledActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    itemName,
                    female,
                    true);
            }

            ++rendered;
            SKSE::log::info(
                "REMOTE IED SLOT queued: connection={} name=\"{}\" proxy={:08X} slot={} plugin=\"{}\" localForm={:X} node=\"{}\" nodeSource={} pos=({:.3f},{:.3f},{:.3f}) rotDeg=({:.3f},{:.3f},{:.3f}) scale={:.3f} capturedAttachment=\"{}\" capturedAnchor=\"{}\"",
                connectionID,
                displayName,
                proxyFormID,
                slot,
                object->form.plugin,
                object->form.localFormID,
                nodeSelection.node,
                nodeSelection.source,
                object->position[0], object->position[1], object->position[2],
                rotationDegrees[0], rotationDegrees[1], rotationDegrees[2],
                object->scale,
                object->attachmentNode,
                object->anchorNode);
        }

        dispatched &= DispatchNoResult(
            "Evaluate",
            static_cast<RE::Actor*>(actor));

        if (dispatched) {
            _appliedSignatures[proxyFormID] = signature;
        }

        SKSE::log::info(
            "REMOTE IED RENDER queued: connection={} name=\"{}\" proxy={:08X} visibleSlots={} dispatchAccepted={} customObjectsIgnored={}",
            connectionID,
            displayName,
            proxyFormID,
            rendered,
            dispatched ? 1 : 0,
            std::ranges::count_if(
                state.objects,
                [](const CapturedIEDObject& object) { return object.kind == IEDObjectKind::kCustom; }));
        return dispatched;
    }

    void RemoteIEDRenderer::ClearProxy(STRPM::ProxyFormID proxyFormID)
    {
        if (proxyFormID == STRPM::kInvalidProxyFormID) {
            return;
        }

        auto* form = RE::TESForm::LookupByID(proxyFormID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (actor && actor != RE::PlayerCharacter::GetSingleton()) {
            ClearOwnedItems(actor);
            SKSE::log::info("REMOTE IED RENDER cleared: proxy={:08X}", proxyFormID);
        }
        _appliedSignatures.erase(proxyFormID);
    }

    void RemoteIEDRenderer::Reset()
    {
        std::vector<STRPM::ProxyFormID> proxies;
        proxies.reserve(_appliedSignatures.size());
        for (const auto& [proxyFormID, _] : _appliedSignatures) {
            proxies.push_back(proxyFormID);
        }

        for (const auto proxyFormID : proxies) {
            ClearProxy(proxyFormID);
        }
        _appliedSignatures.clear();
    }
}
