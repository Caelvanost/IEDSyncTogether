#include "PCH.h"
#include "ActorBlockProbe.h"

#include "SyncService.h"

#include <RE/F/FunctionArguments.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/S/SkyrimVM.h>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr std::string_view kPapyrusClass = "IED";
        constexpr std::string_view kPluginKey = "IEDSyncTogether.esp";
        constexpr auto kProbeInterval = std::chrono::milliseconds(750);

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

        bool QueueActorBlock(RE::Actor* actor, bool blocked)
        {
            if (!actor || !GetVM()) {
                return false;
            }

            const auto operation = blocked ? "AddActorBlock" : "RemoveActorBlock";
            const bool blockAccepted = DispatchNoResult(
                operation,
                static_cast<RE::Actor*>(actor),
                std::string(kPluginKey));
            const bool evaluateAccepted = DispatchNoResult(
                "Evaluate",
                static_cast<RE::Actor*>(actor));

            SKSE::log::debug(
                "IED ActorBlock probe dispatch: proxy={:08X} action={} blockAccepted={} evaluateAccepted={}",
                actor->GetFormID(),
                blocked ? "add" : "remove",
                blockAccepted ? 1 : 0,
                evaluateAccepted ? 1 : 0);

            return blockAccepted && evaluateAccepted;
        }
    }

    ActorBlockProbe& ActorBlockProbe::GetSingleton()
    {
        static ActorBlockProbe instance;
        return instance;
    }

    ActorBlockProbe::~ActorBlockProbe()
    {
        Stop();
    }

    void ActorBlockProbe::Start()
    {
        if (_running.exchange(true)) {
            return;
        }

        _timer = std::jthread([this](std::stop_token token) { TimerLoop(token); });
        SKSE::log::info(
            "IED v0.3.1 ActorBlock probe started: public AddActorBlock will be reasserted on resolved STR proxies after Custom Item rendering");
    }

    void ActorBlockProbe::Stop()
    {
        if (!_running.exchange(false)) {
            return;
        }

        if (_timer.joinable()) {
            _timer.request_stop();
            _timer.join();
        }

        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([]() { ActorBlockProbe::GetSingleton().RemoveTrackedBlocks(); });
        }
    }

    void ActorBlockProbe::Reset()
    {
        RemoveTrackedBlocks();
    }

    void ActorBlockProbe::TimerLoop(std::stop_token token)
    {
        while (!token.stop_requested() && _running.load()) {
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([]() { ActorBlockProbe::GetSingleton().Tick(); });
            }

            auto elapsed = std::chrono::milliseconds(0);
            while (elapsed < kProbeInterval && !token.stop_requested() && _running.load()) {
                constexpr auto slice = std::chrono::milliseconds(100);
                std::this_thread::sleep_for(slice);
                elapsed += slice;
            }
        }
    }

    void ActorBlockProbe::Tick()
    {
        auto& service = SyncService::GetSingleton();
        const auto proxies = service.GetResolvedRemoteProxies();

        if (!service.IsRemoteNpcSuppressionEnabled()) {
            if (!_trackedProxies.empty()) {
                RemoveTrackedBlocks();
            }
            return;
        }

        std::unordered_set<RE::FormID> current;
        current.reserve(proxies.size());

        for (const auto proxyFormID : proxies) {
            if (proxyFormID == 0) {
                continue;
            }

            auto* form = RE::TESForm::LookupByID(proxyFormID);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor) {
                continue;
            }

            current.insert(proxyFormID);
            const bool firstApplication = _trackedProxies.insert(proxyFormID).second;
            const bool accepted = QueueActorBlock(actor, true);

            if (firstApplication) {
                SKSE::log::info(
                    "IED ActorBlock probe active: proxy={:08X} AddActorBlock+Evaluate accepted={}; block will be reasserted every {}ms",
                    proxyFormID,
                    accepted ? 1 : 0,
                    kProbeInterval.count());
            }
        }

        for (auto iterator = _trackedProxies.begin(); iterator != _trackedProxies.end();) {
            if (current.contains(*iterator)) {
                ++iterator;
                continue;
            }

            if (auto* form = RE::TESForm::LookupByID(*iterator)) {
                if (auto* actor = form->As<RE::Actor>()) {
                    QueueActorBlock(actor, false);
                    SKSE::log::info(
                        "IED ActorBlock probe released: proxy={:08X}",
                        *iterator);
                }
            }
            iterator = _trackedProxies.erase(iterator);
        }
    }

    void ActorBlockProbe::RemoveTrackedBlocks()
    {
        for (const auto proxyFormID : _trackedProxies) {
            if (auto* form = RE::TESForm::LookupByID(proxyFormID)) {
                if (auto* actor = form->As<RE::Actor>()) {
                    QueueActorBlock(actor, false);
                }
            }
        }

        if (!_trackedProxies.empty()) {
            SKSE::log::info(
                "IED ActorBlock probe reset: released {} tracked proxy/proxies",
                _trackedProxies.size());
        }
        _trackedProxies.clear();
    }
}
