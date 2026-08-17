#pragma once

#include "PCH.h"

namespace IEDSyncTogether
{
    class IEDDiagnostics
    {
    public:
        static IEDDiagnostics& GetSingleton();

        void Start();
        void Stop();
        void Reset();

    private:
        IEDDiagnostics() = default;
        ~IEDDiagnostics();
        IEDDiagnostics(const IEDDiagnostics&) = delete;
        IEDDiagnostics& operator=(const IEDDiagnostics&) = delete;

        void TimerLoop(std::stop_token token);
        void Tick();
        void LogIEDSettings() const;

        std::jthread _timer;
        std::atomic_bool _running{ false };
        std::unordered_set<RE::FormID> _seenProxies;
    };
}
