#pragma once
// Corps (XXX) and front lines (planns/scarce.md 3, 4).
// The player draws a front line for a corps. Its divisions spread evenly along the line, facing the
// enemy, then hold it (defend) or push straight ahead (attack). Once every division has reached the line,
// the line follows them: winning pushes it forward, retreats pull it back. Fighting is the usual
// contact combat (combat.cpp); this module only gives move orders and facings.

#include "mapdata/mapdata.hpp"
#include "sim/units.hpp"

#include <entt/entity/fwd.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sim {

inline constexpr float ON_LINE_M = 2'000.0f;       // a division this close to its slot has reached the line
inline constexpr float ATTACK_STEP_M = 5'000.0f;   // attackers move toward a goal this far ahead
inline constexpr float ENEMY_PROBE_M = 10'000.0f;  // how far each side of the line is checked for enemy ground

struct FrontLine {
    std::vector<mapdata::Pos> points;
    bool attack = false;
    bool flip = false;          // enemy on the right of the line direction (default: left)
    bool formed = false;        // every division reached the line once; from then on the line follows them
    std::vector<bool> reached;  // per corps member, same order
    bool operator==(const FrontLine&) const = default;
};

struct Corps {
    std::uint32_t id = 0;
    std::uint8_t side = 0;
    std::string name;
    std::vector<UnitId> members;  // order along the front line
    std::optional<FrontLine> front;
    bool operator==(const Corps&) const = default;
};

struct CorpsList { std::vector<Corps> value; };

// A place on the front line for one division, and the unit normal pointing at the enemy.
struct Slot {
    mapdata::Pos pos;
    std::pair<float, float> normal{0.0f, 1.0f};
    bool operator==(const Slot&) const = default;
};

// Centers of `n` equal pieces of the polyline.
std::vector<Slot> front_slots(const std::vector<mapdata::Pos>& line, std::size_t n, bool flip);
// Gives the corps a new line (empty `points`: no front). Members are re-ordered along it by where they stand.
void set_front(entt::registry& world, Corps& corps, std::vector<mapdata::Pos> points);
Corps* find_corps(entt::registry& world, std::uint32_t id);
void front_lines(entt::registry& world);

}  // namespace sim
