#include "sim/sim.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim {

using mapdata::Pos;

namespace {

float dist(Pos a, Pos b) { return std::hypot(a.x - b.x, a.y - b.y); }

// Left-hand normal of a direction, or the right-hand one when flipped.
std::pair<float, float> normal_of(float dx, float dy, bool flip) {
    const auto n = unit(-dy, dx).value_or(std::pair{0.0f, 1.0f});
    return flip ? std::pair{-n.first, -n.second} : n;
}

// Arc length along the line of the point on it closest to `p`.
float arc_param(const std::vector<Pos>& line, Pos p) {
    float best_d = std::numeric_limits<float>::max(), best_s = 0.0f, s = 0.0f;
    for (std::size_t i = 1; i < line.size(); ++i) {
        const Pos a = line[i - 1], b = line[i];
        const float len = dist(a, b);
        float t = 0.0f;
        if (len > 0.0f) t = std::clamp(((p.x - a.x) * (b.x - a.x) + (p.y - a.y) * (b.y - a.y)) / (len * len), 0.0f, 1.0f);
        const float d = dist(p, {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t});
        if (d < best_d) {
            best_d = d;
            best_s = s + len * t;
        }
        s += len;
    }
    return best_s;
}

// Polyline through the divisions, extended by half a gap at each end so the line keeps its length.
std::vector<Pos> line_through(const std::vector<Pos>& p) {
    std::vector<Pos> line{{p[0].x + (p[0].x - p[1].x) * 0.5f, p[0].y + (p[0].y - p[1].y) * 0.5f}};
    line.insert(line.end(), p.begin(), p.end());
    const Pos a = p[p.size() - 2], b = p.back();
    line.push_back({b.x + (b.x - a.x) * 0.5f, b.y + (b.y - a.y) * 0.5f});
    return line;
}

entt::entity find_unit(entt::registry& world, UnitId id) {
    for (auto [e, uid] : world.view<const UnitId>().each()) {
        if (uid == id) return e;
    }
    return entt::null;
}

// Same as a player Move order. False if there is no path.
bool order_move(entt::registry& world, entt::entity e, Pos goal) {
    const auto& grid = world.ctx().get<Map>().value.terrain;
    const auto [w_m, h_m] = grid.size_m();
    const float edge = grid.cell_m * 0.5f;
    goal = {std::clamp(goal.x, edge, w_m - edge), std::clamp(goal.y, edge, h_m - edge)};
    const auto start = grid.cell_at(world.get<Position>(e).value);
    const auto end = grid.cell_at(goal);
    if (!start || !end) return false;
    auto path = find_path(grid, *start, *end);
    if (!path) return false;
    world.emplace_or_replace<MoveOrder>(e, std::move(*path), 0u);
    return true;
}

}  // namespace

std::vector<Slot> front_slots(const std::vector<Pos>& line, std::size_t n, bool flip) {
    std::vector<Slot> slots;
    if (line.empty() || n == 0) return slots;
    float total = 0.0f;
    for (std::size_t i = 1; i < line.size(); ++i) total += dist(line[i - 1], line[i]);
    std::size_t seg = 1;
    float seg_start = 0.0f;
    for (std::size_t k = 0; k < n; ++k) {
        const float want = total * (static_cast<float>(k) + 0.5f) / static_cast<float>(n);
        while (seg + 1 < line.size() && seg_start + dist(line[seg - 1], line[seg]) < want) {
            seg_start += dist(line[seg - 1], line[seg]);
            ++seg;
        }
        if (line.size() < 2) {
            slots.push_back({line[0], normal_of(1.0f, 0.0f, flip)});
            continue;
        }
        const Pos a = line[seg - 1], b = line[seg];
        const float len = dist(a, b);
        const float t = len > 0.0f ? std::clamp((want - seg_start) / len, 0.0f, 1.0f) : 0.0f;
        slots.push_back({{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}, normal_of(b.x - a.x, b.y - a.y, flip)});
    }
    return slots;
}

Corps* find_corps(entt::registry& world, std::uint32_t id) {
    auto& list = world.ctx().get<CorpsList>().value;
    const auto it = std::ranges::find(list, id, &Corps::id);
    return it == list.end() ? nullptr : &*it;
}

