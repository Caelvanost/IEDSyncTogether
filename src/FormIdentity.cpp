#include "PCH.h"
#include "FormIdentity.h"

#include <cmath>

namespace IEDSyncTogether
{
    namespace
    {
        int HexValue(char ch)
        {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
            if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
            return -1;
        }

        std::vector<std::string_view> Split(std::string_view value, char separator)
        {
            std::vector<std::string_view> result;
            std::size_t start = 0;
            while (true) {
                const auto end = value.find(separator, start);
                result.push_back(value.substr(
                    start,
                    end == std::string_view::npos ? value.size() - start : end - start));
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
            return result;
        }

        std::optional<float> ParseFloat(std::string_view value)
        {
            try {
                std::size_t consumed = 0;
                const auto result = std::stof(std::string(value), &consumed);
                if (consumed != value.size() || !std::isfinite(result)) return std::nullopt;
                return result;
            } catch (...) {
                return std::nullopt;
            }
        }
    }

    std::optional<FormIdentity> MakeFormIdentity(RE::TESForm* form)
    {
        if (!form || form->GetFormID() == 0 || form->GetFormID() >= 0xFF000000) return std::nullopt;
        auto* file = form->GetFile(0);
        if (!file) return std::nullopt;
        const auto filename = file->GetFilename();
        if (filename.empty()) return std::nullopt;
        return FormIdentity{ std::string(filename), form->GetLocalFormID(), std::nullopt };
    }

    RE::TESForm* ResolveFormIdentity(const FormIdentity& identity)
    {
        if (identity.plugin.empty() || identity.localFormID == 0) return nullptr;
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        return dataHandler ? dataHandler->LookupForm(identity.localFormID, identity.plugin) : nullptr;
    }

    std::string HexEncode(std::string_view value)
    {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(value.size() * 2);
        for (const unsigned char ch : value) {
            encoded.push_back(digits[(ch >> 4) & 0x0F]);
            encoded.push_back(digits[ch & 0x0F]);
        }
        return encoded;
    }

    std::optional<std::string> HexDecode(std::string_view value)
    {
        if ((value.size() % 2) != 0) return std::nullopt;
        std::string decoded;
        decoded.reserve(value.size() / 2);
        for (std::size_t i = 0; i < value.size(); i += 2) {
            const int high = HexValue(value[i]);
            const int low = HexValue(value[i + 1]);
            if (high < 0 || low < 0) return std::nullopt;
            decoded.push_back(static_cast<char>((high << 4) | low));
        }
        return decoded;
    }

    std::string EncodeSlots(const SlotState& slots)
    {
        std::string encoded;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (i != 0) encoded.push_back(',');
            if (!slots[i]) {
                encoded.push_back('-');
                continue;
            }

            encoded += HexEncode(slots[i]->plugin);
            encoded.push_back(':');
            encoded += fmt::format("{:X}", slots[i]->localFormID);

            if (const auto& placement = slots[i]->placement; placement && !placement->node.empty()) {
                encoded += ":P:";
                encoded += HexEncode(placement->node);
                encoded += fmt::format(
                    ":{:.4f}:{:.4f}:{:.4f}:{:.4f}:{:.4f}:{:.4f}:{:.4f}",
                    placement->position[0], placement->position[1], placement->position[2],
                    placement->rotation[0], placement->rotation[1], placement->rotation[2],
                    placement->scale);
            }
        }
        return encoded;
    }

    std::optional<SlotState> DecodeSlots(std::string_view value)
    {
        SlotState slots{};
        std::size_t start = 0;
        for (std::size_t index = 0; index < slots.size(); ++index) {
            const auto end = value.find(',', start);
            const auto token = value.substr(
                start,
                end == std::string_view::npos ? value.size() - start : end - start);
            if (token.empty()) return std::nullopt;

            if (token != "-") {
                const auto parts = Split(token, ':');
                if (parts.size() != 2 && parts.size() != 11) return std::nullopt;

                auto plugin = HexDecode(parts[0]);
                if (!plugin || plugin->empty()) return std::nullopt;

                std::uint32_t localID = 0;
                try {
                    std::size_t consumed = 0;
                    const auto parsed = std::stoul(std::string(parts[1]), &consumed, 16);
                    if (consumed != parts[1].size() || parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
                    localID = static_cast<std::uint32_t>(parsed);
                } catch (...) {
                    return std::nullopt;
                }

                FormIdentity identity{ *plugin, localID, std::nullopt };
                if (parts.size() == 11) {
                    if (parts[2] != "P") return std::nullopt;
                    auto node = HexDecode(parts[3]);
                    if (!node || node->empty()) return std::nullopt;

                    PlacementTransform placement;
                    placement.node = std::move(*node);
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const auto component = ParseFloat(parts[4 + axis]);
                        if (!component) return std::nullopt;
                        placement.position[axis] = *component;
                    }
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        const auto component = ParseFloat(parts[7 + axis]);
                        if (!component) return std::nullopt;
                        placement.rotation[axis] = *component;
                    }
                    const auto scale = ParseFloat(parts[10]);
                    if (!scale || *scale < 0.01f || *scale > 100.0f) return std::nullopt;
                    placement.scale = *scale;
                    identity.placement = std::move(placement);
                }
                slots[index] = std::move(identity);
            }

            if (index + 1 == slots.size()) {
                if (end != std::string_view::npos) return std::nullopt;
            } else {
                if (end == std::string_view::npos) return std::nullopt;
                start = end + 1;
            }
        }
        return slots;
    }
}
