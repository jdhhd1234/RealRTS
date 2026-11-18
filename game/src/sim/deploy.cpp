#include "sim/deploy.hpp"

#include "sim/combat.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sim {

using mapdata::Pos;

Role role(Arm arm, Posture posture) {
    if (arm == Arm::Artillery) return Role::Support;
    if (arm == Arm::Armor && posture == Posture::Defend) return Role::Reserve;
    return Role::Line;
}

Layout layout(const Battalions& b, Posture posture) {
    std::array<float, 3> strength{};
    std::array<float, 3> fire{};
    for (const auto& bat : b.value) {
        const auto r = index(role(bat.arm, posture));
        strength[r] += bat.strength;
        fire[r] += arm_firepower(bat.arm) * bat.strength;
    }

    const float line_bn = strength[index(Role::Line)];
    float frontage_m = MARCH_FRONTAGE_M;
    float depth_m = b.total() * MARCH_M_PER_BN;
    switch (posture) {
    case Posture::Defend:
        frontage_m = line_bn * DEFEND_M_PER_BN;
        depth_m = DEFEND_DEPTH_M;
        break;
    case Posture::Attack:
        frontage_m = line_bn * ATTACK_M_PER_BN;
        depth_m = ATTACK_DEPTH_M;
        break;
    case Posture::March:
        break;
    }
    const float march = posture == Posture::March ? MARCH_FIRE : 1.0f;
    return {std::max(frontage_m, MIN_FRONTAGE_M), depth_m, strength,
            fire[0] * march, (fire[1] * RESERVE_FIRE + fire[2]) * march};
}

std::pair<Pos, Pos> line_ends(Pos pos, std::pair<float, float> facing, float frontage_m) {
    const float hx = -facing.second * frontage_m / 2.0f;
    const float hy = facing.first * frontage_m / 2.0f;
    return {{pos.x - hx, pos.y - hy}, {pos.x + hx, pos.y + hy}};
}

float point_segment_dist(Pos p, Pos a, Pos b) {
    const float abx = b.x - a.x;
    const float aby = b.y - a.y;
    const float len2 = abx * abx + aby * aby;
    const float t = len2 > 0.0f ? std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / len2, 0.0f, 1.0f) : 0.0f;
    const float dx = a.x + t * abx - p.x;
    const float dy = a.y + t * aby - p.y;
    return std::sqrt(dx * dx + dy * dy);
}

float segment_dist(std::pair<Pos, Pos> a, std::pair<Pos, Pos> b) {
    const auto cross = [](Pos o, Pos p, Pos q) {
        return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x);
    };
    const auto [a0, a1] = a;
    const auto [b0, b1] = b;
    if (cross(a0, a1, b0) * cross(a0, a1, b1) < 0.0f &&
        cross(b0, b1, a0) * cross(b0, b1, a1) < 0.0f) {
        return 0.0f;
    }
    return std::min({point_segment_dist(a0, b0, b1), point_segment_dist(a1, b0, b1),
                     point_segment_dist(b0, a0, a1), point_segment_dist(b1, a0, a1)});
}

std::optional<std::pair<float, float>> unit(float x, float y) {
    const float len = std::sqrt(x * x + y * y);
    if (len > 1e-3f) return std::pair{x / len, y / len};
    return std::nullopt;
}

std::pair<float, float> turn_toward(std::pair<float, float> from, std::pair<float, float> to, float max_rad) {
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float tau = 2.0f * pi;
    const float a = std::atan2(from.second, from.first);
    const float d = std::atan2(to.second, to.first) - a;
    const float wrapped = std::fmod(d + pi, tau);
    const float angle = a + std::clamp((wrapped < 0.0f ? wrapped + tau : wrapped) - pi, -max_rad, max_rad);
    return {std::cos(angle), std::sin(angle)};
}

}  // namespace sim
