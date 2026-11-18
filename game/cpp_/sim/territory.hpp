#pragma once

#include "mapdata/mapdata.hpp"

#include <entt/entity/fwd.hpp>

#include <variant>

namespace sim {

inline constexpr float CONTROL_RADIUS_M = 10'000.0f;
inline constexpr float REGION_CONTROL_SHARE = 0.8f;
inline constexpr std::uint8_t MAX_SIDES = 8;

struct Territory {
    std::vector<std::uint8_t> legal;
    std::vector<std::uint8_t> military;

    static Territory from_legal(const mapdata::MapData& map, std::vector<std::uint8_t> legal);
    std::optional<std::uint8_t> military_at(const mapdata::MapData& map, mapdata::Pos pos) const;
};

struct RegionStatus {
    struct Home { bool operator==(const Home&) const = default; };
    struct Occupied {
        std::uint8_t by;
        bool operator==(const Occupied&) const = default;
    };
    struct Contested { bool operator==(const Contested&) const = default; };

    std::variant<Home, Occupied, Contested> value;
    bool operator==(const RegionStatus&) const = default;
};

std::vector<RegionStatus> region_status(const mapdata::MapData& map, const Territory& t);
void update_military(entt::registry& world);
void capture_depots(entt::registry& world);

}  // namespace sim
