#pragma once

#include "sim/combat.hpp"
#include "sim/deploy.hpp"
#include "sim/economy.hpp"
#include "sim/front.hpp"
#include "sim/partisan.hpp"
#include "sim/path.hpp"
#include "sim/supply.hpp"
#include "sim/territory.hpp"
#include "sim/units.hpp"

#include <entt/entity/registry.hpp>

namespace sim {

inline constexpr std::uint64_t TICKS_PER_HOUR = 60;
inline constexpr std::uint64_t TICKS_PER_DAY = 24 * TICKS_PER_HOUR;

namespace cadence {
inline constexpr std::uint64_t COMMANDS = 1;
inline constexpr std::uint64_t MOVEMENT = 1;
inline constexpr std::uint64_t COMBAT = 5;
inline constexpr std::uint64_t SUPPLY = TICKS_PER_HOUR;
inline constexpr std::uint64_t PARTISAN = 6 * TICKS_PER_HOUR;
inline constexpr std::uint64_t ECONOMY = TICKS_PER_DAY;
}  // namespace cadence

struct SimClock {
    std::uint64_t tick = 0;

    std::uint64_t day() const;
    std::uint64_t hour() const;
    std::uint64_t minute() const;
    bool operator==(const SimClock&) const = default;
};

enum class SimSet { Clock, Commands, Movement, Combat, Supply, Partisan, Economy, Cleanup, Events };

struct Schedule {
    using System = void (*)(entt::registry&);

    struct Stage {
        SimSet set;
        std::uint64_t interval;
        std::vector<System> systems;
    };

    std::vector<Stage> stages;
};

inline auto every(std::uint64_t n) {
    return [n](const SimClock& clock) { return clock.tick % n == 0; };
}

inline constexpr float SCENARIO_BASE_STOCK = 20'000.0f;

void advance_clock(entt::registry& world);
void configure(Schedule& schedule);
void step(entt::registry& world, const Schedule& schedule);
void step(entt::registry& world);
void init_world(entt::registry& world, mapdata::MapData map);
void load_scenario(entt::registry& world, mapdata::Baked baked);

}  // namespace sim
