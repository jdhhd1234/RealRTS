// RealRTS app: Raylib window, camera, map rendering, input. The sim is read-only here;
// player orders go through sim::CommandQueue, map loads and editor edits through sim::load_scenario.
//
// World coordinates are meters with y north (up). Raylib's 2D camera has y down, so world points are
// drawn at (x, -y): `to_draw` is the only place the y axis flips (architect.md 6).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

#include "app/common.hpp"
#include "app/editor.hpp"
#include "app/files.hpp"
#include "app/symbols.hpp"
#include "app/ui.hpp"
#include "raylib.h"
#include "raymath.h"

namespace app {
namespace {

// More ticks than this in one frame are dropped, so a slow frame cannot snowball.
constexpr int MAX_TICKS_PER_FRAME = 240;

Vector2 to_draw(mapdata::Pos p) { return {p.x, -p.y}; }

using Rgba = std::array<unsigned char, 4>;

// Owns a CPU-side RGBA buffer and its GPU texture of the same size.
struct MapImage {
    int w = 0;
    int h = 0;
    std::vector<Rgba> pixels;
    Texture texture;

    // Uploads `pixels`; recreates the texture only when the size changed.
    void upload(int width, int height) {
        if (!texture.loaded() || width != w || height != h) {
            w = width;
            h = height;
            Image img{pixels.data(), w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            texture = Texture(img);
        } else {
            UpdateTexture(texture.get(), pixels.data());
        }
    }

    // Covers the whole map in draw space.
    void draw(const mapdata::TerrainGrid& grid) const {
        const auto [w_m, h_m] = grid.size_m();
        const Rectangle src{0, 0, static_cast<float>(w), static_cast<float>(h)};
        DrawTexturePro(texture.get(), src, {0, -h_m, w_m, h_m}, {0, 0}, 0, WHITE);
    }
};

Rgba terrain_color(mapdata::Terrain t) {
    switch (t) {
        case mapdata::Terrain::Plains: return {196, 208, 160, 255};
        case mapdata::Terrain::Forest: return {90, 140, 80, 255};
        case mapdata::Terrain::Marsh: return {130, 160, 150, 255};
        case mapdata::Terrain::Mountain: return {150, 130, 110, 255};
        case mapdata::Terrain::Urban: return {170, 170, 170, 255};
        case mapdata::Terrain::Water: return {90, 140, 210, 255};
    }
    return {0, 0, 0, 255};
}

void terrain_image(const mapdata::TerrainGrid& grid, MapImage& out) {
    out.pixels.clear();
    out.pixels.reserve(static_cast<std::size_t>(grid.width) * grid.height);
    // Image rows go top-down; grid rows go bottom-up.
    for (std::uint32_t cy = grid.height; cy-- > 0;) {
        for (std::uint32_t cx = 0; cx < grid.width; ++cx) out.pixels.push_back(terrain_color(grid.get(cx, cy)));
    }
    out.upload(static_cast<int>(grid.width), static_cast<int>(grid.height));
}

void overlay_image(OverlayMode mode, const sim::Territory& territory, const mapdata::MapData& m,
                   const sim::PartisanState& partisans, MapImage& out) {
    const std::uint32_t w = m.terrain.width, h = m.terrain.height;
    // Amber shade by risk grade; only the grade is visible to the player (D5).
    const auto risk = [&](std::size_t r) -> Rgba {
        switch (sim::risk_grade(partisans.pool[r])) {
            case sim::RiskGrade::None: return {0, 0, 0, 0};
            case sim::RiskGrade::Low: return {0xf2, 0xb3, 0x40, 50};
            case sim::RiskGrade::Medium: return {0xf2, 0x8c, 0x28, 100};
            case sim::RiskGrade::High: return {0xe0, 0x40, 0x30, 140};
        }
        return {0, 0, 0, 0};
    };
    // Concept-diagram colors: blue #3574c1, red #c95049 at 0.35 alpha, border #475569, region border #64748b.
    const auto fill = [](std::uint32_t side) -> Rgba {
        return side == PLAYER_SIDE ? Rgba{0x35, 0x74, 0xc1, 90} : Rgba{0xc9, 0x50, 0x49, 90};
    };
    const auto owner = [&](std::uint32_t cx, std::uint32_t cy) -> std::uint32_t {
        switch (mode) {
            case OverlayMode::Military: return territory.military[cy * w + cx];
            case OverlayMode::Legal: return territory.legal[m.region_at(cx, cy).value];
            default: return m.region_at(cx, cy).value;
        }
    };
    out.pixels.clear();
    out.pixels.reserve(static_cast<std::size_t>(w) * h);
    for (std::uint32_t cy = h; cy-- > 0;) {
        for (std::uint32_t cx = 0; cx < w; ++cx) {
            if (mode == OverlayMode::Off) {
                out.pixels.push_back({0, 0, 0, 0});
                continue;
            }
            const std::uint32_t o = owner(cx, cy);
            const bool border = (cx + 1 < w && owner(cx + 1, cy) != o) || (cy + 1 < h && owner(cx, cy + 1) != o);
            Rgba px;
            if (mode == OverlayMode::Regions || mode == OverlayMode::Risk) {
                px = border ? Rgba{0x64, 0x74, 0x8b, 200} : mode == OverlayMode::Risk ? risk(o) : Rgba{0, 0, 0, 0};
            } else {
                px = border ? Rgba{0x47, 0x55, 0x69, 230} : fill(o);
            }
            out.pixels.push_back(px);
        }
    }
    out.upload(static_cast<int>(w), static_cast<int>(h));
}

// What the overlay was last drawn from; redrawn only when this changes.
struct OverlayKey {
    OverlayMode mode = OverlayMode::Off;
    std::vector<std::uint8_t> legal;
    std::vector<std::uint8_t> military;
    std::vector<sim::RiskGrade> risk;
    bool operator==(const OverlayKey&) const = default;
};

OverlayKey overlay_key(OverlayMode mode, const entt::registry& world) {
    const auto& t = world.ctx().get<sim::Territory>();
    OverlayKey k{mode, t.legal, t.military, {}};
    if (mode == OverlayMode::Risk) {
        for (float pool : world.ctx().get<sim::PartisanState>().pool) k.risk.push_back(sim::risk_grade(pool));
    }
    return k;
}

// World units per screen pixel.
float world_per_px(const Camera2D& cam) { return 1.0f / cam.zoom; }

mapdata::Pos cursor_world(const Camera2D& cam) {
    const Vector2 p = GetScreenToWorld2D(GetMousePosition(), cam);
    return {p.x, -p.y};
}

// Fits the whole map into the free area between the panels.
void fit_camera(Camera2D& cam, const mapdata::TerrainGrid& grid) {
    const auto [w_m, h_m] = grid.size_m();
    const float free_w = std::max(100.0f, static_cast<float>(GetScreenWidth()) - LEFT_W - RIGHT_W);
    const float free_h = std::max(100.0f, static_cast<float>(GetScreenHeight()) - TOP_H - BOTTOM_H);
    const float scale = std::max(w_m / free_w, h_m / free_h) * 1.05f;
    cam.zoom = 1.0f / scale;
    cam.target = to_draw({w_m / 2.0f, h_m / 2.0f});
    cam.offset = {LEFT_W + free_w / 2.0f, TOP_H + free_h / 2.0f};
    cam.rotation = 0.0f;
}

// WASD / arrows: pan. Mouse wheel: zoom (around the screen center of the map area).
void camera_controls(Camera2D& cam, bool over_ui) {
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f && !over_ui) {
        const float scale = std::clamp(world_per_px(cam) * std::pow(0.9f, wheel), 5.0f, 5000.0f);
        cam.zoom = 1.0f / scale;
    }
    Vector2 dir{0, 0};
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) dir.y -= 1;
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) dir.y += 1;
    if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) dir.x -= 1;
    if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) dir.x += 1;
    cam.target = Vector2Add(cam.target, Vector2Scale(dir, world_per_px(cam) * 800.0f * GetFrameTime()));
}

