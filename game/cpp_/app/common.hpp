#pragma once
// Presentation state shared by the app modules. Nothing here affects sim results.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "sim/sim.hpp"

namespace app {

// Speed steps in ticks per real second. Tick size never changes, only how many run per second.
inline constexpr std::array<double, 4> SPEEDS{1.0, 2.0, 10.0, 60.0};
// Player controls this side. shortcut: hard-coded until scenario setup exists.
inline constexpr std::uint8_t PLAYER_SIDE = 0;

// Territory overlay layer (architect.md 12.5). T cycles.
enum class OverlayMode { Military, Legal, Regions, Risk, Off };

const char* overlay_name(OverlayMode mode);

// Play-mode controls: speed, pause, selection and layer toggles.
struct Play {
    std::optional<sim::UnitId> selected;
    std::size_t speed = 0;  // index into SPEEDS
    bool paused = false;
    bool show_network = true;
    OverlayMode overlay = OverlayMode::Military;
    std::uint32_t raised = 0;  // counter for new division names
    // Corps whose front line is being drawn, and the points so far (world meters).
    std::optional<std::uint32_t> front_corps;
    std::vector<mapdata::Pos> front_draft;

    // Order for a new player division with the next free name.
    sim::SimCommand raise_order();
};

}  // namespace app
