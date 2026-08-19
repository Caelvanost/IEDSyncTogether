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

        std::string SlotNode(const CapturedIEDObject& object, std::size_t slot)
        {
            constexpr std::string_view kReferencePrefix = "OBJECT R ";
            constexpr std::string_view kParentPrefix = "OBJECT P ";

            if (object.attachmentNode.starts_with(kReferencePrefix)) {
                const auto suffix = std::string_view(object.attachmentNode).substr(kReferencePrefix.size());
                if (!suffix.empty()) {
                    return std::string(suffix);
                }
            }
            if (object.attachmentNode.starts_with(kParentPrefix)) {
                const auto suffix = std::string_view(object.attachmentNode).substr(kParentPrefix.size());
                if (!suffix.empty()) {
                    return std::string(suffix);
                }
            }
            return std::string(kGearNodes[slot]);
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
            const auto node = SlotNode(*object, slot);

            dispatched &= DispatchNoResult(
                "CreateItemActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                itemName,
                false,
                remoteForm,
                false,
                node);

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
                    node);

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
                "REMOTE IED SLOT queued: connection={} name=\"{}\" proxy={:08X} slot={} plugin=\"{}\" localForm={:X} node=\"{}\"",
                connectionID,
                displayName,
                proxyFormID,
                slot,
                object->form.plugin,
                object->form.localFormID,
                node);
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
