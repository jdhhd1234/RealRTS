#include "app/ui.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>

#include "app/symbols.hpp"
#include "raygui.h"

namespace app {

namespace {

// Calm staff-room palette: dark slate panels, desaturated sides, amber warnings.
constexpr Color PANEL_BG{18, 23, 28, 235};
constexpr Color PANEL_BORDER{56, 69, 82, 255};
constexpr Color BUTTON_BG{33, 41, 48, 255};
constexpr Color BUTTON_HOVER{48, 59, 69, 255};
constexpr Color BUTTON_ACTIVE{41, 71, 92, 255};
constexpr Color ACTIVE_BORDER{242, 204, 77, 255};
constexpr Color TITLE{153, 191, 217, 255};

Color tone_color(Tone t) {
    switch (t) {
        case Tone::Normal: return {217, 222, 224, 255};
        case Tone::Dim: return {140, 153, 163, 255};
        case Tone::Info: return {115, 166, 230, 255};
        case Tone::Warn: return {242, 179, 64, 255};
        case Tone::Alert: return {235, 82, 71, 255};
        case Tone::Good: return {115, 204, 128, 255};
        case Tone::Friendly: return FRIENDLY;
        case Tone::Enemy: return ENEMY;
    }
    return WHITE;
}

struct ButtonSpec {
    std::string label;
    Action action;
    bool active = false;
};

enum class ElKind { Title, Text, Kv, Buttons, Wide };

// One UI element. `Buttons` is one wrapping row; `Wide` is one full-width button.
struct El {
    ElKind kind = ElKind::Text;
    std::string text;
    std::string value;  // Kv only
    Tone tone = Tone::Normal;
    std::vector<ButtonSpec> buttons;  // Buttons and Wide
};

El title(std::string t) { return {ElKind::Title, std::move(t)}; }
El text(std::string t, Tone tone) { return {ElKind::Text, std::move(t), {}, tone}; }
El kv(std::string k, std::string v, Tone tone) { return {ElKind::Kv, std::move(k), std::move(v), tone}; }
El buttons(std::vector<ButtonSpec> list) { return {ElKind::Buttons, {}, {}, Tone::Normal, std::move(list)}; }
El wide(std::string label, Action a, bool active) {
    return {ElKind::Wide, {}, {}, Tone::Normal, {{std::move(label), a, active}}};
}

Action act(ActionKind kind) { return {.kind = kind}; }

// ---------- derived unit status ("state first" display) ----------

struct Status {
    float supply_frac = 1.0f;
    float forecast_h = 0.0f;
    bool combat_ready = false;
    bool mobility_limited = false;
};

Status status(const sim::Stocks& stocks, const sim::StockTarget& target, const sim::Battalions& bats,
              const sim::Front& front, bool moving) {
    const auto frac = [&](std::size_t g) {
        return target.value[g] > 0.0f ? stocks.value[g] / target.value[g] : 1.0f;
    };
    sim::Goods use_per_day = sim::IDLE_USE_T_PER_DAY;
    if (moving) use_per_day[sim::FUEL] = sim::MOVING_FUEL_T_PER_DAY;
    if (front.ratio) use_per_day[sim::AMMO] = sim::AMMO_PER_STEP_T * 288.0f;
    Status s;
    s.forecast_h = std::numeric_limits<float>::infinity();
    for (std::size_t g = 0; g < 3; ++g) {
        s.forecast_h = std::min(s.forecast_h, stocks.value[g] / use_per_day[g] * 24.0f);
        s.supply_frac = std::min(s.supply_frac, frac(g));
    }
    s.combat_ready = front.org >= 0.5f && bats.total() >= 5.0f && frac(sim::AMMO) >= 0.3f;
    s.mobility_limited = frac(sim::FUEL) < 0.2f;
    return s;
}

std::pair<const char*, Tone> supply_grade(float frac) {
    if (frac >= 0.6f) return {"GREEN", Tone::Good};
    if (frac >= 0.3f) return {"AMBER", Tone::Warn};
    return {"RED", Tone::Alert};
}

const char* posture(const sim::Front& f, bool moving) {
    if (f.encircled) return "Encircled";
    if (f.retreat) return "Retreat";
    if (f.hold) return "Hold";
    if (moving && f.ratio) return "Attack";
    if (moving) return "Move";
    if (f.ratio) return "Defend (engaged)";
    return "Defend";
}

const char* dominant_arm(const sim::Battalions& b) {
    switch (main_arm(b)) {
        case sim::Arm::Armor: return "Armor";
        case sim::Arm::Artillery: return "Artillery";
        case sim::Arm::Infantry: return "Infantry";
    }
    return "";
}

const char* risk_name(sim::RiskGrade g) {
    switch (g) {
        case sim::RiskGrade::None: return "None";
        case sim::RiskGrade::Low: return "Low";
        case sim::RiskGrade::Medium: return "Medium";
        case sim::RiskGrade::High: return "High";
    }
    return "";
}

const char* posture_name(sim::Posture p) {
    switch (p) {
        case sim::Posture::Defend: return "Defend";
        case sim::Posture::Attack: return "Attack";
        case sim::Posture::March: return "March";
    }
    return "";
}

int pct(float have, float want) { return want > 0.0f ? static_cast<int>(std::round(have / want * 100.0f)) : 100; }

float sum(const sim::Goods& g) { return g[0] + g[1] + g[2]; }

// A division the player can see in lists (partisans lack Front and are listed separately).
struct UnitRow {
    entt::entity e{};
    sim::UnitId id;
    std::uint8_t side = 0;
    bool moving = false;
};

std::vector<UnitRow> unit_rows(const entt::registry& w) {
    std::vector<UnitRow> rows;
    auto v = w.view<const sim::UnitId, const sim::UnitName, const sim::Side, const sim::Position, const sim::Stocks,
                    const sim::StockTarget, const sim::SupplyPriority, const sim::Battalions, const sim::Front>();
    for (auto e : v) {
        rows.push_back({e, v.get<const sim::UnitId>(e), v.get<const sim::Side>(e).value, w.all_of<sim::MoveOrder>(e)});
    }
    std::ranges::sort(rows, {}, [](const UnitRow& r) { return r.id.value; });
    return rows;
}

// ---------- panel contents ----------

struct Panels {
    std::vector<El> top, left, right, bottom, menu;
};

std::vector<El> editor_palette(const Editor& ed) {
    const auto& s = ed.baked.scenario;
    std::vector<El> v{title("SCENARIO EDITOR"), title("SIDE")};
    v.push_back(buttons({{"Blue", {.kind = ActionKind::EditSide, .side = PLAYER_SIDE}, ed.side == PLAYER_SIDE},
                         {"Red", {.kind = ActionKind::EditSide, .side = 1}, ed.side == 1}}));
    v.push_back(title("TOOLS"));
    for (const auto& [t, label] : TOOLS) v.push_back(wide(label, {.kind = ActionKind::Tool, .tool = t}, ed.tool == t));
    v.push_back(title("SCENARIO"));
    for (std::uint8_t side : {PLAYER_SIDE, std::uint8_t{1}}) {
        const auto units = std::ranges::count_if(s.units, [&](const auto& u) { return u.side == side; });
        const auto depots = std::ranges::count_if(s.depots, [&](const auto& d) { return d.side == side; });
        const auto bases = std::ranges::count_if(s.bases, [&](const auto& b) { return b.side == side; });
        v.push_back(kv(side_label(side), std::format("{} div, {} depots, {} bases", units, depots, bases),
                       side == PLAYER_SIDE ? Tone::Friendly : Tone::Enemy));
    }
    v.push_back(wide("Save scenario (.json)...", act(ActionKind::SaveScenario), false));
    v.push_back(text("LMB uses the tool. Del deletes the selected unit. Play runs from here; Editor returns here.",
                     Tone::Dim));
    return v;
}

std::vector<El> editor_detail(const Editor& ed, const std::optional<sim::UnitId>& selected, const mapdata::MapData& m) {
    const auto i = ed.selected_index(selected);
    if (!i) {
        const char* hint = "";
        switch (ed.tool) {
            case Tool::Select: hint = "Click a unit to select it; drag to move."; break;
            case Tool::Division: hint = "Click on the map to place a division of the chosen side."; break;
            case Tool::Depot: hint = "Click near a node: adds a field depot, or removes the one there."; break;
            case Tool::Base:
                hint = "Click near a node: adds a supply base (national supply enters here), or removes it.";
                break;
            case Tool::Region: hint = "Click a region: chosen side becomes legal owner and controller."; break;
            case Tool::Occupy: hint = "Click a region: chosen side occupies it (owner unchanged)."; break;
            case Tool::Delete: hint = "Click a unit, depot or base to delete it."; break;
        }
        std::vector<El> v{title("TOOL"), text(hint, Tone::Normal)};
        for (std::uint8_t side : {PLAYER_SIDE, std::uint8_t{1}}) {
            if (std::ranges::none_of(ed.baked.scenario.bases, [&](const auto& b) { return b.side == side; })) {
                v.push_back(text(std::format("{} has no supply base: it gets no supply.", side_label(side)), Tone::Warn));
            }
        }
        return v;
    }
    const auto& u = ed.baked.scenario.units[*i];
    std::string region;
    if (auto c = m.terrain.cell_at(u.pos)) region = m.regions[m.region_at(c->x, c->y).value].name;
    return {
        title("UNIT"),
        text(u.name, u.side == PLAYER_SIDE ? Tone::Friendly : Tone::Enemy),
        kv("Side", side_label(u.side), Tone::Normal),
        kv("Position", std::format("{:.0f}, {:.0f} km", u.pos.x / 1000.0f, u.pos.y / 1000.0f), Tone::Normal),
        kv("Region", region, Tone::Normal),
        text("Standard division: 6 inf, 2 armor, 2 arty bn, full supply.", Tone::Dim),
        buttons({{"Switch side", act(ActionKind::EditFlip), false}, {"Delete (Del)", act(ActionKind::EditDelete), false}}),
    };
}

Panels build_panels(const UiState& ui, const UiView& view) {
    const entt::registry& w = view.world;
    const Play& play = view.play;
    const Editor& ed = view.editor;
    const mapdata::MapData& m = w.ctx().get<sim::Map>().value;
    const auto& territory = w.ctx().get<sim::Territory>();
    const auto& pstate = w.ctx().get<sim::PartisanState>();
    const auto& econ = w.ctx().get<sim::EconomyState>();
    const auto& edges = w.ctx().get<sim::EdgeState>();
    const auto& clock = w.ctx().get<sim::SimClock>();
    const auto node_name = [&](mapdata::NodeId n) { return m.graph.node(n).name.value_or(""); };

    const auto rows = unit_rows(w);
    std::vector<UnitRow> own;
    std::ranges::copy_if(rows, std::back_inserter(own), [](const UnitRow& r) { return r.side == PLAYER_SIDE; });
    const auto regions = sim::region_status(m, territory);

    // ---- Alerts (derived from current state, most urgent first) ----
    std::vector<std::pair<Tone, std::string>> alerts;
    for (const auto& r : own) {
        const auto& name = w.get<sim::UnitName>(r.e).value;
        const auto& f = w.get<sim::Front>(r.e);
        const auto s = status(w.get<sim::Stocks>(r.e), w.get<sim::StockTarget>(r.e), w.get<sim::Battalions>(r.e), f,
                              r.moving);
        if (f.encircled) alerts.emplace_back(Tone::Alert, std::format("[ALERT] {} encircled - no supply", name));
        if (f.retreat) alerts.emplace_back(Tone::Warn, std::format("[WARN] {} retreating, org {:.0f}%", name, f.org * 100.0f));
        if (s.forecast_h < 12.0f) {
            alerts.emplace_back(Tone::Warn, std::format("[WARN] {} supplies exhausted in ~{:.0f}h", name, s.forecast_h));
        }
        if (!f.encircled && !sim::nearest_depot(m.graph, w, PLAYER_SIDE, w.get<sim::Position>(r.e).value)) {
            alerts.emplace_back(Tone::Warn, std::format("[WARN] {} out of truck range", name));
        }
    }
    for (std::size_t i = 0; i < m.graph.edges.size(); ++i) {
        if (edges.damage[i] >= 0.3f) {
            const auto& e = m.graph.edges[i];
            alerts.emplace_back(Tone::Warn, std::format("[WARN] Line {} - {} damaged {:.0f}%", node_name(e.a),
                                                        node_name(e.b), edges.damage[i] * 100.0f));
        }
    }
    for (std::size_t r = 0; r < regions.size(); ++r) {
        const auto* occ = std::get_if<sim::RegionStatus::Occupied>(&regions[r].value);
        const auto grade = sim::risk_grade(pstate.pool[r]);
        if (occ && occ->by == PLAYER_SIDE && (grade == sim::RiskGrade::Medium || grade == sim::RiskGrade::High)) {
            alerts.emplace_back(Tone::Warn, std::format("[WARN] {} partisan risk {}", m.regions[r].name, risk_name(grade)));
        }
    }
    std::ranges::stable_sort(alerts, {}, [](const auto& a) { return a.first != Tone::Alert; });
    const auto urgent = std::ranges::count_if(alerts, [](const auto& a) { return a.first == Tone::Alert; });

    Panels p;

    // ---- Top bar ----
    float have = 0.0f, want = 0.0f;
    for (const auto& r : own) {
        have += sum(w.get<sim::Stocks>(r.e).value);
        want += sum(w.get<sim::StockTarget>(r.e).value);
    }
    const int supply_pct = pct(have, want);
    std::vector<ButtonSpec> speed_buttons{{"II", act(ActionKind::Pause), play.paused}};
    for (std::size_t i = 0; i < SPEEDS.size(); ++i) {
        speed_buttons.push_back({std::format("{}x", SPEEDS[i]), {.kind = ActionKind::Speed, .index = i},
                                 !play.paused && play.speed == i});
    }
    std::string short_text = view.file.text.size() > 60 ? view.file.text.substr(0, 60) + "..." : view.file.text;
    const Tone file_tone = view.file.error ? Tone::Alert : Tone::Dim;
    p.top.push_back(buttons({{"File", act(ActionKind::ToggleFileMenu), ui.file_menu},
                             {ed.on ? "> Play" : "Editor", act(ActionKind::ToggleEditor), ed.on}}));
    if (ed.on) {
        p.top.push_back(text("SCENARIO EDITOR - start state, paused", Tone::Warn));
    } else {
        p.top.push_back(text(std::format("D+{} {:02}:{:02}", clock.day(), clock.hour(), clock.minute()), Tone::Normal));
        p.top.push_back(buttons(std::move(speed_buttons)));
    }
    p.top.push_back(text("BLUE FORCES", Tone::Friendly));
    p.top.push_back(text(std::format("Supply {}%", supply_pct), supply_grade(static_cast<float>(supply_pct) / 100.0f).second));
    p.top.push_back(text(std::format("SITREP: {} alerts", alerts.size()),
                         urgent > 0 ? Tone::Alert : alerts.empty() ? Tone::Dim : Tone::Warn));
    p.top.push_back(text(std::format("Layer: {}{}", overlay_name(play.overlay), play.show_network ? " + Network" : ""),
                         Tone::Dim));
    p.top.push_back(text(short_text, file_tone));

    // ---- File menu ----
    if (ui.file_menu) {
        const char* open = view.file.busy ? "Opening... (see file dialog)" : "Open map (.gpkg / .geojson / .shp / .json)...";
        p.menu.push_back(wide(open, act(ActionKind::OpenMap), false));
        p.menu.push_back(wide("Load built-in test map", act(ActionKind::LoadTestMap), false));
        p.menu.push_back(wide("Save scenario (.json)...", act(ActionKind::SaveScenario), false));
        p.menu.push_back(wide("Quit", act(ActionKind::Quit), false));
        p.menu.push_back(text(view.file.text, file_tone));
        p.menu.push_back(title("QGIS LAYERS (projected CRS, meters)"));
        p.menu.push_back(text("GeoJSON/Shapefile: one file per layer, named terrain.shp, rail.geojson, ...; any one opens the folder",
                              Tone::Dim));
        for (const char* line : {
                 "terrain  polygons  kind: plains|forest|marsh|mountain|urban|water",
                 "regions  polygons  name, side, control, fuel/ammo/food/equipment",
                 "nodes    points    name, kind: junction|station|port, base_side, depot_side",
                 "rail / roads / rivers  lines  capacity (t/day), rail: gauge",
                 "units    points    name, side",
                 "Loading restarts the game on the new map, paused.",
             }) {
            p.menu.push_back(text(line, Tone::Dim));
        }
    }

    // ---- Left: tabs ----
    if (ed.on) {
        p.left = editor_palette(ed);
    } else {
        p.left.push_back(buttons({
            {"ORBAT", {.kind = ActionKind::LeftTab, .left_tab = LeftTab::Orbat}, ui.left == LeftTab::Orbat},
            {"Rear", {.kind = ActionKind::LeftTab, .left_tab = LeftTab::Rear}, ui.left == LeftTab::Rear},
            {"Layers", {.kind = ActionKind::LeftTab, .left_tab = LeftTab::Layers}, ui.left == LeftTab::Layers},
        }));
    }
    if (!ed.on && ui.left == LeftTab::Orbat) {
        p.left.push_back(title("BLUE FORCES"));
        for (const auto& r : own) {
            const auto& f = w.get<sim::Front>(r.e);
            const auto& b = w.get<sim::Battalions>(r.e);
            const auto s = status(w.get<sim::Stocks>(r.e), w.get<sim::StockTarget>(r.e), b, f, r.moving);
            p.left.push_back(wide(std::format("{}  [{}]", w.get<sim::UnitName>(r.e).value, posture(f, r.moving)),
                                  {.kind = ActionKind::Select, .unit = r.id}, play.selected == r.id));
            auto [grade, tone] = supply_grade(s.supply_frac);
            if (f.encircled) tone = Tone::Alert;
            p.left.push_back(text(std::format("   Str {:.1f}  Org {:.0f}%  Sup {}", b.total(), f.org * 100.0f, grade), tone));
        }
        p.left.push_back(title("REAR COMMAND"));
        std::vector<entt::entity> ds;
        for (auto [e, d, st] : w.view<const sim::Depot, const sim::Stocks>().each()) {
            if (d.side == PLAYER_SIDE) ds.push_back(e);
        }
        std::ranges::sort(ds, {}, [&](entt::entity e) { return w.get<sim::Depot>(e).node.value; });
        for (auto e : ds) {
            const int fill = pct(sum(w.get<sim::Stocks>(e).value), sum(sim::DEPOT_TARGET));
            p.left.push_back(kv(std::format("Depot {}", node_name(w.get<sim::Depot>(e).node)), std::format("{}%", fill),
                                supply_grade(static_cast<float>(fill) / 100.0f).second));
        }
        for (const auto& b : w.ctx().get<sim::SupplyBases>().value) {
            if (b.side != PLAYER_SIDE) continue;
            p.left.push_back(kv(std::format("Base {}", node_name(b.node)), std::format("{:.0f} t", sum(b.stock)), Tone::Normal));
        }
        p.left.push_back(kv("Equipment", std::format("{:.0f} t", w.ctx().get<sim::Nations>().equipment[PLAYER_SIDE]),
                            Tone::Normal));
        for (const auto& b : econ.builds) {
            if (b.side != PLAYER_SIDE) continue;
            const std::uint64_t left_ticks = b.ready_tick > clock.tick ? b.ready_tick - clock.tick : 0;
            const std::uint64_t days = (left_ticks + sim::TICKS_PER_DAY - 1) / sim::TICKS_PER_DAY;
            p.left.push_back(kv(std::format("Raising {}", b.name), std::format("~{} d", days), Tone::Info));
        }
        p.left.push_back(wide("Raise new division (B)", act(ActionKind::Build), false));
        p.left.push_back(title("CONTACTS"));
        for (const auto& r : rows) {
            if (r.side == PLAYER_SIDE) continue;
            p.left.push_back(kv(w.get<sim::UnitName>(r.e).value, w.get<sim::Front>(r.e).ratio ? "engaged" : "", Tone::Enemy));
        }
        for (auto [e, name, side, partisan] : w.view<const sim::UnitName, const sim::Side, const sim::Partisan>().each()) {
            p.left.push_back(text(name.value, side.value == PLAYER_SIDE ? Tone::Friendly : Tone::Enemy));
        }
    } else if (!ed.on && ui.left == LeftTab::Rear) {
        p.left.push_back(title("REAR AREA SECURITY"));
        for (std::size_t r = 0; r < regions.size(); ++r) {
            const std::string& name = m.regions[r].name;
            const char* legal = side_label(territory.legal[r]);
            if (std::holds_alternative<sim::RegionStatus::Home>(regions[r].value)) {
                p.left.push_back(kv(name, std::format("{}, home", legal), Tone::Dim));
            } else if (std::holds_alternative<sim::RegionStatus::Contested>(regions[r].value)) {
                p.left.push_back(kv(name, std::format("{}, contested", legal), Tone::Warn));
            } else {
                const auto by = std::get<sim::RegionStatus::Occupied>(regions[r].value).by;
                const auto grade = sim::risk_grade(pstate.pool[r]);
                const Tone tone = grade == sim::RiskGrade::High     ? Tone::Alert
                                  : grade == sim::RiskGrade::Medium ? Tone::Warn
                                                                    : Tone::Normal;
                p.left.push_back(kv(name, std::format("held by {}, risk {}", side_label(by), risk_name(grade)), tone));
                if (by == PLAYER_SIDE) {
                    const mapdata::RegionId rid{static_cast<std::uint16_t>(r)};
                    p.left.push_back(buttons({
                        {"Sweep", {.kind = ActionKind::Sweep, .region = rid, .flag = false}, false},
                        {"Reprisal", {.kind = ActionKind::Sweep, .region = rid, .flag = true}, false},
                        {"Requisition", {.kind = ActionKind::Requisition, .region = rid}, econ.requisition[r]},
                    }));
                }
            }
        }
        p.left.push_back(text("Keys at cursor: K sweep, H reprisal, Q requisition", Tone::Dim));
    } else if (!ed.on) {
        p.left.push_back(title("TERRITORY"));
        for (auto [label, mode] : {std::pair{"Military control", OverlayMode::Military}, {"Legal territory", OverlayMode::Legal},
                                   {"Regions", OverlayMode::Regions}, {"Partisan risk", OverlayMode::Risk},
                                   {"Off", OverlayMode::Off}}) {
            p.left.push_back(wide(label, {.kind = ActionKind::Layer, .overlay = mode}, play.overlay == mode));
        }
        p.left.push_back(title("NETWORK"));
        p.left.push_back(wide(std::format("Supply network: {}", play.show_network ? "ON" : "OFF"),
                              act(ActionKind::ToggleNetwork), play.show_network));
        p.left.push_back(title("LEGEND"));
        for (auto [t, tone] : {std::pair{"Orange line: carrying supply", Tone::Warn}, {"Red line: damaged", Tone::Alert},
                               {"Red link: units in combat", Tone::Alert}, {"Magenta ring: encircled", Tone::Normal},
                               {"White arrow: retreating", Tone::Normal},
                               {"Symbols: NATO APP-6 (blue friend, red hostile)", Tone::Normal},
                               {"T cycles territory layer", Tone::Dim}}) {
            p.left.push_back(text(t, tone));
        }
    }

    // ---- Right: context panel ----
    const UnitRow* sel = nullptr;
    for (const auto& r : rows) {
        if (play.selected == r.id) sel = &r;
    }
    if (ed.on) {
        p.right = editor_detail(ed, play.selected, m);
    } else if (sel) {
        const auto e = sel->e;
        const auto& f = w.get<sim::Front>(e);
        const auto& b = w.get<sim::Battalions>(e);
        const auto& st = w.get<sim::Stocks>(e);
        const auto& tg = w.get<sim::StockTarget>(e);
        const std::uint8_t side = sel->side;
        const auto s = status(st, tg, b, f, sel->moving);
        p.right.push_back(title("UNIT DETAIL"));
        p.right.push_back(text(w.get<sim::UnitName>(e).value, side == PLAYER_SIDE ? Tone::Friendly : Tone::Enemy));
        p.right.push_back(kv("Side", side_label(side), Tone::Normal));
        p.right.push_back(kv("Type", dominant_arm(b), Tone::Normal));
        p.right.push_back(kv("Posture", posture(f, sel->moving), Tone::Normal));
        if (const auto* d = w.try_get<sim::Deployment>(e)) {
            const auto l = sim::layout(b, d->posture);
            p.right.push_back(title("DEPLOYMENT"));
            p.right.push_back(kv("Formation", posture_name(d->posture), Tone::Normal));
            p.right.push_back(kv("Frontage / depth",
                                 std::format("{:.0f} / {:.0f} km", l.frontage_m / 1000.0f, l.depth_m / 1000.0f), Tone::Normal));
            p.right.push_back(text(std::format("line {:.1f}  reserve {:.1f}  support {:.1f} bn", l.strength[0], l.strength[1],
                                               l.strength[2]),
                                   Tone::Dim));
        }
        p.right.push_back(title("STATUS"));
        const auto yes_no = [](bool v, bool good_if) {
            return std::pair{std::string(v ? "YES" : "NO"), v == good_if ? Tone::Good : Tone::Warn};
        };
        auto [ready, ready_tone] = yes_no(s.combat_ready, true);
        p.right.push_back(kv("Combat ready", ready, ready_tone));
        const auto [grade, grade_tone] = supply_grade(s.supply_frac);
        p.right.push_back(kv("Supply", grade, grade_tone));
        const std::string forecast = s.forecast_h > 999.0f ? "> 40 d" : std::format("{:.0f} h", s.forecast_h);
        p.right.push_back(kv("Sustainment", forecast, s.forecast_h < 12.0f ? Tone::Warn : Tone::Normal));
        auto [mob, mob_tone] = yes_no(s.mobility_limited, false);
        p.right.push_back(kv("Mobility limited", mob, mob_tone));
        const auto link = sim::nearest_depot(m.graph, w, side, w.get<sim::Position>(e).value);
        if (f.encircled) {
            p.right.push_back(kv("Isolation risk", "HIGH (pocket)", Tone::Alert));
        } else if (!link) {
            p.right.push_back(kv("Isolation risk", "MEDIUM (no depot in range)", Tone::Warn));
        } else {
            p.right.push_back(kv("Isolation risk", std::format("LOW ({:.0f} km to depot)", link->first / 1000.0f), Tone::Good));
        }
        if (!f.ratio) {
            p.right.push_back(kv("Contact", "None", Tone::Dim));
        } else if (*f.ratio >= sim::ADVANCE_RATIO) {
            p.right.push_back(kv("Contact", std::format("Winning, R {:.2f}", *f.ratio), Tone::Good));
        } else if (*f.ratio <= sim::RETREAT_RATIO) {
            p.right.push_back(kv("Contact", std::format("Losing, R {:.2f}", *f.ratio), Tone::Alert));
        } else {
            p.right.push_back(kv("Contact", std::format("Stalemate, R {:.2f}", *f.ratio), Tone::Warn));
        }
        p.right.push_back(title("DETAIL"));
        p.right.push_back(kv("Strength", std::format("{:.1f} / {} bn", b.total(), b.value.size()), Tone::Normal));
        p.right.push_back(kv("Organization", std::format("{:.0f}%", f.org * 100.0f), Tone::Normal));
        for (auto [label, g] : {std::pair{"Fuel", sim::FUEL}, {"Ammo", sim::AMMO}, {"Food", sim::FOOD}}) {
            p.right.push_back(kv(label, std::format("{:.0f} / {:.0f} t", st.value[g], tg.value[g]), Tone::Normal));
        }
        const auto arm = [&](sim::Arm a) {
            float total = 0.0f;
            for (const auto& x : b.value) {
                if (x.arm == a) total += x.strength;
            }
            return total;
        };
        p.right.push_back(text(std::format("inf {:.1f}  armor {:.1f}  arty {:.1f}", arm(sim::Arm::Infantry),
                                           arm(sim::Arm::Armor), arm(sim::Arm::Artillery)),
                               Tone::Dim));
        if (side == PLAYER_SIDE) {
            const auto prio = w.get<sim::SupplyPriority>(e).value;
            p.right.push_back(title("ORDERS"));
            p.right.push_back(text("RMB on map: Move / Attack (engages on contact)", Tone::Dim));
            p.right.push_back(wide(std::format("Hold - no retreat (G): {}", f.hold ? "ON" : "OFF"), act(ActionKind::ToggleHold),
                                   f.hold));
            p.right.push_back(wide(std::format("Resupply priority (P): {}", prio > 0 ? "HIGH" : "NORMAL"),
                                   act(ActionKind::TogglePriority), prio > 0));
            p.right.push_back(wide("Deselect", act(ActionKind::Deselect), false));
        }
    } else {
        p.right.push_back(title("THEATER SUMMARY"));
        p.right.push_back(kv("Divisions", std::to_string(own.size()), Tone::Normal));
        p.right.push_back(kv("Supply level", std::format("{}%", supply_pct),
                             supply_grade(static_cast<float>(supply_pct) / 100.0f).second));
        const auto pockets = std::ranges::count_if(own, [&](const UnitRow& r) { return w.get<sim::Front>(r.e).encircled; });
        p.right.push_back(kv("Units encircled", std::to_string(pockets), pockets > 0 ? Tone::Alert : Tone::Good));
        const auto engaged =
            std::ranges::count_if(own, [&](const UnitRow& r) { return w.get<sim::Front>(r.e).ratio.has_value(); });
        p.right.push_back(kv("Units engaged", std::to_string(engaged), Tone::Normal));
        p.right.push_back(text("Select a unit: LMB on map or ORBAT list", Tone::Dim));
    }

    // ---- Bottom: Alerts / Event Log ----
    p.bottom.push_back(buttons({
        {std::format("Alerts ({})", alerts.size()), {.kind = ActionKind::BottomTab, .bottom_tab = BottomTab::Alerts},
         ui.bottom == BottomTab::Alerts},
        {"Event Log", {.kind = ActionKind::BottomTab, .bottom_tab = BottomTab::Events}, ui.bottom == BottomTab::Events},
    }));
    if (ui.bottom == BottomTab::Alerts) {
        if (alerts.empty()) p.bottom.push_back(text("No alerts.", Tone::Dim));
        for (std::size_t i = 0; i < alerts.size() && i < 7; ++i) p.bottom.push_back(text(alerts[i].second, alerts[i].first));
    } else {
        const auto& log = w.ctx().get<sim::EventLog>().value;
        for (std::size_t n = 0; n < log.size() && n < 7; ++n) {
            const auto& [tick, msg] = log[log.size() - 1 - n];
            const sim::SimClock c{tick};
            const bool warn = std::ranges::any_of(std::array{"Partisan", "Sabotage", "Cannot", "reprisals"},
                                                  [&](const char* k) { return msg.find(k) != std::string::npos; });
            p.bottom.push_back(text(std::format("D+{} {:02}:{:02} [{}] {}", c.day(), c.hour(), c.minute(),
                                                warn ? "WARN" : "INFO", msg),
                                    warn ? Tone::Warn : Tone::Info));
        }
    }
    return p;
}

// ---------- drawing ----------

constexpr float FONT = 13.0f;
constexpr float TITLE_FONT = 12.0f;
constexpr float BUTTON_H = 20.0f;
constexpr float PAD = 8.0f;

float spacing(float size) { return size / 10.0f; }

float text_w(const std::string& s, float size) { return MeasureTextEx(GetFontDefault(), s.c_str(), size, spacing(size)).x; }

void draw_text(const std::string& s, float x, float y, float size, Color c) {
    DrawTextEx(GetFontDefault(), s.c_str(), {x, y}, size, spacing(size), c);
}

// Greedy word wrap to `width` pixels.
std::vector<std::string> wrap(const std::string& s, float width, float size) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = s.find(' ', i + 1);
        if (j == std::string::npos) j = s.size();
        const std::string word = s.substr(i, j - i);
        if (!line.empty() && text_w(line + word, size) > width) {
            lines.push_back(line);
            line = word.substr(word.find_first_not_of(' ') == std::string::npos ? word.size() : word.find_first_not_of(' '));
        } else {
            line += word;
        }
        i = j;
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

float button_w(const ButtonSpec& b) { return text_w(b.label, FONT) + 16.0f; }

// Draws one raygui button with the palette; returns true when clicked.
bool draw_button(Rectangle r, const ButtonSpec& b) {
    GuiSetStyle(BUTTON, BASE_COLOR_NORMAL, ColorToInt(b.active ? BUTTON_ACTIVE : BUTTON_BG));
    GuiSetStyle(BUTTON, BORDER_COLOR_NORMAL, ColorToInt(b.active ? ACTIVE_BORDER : PANEL_BORDER));
    return GuiButton(r, b.label.c_str());
}

// Lays out a column of elements inside `area`. Draws and collects clicks only when `draw` is set.
// Returns the height used.
float column(const std::vector<El>& els, Rectangle area, bool draw, std::vector<Action>& clicked) {
    const float x0 = area.x, w = area.width;
    float y = area.y;
    for (const El& el : els) {
        switch (el.kind) {
            case ElKind::Title:
                y += 8.0f;
                if (draw) draw_text(el.text, x0, y, TITLE_FONT, TITLE);
                y += TITLE_FONT + 3.0f;
                break;
            case ElKind::Text:
                for (const auto& line : wrap(el.text, w, FONT)) {
                    if (draw) draw_text(line, x0, y, FONT, tone_color(el.tone));
                    y += FONT + 3.0f;
                }
                break;
            case ElKind::Kv:
                if (draw) {
                    draw_text(el.text, x0, y, FONT, tone_color(Tone::Dim));
                    draw_text(el.value, x0 + w - text_w(el.value, FONT), y, FONT, tone_color(el.tone));
                }
                y += FONT + 3.0f;
                break;
            case ElKind::Buttons: {
                float x = x0;
                for (const auto& b : el.buttons) {
                    const float bw = button_w(b);
                    if (x > x0 && x + bw > x0 + w) {
                        x = x0;
                        y += BUTTON_H + 4.0f;
                    }
                    if (draw && draw_button({x, y, bw, BUTTON_H}, b)) clicked.push_back(b.action);
                    x += bw + 4.0f;
                }
                y += BUTTON_H + 4.0f;
                break;
            }
            case ElKind::Wide:
                if (draw && draw_button({x0, y, w, BUTTON_H}, el.buttons[0])) clicked.push_back(el.buttons[0].action);
                y += BUTTON_H + 3.0f;
                break;
        }
    }
    return y - area.y;
}

void panel_bg(Rectangle r) {
    DrawRectangleRec(r, PANEL_BG);
    DrawRectangleLinesEx(r, 1.0f, PANEL_BORDER);
}

void draw_column_panel(Rectangle r, const std::vector<El>& els, std::vector<Action>& clicked) {
    panel_bg(r);
    BeginScissorMode(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width), static_cast<int>(r.height));
    column(els, {r.x + PAD, r.y + PAD, r.width - 2 * PAD, r.height - 2 * PAD}, true, clicked);
    EndScissorMode();
}

// Top bar: one row, left to right.
void draw_top(Rectangle r, const std::vector<El>& els, std::vector<Action>& clicked) {
    panel_bg(r);
    float x = r.x + 10.0f;
    const float y = r.y + (r.height - BUTTON_H) / 2.0f;
    for (const El& el : els) {
        if (el.kind == ElKind::Buttons) {
            for (const auto& b : el.buttons) {
                const float bw = button_w(b);
                if (draw_button({x, y, bw, BUTTON_H}, b)) clicked.push_back(b.action);
                x += bw + 4.0f;
            }
        } else {
            draw_text(el.text, x, r.y + (r.height - FONT) / 2.0f, FONT, tone_color(el.tone));
            x += text_w(el.text, FONT);
        }
        x += 18.0f;
    }
}

}  // namespace

