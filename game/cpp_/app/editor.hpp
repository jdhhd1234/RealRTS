#pragma once
// Scenario editor (sandbox): place / drag / delete divisions, toggle depots and supply bases on
// nodes, set region owners, draw new regions (polygon or circle) and rail / road lines.
// Edits change `Editor::baked` (map + scenario source) and the sim is reloaded from it, so the map
// always shows the exact start state. Play runs from that state; going back to the editor restores it.
// Terrain comes from QGIS and is not edited.

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/common.hpp"
#include "mapdata/mapdata.hpp"

namespace app {

enum class Tool {
    Select,
    Division,
    Depot,
    Base,
    Region,  // legal owner and controller of the region under the cursor
    Occupy,  // military controller only (occupied territory)
    Border,  // polygon: cells inside become a new region
    Circle,  // circle: cells inside become a new region
    Rail,
    Road,
    Delete,
};

inline constexpr std::array<std::pair<Tool, const char*>, 11> TOOLS{{
    {Tool::Select, "Select / drag unit"},
    {Tool::Division, "Place division"},
    {Tool::Depot, "Depot on node (toggle)"},
    {Tool::Base, "Supply base on node (toggle)"},
    {Tool::Region, "Region owner"},
    {Tool::Occupy, "Occupy region"},
    {Tool::Border, "Draw region (polygon)"},
    {Tool::Circle, "Draw region (circle)"},
    {Tool::Rail, "Draw railway"},
    {Tool::Road, "Draw road"},
    {Tool::Delete, "Delete"},
}};

// Mouse state for one frame of editing, in world meters.
struct EditInput {
    mapdata::Pos cursor;
    float reach = 0.0f;  // pick radius
    bool left_pressed = false;  // went down this frame
    bool left_down = false;
    bool delete_pressed = false;
    bool finish_pressed = false;  // Enter or right click: close the polygon / end the line
    bool undo_pressed = false;    // Backspace: drop the last point
    bool cancel_pressed = false;  // Esc: throw the drawing away
    bool over_ui = false;
};

class Editor {
public:
    explicit Editor(mapdata::Baked source);

    void set_tool(Tool t);
    // A newly loaded map replaces the scenario source (the sim was already reset by the loader).
    void set_source(mapdata::Baked source);
    // Editor: restart at the scenario start and freeze. Play: run from there.
    void toggle(Play& play);
    // Unit index of the selection; divisions get ids index + 1 on load.
    std::optional<std::size_t> selected_index(const std::optional<sim::UnitId>& selected) const;
    void delete_unit(std::size_t i, std::optional<sim::UnitId>& selected);
    void flip_side(std::size_t i);
    // Rebuilds the sim from the edited scenario when something changed (tick 0, paused). True if it did.
    bool apply_edits(entt::registry& world);
    void edit_mouse(const EditInput& in, const mapdata::MapData& m, std::optional<sim::UnitId>& selected);

    bool on = false;
    Tool tool = Tool::Select;
    std::uint8_t side = PLAYER_SIDE;  // side used by placing tools
    mapdata::Baked baked;  // scenario source: play starts from here
    bool reload = false;
    // Unit being dragged and where its symbol is drawn now; the scenario changes on release.
    std::optional<std::size_t> drag;
    mapdata::Pos drag_pos;
    // Points of the polygon / line being drawn; for Circle: center, then the rim point under the cursor.
    std::vector<mapdata::Pos> draft;
    std::string message;  // why the last click did nothing; shown in the tool panel

private:
    mapdata::ScenarioData& scenario();
    mapdata::MapData& map();
    void finish_draft(float snap);
};

const char* side_label(std::uint8_t side);

// Removes every entry at `node`; adds `(side, node)` if there was none. One per node.
void toggle_at(std::vector<mapdata::SideNode>& list, std::uint8_t side, mapdata::NodeId node);

// Cells whose center lies inside become one new region of `side` (legal owner and controller).
// Output moves with the cells: each source region gives up the share of cells it lost.
// Regions left without cells are removed. Returns the number of cells moved; 0 changes nothing.
std::size_t carve_polygon(mapdata::Baked& b, const std::vector<mapdata::Pos>& polygon, std::uint8_t side);
std::size_t carve_circle(mapdata::Baked& b, mapdata::Pos center, float radius, std::uint8_t side);

// Rail or road through `points`. A point within `snap` of a node joins that node, otherwise it becomes
// a new junction. Returns the number of edges added.
std::size_t add_line(mapdata::MapData& m, const std::vector<mapdata::Pos>& points, mapdata::EdgeKind kind, float snap);

}  // namespace app