void push(entt::registry& world, sim::SimCommand cmd) { world.ctx().get<sim::CommandQueue>().value.push_back(std::move(cmd)); }

// Space: pause/resume. 1-4: 1x / 2x / 10x / 60x.
void speed_controls(Play& play) {
    if (IsKeyPressed(KEY_SPACE)) play.paused = !play.paused;
    constexpr KeyboardKey digits[] = {KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR};
    for (std::size_t i = 0; i < 4; ++i) {
        if (IsKeyPressed(digits[i])) play.speed = i;
    }
}

// Left click: select own unit. Right click: order the selected unit to move there.
void mouse_orders(entt::registry& world, Play& play, const Camera2D& cam, bool over_ui) {
    if (over_ui) return;
    const mapdata::Pos p = cursor_world(cam);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        // Nearest own unit whose symbol is under the cursor. Partisans are not player-controlled yet (A5).
        float best = SYMBOL_PX * 0.7f * world_per_px(cam);
        play.selected.reset();
        auto v = world.view<const sim::UnitId, const sim::Side, const sim::Position>(entt::exclude<sim::Partisan>);
        for (auto [e, id, side, pos] : v.each()) {
            const float d = std::hypot(pos.value.x - p.x, pos.value.y - p.y);
            if (side.value == PLAYER_SIDE && d < best) {
                best = d;
                play.selected = id;
            }
        }
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && play.selected) push(world, {sim::SimCommand::Move{*play.selected, p}});
}

