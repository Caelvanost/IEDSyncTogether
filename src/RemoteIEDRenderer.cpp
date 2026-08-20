#include "PCH.h"
#include "RemoteIEDRenderer.h"

#include "FormIdentity.h"

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
        constexpr std::uint32_t kRawPatchMaxAttempts = 45;
        constexpr std::uint32_t kRawPatchRequiredSuccesses = 3;

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
            bool parentAttachment{ false };
        };

        NodeSelection SlotNode(const CapturedIEDObject& object, std::size_t slot)
        {
            if (!object.anchorNode.empty() &&
                (object.anchorNode.starts_with("MOV ") || object.anchorNode.starts_with("CME "))) {
                // The captured OBJECT ... local transform is already expressed
                // in the evaluated MOV/CME parent frame. Parent attachment mode
                // lets us restore that raw local transform without IED
                // pre-multiplying a managed/reference-node transform again.
                return { object.anchorNode, "captured-anchor", true };
            }

            constexpr std::string_view kReferencePrefix = "OBJECT R ";
            constexpr std::string_view kParentPrefix = "OBJECT P ";

            if (object.attachmentNode.starts_with(kReferencePrefix)) {
                const auto suffix = std::string_view(object.attachmentNode).substr(kReferencePrefix.size());
                if (!suffix.empty()) {
                    return { std::string(suffix), "captured-attachment", false };
                }
            }
            if (object.attachmentNode.starts_with(kParentPrefix)) {
                const auto suffix = std::string_view(object.attachmentNode).substr(kParentPrefix.size());
                if (!suffix.empty()) {
                    return { std::string(suffix), "captured-attachment", true };
                }
            }
            return { std::string(kGearNodes[slot]), "slot-fallback", false };
        }

        std::optional<RE::FormID> ParseIEDObjectFormID(std::string_view name)
        {
            if (!name.starts_with("OBJECT ")) {
                return std::nullopt;
            }

            const auto open = name.find('[');
            if (open == std::string_view::npos || open + 9 > name.size()) {
                return std::nullopt;
            }

            const auto hex = name.substr(open + 1, 8);
            std::uint32_t value = 0;
            for (const char ch : hex) {
                value <<= 4;
                if (ch >= '0' && ch <= '9') {
                    value |= static_cast<std::uint32_t>(ch - '0');
                } else if (ch >= 'a' && ch <= 'f') {
                    value |= static_cast<std::uint32_t>(10 + ch - 'a');
                } else if (ch >= 'A' && ch <= 'F') {
                    value |= static_cast<std::uint32_t>(10 + ch - 'A');
                } else {
                    return std::nullopt;
                }
            }
            return value ? std::optional<RE::FormID>(value) : std::nullopt;
        }

        RE::NiAVObject* FindRemoteIEDObject(
            RE::NiAVObject* current,
            RE::FormID remoteFormID,
            std::string_view expectedAttachment)
        {
            if (!current) {
                return nullptr;
            }

            const auto* rawName = current->name.c_str();
            const std::string_view name = rawName ? std::string_view(rawName) : std::string_view{};
            if (auto formID = ParseIEDObjectFormID(name); formID && *formID == remoteFormID) {
                const auto* parentRaw = current->parent && current->parent->name.c_str() ?
                    current->parent->name.c_str() : nullptr;
                const std::string_view parentName = parentRaw ?
                    std::string_view(parentRaw) : std::string_view{};
                if (parentName == expectedAttachment) {
                    return current;
                }
            }

            if (auto* node = current->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    if (child) {
                        if (auto* found = FindRemoteIEDObject(
                                child.get(), remoteFormID, expectedAttachment)) {
                            return found;
                        }
                    }
                }
            }
            return nullptr;
        }

        float MaxMatrixDelta(
            const RE::NiMatrix3& matrix,
            const std::array<float, 9>& expected)
        {
            float delta = 0.0f;
            std::size_t index = 0;
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t col = 0; col < 3; ++col) {
                    delta = std::max(delta, std::abs(matrix.entry[row][col] - expected[index++]));
                }
            }
            return delta;
        }

        float MaxPositionDelta(
            const RE::NiPoint3& point,
            const std::array<float, 3>& expected)
        {
            return std::max({
                std::abs(point.x - expected[0]),
                std::abs(point.y - expected[1]),
                std::abs(point.z - expected[2])
            });
        }

        struct RawTransformPatchRequest
        {
            STRPM::ConnectionID connectionID{ 0 };
            STRPM::ProxyFormID proxyFormID{ STRPM::kInvalidProxyFormID };
            std::string displayName;
            std::size_t slot{ 0 };
            RE::FormID remoteFormID{ 0 };
            std::string expectedAttachment;
            CapturedIEDObject object;
            std::uint32_t attempts{ 0 };
            std::uint32_t successfulPatches{ 0 };
            bool firstSuccessLogged{ false };
        };

        void QueueRawTransformPatch(std::shared_ptr<RawTransformPatchRequest> request);

        void RunRawTransformPatch(const std::shared_ptr<RawTransformPatchRequest>& request)
        {
            if (!request) {
                return;
            }

            ++request->attempts;

            auto* form = RE::TESForm::LookupByID(request->proxyFormID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            auto* root = actor ? actor->Get3D1(false) : nullptr;
            auto* remoteObject = root ? FindRemoteIEDObject(
                root,
                request->remoteFormID,
                request->expectedAttachment) : nullptr;

            if (remoteObject) {
                const auto before = remoteObject->local;

                remoteObject->local.translate.x = request->object.position[0];
                remoteObject->local.translate.y = request->object.position[1];
                remoteObject->local.translate.z = request->object.position[2];
                remoteObject->local.scale = std::clamp(request->object.scale, 0.01f, 100.0f);

                std::size_t index = 0;
                for (std::size_t row = 0; row < 3; ++row) {
                    for (std::size_t col = 0; col < 3; ++col) {
                        remoteObject->local.rotate.entry[row][col] = request->object.rotationMatrix[index++];
                    }
                }

                // Recompute world-space data from the exact local transform we
                // just restored. Avoid NiAVObject::Update() here because that
                // also runs controllers and could alter the local transform.
                RE::NiUpdateData updateData{};
                updateData.time = 0.0f;
                remoteObject->UpdateWorldData(&updateData);
                remoteObject->UpdateWorldBound();

                const auto after = remoteObject->local;
                const auto beforeMatrixDelta = MaxMatrixDelta(before.rotate, request->object.rotationMatrix);
                const auto afterMatrixDelta = MaxMatrixDelta(after.rotate, request->object.rotationMatrix);
                const auto beforePositionDelta = MaxPositionDelta(before.translate, request->object.position);
                const auto afterPositionDelta = MaxPositionDelta(after.translate, request->object.position);
                const auto beforeScaleDelta = std::abs(before.scale - request->object.scale);
                const auto afterScaleDelta = std::abs(after.scale - request->object.scale);

                if (!request->firstSuccessLogged) {
                    request->firstSuccessLogged = true;
                    SKSE::log::info(
                        "REMOTE IED RAW XFORM applied: connection={} name=\"{}\" proxy={:08X} slot={} remoteForm={:08X} parent=\"{}\" attempt={} beforeDelta(rot={:.6f},pos={:.6f},scale={:.6f}) afterDelta(rot={:.6f},pos={:.6f},scale={:.6f}) expectedPos=({:.3f},{:.3f},{:.3f}) expectedScale={:.3f} expectedRot=[{:.3f},{:.3f},{:.3f};{:.3f},{:.3f},{:.3f};{:.3f},{:.3f},{:.3f}] beforeRot=[{:.3f},{:.3f},{:.3f};{:.3f},{:.3f},{:.3f};{:.3f},{:.3f},{:.3f}] afterRot=[{:.3f},{:.3f},{:.3f};{:.3f},{:.3f},{:.3f};{:.3f},{:.3f},{:.3f}]",
                        request->connectionID,
                        request->displayName,
                        request->proxyFormID,
                        request->slot,
                        request->remoteFormID,
                        request->expectedAttachment,
                        request->attempts,
                        beforeMatrixDelta,
                        beforePositionDelta,
                        beforeScaleDelta,
                        afterMatrixDelta,
                        afterPositionDelta,
                        afterScaleDelta,
                        request->object.position[0], request->object.position[1], request->object.position[2],
                        request->object.scale,
                        request->object.rotationMatrix[0], request->object.rotationMatrix[1], request->object.rotationMatrix[2],
                        request->object.rotationMatrix[3], request->object.rotationMatrix[4], request->object.rotationMatrix[5],
                        request->object.rotationMatrix[6], request->object.rotationMatrix[7], request->object.rotationMatrix[8],
                        before.rotate.entry[0][0], before.rotate.entry[0][1], before.rotate.entry[0][2],
                        before.rotate.entry[1][0], before.rotate.entry[1][1], before.rotate.entry[1][2],
                        before.rotate.entry[2][0], before.rotate.entry[2][1], before.rotate.entry[2][2],
                        after.rotate.entry[0][0], after.rotate.entry[0][1], after.rotate.entry[0][2],
                        after.rotate.entry[1][0], after.rotate.entry[1][1], after.rotate.entry[1][2],
                        after.rotate.entry[2][0], after.rotate.entry[2][1], after.rotate.entry[2][2]);
                }

                ++request->successfulPatches;
                if (request->successfulPatches >= kRawPatchRequiredSuccesses) {
                    SKSE::log::info(
                        "REMOTE IED RAW XFORM verified: connection={} name=\"{}\" proxy={:08X} slot={} remoteForm={:08X} attempts={} successfulPatches={} matrixDelta={:.6f} positionDelta={:.6f} scaleDelta={:.6f}",
                        request->connectionID,
                        request->displayName,
                        request->proxyFormID,
                        request->slot,
                        request->remoteFormID,
                        request->attempts,
                        request->successfulPatches,
                        afterMatrixDelta,
                        afterPositionDelta,
                        afterScaleDelta);
                    return;
                }
            }

            if (request->attempts < kRawPatchMaxAttempts) {
                QueueRawTransformPatch(request);
                return;
            }

            SKSE::log::warn(
                "REMOTE IED RAW XFORM failed: connection={} name=\"{}\" proxy={:08X} slot={} remoteForm={:08X} parent=\"{}\" attempts={} successfulPatches={}",
                request->connectionID,
                request->displayName,
                request->proxyFormID,
                request->slot,
                request->remoteFormID,
                request->expectedAttachment,
                request->attempts,
                request->successfulPatches);
        }

        void QueueRawTransformPatch(std::shared_ptr<RawTransformPatchRequest> request)
        {
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([request = std::move(request)]() {
                    RunRawTransformPatch(request);
                });
            }
        }

        std::size_t QueueRawPatches(
            STRPM::ConnectionID connectionID,
            STRPM::ProxyFormID proxyFormID,
            std::string_view displayName,
            const std::array<const CapturedIEDObject*, 19>& visibleSlots)
        {
            std::size_t queued = 0;
            for (std::size_t slot = 0; slot < visibleSlots.size(); ++slot) {
                const auto* object = visibleSlots[slot];
                if (!object) {
                    continue;
                }

                auto* remoteForm = ResolveFormIdentity(object->form);
                if (!remoteForm) {
                    continue;
                }

                const auto nodeSelection = SlotNode(*object, slot);
                const auto attachmentName = fmt::format(
                    "OBJECT {} {}",
                    nodeSelection.parentAttachment ? "P" : "R",
                    nodeSelection.node);

                auto request = std::make_shared<RawTransformPatchRequest>();
                request->connectionID = connectionID;
                request->proxyFormID = proxyFormID;
                request->displayName = std::string(displayName);
                request->slot = slot;
                request->remoteFormID = remoteForm->GetFormID();
                request->expectedAttachment = attachmentName;
                request->object = *object;
                QueueRawTransformPatch(std::move(request));
                ++queued;
            }
            return queued;
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

        const auto signature = EncodeLocalIEDState(state);
        if (!force) {
            const auto it = _appliedSignatures.find(proxyFormID);
            if (it != _appliedSignatures.end() && it->second == signature) {
                const auto rawQueued = QueueRawPatches(
                    connectionID,
                    proxyFormID,
                    displayName,
                    visibleSlots);
                SKSE::log::trace(
                    "REMOTE IED RAW refresh queued: connection={} name=\"{}\" proxy={:08X} slots={}",
                    connectionID,
                    displayName,
                    proxyFormID,
                    rawQueued);
                return true;
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
                if (nodeSelection.parentAttachment) {
                    dispatched &= DispatchNoResult(
                        "SetItemAttachmentModeActor",
                        static_cast<RE::Actor*>(actor),
                        std::string(kPluginKey),
                        itemName,
                        female,
                        1,
                        false);
                }

                dispatched &= DispatchNoResult(
                    "SetItemNodeActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    itemName,
                    female,
                    nodeSelection.node);

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
                "REMOTE IED SLOT queued: connection={} name=\"{}\" proxy={:08X} slot={} plugin=\"{}\" localForm={:X} remoteForm={:08X} node=\"{}\" nodeSource={} attachmentMode={} rawTransform=deferred expectedParent=\"OBJECT {} {}\" capturedAttachment=\"{}\" capturedAnchor=\"{}\"",
                connectionID,
                displayName,
                proxyFormID,
                slot,
                object->form.plugin,
                object->form.localFormID,
                remoteForm->GetFormID(),
                nodeSelection.node,
                nodeSelection.source,
                nodeSelection.parentAttachment ? "parent" : "reference",
                nodeSelection.parentAttachment ? "P" : "R",
                nodeSelection.node,
                object->attachmentNode,
                object->anchorNode);
        }

        dispatched &= DispatchNoResult(
            "Evaluate",
            static_cast<RE::Actor*>(actor));

        const auto rawQueued = QueueRawPatches(
            connectionID,
            proxyFormID,
            displayName,
            visibleSlots);

        if (dispatched) {
            _appliedSignatures[proxyFormID] = signature;
        }

        SKSE::log::info(
            "REMOTE IED RENDER queued: connection={} name=\"{}\" proxy={:08X} visibleSlots={} dispatchAccepted={} rawPatchesQueued={} customObjectsIgnored={}",
            connectionID,
            displayName,
            proxyFormID,
            rendered,
            dispatched ? 1 : 0,
            rawQueued,
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
