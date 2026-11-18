#include "sim/sim.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace sim {

float Front::fire_factor() const {
    return (0.3f + 0.7f * org) * (retreat ? RETREAT_FIRE : 1.0f);
}

float advance_speed(float r) {
    return r >= ADVANCE_RATIO ? std::min(0.2f + 0.2f * (r - ADVANCE_RATIO), 0.6f) : 0.0f;
}

void retreat_units(entt::registry& world) {
    const auto& map = world.ctx().get<Map>().value;
    const auto& territory = world.ctx().get<Territory>();
    const auto units = world.view<const Side, Position, const Front>();
    for (auto [entity, side, pos, front] : units.each()) {
        if (!front.retreat) continue;
        const auto [dx, dy] = *front.retreat;
        for (const float deg : {0.0f, 45.0f, -45.0f, 90.0f, -90.0f}) {
            const float rad = deg * (std::numbers::pi_v<float> / 180.0f);
            const float s = std::sin(rad);
            const float c = std::cos(rad);
            const mapdata::Pos next{pos.value.x + (dx * c - dy * s) * RETREAT_M_PER_TICK,
                                    pos.value.y + (dx * s + dy * c) * RETREAT_M_PER_TICK};
            const auto cell = map.terrain.cell_at(next);
            if (cell && map.terrain.get(cell->x, cell->y) != mapdata::Terrain::Water &&
                territory.military[static_cast<std::size_t>(cell->y) * map.terrain.width + cell->x] == side.value) {
                pos.value = next;
                break;
            }
        }
    }
}

void detect_pockets(entt::registry& world) {
    const auto& map = world.ctx().get<Map>().value;
    const auto& territory = world.ctx().get<Territory>();
    const auto& bases = world.ctx().get<SupplyBases>().value;
    const auto depots = world.view<const Depot>();
    const auto units = world.view<const Side, const Position, Front>();
    const std::size_t w = map.terrain.width;
    const std::size_t h = map.terrain.height;
    std::array<bool, MAX_SIDES> scanned{};
    std::array<std::vector<bool>, MAX_SIDES> reach;

    const auto fill = [&](std::uint8_t s, std::vector<bool>& seen) {
        std::vector<std::size_t> stack;
        const auto source = [&](mapdata::NodeId node) {
            if (seen.empty()) seen.resize(w * h);
            if (const auto at = map.terrain.cell_at(map.graph.node(node).pos)) {
                const auto c = at->y * w + at->x;
                if (territory.military[c] == s && !seen[c]) {
                    seen[c] = true;
                    stack.push_back(c);
                }
            }
        };
        for (const auto [entity, d] : depots.each()) if (d.side == s) source(d.node);
        for (const auto& b : bases) if (b.side == s) source(b.node);
        while (!stack.empty()) {
            const auto c = stack.back();
            stack.pop_back();
            const auto x = c % w;
            const auto y = c / w;
            const std::array<std::pair<bool, std::size_t>, 4> near{{
                {x > 0, c - 1}, {x + 1 < w, c + 1}, {y > 0, c - w}, {y + 1 < h, c + w},
            }};
            for (const auto& [valid, n] : near) {
                if (valid && !seen[n] && territory.military[n] == s) {
                    seen[n] = true;
                    stack.push_back(n);
                }
            }
        }
    };

    for (auto [entity, side, pos, front] : units.each()) {
        const auto s = side.value;
        if (s >= MAX_SIDES) continue;
        auto& seen = reach[s];
        if (!scanned[s]) {
            scanned[s] = true;
            fill(s, seen);
        }
        const auto cell = map.terrain.cell_at(pos.value);
        front.encircled = !seen.empty() && cell && !seen[cell->y * w + cell->x];
    }
}

}  // namespace sim