// P: toggle supply priority (0 / 5) of the selected unit. G: toggle hold (no retreat).
void unit_keys(entt::registry& world, const Play& play) {
    if (!play.selected) return;
    for (auto [e, id, prio, front] : world.view<const sim::UnitId, const sim::SupplyPriority, const sim::Front>().each()) {
        if (id != *play.selected) continue;
        if (IsKeyPressed(KEY_P)) {
            push(world, {sim::SimCommand::SetSupplyPriority{id, std::uint8_t(prio.value == 0 ? 5 : 0)}});
        }
        if (IsKeyPressed(KEY_G)) push(world, {sim::SimCommand::SetHold{id, !front.hold}});
        return;
    }
}

// Region keys act on the region under the cursor: K sweep, H harsh reprisals, Q toggle requisition.
// B raises a new division for the player.
void region_keys(entt::registry& world, Play& play, const Camera2D& cam, bool over_ui) {
    if (IsKeyPressed(KEY_B)) push(world, play.raise_order());
    if (over_ui) return;
    const auto& m = world.ctx().get<sim::Map>().value;
    const auto cell = m.terrain.cell_at(cursor_world(cam));
    if (!cell) return;
    const mapdata::RegionId region = m.region_at(cell->x, cell->y);
    if (IsKeyPressed(KEY_K)) push(world, {sim::SimCommand::AntiPartisan{region, false}});
    if (IsKeyPressed(KEY_H)) push(world, {sim::SimCommand::AntiPartisan{region, true}});
    if (IsKeyPressed(KEY_Q)) {
        const bool on = !world.ctx().get<sim::EconomyState>().requisition[region.value];
        push(world, {sim::SimCommand::SetRequisition{region, on}});
    }
}

void overlay_key_press(Play& play) {
    if (!IsKeyPressed(KEY_T)) return;
    switch (play.overlay) {
        case OverlayMode::Military: play.overlay = OverlayMode::Legal; break;
        case OverlayMode::Legal: play.overlay = OverlayMode::Regions; break;
        case OverlayMode::Regions: play.overlay = OverlayMode::Risk; break;
        case OverlayMode::Risk: play.overlay = OverlayMode::Off; break;
        case OverlayMode::Off: play.overlay = OverlayMode::Military; break;
    }
}