std::vector<Action> draw_ui(UiState& ui, const UiView& view) {
    const Panels p = build_panels(ui, view);
    const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
    const Rectangle top{0, 0, sw, TOP_H};
    const Rectangle left{0, TOP_H, LEFT_W, sh - TOP_H - BOTTOM_H};
    const Rectangle right{sw - RIGHT_W, TOP_H, RIGHT_W, sh - TOP_H - BOTTOM_H};
    const Rectangle bottom{0, sh - BOTTOM_H, sw, BOTTOM_H};

    GuiSetStyle(DEFAULT, TEXT_SIZE, static_cast<int>(FONT));
    GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL, ColorToInt(tone_color(Tone::Normal)));
    GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED, ColorToInt(BUTTON_HOVER));
    GuiSetStyle(BUTTON, TEXT_COLOR_FOCUSED, ColorToInt(tone_color(Tone::Normal)));
    GuiSetStyle(BUTTON, BASE_COLOR_PRESSED, ColorToInt(BUTTON_HOVER));
    GuiSetStyle(BUTTON, TEXT_COLOR_PRESSED, ColorToInt(tone_color(Tone::Normal)));

    std::vector<Action> clicked;
    // The open File menu sits on top: buttons under it must not react.
    const bool menu_hot = !p.menu.empty() && CheckCollisionPointRec(GetMousePosition(), ui.menu);
    if (menu_hot) GuiLock();
    draw_top(top, p.top, clicked);
    draw_column_panel(left, p.left, clicked);
    draw_column_panel(right, p.right, clicked);
    draw_column_panel(bottom, p.bottom, clicked);
    if (menu_hot) GuiUnlock();

    ui.panels = {top, left, right, bottom};
    ui.menu = {};
    if (!p.menu.empty()) {
        const float h = column(p.menu, {0, 0, 420 - 2 * PAD, 0}, false, clicked) + 2 * PAD;
        ui.menu = {4, TOP_H, 420, h};
        draw_column_panel(ui.menu, p.menu, clicked);
        ui.panels.push_back(ui.menu);
    }
    return clicked;
}

