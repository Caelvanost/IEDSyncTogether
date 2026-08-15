#include "PCH.h"
#include "ProxyResolver.h"

#include <RE/F/FunctionArguments.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/P/PackUnpack.h>
#include <RE/S/SkyrimVM.h>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr std::string_view kSTRPapyrusClass = "SkyrimTogetherUtils";
        constexpr std::string_view kSTRIsRemotePlayer = "IsRemotePlayer";
        constexpr RE::FormID kDynamicFormMask = 0xFF000000;

        std::mutex g_verifiedMutex;
        std::unordered_set<RE::FormID> g_verifiedRemotePlayers;

        RE::BSScript::IVirtualMachine* GetVM()
        {
            auto* skyrimVM = RE::SkyrimVM::GetSingleton();
            return skyrimVM && skyrimVM->impl ? skyrimVM->impl.get() : nullptr;
        }

        std::vector<RE::Actor*> CollectDynamicActorCandidates()
        {
            std::vector<RE::Actor*> result;
            std::unordered_set<RE::FormID> seen;
            auto* processLists = RE::ProcessLists::GetSingleton();
            if (!processLists) {
                return result;
            }

            auto scan = [&](const auto& handles) {
                for (const auto& handle : handles) {
                    const auto actorPointer = handle.get();
                    auto* actor = actorPointer.get();
                    if (!actor || actor->IsPlayerRef()) {
                        continue;
                    }

                    const auto formID = actor->GetFormID();
                    if ((formID & kDynamicFormMask) != kDynamicFormMask) {
                        continue;
                    }

                    if (seen.insert(formID).second) {
                        result.push_back(actor);
                    }
                }
            };

            scan(processLists->highActorHandles);
            scan(processLists->middleHighActorHandles);
            scan(processLists->middleLowActorHandles);
            scan(processLists->lowActorHandles);
            return result;
        }

        void ReplaceVerifiedRemotePlayers(const std::vector<RE::FormID>& ids)
        {
            std::scoped_lock lock(g_verifiedMutex);
            g_verifiedRemotePlayers.clear();
            g_verifiedRemotePlayers.insert(ids.begin(), ids.end());
        }

        struct ScanRequest
        {
            ScanRequest(std::size_t count, RemoteProxyScanCallback value) :
                remaining(count),
                callback(std::move(value))
            {}

            void Complete(RE::FormID formID, bool isRemote)
            {
                RemoteProxyScanCallback finalCallback;
                std::vector<RE::FormID> finalIDs;

                {
                    std::scoped_lock lock(mutex);
                    if (!completed.insert(formID).second) {
                        return;
                    }

                    if (isRemote) {
                        remoteIDs.push_back(formID);
                    }

                    if (remaining > 0) {
                        --remaining;
                    }
                    if (remaining == 0) {
                        finalIDs = remoteIDs;
                        finalCallback = std::move(callback);
                    }
                }

                if (!finalCallback) {
                    return;
                }

                auto finish = [ids = std::move(finalIDs), callback = std::move(finalCallback)]() mutable {
                    ReplaceVerifiedRemotePlayers(ids);
                    callback(std::move(ids));
                };

                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask(std::move(finish));
                } else {
                    finish();
                }
            }

            std::mutex mutex;
            std::unordered_set<RE::FormID> completed;
            std::vector<RE::FormID> remoteIDs;
            std::size_t remaining{ 0 };
            RemoteProxyScanCallback callback;
        };

        class RemotePlayerResultCallback final :
            public RE::BSScript::IStackCallbackFunctor
        {
        public:
            RemotePlayerResultCallback(
                std::shared_ptr<ScanRequest> request,
                RE::FormID formID) :
                _request(std::move(request)),
                _formID(formID)
            {}

            void operator()(RE::BSScript::Variable result) override
            {
                const bool isRemote = result.Unpack<bool>();
                _request->Complete(_formID, isRemote);
            }

            void SetObject(
                const RE::BSTSmartPointer<RE::BSScript::Object>& object) override
            {
                _object = object;
            }

        private:
            std::shared_ptr<ScanRequest> _request;
            RE::FormID _formID{ 0 };
            RE::BSTSmartPointer<RE::BSScript::Object> _object;
        };
    }

    bool EqualsInsensitive(std::string_view left, std::string_view right)
    {
        return left.size() == right.size() && std::equal(
            left.begin(),
            left.end(),
            right.begin(),
            [](char lhs, char rhs) {
                return std::tolower(static_cast<unsigned char>(lhs)) ==
                       std::tolower(static_cast<unsigned char>(rhs));
            });
    }

    bool RequestRemotePlayerProxyScan(RemoteProxyScanCallback callback)
    {
        auto* vm = GetVM();
        if (!vm || !callback) {
            return false;
        }

        const auto candidates = CollectDynamicActorCandidates();
        if (candidates.empty()) {
            ReplaceVerifiedRemotePlayers({});
            callback({});
            return true;
        }

        auto request = std::make_shared<ScanRequest>(candidates.size(), std::move(callback));
        SKSE::log::debug(
            "STR proxy validation: querying {} dynamic actor candidate(s) through SkyrimTogetherUtils.IsRemotePlayer",
            candidates.size());

        for (auto* actor : candidates) {
            const auto formID = actor->GetFormID();
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result(
                new RemotePlayerResultCallback(request, formID));

            const bool dispatched = vm->DispatchStaticCall(
                RE::BSFixedString(kSTRPapyrusClass.data()),
                RE::BSFixedString(kSTRIsRemotePlayer.data()),
                RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor)),
                result);

            if (!dispatched) {
                request->Complete(formID, false);
            }
        }

        return true;
    }

    void ClearRemotePlayerProxyCache()
    {
        ReplaceVerifiedRemotePlayers({});
    }

    bool IsVerifiedRemotePlayerProxy(RE::Actor* actor)
    {
        if (!actor || actor->IsPlayerRef()) {
            return false;
        }

        std::scoped_lock lock(g_verifiedMutex);
        return g_verifiedRemotePlayers.contains(actor->GetFormID());
    }

    std::vector<RE::Actor*> FindRemotePlayerProxies()
    {
        std::vector<RE::FormID> ids;
        {
            std::scoped_lock lock(g_verifiedMutex);
            ids.assign(g_verifiedRemotePlayers.begin(), g_verifiedRemotePlayers.end());
        }

        std::vector<RE::Actor*> result;
        result.reserve(ids.size());
        for (const auto formID : ids) {
            auto* form = RE::TESForm::LookupByID(formID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (actor) {
                result.push_back(actor);
            }
        }
        return result;
    }

    RE::Actor* FindRemotePlayerProxy(std::string_view playerName)
    {
        if (playerName.empty()) {
            return nullptr;
        }

        auto* localPlayer = RE::PlayerCharacter::GetSingleton();
        RE::Actor* best = nullptr;
        float bestDistance = std::numeric_limits<float>::max();

        for (auto* actor : FindRemotePlayerProxies()) {
            const auto* name = actor->GetName();
            if (!name || !EqualsInsensitive(name, playerName)) {
                continue;
            }

            const float distance = localPlayer ?
                actor->GetPosition().GetDistance(localPlayer->GetPosition()) : 0.0F;
            if (!best || distance < bestDistance) {
                best = actor;
                bestDistance = distance;
            }
        }
        return best;
    }
}
