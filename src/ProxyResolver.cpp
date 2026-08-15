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

        std::vector<RE::FormID> CollectDynamicActorCandidates()
        {
            std::vector<RE::FormID> result;
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
                        result.push_back(formID);
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
            ScanRequest(
                std::vector<RE::FormID> candidateIDs,
                RemoteProxyScanCallback value) :
                candidates(std::move(candidateIDs)),
                callback(std::move(value))
            {}

            std::vector<RE::FormID> candidates;
            std::vector<RE::FormID> remoteIDs;
            RemoteProxyScanCallback callback;
        };

        void DispatchProxyCandidate(
            const std::shared_ptr<ScanRequest>& request,
            std::size_t index);

        void CompleteProxyCandidate(
            const std::shared_ptr<ScanRequest>& request,
            std::size_t index,
            bool isRemote)
        {
            if (index >= request->candidates.size()) {
                return;
            }

            const auto formID = request->candidates[index];
            if (isRemote) {
                request->remoteIDs.push_back(formID);
            }

            const auto nextIndex = index + 1;
            if (nextIndex < request->candidates.size()) {
                DispatchProxyCandidate(request, nextIndex);
                return;
            }

            ReplaceVerifiedRemotePlayers(request->remoteIDs);
            SKSE::log::debug(
                "STR proxy validation complete: {}/{} candidate(s) confirmed remote players",
                request->remoteIDs.size(),
                request->candidates.size());

            auto callback = std::move(request->callback);
            if (callback) {
                callback(std::move(request->remoteIDs));
            }
        }

        class RemotePlayerResultCallback final :
            public RE::BSScript::IStackCallbackFunctor
        {
        public:
            RemotePlayerResultCallback(
                std::shared_ptr<ScanRequest> request,
                std::size_t index) :
                _request(std::move(request)),
                _index(index)
            {}

            void operator()(RE::BSScript::Variable result) override
            {
                const bool isRemote = result.Unpack<bool>();
                auto request = _request;
                const auto index = _index;

                auto complete = [request = std::move(request), index, isRemote]() {
                    CompleteProxyCandidate(request, index, isRemote);
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
            std::shared_ptr<ScanRequest> _request;
            std::size_t _index{ 0 };
            RE::BSTSmartPointer<RE::BSScript::Object> _object;
        };

        void DispatchProxyCandidate(
            const std::shared_ptr<ScanRequest>& request,
            std::size_t index)
        {
            if (index >= request->candidates.size()) {
                return;
            }

            auto* vm = GetVM();
            auto* form = RE::TESForm::LookupByID(request->candidates[index]);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!vm || !actor) {
                auto complete = [request, index]() {
                    CompleteProxyCandidate(request, index, false);
                };
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask(std::move(complete));
                } else {
                    complete();
                }
                return;
            }

            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> result(
                new RemotePlayerResultCallback(request, index));

            const bool dispatched = vm->DispatchStaticCall(
                RE::BSFixedString(kSTRPapyrusClass.data()),
                RE::BSFixedString(kSTRIsRemotePlayer.data()),
                RE::MakeFunctionArguments(static_cast<RE::Actor*>(actor)),
                result);

            if (!dispatched) {
                auto complete = [request, index]() {
                    CompleteProxyCandidate(request, index, false);
                };
                if (auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask(std::move(complete));
                } else {
                    complete();
                }
            }
        }
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

        auto candidates = CollectDynamicActorCandidates();
        if (candidates.empty()) {
            ReplaceVerifiedRemotePlayers({});
            callback({});
            return true;
        }

        SKSE::log::debug(
            "STR proxy validation: sequentially querying {} dynamic actor candidate(s) through SkyrimTogetherUtils.IsRemotePlayer",
            candidates.size());

        auto request = std::make_shared<ScanRequest>(
            std::move(candidates),
            std::move(callback));
        DispatchProxyCandidate(request, 0);
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
