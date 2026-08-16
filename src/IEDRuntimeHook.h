#pragma once

namespace IEDSyncTogether
{
    class IEDRuntimeHook
    {
    public:
        static bool Install();
        static bool IsInstalled() noexcept;
    };
}
