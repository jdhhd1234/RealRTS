#include "sim/sim.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>

namespace sim {

Territory Territory::from_legal(const mapdata::MapData& map, std::vector<std::uint8_t> legal) {
    if (legal.size() != map.regions.size() || std::ranges::any_of(legal, [](auto s) { return s >= MAX_SIDES; })) {
        throw std::runtime_error("Expected one valid legal owner per region");
    }
    std::vector<std::uint8_t> military;
    military.reserve(map.region_cells.size());
    for (const auto region : map.region_cells) military.push_back(legal[region.value]);
    return {std::move(legal), std::move(military)};
}

std::optional<std::uint8_t> Territory::military_at(const mapdata::MapData& map, mapdata::Pos pos) const {
    if (const auto c = map.terrain.cell_at(pos)) {
        return military[static_cast<std::size_t>(c->y) * map.terrain.width + c->x];
    }
    return std::nullopt;
}

std::vector<RegionStatus> region_status(const mapdata::MapData& map, const Territory& t) {
    std::vector<std::array<std::uint32_t, MAX_SIDES>> counts(map.regions.size());
    for (std::size_t cell = 0; cell < map.region_cells.size(); ++cell) {
        ++counts[map.region_cells[cell].value][t.military[cell]];
    }
    std::vector<RegionStatus> status;
    status.reserve(counts.size());
    for (std::size_t r = 0; r < counts.size(); ++r) {
        const auto& c = counts[r];
        const auto total = std::accumulate(c.begin(), c.end(), 0u);
        const auto strongest = std::max_element(c.begin(), c.end());
        const auto side = static_cast<std::uint8_t>(strongest - c.begin());
        if (total == 0 || static_cast<float>(*strongest) < REGION_CONTROL_SHARE * static_cast<float>(total)) {
            status.push_back({RegionStatus::Contested{}});
        } else if (side == t.legal[r]) {
            status.push_back({RegionStatus::Home{}});
        } else {
            status.push_back({RegionStatus::Occupied{side}});
        }
    }
    return status;
}

void update_military(entt::registry& world) {
    const auto& grid = world.ctx().get<Map>().value.terrain;
    auto& territory = world.ctx().get<Territory>();
    std::vector<std::uint8_t> touched(grid.cells.size());
    const auto units = world.view<const Side, const Position, const UnitId>(entt::exclude<Partisan>);
    for (const auto [entity, side, pos, id] : units.each()) {
        if (side.value >= MAX_SIDES) continue;
        const auto cell = grid.cell_at(pos.value);
        if (!cell) continue;

        const auto* dep = world.try_get<Deployment>(entity);
        const auto* bats = world.try_get<Battalions>(entity);
        const float frontage = dep && bats ? layout(*bats, dep->posture).frontage_m : 0.0f;
        const auto [a, b] = line_ends(pos.value, dep ? dep->facing : std::pair{1.0f, 0.0f}, frontage);
        const auto radius = static_cast<std::int64_t>(std::ceil((CONTROL_RADIUS_M + frontage / 2.0f) / grid.cell_m));
        const auto cx = static_cast<std::int64_t>(cell->x);
        const auto cy = static_cast<std::int64_t>(cell->y);
        const auto x_end = std::min(cx + radius, static_cast<std::int64_t>(grid.width) - 1);
        const auto y_end = std::min(cy + radius, static_cast<std::int64_t>(grid.height) - 1);
        for (auto y = std::max<std::int64_t>(cy - radius, 0); y <= y_end; ++y) {
            for (auto x = std::max<std::int64_t>(cx - radius, 0); x <= x_end; ++x) {
                const mapdata::Pos p{(static_cast<float>(x) + 0.5f) * grid.cell_m,
                                     (static_cast<float>(y) + 0.5f) * grid.cell_m};
                if (point_segment_dist(p, a, b) <= CONTROL_RADIUS_M) {
                    touched[static_cast<std::size_t>(y) * grid.width + static_cast<std::size_t>(x)] |=
                        static_cast<std::uint8_t>(1u << side.value);
                }
            }
        }
    }
    // QUESTION: Does the app need an explicit territory revision counter for overlay invalidation?
    for (std::size_t cell = 0; cell < touched.size(); ++cell) {
        if (std::has_single_bit(touched[cell])) {
            const auto side = static_cast<std::uint8_t>(std::countr_zero(touched[cell]));
            if (territory.military[cell] != side) territory.military[cell] = side;
        }
    }
}

void capture_depots(entt::registry& world) {
    const auto& map = world.ctx().get<Map>().value;
    const auto& territory = world.ctx().get<Territory>();
    const auto depots = world.view<Depot>();
    for (auto [entity, d] : depots.each()) {
        if (const auto owner = territory.military_at(map, map.graph.node(d.node).pos)) d.side = *owner;
    }
}

}  // namespace sim
