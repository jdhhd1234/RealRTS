#include "sim/path.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <limits>
#include <queue>

namespace sim {

float terrain_cost(mapdata::Terrain t) {
    using enum mapdata::Terrain;
    switch (t) {
    case Plains: return 1.0f;
    case Urban: return 1.5f;
    case Forest: return 2.0f;
    case Marsh: return 3.0f;
    case Mountain: return 4.0f;
    case Water: return 10.0f;
    }
    return 1.0f;
}

namespace {

std::uint64_t int_cost(mapdata::Terrain t) {
    return static_cast<std::uint64_t>(terrain_cost(t) * 10.0f);
}

}  // namespace

std::optional<std::vector<Cell>> find_path(const mapdata::TerrainGrid& grid, Cell start, Cell goal) {
    const std::int64_t w = grid.width;
    const std::int64_t h = grid.height;
    if (start.x >= w || start.y >= h || goal.x >= w || goal.y >= h) {
        return std::nullopt;
    }

    const auto idx = [w](Cell c) { return static_cast<std::size_t>(c.y * w + c.x); };
    const auto cell = [w = static_cast<std::size_t>(w)](std::size_t i) {
        return Cell{static_cast<std::uint32_t>(i % w), static_cast<std::uint32_t>(i / w)};
    };
    // Octile distance in integer plains costs (10 orthogonal, 14 diagonal).
    const auto heur = [goal](Cell c) {
        const auto dx = std::abs(static_cast<std::int64_t>(c.x) - goal.x);
        const auto dy = std::abs(static_cast<std::int64_t>(c.y) - goal.y);
        return static_cast<std::uint64_t>(10 * (dx + dy) - 6 * std::min(dx, dy));
    };

    constexpr auto no_parent = std::numeric_limits<std::size_t>::max();
    std::vector<std::uint64_t> g(grid.cells.size(), std::numeric_limits<std::uint64_t>::max());
    std::vector<std::size_t> came(grid.cells.size(), no_parent);
    using Entry = std::pair<std::uint64_t, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    g[idx(start)] = 0;
    open.emplace(heur(start), idx(start));

    constexpr std::array<std::pair<int, int>, 8> offsets{{
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    }};
    while (!open.empty()) {
        const auto i = open.top().second;
        open.pop();
        const Cell cur = cell(i);
        if (cur == goal) {
            std::vector<Cell> path;
            for (auto c = i; came[c] != no_parent; c = came[c]) {
                path.push_back(cell(c));
            }
            std::reverse(path.begin(), path.end());
            return path;
        }
        for (const auto& [dx, dy] : offsets) {
            const auto nx = static_cast<std::int64_t>(cur.x) + dx;
            const auto ny = static_cast<std::int64_t>(cur.y) + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                continue;
            }
            const Cell next{static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny)};
            const auto step = dx != 0 && dy != 0 ? 14u : 10u;
            const auto cost = g[i] + step * int_cost(grid.get(next.x, next.y)) / 10;
            const auto ni = idx(next);
            if (cost < g[ni]) {
                g[ni] = cost;
                came[ni] = i;
                open.emplace(cost + heur(next), ni);
            }
        }
    }
    return std::nullopt;
}

}  // namespace sim
