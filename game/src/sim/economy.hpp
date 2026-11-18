#pragma once

#include "sim/supply.hpp"

namespace sim {

struct EventLog;

inline constexpr std::size_t EQUIPMENT = 3;
inline constexpr float CONTESTED_OUTPUT = 0.5f;
inline constexpr float REQUISITION_SHARE = 0.5f;
inline constexpr float REQUISITION_SUPPORT_LOSS = 0.02f;
inline constexpr float EQUIPMENT_PER_BN = 50.0f;
inline constexpr float REPLACE_PER_DAY = 0.05f;
inline constexpr std::uint64_t BUILD_DAYS = 14;
inline constexpr float DIVISION_EQUIPMENT = 10.0f * EQUIPMENT_PER_BN;
inline constexpr std::uint32_t BUILT_ID_BASE = 10'000;

struct Nations {
    std::vector<float> equipment;
    float equipment_produced = 0.0f;
    float equipment_used = 0.0f;
    Nations();
};

struct Build {
    std::uint8_t side = 0;
    std::string name;
    std::uint64_t ready_tick = 0;
    Goods stocks{};
};

struct EconomyState {
    std::vector<bool> requisition;
    std::vector<Build> builds;
    std::uint32_t next_unit_id = BUILT_ID_BASE;

    static EconomyState make(std::size_t regions);
};

bool order_division(std::uint8_t side, std::string name, std::uint64_t tick, SupplyBases& bases,
                    Nations& nations, EconomyState& econ, EventLog& log);
void economy_step(entt::registry& world);

}  // namespace sim
