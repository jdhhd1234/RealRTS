#pragma once
// QGIS data -> `mapdata::Baked` (architect.md 5.4). Input is one GeoPackage, or a folder of
// GeoJSON / Shapefile files named after the layers (`terrain.shp`, `rail.geojson`, ...).
//
// Layer contract (table names, case-insensitive; every layer optional, unknown layers ignored):
// - `terrain`  polygons, `kind` = plains | forest | marsh | mountain | urban | water (default plains)
// - `regions`  polygons, `name`, `side` (legal owner, default 0), `control` (holder at start, default side),
//   `fuel` `ammo` `food` `equipment` (t/day output, default 0)
// - `nodes`    points, `name`, `kind` = junction | station | port, `base_side`, `depot_side`
// - `rail`     lines, `gauge` (default 0), `capacity` (t/day, default 3000)
// - `roads`    lines, `capacity` (default 600)
// - `rivers`   lines, `capacity` (default 1500)
// - `units`    points, `name`, `side` (default 0)
//
// All layers must share one projected (metric) CRS; geographic CRS (degrees) is rejected because
// projection is done in QGIS, never in the game. Line ends snap to the nearest node within 2 km,
// otherwise a junction node is created there.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mapbake/folder.hpp"
#include "mapbake/geom.hpp"
#include "mapdata/mapdata.hpp"

struct sqlite3;

namespace mapbake {

inline constexpr double CELL_M = 1000.0;
// shortcut: caps the grid at 2000 x 2000 km (16 MB of cells); raise with chunked storage if needed.
inline constexpr std::uint32_t MAX_CELLS_PER_SIDE = 2000;
inline constexpr double SNAP_M = 2000.0;
inline constexpr std::array<std::string_view, 7> LAYERS = {
    "terrain", "regions", "nodes", "rail", "roads", "rivers", "units",
};

enum class AttrKind : std::uint8_t { Num, Text, Null };

struct Attr {
    AttrKind kind = AttrKind::Null;
    double num_val = 0.0;
    std::string text_val;

    static Attr num(double v) { return Attr{AttrKind::Num, v, {}}; }
    static Attr text(std::string s) { return Attr{AttrKind::Text, 0.0, std::move(s)}; }
    static Attr null() { return Attr{AttrKind::Null, 0.0, {}}; }
};

struct Feature {
    Geom geom;
    std::vector<std::pair<std::string, Attr>> attrs;

    const Attr* get(std::string_view key) const;
    std::optional<std::string> text(std::string_view key) const;
    std::optional<double> num(std::string_view key) const;
    std::optional<std::uint8_t> side(std::string_view key) const;
};

using LayerFeatures = std::pair<std::string, std::vector<Feature>>;

// Format-neutral core: features per known layer (already checked to be in one projected CRS).
mapdata::Baked bake_layers(std::span<const LayerFeatures> layers, std::string_view name);

// A folder, or any file in it with a GeoJSON/Shapefile extension, bakes the whole folder;
// anything else is opened as a GeoPackage.
mapdata::Baked bake_path(const std::filesystem::path& path);

mapdata::Baked bake_file(const std::filesystem::path& path);

// QUESTION: Rust `bake(conn: &rusqlite::Connection, name: &str)` takes a rusqlite Connection;
// in C++ with the SQLite amalgamation, `sqlite3*` is used directly as the connection handle.
mapdata::Baked bake(sqlite3* conn, std::string_view name);

// String utilities shared across mapbake translation units.
std::string to_ascii_lowercase(std::string_view s);
std::string to_ascii_uppercase(std::string_view s);
bool eq_ignore_ascii_case(std::string_view a, std::string_view b);
std::string_view trim(std::string_view s);
std::string_view trim_start(std::string_view s);
std::string path_to_utf8(const std::filesystem::path& p);

}  // namespace mapbake
