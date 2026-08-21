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
        constexpr auto kWatchdogInterval = std::chrono::milliseconds(100);
        constexpr float kRotationEpsilon = 1.0e-4f;
        constexpr float kPositionEpsilon = 1.0e-3f;
        constexpr float kScaleEpsilon = 1.0e-4f;

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

        std::optional<NodeSelection> CustomNode(const CapturedIEDObject& object)
        {
            constexpr std::string_view kReferencePrefix = "OBJECT R ";
            constexpr std::string_view kParentPrefix = "OBJECT P ";

            if (object.attachmentNode.starts_with(kReferencePrefix)) {
                const auto suffix = std::string_view(object.attachmentNode).substr(kReferencePrefix.size());
                if (!suffix.empty()) {
                    return NodeSelection{ std::string(suffix), "captured-custom-attachment", false };
                }
            }
            if (object.attachmentNode.starts_with(kParentPrefix)) {
                const auto suffix = std::string_view(object.attachmentNode).substr(kParentPrefix.size());
                if (!suffix.empty()) {
                    return NodeSelection{ std::string(suffix), "captured-custom-attachment", true };
                }
            }

            if (!object.anchorNode.empty()) {
                return NodeSelection{ object.anchorNode, "custom-anchor-fallback", true };
            }
            return std::nullopt;
        }

        std::string ExpectedAttachment(const NodeSelection& node)
        {
            return fmt::format(
                "OBJECT {} {}",
                node.parentAttachment ? "P" : "R",
                node.node);
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

        bool IsAnimSyncLoaded() noexcept
        {
            return GetModuleHandleW(L"AnimSyncTogether.dll") != nullptr;
        }

        bool IsAnimSyncTransientAttachment(std::string_view parentName) noexcept
        {
            if (!IsAnimSyncLoaded()) {
                return false;
            }

            return parentName == "OBJECT P AnimObjectR" ||
                parentName == "OBJECT R AnimObjectR" ||
                parentName == "OBJECT P AnimObjectL" ||
                parentName == "OBJECT R AnimObjectL";
        }

        struct SceneIEDObject
        {
            RE::NiAVObject* object{ nullptr };
            RE::FormID formID{ 0 };
            std::string parentName;
            std::string objectName;
        };

        void CollectIEDSceneObjects(
            RE::NiAVObject* current,
            std::vector<SceneIEDObject>& out)
        {
            if (!current) {
                return;
            }

            const auto* rawName = current->name.c_str();
            const std::string_view name = rawName ? std::string_view(rawName) : std::string_view{};
            if (const auto formID = ParseIEDObjectFormID(name)) {
                const auto* parentRaw = current->parent && current->parent->name.c_str() ?
                    current->parent->name.c_str() : nullptr;
                out.push_back(SceneIEDObject{
                    current,
                    *formID,
                    parentRaw ? std::string(parentRaw) : std::string{},
                    std::string(name)
                });
            }

            if (auto* node = current->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    if (child) {
                        CollectIEDSceneObjects(child.get(), out);
                    }
                }
            }
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

        float MatchScore(
            const RE::NiAVObject& object,
            const CapturedIEDObject& expected)
        {
            const auto rotationDelta = MaxMatrixDelta(object.local.rotate, expected.rotationMatrix);
            const auto positionDelta = MaxPositionDelta(object.local.translate, expected.position);
            const auto scaleDelta = std::abs(object.local.scale - expected.scale);
            return rotationDelta * 1000.0f + positionDelta + scaleDelta * 100.0f;
        }

        void ApplyRawTransform(RE::NiAVObject* object, const CapturedIEDObject& captured)
        {
            if (!object) {
                return;
            }

            object->local.translate.x = captured.position[0];
            object->local.translate.y = captured.position[1];
            object->local.translate.z = captured.position[2];
            object->local.scale = std::clamp(captured.scale, 0.01f, 100.0f);

            std::size_t index = 0;
            for (std::size_t row = 0; row < 3; ++row) {
                for (std::size_t col = 0; col < 3; ++col) {
                    object->local.rotate.entry[row][col] = captured.rotationMatrix[index++];
                }
            }

            RE::NiUpdateData updateData{};
            updateData.time = 0.0f;
            object->UpdateWorldData(&updateData);
            object->UpdateWorldBound();
        }

        bool ConfigureRemoteItem(
            RE::Actor* actor,
            const std::string& itemName,
            RE::TESForm* remoteForm,
            const NodeSelection& node,
            bool leftWeapon)
        {
            bool dispatched = true;
            dispatched &= DispatchNoResult(
                "CreateItemActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                itemName,
                false,
                remoteForm,
                false,
                node.node);

            dispatched &= DispatchNoResult(
                "SetItemFormActor",
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey),
                itemName,
                true,
                remoteForm);

            for (const bool female : { false, true }) {
                dispatched &= DispatchNoResult(
                    "SetItemAttachmentModeActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    itemName,
                    female,
                    node.parentAttachment ? 1 : 0,
                    false);

                dispatched &= DispatchNoResult(
                    "SetItemNodeActor",
                    static_cast<RE::Actor*>(actor),
                    std::string(kPluginKey),
                    itemName,
                    female,
                    node.node);

                if (leftWeapon) {
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

    void RemoteIEDRenderer::Start()
    {
        if (_watchdogThread.joinable()) {
            return;
        }

        _watchdogThread = std::jthread(
            [this](std::stop_token stopToken) {
                WatchdogLoop(stopToken);
            });
        SKSE::log::info(
            "REMOTE IED transform/isolation watchdog started: interval={}ms",
            kWatchdogInterval.count());
    }

    void RemoteIEDRenderer::WatchdogLoop(std::stop_token stopToken)
    {
        while (!stopToken.stop_requested()) {
            std::this_thread::sleep_for(kWatchdogInterval);
            if (stopToken.stop_requested()) {
                break;
            }
            ScheduleWatchdogTick();
        }
    }

    void RemoteIEDRenderer::ScheduleWatchdogTick()
    {
        if (_watchdogTaskQueued.exchange(true)) {
            return;
        }

        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            _watchdogTaskQueued.store(false);
            return;
        }

        tasks->AddTask(
            [this]() {
                _watchdogTaskQueued.store(false);
                WatchdogTick();
            });
    }

    void RemoteIEDRenderer::WatchdogTick()
    {
        for (auto& [proxyFormID, proxyState] : _trackedProxies) {
            auto* form = RE::TESForm::LookupByID(proxyFormID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            auto* root = actor ? actor->Get3D1(false) : nullptr;
            if (!root || actor == RE::PlayerCharacter::GetSingleton()) {
                continue;
            }

            std::erase_if(
                proxyState.suppressedNodes,
                [](const RE::NiPointer<RE::NiAVObject>& object) {
                    return !object || !object->parent;
                });

            std::vector<SceneIEDObject> sceneObjects;
            CollectIEDSceneObjects(root, sceneObjects);
            std::vector<bool> assigned(sceneObjects.size(), false);

            const auto forgetSuppressed = [&proxyState](RE::NiAVObject* object) {
                std::erase_if(
                    proxyState.suppressedNodes,
                    [object](const RE::NiPointer<RE::NiAVObject>& suppressed) {
                        return !suppressed || suppressed.get() == object || !suppressed->parent;
                    });
            };

            for (auto& tracked : proxyState.objects) {
                std::size_t bestIndex = sceneObjects.size();
                float bestScore = std::numeric_limits<float>::max();

                for (std::size_t index = 0; index < sceneObjects.size(); ++index) {
                    if (assigned[index]) {
                        continue;
                    }

                    const auto& candidate = sceneObjects[index];
                    if (!candidate.object ||
                        candidate.formID != tracked.remoteFormID ||
                        candidate.parentName != tracked.expectedAttachment) {
                        continue;
                    }

                    const auto score = MatchScore(*candidate.object, tracked.object);
                    if (score < bestScore) {
                        bestScore = score;
                        bestIndex = index;
                    }
                }

                if (bestIndex == sceneObjects.size()) {
                    continue;
                }

                assigned[bestIndex] = true;
                auto* remoteObject = sceneObjects[bestIndex].object;
                forgetSuppressed(remoteObject);
                remoteObject->SetAppCulled(false);

                const auto rotationDelta = MaxMatrixDelta(
                    remoteObject->local.rotate,
                    tracked.object.rotationMatrix);
                const auto positionDelta = MaxPositionDelta(
                    remoteObject->local.translate,
                    tracked.object.position);
                const auto scaleDelta = std::abs(remoteObject->local.scale - tracked.object.scale);
                const bool needsCorrection =
                    rotationDelta > kRotationEpsilon ||
                    positionDelta > kPositionEpsilon ||
                    scaleDelta > kScaleEpsilon;

                if (!tracked.acquired) {
                    tracked.acquired = true;
                    SKSE::log::info(
                        "REMOTE IED WATCHDOG acquired: connection={} name=\"{}\" proxy={:08X} kind={} slot={} item=\"{}\" remoteForm={:08X} parent=\"{}\" initialDelta(rot={:.6f},pos={:.6f},scale={:.6f})",
                        proxyState.connectionID,
                        proxyState.displayName,
                        proxyFormID,
                        tracked.kind,
                        tracked.slot ? static_cast<int>(*tracked.slot) : -1,
                        tracked.itemName,
                        tracked.remoteFormID,
                        tracked.expectedAttachment,
                        rotationDelta,
                        positionDelta,
                        scaleDelta);
                }

                if (!needsCorrection) {
                    continue;
                }

                ApplyRawTransform(remoteObject, tracked.object);

                const auto afterRotationDelta = MaxMatrixDelta(
                    remoteObject->local.rotate,
                    tracked.object.rotationMatrix);
                const auto afterPositionDelta = MaxPositionDelta(
                    remoteObject->local.translate,
                    tracked.object.position);
                const auto afterScaleDelta = std::abs(remoteObject->local.scale - tracked.object.scale);
                ++tracked.corrections;

                if (tracked.corrections == 1) {
                    SKSE::log::info(
                        "REMOTE IED WATCHDOG corrected: connection={} name=\"{}\" proxy={:08X} kind={} slot={} item=\"{}\" remoteForm={:08X} parent=\"{}\" beforeDelta(rot={:.6f},pos={:.6f},scale={:.6f}) afterDelta(rot={:.6f},pos={:.6f},scale={:.6f})",
                        proxyState.connectionID,
                        proxyState.displayName,
                        proxyFormID,
                        tracked.kind,
                        tracked.slot ? static_cast<int>(*tracked.slot) : -1,
                        tracked.itemName,
                        tracked.remoteFormID,
                        tracked.expectedAttachment,
                        rotationDelta,
                        positionDelta,
                        scaleDelta,
                        afterRotationDelta,
                        afterPositionDelta,
                        afterScaleDelta);
                } else {
                    SKSE::log::trace(
                        "REMOTE IED WATCHDOG recorrected: proxy={:08X} kind={} slot={} item=\"{}\" correction={} beforeDelta(rot={:.6f},pos={:.6f},scale={:.6f})",
                        proxyFormID,
                        tracked.kind,
                        tracked.slot ? static_cast<int>(*tracked.slot) : -1,
                        tracked.itemName,
                        tracked.corrections,
                        rotationDelta,
                        positionDelta,
                        scaleDelta);
                }
            }

            for (std::size_t index = 0; index < sceneObjects.size(); ++index) {
                if (assigned[index]) {
                    continue;
                }

                auto* object = sceneObjects[index].object;
                if (!object) {
                    continue;
                }

                if (IsAnimSyncTransientAttachment(sceneObjects[index].parentName)) {
                    const bool suppressedByIEDSync = std::ranges::any_of(
                        proxyState.suppressedNodes,
                        [object](const RE::NiPointer<RE::NiAVObject>& suppressed) {
                            return suppressed && suppressed.get() == object;
                        });

                    if (suppressedByIEDSync) {
                        forgetSuppressed(object);
                        object->SetAppCulled(false);
                        SKSE::log::info(
                            "REMOTE IED ISOLATION restored AnimSync transient: connection={} name=\"{}\" proxy={:08X} remoteForm={:08X} parent=\"{}\" object=\"{}\" reason=AnimSync-owns-AnimObject",
                            proxyState.connectionID,
                            proxyState.displayName,
                            proxyFormID,
                            sceneObjects[index].formID,
                            sceneObjects[index].parentName,
                            sceneObjects[index].objectName);
                    }
                    continue;
                }

                const bool alreadySuppressed = std::ranges::any_of(
                    proxyState.suppressedNodes,
                    [object](const RE::NiPointer<RE::NiAVObject>& suppressed) {
                        return suppressed && suppressed.get() == object;
                    });

                if (!alreadySuppressed && !object->GetAppCulled()) {
                    proxyState.suppressedNodes.emplace_back(object);
                    SKSE::log::info(
                        "REMOTE IED ISOLATION suppressed: connection={} name=\"{}\" proxy={:08X} remoteForm={:08X} parent=\"{}\" object=\"{}\" reason=not-in-authoritative-remote-state",
                        proxyState.connectionID,
                        proxyState.displayName,
                        proxyFormID,
                        sceneObjects[index].formID,
                        sceneObjects[index].parentName,
                        sceneObjects[index].objectName);
                }

                if (alreadySuppressed || !object->GetAppCulled()) {
                    object->SetAppCulled(true);
                }
            }
        }
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
                if (auto tracked = _trackedProxies.find(proxyFormID); tracked != _trackedProxies.end()) {
                    tracked->second.connectionID = connectionID;
                    tracked->second.displayName = std::string(displayName);
                }
                ScheduleWatchdogTick();
                return true;
            }
        }

        std::vector<RE::NiPointer<RE::NiAVObject>> suppressedNodes;
        if (auto existing = _trackedProxies.find(proxyFormID); existing != _trackedProxies.end()) {
            suppressedNodes = std::move(existing->second.suppressedNodes);
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

        TrackedProxyState trackedProxy;
        trackedProxy.connectionID = connectionID;
        trackedProxy.displayName = std::string(displayName);
        trackedProxy.suppressedNodes = std::move(suppressedNodes);

        std::size_t renderedSlots = 0;
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
            dispatched &= ConfigureRemoteItem(
                actor,
                itemName,
                remoteForm,
                nodeSelection,
                kLeftWeaponSlots[slot]);

            TrackedRemoteObject tracked;
            tracked.itemName = itemName;
            tracked.kind = "slot";
            tracked.slot = slot;
            tracked.remoteFormID = remoteForm->GetFormID();
            tracked.expectedAttachment = ExpectedAttachment(nodeSelection);
            tracked.object = *object;
            trackedProxy.objects.emplace_back(std::move(tracked));

            ++renderedSlots;
            SKSE::log::info(
                "REMOTE IED SLOT queued: connection={} name=\"{}\" proxy={:08X} slot={} plugin=\"{}\" localForm={:X} remoteForm={:08X} node=\"{}\" nodeSource={} attachmentMode={} rawTransform=watchdog expectedParent=\"{}\" capturedAttachment=\"{}\" capturedAnchor=\"{}\"",
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
                ExpectedAttachment(nodeSelection),
                object->attachmentNode,
                object->anchorNode);
        }

        std::size_t renderedCustom = 0;
        std::size_t customIndex = 0;
        for (const auto& object : state.objects) {
            if (object.kind != IEDObjectKind::kCustom || !object.visible) {
                continue;
            }

            const auto nodeSelection = CustomNode(object);
            if (!nodeSelection) {
                SKSE::log::warn(
                    "REMOTE IED CUSTOM skipped missing attachment: connection={} proxy={:08X} plugin=\"{}\" localForm={:X} attachment=\"{}\" anchor=\"{}\"",
                    connectionID,
                    proxyFormID,
                    object.form.plugin,
                    object.form.localFormID,
                    object.attachmentNode,
                    object.anchorNode);
                continue;
            }

            auto* remoteForm = ResolveFormIdentity(object.form);
            if (!remoteForm) {
                SKSE::log::warn(
                    "REMOTE IED CUSTOM skipped unresolved form: connection={} proxy={:08X} plugin=\"{}\" localForm={:X}",
                    connectionID,
                    proxyFormID,
                    object.form.plugin,
                    object.form.localFormID);
                continue;
            }

            const auto itemName = fmt::format("remote-custom-{:02}", customIndex++);
            dispatched &= ConfigureRemoteItem(
                actor,
                itemName,
                remoteForm,
                *nodeSelection,
                false);

            TrackedRemoteObject tracked;
            tracked.itemName = itemName;
            tracked.kind = "custom";
            tracked.remoteFormID = remoteForm->GetFormID();
            tracked.expectedAttachment = object.attachmentNode.starts_with("OBJECT R ") ||
                    object.attachmentNode.starts_with("OBJECT P ") ?
                object.attachmentNode : ExpectedAttachment(*nodeSelection);
            tracked.object = object;
            trackedProxy.objects.emplace_back(std::move(tracked));

            ++renderedCustom;
            SKSE::log::info(
                "REMOTE IED CUSTOM queued: connection={} name=\"{}\" proxy={:08X} plugin=\"{}\" localForm={:X} remoteForm={:08X} objectNode=\"{}\" node=\"{}\" nodeSource={} attachmentMode={} rawTransform=watchdog expectedParent=\"{}\" capturedAttachment=\"{}\" capturedAnchor=\"{}\"",
                connectionID,
                displayName,
                proxyFormID,
                object.form.plugin,
                object.form.localFormID,
                remoteForm->GetFormID(),
                object.objectNode,
                nodeSelection->node,
                nodeSelection->source,
                nodeSelection->parentAttachment ? "parent" : "reference",
                trackedProxy.objects.back().expectedAttachment,
                object.attachmentNode,
                object.anchorNode);
        }

        dispatched &= DispatchNoResult(
            "Evaluate",
            static_cast<RE::Actor*>(actor));

        _trackedProxies[proxyFormID] = std::move(trackedProxy);
        ScheduleWatchdogTick();

        if (dispatched) {
            _appliedSignatures[proxyFormID] = signature;
        }

        SKSE::log::info(
            "REMOTE IED RENDER queued: connection={} name=\"{}\" proxy={:08X} visibleSlots={} customRendered={} trackedTransforms={} dispatchAccepted={} watchdogIntervalMs={} npcDisplayIsolation=1",
            connectionID,
            displayName,
            proxyFormID,
            renderedSlots,
            renderedCustom,
            _trackedProxies[proxyFormID].objects.size(),
            dispatched ? 1 : 0,
            kWatchdogInterval.count());
        return dispatched;
    }

    void RemoteIEDRenderer::ClearProxy(STRPM::ProxyFormID proxyFormID)
    {
        if (proxyFormID == STRPM::kInvalidProxyFormID) {
            return;
        }

        if (const auto tracked = _trackedProxies.find(proxyFormID); tracked != _trackedProxies.end()) {
            for (const auto& suppressed : tracked->second.suppressedNodes) {
                if (suppressed && suppressed->parent) {
                    suppressed->SetAppCulled(false);
                }
            }
        }

        _trackedProxies.erase(proxyFormID);
        _appliedSignatures.erase(proxyFormID);

        auto* form = RE::TESForm::LookupByID(proxyFormID);
        auto* actor = form ? form->As<RE::Actor>() : nullptr;
        if (actor && actor != RE::PlayerCharacter::GetSingleton()) {
            ClearOwnedItems(actor);
            SKSE::log::info("REMOTE IED RENDER cleared: proxy={:08X} suppressedNodesRestored=1", proxyFormID);
        }
    }

    void RemoteIEDRenderer::Reset()
    {
        std::unordered_set<STRPM::ProxyFormID> proxies;
        proxies.reserve(_appliedSignatures.size() + _trackedProxies.size());
        for (const auto& [proxyFormID, _] : _appliedSignatures) {
            proxies.emplace(proxyFormID);
        }
        for (const auto& [proxyFormID, _] : _trackedProxies) {
            proxies.emplace(proxyFormID);
        }

        for (const auto proxyFormID : proxies) {
            ClearProxy(proxyFormID);
        }
        _appliedSignatures.clear();
        _trackedProxies.clear();
    }
}