void draw_arrow(Vector2 from, Vector2 to, float head, Color c) {
    DrawLineV(from, to, c);
    const Vector2 back = Vector2Scale(Vector2Normalize(Vector2Subtract(from, to)), head);
    DrawLineV(to, Vector2Add(to, Vector2Rotate(back, 0.5f)), c);
    DrawLineV(to, Vector2Add(to, Vector2Rotate(back, -0.5f)), c);
}

// Transport graph and division lines, drawn under the symbols.
void draw_network(const entt::registry& world, const Play& play, float px) {
    const auto& m = world.ctx().get<sim::Map>().value;
    const auto& g = m.graph;
    const auto& es = world.ctx().get<sim::EdgeState>();
    if (play.show_network) {
        for (std::size_t i = 0; i < g.edges.size(); ++i) {
            const auto& e = g.edges[i];
            // Damaged (sabotaged) edges red, edges carrying supply orange.
            Color c;
            if (es.damage[i] > 0.0f) {
                c = ColorAlpha({230, 26, 26, 255}, 0.4f + 0.6f * es.damage[i]);
            } else if (es.last_flow[i] > 0.0f) {
                c = {255, 140, 0, 255};
            } else if (e.kind == mapdata::EdgeKind::Rail) {
                c = {26, 26, 26, 255};
            } else if (e.kind == mapdata::EdgeKind::Road) {
                c = {140, 89, 38, 255};
            } else {
                c = {51, 77, 204, 255};
            }
            DrawLineV(to_draw(g.node(e.a).pos), to_draw(g.node(e.b).pos), c);
        }
        for (const auto& n : g.nodes) DrawCircleLinesV(to_draw(n.pos), 3.0f * px, BLACK);
    }
    // Division lines (frontage across the facing) with a tick toward the enemy side, and depth.
    const float ring = SYMBOL_PX * 0.75f * px;
    for (auto [e, pos, side, d, b] :
         world.view<const sim::Position, const sim::Side, const sim::Deployment, const sim::Battalions>().each()) {
        const auto l = sim::layout(b, d.posture);
        const auto [a, z] = sim::line_ends(pos.value, d.facing, l.frontage_m);
        const Color c = side.value == PLAYER_SIDE ? FRIENDLY : ENEMY;
        const Vector2 p0 = to_draw(pos.value);
        const Vector2 f{d.facing.first, -d.facing.second};
        DrawLineV(to_draw(a), to_draw(z), c);
        DrawLineV(p0, Vector2Add(p0, Vector2Scale(f, ring * 1.6f)), c);
        DrawLineV(p0, Vector2Subtract(p0, Vector2Scale(f, l.depth_m)), ColorAlpha(c, 0.25f));
    }
}

// Symbols in screen space so they keep a constant size at every zoom.
void draw_symbols(const entt::registry& world, const Editor& ed, const Symbols& symbols, const Camera2D& cam) {
    const auto& m = world.ctx().get<sim::Map>().value;
    const auto screen = [&](mapdata::Pos p) { return GetWorldToScreen2D(to_draw(p), cam); };
    // Depots get a supply symbol; it switches sides when the depot is captured.
    for (auto [e, d] : world.view<const sim::Depot>().each()) {
        symbols.draw(d.side, Kind::Depot, screen(m.graph.node(d.node).pos), SYMBOL_PX * 0.8f);
    }
    for (auto [e, id, side, pos, bats] :
         world.view<const sim::UnitId, const sim::Side, const sim::Position, const sim::Battalions>().each()) {
        const bool partisan = world.all_of<sim::Partisan>(e);
        Kind kind = Kind::Infantry;
        if (partisan) {
            kind = Kind::Partisan;
        } else if (main_arm(bats) == sim::Arm::Armor) {
            kind = Kind::Armor;
        } else if (main_arm(bats) == sim::Arm::Artillery) {
            kind = Kind::Artillery;
        }
        // shortcut: snaps to the sim position each frame, no interpolation between ticks yet.
        mapdata::Pos p = pos.value;
        if (ed.drag && id.value == *ed.drag + 1) p = ed.drag_pos;
        symbols.draw(side.value, kind, screen(p), partisan ? SYMBOL_PX * 0.75f : SYMBOL_PX);
    }
}

