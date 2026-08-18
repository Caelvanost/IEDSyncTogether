#pragma once

#include "PCH.h"
#include "FormIdentity.h"

namespace IEDSyncTogether
{
    struct CapturedIEDObject
    {
        FormIdentity form;
        std::optional<std::uint32_t> slot;
        bool visible{ false };
        std::string objectNode;
        std::string attachmentNode;
        std::string anchorNode;
        std::array<float, 3> position{};
        std::array<float, 9> rotationMatrix{};
        float scale{ 1.0f };
    };

    struct LocalIEDState
    {
        SlotState slots{};
        std::vector<CapturedIEDObject> objects;
    };

    class LocalCaptureProbe
    {
    public:
        static LocalCaptureProbe& GetSingleton();

        void Start();
        void Stop();
        void Reset();

    private:
        void TimerLoop(std::stop_token token);
        void Tick();
        void CompleteCapture(SlotState slots);

        std::atomic_bool _running{ false };
        std::atomic_bool _captureInFlight{ false };
        std::jthread _timer;
        std::string _lastSignature;
    };
}
