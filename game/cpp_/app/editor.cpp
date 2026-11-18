#include "app/editor.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace app {

namespace {

float distance(mapdata::Pos a, mapdata::Pos b) { return std::hypot(a.x - b.x, a.y - b.y); }

std::optional<std::size_t> nearest_unit(const mapdata::ScenarioData& s, mapdata::Pos p, float reach) {
    std::optional<std::size_t> best;
    float best_d = reach;
    for (std::size_t i = 0; i < s.units.size(); ++i) {
        const float d = distance(s.units[i].pos, p);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

std::optional<mapdata::NodeId> nearest_node(const mapdata::MapData& m, mapdata::Pos p, float reach) {
    std::optional<mapdata::NodeId> best;
    float best_d = reach;
    for (std::size_t i = 0; i < m.graph.nodes.size(); ++i) {
        const float d = distance(m.graph.nodes[i].pos, p);
        if (d < best_d) {
            best_d = d;
            best = mapdata::NodeId{static_cast<std::uint32_t>(i)};
        }
    }
    return best;
}

void remove_at(std::vector<mapdata::SideNode>& list, mapdata::NodeId node) {
    std::erase_if(list, [&](const mapdata::SideNode& sn) { return sn.node == node; });
}

// Even-odd rule.
bool inside_polygon(const std::vector<mapdata::Pos>& poly, mapdata::Pos p) {
    bool in = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const mapdata::Pos a = poly[i], b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

template <class Inside>
std::size_t carve(mapdata::Baked& b, Inside inside, std::uint8_t side) {
    auto& m = b.map;
    auto& s = b.scenario;
    const std::size_t old_n = m.regions.size();
    if (old_n > std::numeric_limits<std::uint16_t>::max()) return 0;  // no free RegionId left
    const mapdata::RegionId new_id{static_cast<std::uint16_t>(old_n)};
    const auto& t = m.terrain;
    std::vector<std::size_t> total(old_n), taken(old_n);
    std::size_t moved = 0;
    for (std::uint32_t cy = 0; cy < t.height; ++cy) {
        for (std::uint32_t cx = 0; cx < t.width; ++cx) {
            auto& r = m.region_cells[std::size_t(cy) * t.width + cx];
            ++total[r.value];
            const mapdata::Pos center{(float(cx) + 0.5f) * t.cell_m, (float(cy) + 0.5f) * t.cell_m};
            if (!inside(center)) continue;
            ++taken[r.value];
            r = new_id;
            ++moved;
        }
    }
    if (moved == 0) return 0;

    const auto drawn = std::ranges::count_if(m.regions, [](const auto& r) { return r.name.starts_with("Drawn region "); });
    mapdata::Region region{std::format("Drawn region {}", drawn + 1), {}};
    for (std::size_t r = 0; r < old_n; ++r) {
        if (taken[r] == 0) continue;
        const float share = float(taken[r]) / float(total[r]);
        for (std::size_t k = 0; k < region.daily_output.size(); ++k) {
            const float out = m.regions[r].daily_output[k] * share;
            m.regions[r].daily_output[k] -= out;
            region.daily_output[k] += out;
        }
    }
    m.regions.push_back(std::move(region));
    s.legal.push_back(side);
    s.control.push_back(side);

    // Drop regions that lost every cell; the ids after them shift down.
    std::vector<std::uint16_t> remap(m.regions.size());
    std::vector<mapdata::Region> regions;
    std::vector<std::uint8_t> legal, control;
    for (std::size_t r = 0; r < m.regions.size(); ++r) {
        if (r < old_n && taken[r] == total[r]) continue;
        remap[r] = static_cast<std::uint16_t>(regions.size());
        regions.push_back(std::move(m.regions[r]));
        legal.push_back(s.legal[r]);
        control.push_back(s.control[r]);
    }
    m.regions = std::move(regions);
    s.legal = std::move(legal);
    s.control = std::move(control);
    for (auto& r : m.region_cells) r.value = remap[r.value];
    return moved;
}

}  // namespace

const char* overlay_name(OverlayMode mode) {
    switch (mode) {
        case OverlayMode::Military: return "Military";
        case OverlayMode::Legal: return "Legal";
        case OverlayMode::Regions: return "Regions";
        case OverlayMode::Risk: return "Risk";
        case OverlayMode::Off: return "Off";
    }
    return "";
}

sim::SimCommand Play::raise_order() {
    ++raised;
    return {sim::SimCommand::BuildDivision{PLAYER_SIDE, std::format("New Div #{}", raised)}};
}

const char* side_label(std::uint8_t side) { return side == PLAYER_SIDE ? "Blue" : "Red"; }

void toggle_at(std::vector<mapdata::SideNode>& list, std::uint8_t side, mapdata::NodeId node) {
    const std::size_t before = list.size();
    remove_at(list, node);
    if (list.size() == before) list.push_back({side, node});
}

std::size_t carve_polygon(mapdata::Baked& b, const std::vector<mapdata::Pos>& polygon, std::uint8_t side) {
    if (polygon.size() < 3) return 0;
    return carve(b, [&](mapdata::Pos p) { return inside_polygon(polygon, p); }, side);
}

std::size_t carve_circle(mapdata::Baked& b, mapdata::Pos center, float radius, std::uint8_t side) {
    return carve(b, [&](mapdata::Pos p) { return distance(p, center) <= radius; }, side);
}

std::size_t add_line(mapdata::MapData& m, const std::vector<mapdata::Pos>& points, mapdata::EdgeKind kind, float snap) {
    auto& g = m.graph;
    std::vector<mapdata::NodeId> ids;
    for (const mapdata::Pos p : points) {
        auto n = nearest_node(m, p, snap);
        if (!n) {
            g.nodes.push_back({p, mapdata::NodeKind::Junction, std::nullopt});
            n = mapdata::NodeId{static_cast<std::uint32_t>(g.nodes.size() - 1)};
        }
        if (ids.empty() || ids.back() != *n) ids.push_back(*n);
    }
    std::size_t added = 0;
    for (std::size_t i = 1; i < ids.size(); ++i) {
        const mapdata::NodeId a = ids[i - 1], b = ids[i];
        const bool exists = std::ranges::any_of(g.edges, [&](const mapdata::Edge& e) {
            return e.kind == kind && ((e.a == a && e.b == b) || (e.a == b && e.b == a));
        });
        const float len = distance(g.node(a).pos, g.node(b).pos);
        if (exists || !(len > 0.0f)) continue;
        // Same default capacities (t/day) as mapbake.
        g.edges.push_back({a, b, kind, 0, len, kind == mapdata::EdgeKind::Rail ? 3000.0f : 600.0f});
        ++added;
    }
    return added;
}

Editor::Editor(mapdata::Baked source) : baked(std::move(source)) {}

void Editor::set_source(mapdata::Baked source) {
    baked = std::move(source);
    drag.reset();
    draft.clear();
    message.clear();
}

void Editor::set_tool(Tool t) {
    tool = t;
    draft.clear();
    message.clear();
}

void Editor::toggle(Play& play) {
    on = !on;
    drag.reset();
    if (on) {
        reload = true;
        play.selected.reset();
        play.paused = true;
    } else {
        play.paused = false;
    }
}

mapdata::ScenarioData& Editor::scenario() {
    reload = true;
    return baked.scenario;
}

mapdata::MapData& Editor::map() {
    reload = true;
    return baked.map;
}

void Editor::finish_draft(float snap) {
    const auto points = std::exchange(draft, {});
    switch (tool) {
        case Tool::Border:
            if (carve_polygon(baked, points, side) > 0) {
                reload = true;
            } else {
                message = points.size() < 3 ? "A region needs at least 3 points." : "No cell center inside the shape.";
            }
            break;
        case Tool::Circle:
            if (carve_circle(baked, points[0], distance(points[0], points[1]), side) > 0) {
                reload = true;
            } else {
                message = "Circle too small: no cell center inside.";
            }
            break;
        case Tool::Rail:
        case Tool::Road: {
            const auto kind = tool == Tool::Rail ? mapdata::EdgeKind::Rail : mapdata::EdgeKind::Road;
            if (add_line(map(), points, kind, snap) == 0) message = "A line needs two separate points.";
            break;
        }
        default: break;
    }
}

std::optional<std::size_t> Editor::selected_index(const std::optional<sim::UnitId>& selected) const {
    if (!on || !selected || selected->value == 0) return std::nullopt;
    const std::size_t i = selected->value - 1;
    if (i >= baked.scenario.units.size()) return std::nullopt;
    return i;
}

void Editor::delete_unit(std::size_t i, std::optional<sim::UnitId>& selected) {
    auto& units = scenario().units;
    units.erase(units.begin() + static_cast<std::ptrdiff_t>(i));
    selected.reset();
}

void Editor::flip_side(std::size_t i) {
    auto& u = scenario().units[i];
    u.side = u.side == PLAYER_SIDE ? 1 : PLAYER_SIDE;
}

bool Editor::apply_edits(entt::registry& world) {
    if (!std::exchange(reload, false)) return false;
    sim::load_scenario(world, baked);
    return true;
}

void Editor::edit_mouse(const EditInput& in, const mapdata::MapData& m, std::optional<sim::UnitId>& selected) {
    if (in.delete_pressed) {
        if (auto i = selected_index(selected)) delete_unit(*i, selected);
    }
    const mapdata::Pos pos = in.cursor;

    // Dragging: only the drawn symbol moves; the scenario changes on release.
    if (drag) {
        if (in.left_down) {
            drag_pos = pos;
            return;
        }
        const std::size_t i = *drag;
        drag.reset();  // not moved far enough: the symbol snaps back to the scenario position
        if (m.terrain.cell_at(pos) && distance(baked.scenario.units[i].pos, pos) > in.reach * 0.2f) {
            scenario().units[i].pos = pos;
        }
        return;
    }

    // Circle: press at the center, drag to the rim, release.
    if (tool == Tool::Circle && draft.size() == 2) {
        draft[1] = pos;
        if (!in.left_down) finish_draft(in.reach);
        return;
    }
    if (tool == Tool::Border || tool == Tool::Rail || tool == Tool::Road) {
        if (in.cancel_pressed) draft.clear();
        if (in.undo_pressed && !draft.empty()) draft.pop_back();
        if (in.finish_pressed) {
            finish_draft(in.reach);
            return;
        }
    }

    if (!in.left_pressed || in.over_ui) return;
    message.clear();
    std::optional<std::size_t> region;
    if (auto c = m.terrain.cell_at(pos)) region = m.region_at(c->x, c->y).value;
    switch (tool) {
        case Tool::Select: {
            const auto hit = nearest_unit(baked.scenario, pos, in.reach);
            selected.reset();
            if (hit) {
                selected = sim::UnitId{static_cast<std::uint32_t>(*hit + 1)};
                drag = hit;
                drag_pos = baked.scenario.units[*hit].pos;
            }
            break;
        }
        case Tool::Division: {
            if (!region) return;
            auto& s = scenario();
            const auto n = std::ranges::count_if(s.units, [&](const auto& u) { return u.side == side; }) + 1;
            s.units.push_back({side, std::format("{} Division {}", side_label(side), n), pos});
            selected = sim::UnitId{static_cast<std::uint32_t>(s.units.size())};
            break;
        }
        case Tool::Depot:
        case Tool::Base: {
            const auto node = nearest_node(m, pos, std::max(in.reach, 2.0f * m.terrain.cell_m));
            if (!node) {
                message = "No node here.";
                return;
            }
            auto& s = scenario();
            toggle_at(tool == Tool::Depot ? s.depots : s.bases, side, *node);
            break;
        }
        case Tool::Region:
        case Tool::Occupy: {
            if (!region) return;
            auto& s = scenario();
            s.control[*region] = side;
            if (tool == Tool::Region) s.legal[*region] = side;
            break;
        }
        case Tool::Border:
        case Tool::Circle:
        case Tool::Rail:
        case Tool::Road: {
            if (!region) {
                message = "Outside the map.";
                return;
            }
            mapdata::Pos p = pos;
            if (tool == Tool::Rail || tool == Tool::Road) {
                if (auto n = nearest_node(m, pos, in.reach)) p = m.graph.node(*n).pos;  // preview where it joins
            }
            draft.push_back(p);
            if (tool == Tool::Circle) draft.push_back(p);
            break;
        }
        case Tool::Delete: {
            if (auto i = nearest_unit(baked.scenario, pos, in.reach)) {
                delete_unit(*i, selected);
            } else if (auto node = nearest_node(m, pos, in.reach)) {
                auto& s = scenario();
                remove_at(s.depots, *node);
                remove_at(s.bases, *node);
            }
            break;
        }
    }
}

}  // namespace app