// Combat links, pockets, retreats, selection ring and remaining path, drawn over the symbols.
void draw_markers(const entt::registry& world, const Play& play, float px) {
    const auto& m = world.ctx().get<sim::Map>().value;
    const float ring = SYMBOL_PX * 0.75f * px;
    for (const auto& [a, b] : world.ctx().get<sim::Engagements>().value) {
        const auto* pa = world.valid(a) ? world.try_get<sim::Position>(a) : nullptr;
        const auto* pb = world.valid(b) ? world.try_get<sim::Position>(b) : nullptr;
        if (pa && pb) DrawLineV(to_draw(pa->value), to_draw(pb->value), {255, 26, 26, 255});
    }
    // Pockets: magenta ring. Retreat: white arrow along the retreat direction.
    for (auto [e, pos, f] : world.view<const sim::Position, const sim::Front>().each()) {
        const Vector2 p = to_draw(pos.value);
        if (f.encircled) DrawCircleLinesV(p, ring * 1.25f, MAGENTA);
        if (f.retreat) {
            const Vector2 dir{f.retreat->first, -f.retreat->second};
            draw_arrow(p, Vector2Add(p, Vector2Scale(dir, ring * 2.0f)), ring * 0.5f, WHITE);
        }
    }
    if (!play.selected) return;
    for (auto [e, id, pos] : world.view<const sim::UnitId, const sim::Position>().each()) {
        if (id != *play.selected) continue;
        constexpr Color yellow{255, 230, 0, 255};
        DrawCircleLinesV(to_draw(pos.value), ring, yellow);
        if (const auto* order = world.try_get<sim::MoveOrder>(e)) {
            const float cell_m = m.terrain.cell_m;
            Vector2 prev = to_draw(pos.value);
            for (std::size_t i = order->next; i < order->path.size(); ++i) {
                const auto c = order->path[i];
                const Vector2 next = to_draw({(static_cast<float>(c.x) + 0.5f) * cell_m, (static_cast<float>(c.y) + 0.5f) * cell_m});
                DrawLineV(prev, next, yellow);
                prev = next;
            }
        }
        return;
    }
}

// Editor markers: every node, and supply bases (not drawn in play) as side-colored squares.
void draw_editor(const Editor& ed, const mapdata::MapData& m, float px) {
    for (const auto& n : m.graph.nodes) DrawCircleLinesV(to_draw(n.pos), 4.0f * px, BLACK);
    for (const auto& b : ed.baked.scenario.bases) {
        const Vector2 p = to_draw(m.graph.node(b.node).pos);
        const Color c = b.side == PLAYER_SIDE ? FRIENDLY : ENEMY;
        for (float k : {1.1f, 1.0f}) {
            const float s = SYMBOL_PX * k * px;
            DrawRectangleLinesEx({p.x - s / 2, p.y - s / 2, s, s}, px, c);
        }
    }
}

