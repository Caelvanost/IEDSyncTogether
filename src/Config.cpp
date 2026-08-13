#include "PCH.h"
#include "Config.h"

namespace IEDSyncTogether
{
    namespace
    {
        constexpr auto kIniPath = L"Data\\SKSE\\Plugins\\IEDSyncTogether.ini";

        std::uint32_t ReadUInt(
            const wchar_t* section,
            const wchar_t* key,
            std::uint32_t fallback)
        {
            return static_cast<std::uint32_t>(GetPrivateProfileIntW(
                section,
                key,
                static_cast<int>(fallback),
                kIniPath));
        }

        bool ReadBool(
            const wchar_t* section,
            const wchar_t* key,
            bool fallback)
        {
            return ReadUInt(section, key, fallback ? 1U : 0U) != 0;
        }

        std::string ReadString(
            const wchar_t* section,
            const wchar_t* key,
            const wchar_t* fallback)
        {
            std::array<wchar_t, 256> buffer{};
            GetPrivateProfileStringW(
                section,
                key,
                fallback,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                kIniPath);

            const int needed = WideCharToMultiByte(
                CP_UTF8,
                0,
                buffer.data(),
                -1,
                nullptr,
                0,
                nullptr,
                nullptr);

            if (needed <= 1) {
                return {};
            }

            std::string value(static_cast<std::size_t>(needed), '\0');
            WideCharToMultiByte(
                CP_UTF8,
                0,
                buffer.data(),
                -1,
                value.data(),
                needed,
                nullptr,
                nullptr);
            value.pop_back();
            return value;
        }

        std::uint16_t ClampPort(std::uint32_t value, std::uint16_t fallback)
        {
            return value > 0 && value <= 65535 ?
                static_cast<std::uint16_t>(value) : fallback;
        }
    }

    Config Config::Load()
    {
        Config config{};
        config.networkEnabled = ReadBool(L"Network", L"Enabled", config.networkEnabled);
        config.autoDiscovery = ReadBool(L"Network", L"AutoDiscovery", config.autoDiscovery);
        config.suppressRemoteNpcDisplays = ReadBool(
            L"Compatibility",
            L"SuppressRemoteNpcDisplays",
            config.suppressRemoteNpcDisplays);
        config.localPort = ClampPort(
            ReadUInt(L"Network", L"LocalPort", config.localPort),
            config.localPort);
        config.peerPort = ClampPort(
            ReadUInt(L"Network", L"PeerPort", config.peerPort),
            config.peerPort);
        config.captureIntervalMs = std::clamp(
            ReadUInt(L"Timing", L"CaptureIntervalMs", config.captureIntervalMs),
            250U,
            30000U);
        config.resendIntervalMs = std::clamp(
            ReadUInt(L"Timing", L"ResendIntervalMs", config.resendIntervalMs),
            1000U,
            60000U);
        config.discoveryIntervalMs = std::clamp(
            ReadUInt(L"Network", L"DiscoveryIntervalMs", config.discoveryIntervalMs),
            500U,
            30000U);
        config.peerTimeoutMs = std::clamp(
            ReadUInt(L"Network", L"PeerTimeoutMs", config.peerTimeoutMs),
            2000U,
            120000U);
        config.peerHost = ReadString(L"Network", L"PeerHost", L"127.0.0.1");
        return config;
    }
}
