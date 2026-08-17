#pragma once

#include "PCH.h"

namespace IEDSyncTogether
{
    struct PlacementTransform
    {
        std::string node;
        std::array<float, 3> position{};
        std::array<float, 3> rotation{};
        float scale{ 1.0f };

        bool operator==(const PlacementTransform&) const = default;
        auto operator<=>(const PlacementTransform&) const = default;
    };

    struct FormIdentity
    {
        std::string plugin;
        std::uint32_t localFormID{ 0 };
        std::optional<PlacementTransform> placement;

        bool operator==(const FormIdentity&) const = default;
        auto operator<=>(const FormIdentity&) const = default;
    };

    using SlotState = std::array<std::optional<FormIdentity>, 19>;

    std::optional<FormIdentity> MakeFormIdentity(RE::TESForm* form);
    RE::TESForm* ResolveFormIdentity(const FormIdentity& identity);

    std::string HexEncode(std::string_view value);
    std::optional<std::string> HexDecode(std::string_view value);
    std::string EncodeSlots(const SlotState& slots);
    std::optional<SlotState> DecodeSlots(std::string_view value);
}
