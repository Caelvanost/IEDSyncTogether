#pragma once

#include "PCH.h"

namespace IEDSyncTogether
{
    struct Config
    {
        bool networkEnabled{ true };
        bool autoDiscovery{ true };
        bool suppressRemoteNpcDisplays{ false };
        std::uint16_t localPort{ 38471 };
        std::uint16_t peerPort{ 38471 };
        std::uint32_t captureIntervalMs{ 750 };
        std::uint32_t resendIntervalMs{ 5000 };
        std::uint32_t discoveryIntervalMs{ 2000 };
        std::uint32_t peerTimeoutMs{ 10000 };
        std::string peerHost{ "127.0.0.1" };

        static Config Load();
    };
}
