#pragma once

#include "sim/deploy.hpp"

#include <entt/entity/fwd.hpp>

namespace sim {

enum class Arm { Infantry, Armor, Artillery };

struct Battalion {
    Arm arm = Arm::Infantry;
    float strength = 0.0f;
    bool operator==(const Battalion&) const = default;
};

struct Battalions {
    std::vector<Battalion> value;
    static Battalions standard();
    float total() const;
    float firepower() const;
    void take_loss(float loss, Posture posture);
    bool operator==(const Battalions&) const = default;
};

inline constexpr float CONTACT_RANGE_M = 15'000.0f;
inline constexpr float FLANK_DEFENSE = 0.5f;
inline constexpr float COMBAT_K = 2.5e-4f;
inline constexpr float AMMO_PER_STEP_T = 600.0f / 288.0f;
inline constexpr float NO_AMMO_FACTOR = 0.1f;
inline constexpr float MIN_BATTALION_STRENGTH = 0.05f;

struct Engagements { std::vector<std::pair<entt::entity, entt::entity>> value; };

float arm_firepower(Arm arm);
float terrain_defense(mapdata::Terrain t);
float posture_defense(Posture p);
float ammo_factor(float ammo);
float food_factor(float food);
void resolve_combat(entt::registry& world);
void remove_destroyed(entt::registry& world);

}  // namespace sim
