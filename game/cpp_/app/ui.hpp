#pragma once
// C2 workstation layout (ui_improve.md, stage 1): top status bar, left ORBAT / Rear / Layers tabs,
// right context panel with orders, bottom Alerts / Event Log. Status comes first, numbers second.
// Panels are described as element lists each frame and drawn with raygui (immediate mode).

#include <cstdint>
#include <string>
#include <vector>

#include "app/common.hpp"
#include "app/editor.hpp"
#include "app/files.hpp"
#include "raylib.h"

namespace app {

inline constexpr float TOP_H = 30.0f;
inline constexpr float LEFT_W = 270.0f;
inline constexpr float RIGHT_W = 300.0f;
inline constexpr float BOTTOM_H = 150.0f;

inline constexpr Color FRIENDLY{102, 153, 204, 255};
inline constexpr Color ENEMY{199, 97, 89, 255};

enum class Tone { Normal, Dim, Info, Warn, Alert, Good, Friendly, Enemy };

enum class LeftTab { Orbat, Rear, Layers };
enum class BottomTab { Alerts, Events };

// What a button does. Only the fields named in the kind's comment are used.
enum class ActionKind {
    LeftTab,         // left_tab
    BottomTab,       // bottom_tab
    Select,          // unit
    Deselect,
    ToggleHold,
    TogglePriority,
    Layer,           // overlay
    ToggleNetwork,
    Build,
    Sweep,           // region, flag = harsh
    Requisition,     // region
    Pause,
    Speed,           // index
    ToggleFileMenu,
    OpenMap,
    LoadTestMap,
    Quit,
    ToggleEditor,    // editor on (restarts at the scenario start) / play
    Tool,            // tool
    EditSide,        // side
    EditFlip,
    EditDelete,
    SaveScenario,
    CreateCorps,
    LeaveCorps,
    DrawFront,       // index = corps id
    FrontAttack,     // index = corps id, flag = attack
    ClearFront,      // index = corps id
};

struct Action {
    ActionKind kind = ActionKind::Deselect;
    LeftTab left_tab = LeftTab::Orbat;
    BottomTab bottom_tab = BottomTab::Alerts;
    sim::UnitId unit;
    OverlayMode overlay = OverlayMode::Military;
    mapdata::RegionId region;
    bool flag = false;
    std::size_t index = 0;
    app::Tool tool = app::Tool::Select;
    std::uint8_t side = 0;
};

struct UiState {
    LeftTab left = LeftTab::Orbat;
    BottomTab bottom = BottomTab::Alerts;
    bool file_menu = false;
    // Screen rectangles of the panels drawn last frame; clicks there never reach the map.
    std::vector<Rectangle> panels;
    Rectangle menu{};  // File drop-down; empty while closed
};

// Everything the UI shows. The registry is only read; changes go through `ui_actions`.
struct UiView {
    const entt::registry& world;
    const Play& play;
    const Editor& editor;
    const FileStatus& file;
};

// Draws all panels and returns the buttons clicked this frame.
std::vector<Action> draw_ui(UiState& ui, const UiView& view);

// Things an action may change. Sim changes only go through the command queue or a reload.
struct UiTargets {
    entt::registry& world;
    Play& play;
    Editor& editor;
    Loader& loader;
    FileStatus& file;
    bool& quit;
};

void ui_actions(const std::vector<Action>& actions, UiState& ui, UiTargets t);

bool cursor_over_ui(const UiState& ui);

}  // namespace app
