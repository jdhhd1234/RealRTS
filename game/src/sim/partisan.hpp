#pragma once

#include "mapdata/mapdata.hpp"

#include <entt/entity/fwd.hpp>

namespace sim {

struct SimRng {
    std::uint64_t value = 0x5EED'2026;
    std::uint64_t next_u64();
    float next_f32();
};

struct Partisan { mapdata::RegionId region; };

inline constexpr float INITIAL_SUPPORT = 0.3f;
inline constexpr std::uint32_t PARTISAN_ID_BASE = 100'000;
inline constexpr float RECRUIT_PER_STEP = 0.15f;
inline constexpr float SUPPRESS_PER_BN = 0.05f;
inline constexpr float SABOTAGE_DAMAGE = 0.3f;
inline constexpr float RAID_LOSS = 0.3f;
inline constexpr float GARRISON_ATTACK_LOSS = 0.2f;
inline constexpr float REPAIR_PER_STEP = 0.05f;
inline constexpr float SPOT_THRESHOLD = 1.0f;
inline constexpr float BAND_MAX = 2.0f;

struct PartisanState {
    std::vector<float> pool;
    std::vector<float> support;
    std::uint32_t next_unit_id = PARTISAN_ID_BASE;

    static PartisanState make(std::size_t regions);
};

struct EventLog {
    std::vector<std::pair<std::uint64_t, std::string>> value;
    void push(std::uint64_t tick, std::string text);
};

enum class RiskGrade { None, Low, Medium, High };

RiskGrade risk_grade(float pool);
void anti_partisan(PartisanState& state, EventLog& log, const mapdata::MapData& map,
                   std::uint64_t tick, mapdata::RegionId region, bool harsh);
void partisan_step(entt::registry& world);

}  // namespace sim
