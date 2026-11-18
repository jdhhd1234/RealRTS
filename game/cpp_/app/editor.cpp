#include "app/editor.hpp"

#include <algorithm>
#include <cmath>
#include <format>

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

Editor::Editor(mapdata::Baked source) : baked(std::move(source)) {}

void Editor::set_source(mapdata::Baked source) {
    baked = std::move(source);
    drag.reset();
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

void Editor::apply_edits(entt::registry& world) {
    if (!std::exchange(reload, false)) return;
    sim::load_scenario(world, baked);
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

    if (!in.left_pressed || in.over_ui) return;
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
            if (!node) return;
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
