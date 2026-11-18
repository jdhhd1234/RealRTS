#pragma once
// Static map data (architect.md 4.1, 5). Only what never changes during a game lives here;
// runtime state such as edge damage or region control belongs to `sim`.
//
// Coordinates: meters in the scenario's projected CRS. Origin at the map's bottom-left,
// x east, y north.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mapdata {

enum class Terrain : std::uint8_t { Plains, Forest, Marsh, Mountain, Urban, Water };

struct Pos {
    float x = 0.0f;
    float y = 0.0f;
    bool operator==(const Pos&) const = default;
};

struct Cell {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    bool operator==(const Cell&) const = default;
};

// Square grid, row-major from the bottom-left cell. Rendering is continuous; only sim math uses cells.
struct TerrainGrid {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float cell_m = 0.0f;
    std::vector<Terrain> cells;

    static TerrainGrid filled(std::uint32_t width, std::uint32_t height, float cell_m, Terrain t);
    std::pair<float, float> size_m() const;
    // Cell containing a point, or nullopt outside the map.
    std::optional<Cell> cell_at(Pos pos) const;
    Terrain get(std::uint32_t cx, std::uint32_t cy) const;
    void set(std::uint32_t cx, std::uint32_t cy, Terrain t);

    bool operator==(const TerrainGrid&) const = default;
};

struct NodeId {
    std::uint32_t value = 0;
    bool operator==(const NodeId&) const = default;
};

enum class NodeKind : std::uint8_t { Junction, Station, Port };

struct Node {
    Pos pos;
    NodeKind kind = NodeKind::Junction;
    std::optional<std::string> name;
    bool operator==(const Node&) const = default;
};

enum class EdgeKind : std::uint8_t { Rail, Road, River };

// Undirected transport link.
struct Edge {
    NodeId a;
    NodeId b;
    EdgeKind kind = EdgeKind::Road;
    // Rail only: gauge id (meaning set by scenario Lua). Different gauges need engineers to convert (E3).
    std::uint8_t gauge = 0;
    float length_m = 0.0f;
    float capacity_t_per_day = 0.0f;
    bool operator==(const Edge&) const = default;
};

struct TransportGraph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;

    const Node& node(NodeId id) const { return nodes[id.value]; }
    bool operator==(const TransportGraph&) const = default;
};

struct RegionId {
    std::uint16_t value = 0;
    bool operator==(const RegionId&) const = default;
};

// Administrative region (architect.md 12): fixed unit for economy, population, partisans, legal ownership.
struct Region {
    std::string name;
    // Tons per day of fuel, ammo, food, equipment at full output (G1). mapbake will sum industry points.
    std::array<float, 4> daily_output{};
    bool operator==(const Region&) const = default;
};

struct MapData {
    std::string name;
    TerrainGrid terrain;
    TransportGraph graph;
    std::vector<Region> regions;
    // Region of each terrain cell, same layout as `TerrainGrid::cells`.
    std::vector<RegionId> region_cells;

    // Map with a single region covering everything (for tests and quick setups).
    static MapData single_region(const std::string& name, TerrainGrid terrain, TransportGraph graph);
    RegionId region_at(std::uint32_t cx, std::uint32_t cy) const;

    // Parse and validate. Map files are untrusted input (user/Lua-made), so always validate.
    static MapData from_json(const std::string& text);
    std::string to_json() const;
    // Throws MapError.
    void validate() const;

    bool operator==(const MapData&) const = default;
};

struct SideNode {
    std::uint8_t side = 0;
    NodeId node;
    bool operator==(const SideNode&) const = default;
};

struct UnitSpec {
    std::uint8_t side = 0;
    std::string name;
    Pos pos;
    bool operator==(const UnitSpec&) const = default;
};

// Corps (XXX): 2-5 divisions of one side under one command.
inline constexpr std::size_t CORPS_MIN = 2;
inline constexpr std::size_t CORPS_MAX = 5;

struct CorpsSpec {
    std::uint8_t side = 0;
    std::string name;
    std::vector<std::uint32_t> units;  // indices into `ScenarioData::units`, in order along the front
    bool operator==(const CorpsSpec&) const = default;
};

// Starting situation that comes with a map (QGIS attributes today; Lua scenarios later).
struct ScenarioData {
    // Legal owner per region (same order as `MapData::regions`).
    std::vector<std::uint8_t> legal;
    // Side holding each region militarily at start (usually equal to `legal`).
    std::vector<std::uint8_t> control;
    std::vector<SideNode> bases;   // supply bases
    std::vector<SideNode> depots;  // field depots
    std::vector<UnitSpec> units;
    std::vector<CorpsSpec> corps;  // each unit in at most one corps
    bool operator==(const ScenarioData&) const = default;
};

// A map plus its starting situation; what `mapbake` produces and the game opens.
struct Baked {
    MapData map;
    ScenarioData scenario;

    static Baked from_json(const std::string& text);
    std::string to_json() const;
    // Map checks plus scenario references (untrusted file input). Throws MapError.
    void validate() const;

    bool operator==(const Baked&) const = default;
};

enum class MapErrorKind {
    Scenario,
    Parse,
    CellCount,
    BadCellSize,
    EdgeNode,
    SelfLoop,
    BadEdgeValue,
    NodeOutside,
    RegionCellCount,
    BadRegionId,
};

class MapError : public std::runtime_error {
public:
    // `index`: offending edge/node/cell for the kinds that have one.
    MapError(MapErrorKind kind, const std::string& message, std::size_t index = 0);

    MapErrorKind kind() const { return kind_; }
    std::size_t index() const { return index_; }

private:
    MapErrorKind kind_;
    std::size_t index_;
};

// `test_map` with its demo situation: east region column Red, the rest Blue; Red already holds
// N Province (legally Blue), like the occupied city in military_area.svg.
Baked test_baked();

// Hand-made 300 km x 300 km test map (C1 minimum size), no QGIS needed.
// A rail line runs west to east through a city; a road branches north; a river and forest split the map.
MapData test_map();

}  // namespace mapdata
