#pragma once
// Scenario editor (sandbox): place / drag / delete divisions, toggle depots and supply bases on
// nodes, set region owners. Edits change `Editor::baked` (the scenario source) and the sim is
// reloaded from it, so the map always shows the exact start state. Play runs from that state;
// going back to the editor restores it. Terrain and the network come from QGIS and are not edited.

#include <array>
#include <optional>
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
    Delete,
};

inline constexpr std::array<std::pair<Tool, const char*>, 7> TOOLS{{
    {Tool::Select, "Select / drag unit"},
    {Tool::Division, "Place division"},
    {Tool::Depot, "Depot on node (toggle)"},
    {Tool::Base, "Supply base on node (toggle)"},
    {Tool::Region, "Region owner"},
    {Tool::Occupy, "Occupy region"},
    {Tool::Delete, "Delete"},
}};

// Mouse state for one frame of editing, in world meters.
struct EditInput {
    mapdata::Pos cursor;
    float reach = 0.0f;  // pick radius
    bool left_pressed = false;  // went down this frame
    bool left_down = false;
    bool delete_pressed = false;
    bool over_ui = false;
};

class Editor {
public:
    explicit Editor(mapdata::Baked source);

    // A newly loaded map replaces the scenario source (the sim was already reset by the loader).
    void set_source(mapdata::Baked source);
    // Editor: restart at the scenario start and freeze. Play: run from there.
    void toggle(Play& play);
    // Unit index of the selection; divisions get ids index + 1 on load.
    std::optional<std::size_t> selected_index(const std::optional<sim::UnitId>& selected) const;
    void delete_unit(std::size_t i, std::optional<sim::UnitId>& selected);
    void flip_side(std::size_t i);
    // Rebuilds the sim from the edited scenario when something changed (tick 0, paused).
    void apply_edits(entt::registry& world);
    void edit_mouse(const EditInput& in, const mapdata::MapData& m, std::optional<sim::UnitId>& selected);

    bool on = false;
    Tool tool = Tool::Select;
    std::uint8_t side = PLAYER_SIDE;  // side used by placing tools
    mapdata::Baked baked;  // scenario source: play starts from here
    bool reload = false;
    // Unit being dragged and where its symbol is drawn now; the scenario changes on release.
    std::optional<std::size_t> drag;
    mapdata::Pos drag_pos;

private:
    mapdata::ScenarioData& scenario();
};

const char* side_label(std::uint8_t side);

// Removes every entry at `node`; adds `(side, node)` if there was none. One per node.
void toggle_at(std::vector<mapdata::SideNode>& list, std::uint8_t side, mapdata::NodeId node);

}  // namespace app
