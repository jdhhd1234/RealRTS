#pragma once

#include "sim/combat.hpp"
#include "sim/front.hpp"
#include "sim/path.hpp"
#include "sim/supply.hpp"

#include <entt/entity/fwd.hpp>

#include <string_view>
#include <tuple>
#include <variant>

namespace sim {

struct Map { mapdata::MapData value; };
struct UnitId {
    std::uint32_t value = 0;
    bool operator==(const UnitId&) const = default;
};
struct Side {
    std::uint8_t value = 0;
    bool operator==(const Side&) const = default;
};
struct UnitName { std::string value; };
struct Position {
    mapdata::Pos value;
    bool operator==(const Position&) const = default;
};

struct MoveOrder {
    std::vector<Cell> path;
    std::size_t next = 0;
};

inline constexpr float DIVISION_SPEED_M_PER_TICK = 20'000.0f / 60.0f;

using Division = std::tuple<Battalions, UnitId, Side, UnitName, Position, Stocks,
                            StockTarget, SupplyPriority, Front, Deployment>;
Division division(std::uint32_t id, std::uint8_t side, std::string_view name, mapdata::Pos pos);
entt::entity division(entt::registry& world, std::uint32_t id, std::uint8_t side,
                      std::string_view name, mapdata::Pos pos);

struct SimCommand {
    struct Move {
        UnitId unit;
        mapdata::Pos goal;
        bool operator==(const Move&) const = default;
    };
    struct SetHold {
        UnitId unit;
        bool hold;
        bool operator==(const SetHold&) const = default;
    };
    struct SetSupplyPriority {
        UnitId unit;
        std::uint8_t priority;
        bool operator==(const SetSupplyPriority&) const = default;
    };
    struct AntiPartisan {
        mapdata::RegionId region;
        bool harsh;
        bool operator==(const AntiPartisan&) const = default;
    };
    struct SetRequisition {
        mapdata::RegionId region;
        bool on;
        bool operator==(const SetRequisition&) const = default;
    };
    struct BuildDivision {
        std::uint8_t side;
        std::string name;
        bool operator==(const BuildDivision&) const = default;
    };
    // New front line for a corps; fewer than 2 points removes its front.
    struct SetFront {
        std::uint32_t corps;
        std::vector<mapdata::Pos> points;
        bool operator==(const SetFront&) const = default;
    };
    struct SetFrontAttack {
        std::uint32_t corps;
        bool attack;
        bool operator==(const SetFrontAttack&) const = default;
    };

    std::variant<Move, SetHold, SetSupplyPriority, AntiPartisan, SetRequisition, BuildDivision, SetFront,
                 SetFrontAttack>
        value;
    bool operator==(const SimCommand&) const = default;
};

struct CommandQueue { std::vector<SimCommand> value; };

void apply_commands(entt::registry& world);
void move_units(entt::registry& world);

}  // namespace sim
