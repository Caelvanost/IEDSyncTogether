#include "PCH.h"
#include "IEDDiagnostics.h"

#include "SyncService.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace IEDSyncTogether
{
    namespace
    {
        constexpr std::string_view kIEDSettingsPath = "Data\\SKSE\\Plugins\\IED\\Settings.json";
        constexpr std::string_view kDisableNpcSlotsKey = "disable_npc_slots";
        constexpr auto kDiagnosticInterval = std::chrono::milliseconds(750);

        std::optional<bool> ParseBooleanSetting(
            std::string_view document,
            std::string_view key)
        {
            const auto quotedKey = fmt::format("\"{}\"", key);
            auto position = document.find(quotedKey);
            if (position == std::string_view::npos) {
                return std::nullopt;
            }

            position = document.find(':', position + quotedKey.size());
            if (position == std::string_view::npos) {
                return std::nullopt;
            }
            ++position;

            while (position < document.size() &&
                   std::isspace(static_cast<unsigned char>(document[position]))) {
                ++position;
            }

            if (document.substr(position).starts_with("true")) {
                return true;
            }
            if (document.substr(position).starts_with("false")) {
                return false;
            }
            return std::nullopt;
        }
    }

    IEDDiagnostics& IEDDiagnostics::GetSingleton()
    {
        static IEDDiagnostics instance;
        return instance;
    }

    IEDDiagnostics::~IEDDiagnostics()
    {
        Stop();
    }

    void IEDDiagnostics::Start()
    {
        if (_running.exchange(true)) {
            return;
        }

        LogIEDSettings();
        _timer = std::jthread([this](std::stop_token token) { TimerLoop(token); });
        SKSE::log::info(
            "IED v0.4.0 diagnostics started: read-only IED settings + STR proxy PlayerCharacter classification");
    }

    void IEDDiagnostics::Stop()
    {
        if (!_running.exchange(false)) {
            return;
        }

        if (_timer.joinable()) {
            _timer.request_stop();
            _timer.join();
        }
        _seenProxies.clear();
    }

    void IEDDiagnostics::Reset()
    {
        _seenProxies.clear();
    }

    void IEDDiagnostics::TimerLoop(std::stop_token token)
    {
        while (!token.stop_requested() && _running.load()) {
            if (auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([]() { IEDDiagnostics::GetSingleton().Tick(); });
            }

            auto elapsed = std::chrono::milliseconds(0);
            while (elapsed < kDiagnosticInterval && !token.stop_requested() && _running.load()) {
                constexpr auto slice = std::chrono::milliseconds(100);
                std::this_thread::sleep_for(slice);
                elapsed += slice;
            }
        }
    }

    void IEDDiagnostics::Tick()
    {
        const auto proxies = SyncService::GetSingleton().GetResolvedRemoteProxies();
        std::unordered_set<RE::FormID> current;
        current.reserve(proxies.size());

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* playerActor = static_cast<RE::Actor*>(player);
        const RE::FormID playerFormID = player ? player->GetFormID() : 0;

        for (const auto proxyFormID : proxies) {
            if (proxyFormID == 0) {
                continue;
            }
            current.insert(proxyFormID);

            if (!_seenProxies.insert(proxyFormID).second) {
                continue;
            }

            auto* form = RE::TESForm::LookupByID(proxyFormID);
            auto* proxy = form ? form->As<RE::Actor>() : nullptr;
            if (!proxy) {
                SKSE::log::warn(
                    "IED proxy classification diagnostic: proxy={:08X} could not be resolved to Actor*",
                    proxyFormID);
                continue;
            }

            const bool samePointer = playerActor && proxy == playerActor;
            const bool sameFormID = player && proxyFormID == playerFormID;

            SKSE::log::info(
                "IED proxy classification diagnostic: proxy={:08X} player={:08X} proxyIsPlayerPointer={} proxyIsPlayerFormID={}",
                proxyFormID,
                playerFormID,
                samePointer ? 1 : 0,
                sameFormID ? 1 : 0);
        }

        for (auto iterator = _seenProxies.begin(); iterator != _seenProxies.end();) {
            if (current.contains(*iterator)) {
                ++iterator;
            } else {
                iterator = _seenProxies.erase(iterator);
            }
        }
    }

    void IEDDiagnostics::LogIEDSettings() const
    {
        const std::filesystem::path path(kIEDSettingsPath);
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            SKSE::log::warn(
                "IED settings diagnostic: could not open \"{}\"; disable_npc_slots is unknown",
                path.string());
            return;
        }

        const std::string document{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{}
        };

        const auto disableNpcSlots = ParseBooleanSetting(document, kDisableNpcSlotsKey);
        if (!disableNpcSlots) {
            SKSE::log::warn(
                "IED settings diagnostic: \"{}\" does not contain a readable boolean disable_npc_slots value",
                path.string());
            return;
        }

        std::error_code ec;
        const auto absolutePath = std::filesystem::absolute(path, ec);
        const auto displayPath = ec ? path.string() : absolutePath.string();

        SKSE::log::info(
            "IED settings diagnostic: path=\"{}\" disable_npc_slots={} source=IED Settings.json (read-only)",
            displayPath,
            *disableNpcSlots ? "true" : "false");

        if (!*disableNpcSlots) {
            SKSE::log::warn(
                "IED diagnostic finding: disable_npc_slots=false; IED will continue ordinary NPC ProcessSlots rendering");
        }
    }
}