bool cursor_over_ui(const UiState& ui) {
    const Vector2 m = GetMousePosition();
    return std::ranges::any_of(ui.panels, [&](const Rectangle& r) { return CheckCollisionPointRec(m, r); });
}

void ui_actions(const std::vector<Action>& actions, UiState& ui, UiTargets t) {
    auto& queue = t.world.ctx().get<sim::CommandQueue>().value;
    for (const Action& a : actions) {
        // Selected division, if it still exists.
        entt::entity sel = entt::null;
        if (t.play.selected) {
            for (auto [e, id, prio, front] : t.world.view<const sim::UnitId, const sim::SupplyPriority, const sim::Front>().each()) {
                if (id == *t.play.selected) sel = e;
            }
        }
        if (a.kind != ActionKind::ToggleFileMenu) ui.file_menu = false;  // any other click closes the menu
        switch (a.kind) {
            case ActionKind::ToggleFileMenu: ui.file_menu = !ui.file_menu; break;
            case ActionKind::OpenMap: t.loader.open_dialog(t.file); break;
            case ActionKind::LoadTestMap: t.loader.load_test_map(); break;
            case ActionKind::Quit: t.quit = true; break;
            case ActionKind::ToggleEditor: t.editor.toggle(t.play); break;
            case ActionKind::Tool: t.editor.tool = a.tool; break;
            case ActionKind::EditSide: t.editor.side = a.side; break;
            case ActionKind::EditFlip:
                if (auto i = t.editor.selected_index(t.play.selected)) t.editor.flip_side(*i);
                break;
            case ActionKind::EditDelete:
                if (auto i = t.editor.selected_index(t.play.selected)) t.editor.delete_unit(*i, t.play.selected);
                break;
            case ActionKind::SaveScenario: t.loader.save_dialog(t.editor.baked, t.file); break;
            case ActionKind::LeftTab: ui.left = a.left_tab; break;
            case ActionKind::BottomTab: ui.bottom = a.bottom_tab; break;
            case ActionKind::Select: t.play.selected = a.unit; break;
            case ActionKind::Deselect: t.play.selected.reset(); break;
            case ActionKind::ToggleHold:
                if (sel != entt::null) {
                    queue.push_back({sim::SimCommand::SetHold{*t.play.selected, !t.world.get<sim::Front>(sel).hold}});
                }
                break;
            case ActionKind::TogglePriority:
                if (sel != entt::null) {
                    const auto p = t.world.get<sim::SupplyPriority>(sel).value;
                    queue.push_back({sim::SimCommand::SetSupplyPriority{*t.play.selected, std::uint8_t(p == 0 ? 5 : 0)}});
                }
                break;
            case ActionKind::Layer: t.play.overlay = a.overlay; break;
            case ActionKind::ToggleNetwork: t.play.show_network = !t.play.show_network; break;
            case ActionKind::Build: queue.push_back(t.play.raise_order()); break;
            case ActionKind::Sweep: queue.push_back({sim::SimCommand::AntiPartisan{a.region, a.flag}}); break;
            case ActionKind::Requisition: {
                const bool on = !t.world.ctx().get<sim::EconomyState>().requisition[a.region.value];
                queue.push_back({sim::SimCommand::SetRequisition{a.region, on}});
                break;
            }
            case ActionKind::Pause: t.play.paused = !t.play.paused; break;
            case ActionKind::Speed:
                t.play.speed = a.index;
                t.play.paused = false;
                break;
        }
    }
}

}  // namespace app
