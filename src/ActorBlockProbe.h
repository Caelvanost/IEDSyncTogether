#pragma once

#include "PCH.h"

namespace IEDSyncTogether
{
    class ActorBlockProbe
    {
    public:
        static ActorBlockProbe& GetSingleton();

        void Start();
        void Stop();
        void Reset();

    private:
        ActorBlockProbe() = default;
        ~ActorBlockProbe();
        ActorBlockProbe(const ActorBlockProbe&) = delete;
        ActorBlockProbe& operator=(const ActorBlockProbe&) = delete;

        void TimerLoop(std::stop_token token);
        void Tick();
        void RemoveTrackedBlocks();

        std::jthread _timer;
        std::atomic_bool _running{ false };
        std::unordered_set<RE::FormID> _trackedProxies;
    };
}