int run(int argc, char** argv) {
    // `app [map.gpkg|map.json|layer.geojson]` opens that map; otherwise the built-in test map.
    FileStatus file;
    mapdata::Baked baked = mapdata::test_baked();
    if (argc > 1) {
        try {
            auto [b, label] = read_map(path_from_utf8(argv[1]));
            baked = std::move(b);
            file.text = "Map: " + label;
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
            file = {"Load failed: " + std::string(e.what()), true, false};
        }
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1600, 900, "RealRTS");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);  // Esc must not quit

    {  // GPU resources must be released before CloseWindow.
        Symbols symbols;
        symbols.load(std::string(ASSETS_DIR) + "/symbols");
        MapImage terrain, overlay;
        std::optional<OverlayKey> overlay_last;

        entt::registry world;
        Editor editor(baked);
        sim::load_scenario(world, std::move(baked));
        Play play;
        UiState ui;
        Loader loader;
        Camera2D cam{};
        bool map_changed = true;  // rebuild terrain and fit the camera
        bool quit = false;
        double acc = 0.0;

        while (!WindowShouldClose() && !quit) {
            // Swap in a finished load: the whole sim restarts on the new map, paused.
            if (auto msg = loader.try_recv()) {
                FileStatus status;
                if (msg->ok && !msg->baked) {
                    status.text = msg->text;
                } else if (msg->ok) {
                    status.text = loaded_text(*msg->baked, msg->text);
                    editor.set_source(*msg->baked);
                    sim::load_scenario(world, std::move(*msg->baked));
                    play.selected.reset();
                    play.paused = true;
                    map_changed = true;
                } else if (msg->text.empty()) {
                    status.text = file.text;  // cancelled
                } else {
                    status.text = msg->text.starts_with("Save failed") ? msg->text : "Load failed: " + msg->text;
                    status.error = true;
                }
                file = status;
            }
            editor.apply_edits(world);

            const auto& m = world.ctx().get<sim::Map>().value;
            if (map_changed) {
                terrain_image(m.terrain, terrain);
                fit_camera(cam, m.terrain);
                overlay_last.reset();
                map_changed = false;
            }

            const bool over_ui = cursor_over_ui(ui);
            if (editor.on) {
                editor.edit_mouse({cursor_world(cam), SYMBOL_PX * 0.7f * world_per_px(cam), IsMouseButtonPressed(MOUSE_BUTTON_LEFT),
                                   IsMouseButtonDown(MOUSE_BUTTON_LEFT), IsKeyPressed(KEY_DELETE), over_ui},
                                  m, play.selected);
            } else {
                speed_controls(play);
                mouse_orders(world, play, cam, over_ui);
                unit_keys(world, play);
                region_keys(world, play, cam, over_ui);
            }
            camera_controls(cam, over_ui);
            overlay_key_press(play);

            // Fixed ticks: speed changes how many run per second, never the tick size.
            if (!play.paused) {
                acc += GetFrameTime() * SPEEDS[play.speed];
                int n = 0;
                while (acc >= 1.0 && n++ < MAX_TICKS_PER_FRAME) {
                    sim::step(world);
                    acc -= 1.0;
                }
                if (acc >= 1.0) acc = 0.0;
            }

            OverlayKey key = overlay_key(play.overlay, world);
            if (overlay_last != key) {
                overlay_image(play.overlay, world.ctx().get<sim::Territory>(), m, world.ctx().get<sim::PartisanState>(),
                              overlay);
                overlay_last = std::move(key);
            }

            BeginDrawing();
            ClearBackground({13, 15, 18, 255});
            const float px = world_per_px(cam);
            BeginMode2D(cam);
            terrain.draw(m.terrain);
            overlay.draw(m.terrain);
            draw_network(world, play, px);
            EndMode2D();
            draw_symbols(world, editor, symbols, cam);
            BeginMode2D(cam);
            draw_markers(world, play, px);
            if (editor.on) draw_editor(editor, m, px);
            EndMode2D();
            const auto clicked = draw_ui(ui, {world, play, editor, file});
            EndDrawing();
            ui_actions(clicked, ui, {world, play, editor, loader, file, quit});
        }
    }
    CloseWindow();
    return 0;
}

}  // namespace
}  // namespace app

int main(int argc, char** argv) { return app::run(argc, argv); }