void set_front(entt::registry& world, Corps& corps, std::vector<Pos> points) {
    std::erase_if(points, [](Pos p) { return !std::isfinite(p.x) || !std::isfinite(p.y); });
    for (const UnitId id : corps.members) {
        if (const auto e = find_unit(world, id); e != entt::null) world.remove<MoveOrder>(e);
    }
    if (points.size() < 2) {
        corps.front.reset();
        return;
    }
    // Each division takes the slot nearest to where it stands, so paths do not cross.
    std::vector<std::pair<float, UnitId>> order;
    for (const UnitId id : corps.members) {
        const auto e = find_unit(world, id);
        order.push_back({e == entt::null ? 0.0f : arc_param(points, world.get<Position>(e).value), id});
    }
    std::ranges::stable_sort(order, {}, &std::pair<float, UnitId>::first);
    for (std::size_t i = 0; i < order.size(); ++i) corps.members[i] = order[i].second;

    // The enemy is on the side of the line with more ground the corps' side does not hold.
    const auto& map = world.ctx().get<Map>().value;
    const auto& territory = world.ctx().get<Territory>();
    // Same ground both sides: the side with more enemy divisions in front of the line.
    float left = 0.0f, right = 0.0f;
    const auto probes = front_slots(points, std::max<std::size_t>(corps.members.size(), 4), false);
    for (const Slot& s : probes) {
        const auto probe = [&](float k) {
            const auto owner = territory.military_at(map, {s.pos.x + s.normal.first * k, s.pos.y + s.normal.second * k});
            return owner && *owner != corps.side;
        };
        left += probe(ENEMY_PROBE_M);
        right += probe(-ENEMY_PROBE_M);
    }
    if (left == right) {
        const Slot& mid = probes[probes.size() / 2];
        for (auto [e, side, pos, front] : world.view<const Side, const Position, const Front>().each()) {
            if (side.value == corps.side) continue;
            const float d = (pos.value.x - mid.pos.x) * mid.normal.first + (pos.value.y - mid.pos.y) * mid.normal.second;
            (d > 0.0f ? left : right) += 1.0f;
        }
    }
    corps.front = FrontLine{std::move(points), false, right > left, false, std::vector<bool>(corps.members.size())};
}

void front_lines(entt::registry& world) {
    const float near = std::max(ON_LINE_M, world.ctx().get<Map>().value.terrain.cell_m);
    for (Corps& c : world.ctx().get<CorpsList>().value) {
        // Destroyed divisions leave the corps.
        for (std::size_t i = c.members.size(); i-- > 0;) {
            if (find_unit(world, c.members[i]) != entt::null) continue;
            c.members.erase(c.members.begin() + static_cast<std::ptrdiff_t>(i));
            if (c.front) c.front->reached.erase(c.front->reached.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (!c.front || c.members.empty()) continue;
        FrontLine& f = *c.front;
        const std::size_t n = c.members.size();
        std::vector<entt::entity> es;
        for (const UnitId id : c.members) es.push_back(find_unit(world, id));

        std::vector<Slot> slots;
        if (f.formed && n >= 2) {
            std::vector<Pos> at;
            for (const auto e : es) at.push_back(world.get<Position>(e).value);
            f.points = line_through(at);
            for (std::size_t i = 0; i < n; ++i) {
                const Pos a = f.points[i], b = f.points[i + 2];  // neighbors of member i
                slots.push_back({at[i], normal_of(b.x - a.x, b.y - a.y, f.flip)});
            }
        } else {
            slots = front_slots(f.points, n, f.flip);
        }

        for (std::size_t i = 0; i < n; ++i) {
            const auto e = es[i];
            const auto& front = world.get<Front>(e);
            if (front.retreat) continue;  // combat moves it
            const bool contact = front.ratio.has_value();
            const bool moving = world.all_of<MoveOrder>(e);
            const Pos pos = world.get<Position>(e).value;
            if (!f.reached[i] && (contact || dist(pos, slots[i].pos) <= near)) f.reached[i] = true;
            if (!f.reached[i]) {
                if (!moving && !order_move(world, e, slots[i].pos)) f.reached[i] = true;
                continue;
            }
            const auto [nx, ny] = slots[i].normal;
            if (f.attack) {
                if (!moving) order_move(world, e, {pos.x + nx * ATTACK_STEP_M, pos.y + ny * ATTACK_STEP_M});
            } else if (!contact && !moving) {
                world.get<Deployment>(e).facing = slots[i].normal;
            }
        }
        f.formed = std::ranges::all_of(f.reached, [](bool r) { return r; });
    }
}

}  // namespace sim
