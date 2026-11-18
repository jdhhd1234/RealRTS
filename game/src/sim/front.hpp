#pragma once

#include <entt/entity/fwd.hpp>

#include <optional>
#include <utility>

namespace sim {

inline constexpr float ADVANCE_RATIO = 1.5f;
inline constexpr float RETREAT_RATIO = 0.67f;
inline constexpr float ORG_BREAK = 0.25f;
inline constexpr float ORG_RALLY = 0.5f;
inline constexpr float ORG_RECOVER_PER_DAY = 0.2f;
inline constexpr float ORG_PER_BN_LOST = 0.8f;
inline constexpr float RETREAT_ORG_FACTOR = 1.5f;
inline constexpr float RETREAT_M_PER_TICK = 200.0f;
inline constexpr float RETREAT_FIRE = 0.5f;
inline constexpr float HOLD_LOSS = 1.3f;
inline constexpr float POCKET_LOSS = 1.5f;

struct Front {
    float org = 1.0f;
    std::optional<float> ratio;
    float speed = 1.0f;
    std::optional<std::pair<float, float>> retreat;
    bool hold = false;
    bool encircled = false;

    float fire_factor() const;
    bool operator==(const Front&) const = default;
};

float advance_speed(float r);
void retreat_units(entt::registry& world);
void detect_pockets(entt::registry& world);

}  // namespace sim
