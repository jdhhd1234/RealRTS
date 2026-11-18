#pragma once

#include "mapdata/mapdata.hpp"

namespace sim {

enum class Arm;
struct Battalions;

inline constexpr float DEFEND_M_PER_BN = 3'000.0f;
inline constexpr float ATTACK_M_PER_BN = 1'500.0f;
inline constexpr float MARCH_FRONTAGE_M = 1'000.0f;
inline constexpr float MIN_FRONTAGE_M = 1'000.0f;
inline constexpr float DEFEND_DEPTH_M = 8'000.0f;
inline constexpr float ATTACK_DEPTH_M = 5'000.0f;
inline constexpr float MARCH_M_PER_BN = 1'500.0f;
inline constexpr float RESERVE_FIRE = 0.3f;
inline constexpr float MARCH_FIRE = 0.5f;
inline constexpr std::array<float, 3> LOSS_SHARE{0.7f, 0.15f, 0.15f};
inline constexpr float FLANK_COS = 0.5f;
inline constexpr float TURN_PER_STEP_RAD = 0.044f;

enum class Posture { Defend, Attack, March };
enum class Role { Line, Reserve, Support };
constexpr std::size_t index(Role role) { return static_cast<std::size_t>(role); }

struct Deployment {
    std::pair<float, float> facing{1.0f, 0.0f};
    Posture posture = Posture::Defend;
    bool operator==(const Deployment&) const = default;
};

struct Layout {
    float frontage_m = 0.0f;
    float depth_m = 0.0f;
    std::array<float, 3> strength{};
    float line_fire = 0.0f;
    float depth_fire = 0.0f;
    bool operator==(const Layout&) const = default;
};

Role role(Arm arm, Posture posture);
Layout layout(const Battalions& b, Posture posture);
std::pair<mapdata::Pos, mapdata::Pos> line_ends(mapdata::Pos pos, std::pair<float, float> facing, float frontage_m);
float point_segment_dist(mapdata::Pos p, mapdata::Pos a, mapdata::Pos b);
float segment_dist(std::pair<mapdata::Pos, mapdata::Pos> a, std::pair<mapdata::Pos, mapdata::Pos> b);
std::optional<std::pair<float, float>> unit(float x, float y);
std::pair<float, float> turn_toward(std::pair<float, float> from, std::pair<float, float> to, float max_rad);

}  // namespace sim
