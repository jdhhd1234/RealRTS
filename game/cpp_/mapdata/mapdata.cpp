#include "mapdata/mapdata.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace mapdata {

// ---------- JSON 변환 (nlohmann 규약: 같은 namespace의 to_json / from_json) ----------

NLOHMANN_JSON_SERIALIZE_ENUM(Terrain, {
    {Terrain::Plains, "Plains"},
    {Terrain::Forest, "Forest"},
    {Terrain::Marsh, "Marsh"},
    {Terrain::Mountain, "Mountain"},
    {Terrain::Urban, "Urban"},
    {Terrain::Water, "Water"},
})
NLOHMANN_JSON_SERIALIZE_ENUM(NodeKind, {
    {NodeKind::Junction, "Junction"},
    {NodeKind::Station, "Station"},
    {NodeKind::Port, "Port"},
})
NLOHMANN_JSON_SERIALIZE_ENUM(EdgeKind, {
    {EdgeKind::Rail, "Rail"},
    {EdgeKind::Road, "Road"},
    {EdgeKind::River, "River"},
})

void to_json(nlohmann::json& j, const NodeId& v) { j = v.value; }
void from_json(const nlohmann::json& j, NodeId& v) { j.get_to(v.value); }
void to_json(nlohmann::json& j, const RegionId& v) { j = v.value; }
void from_json(const nlohmann::json& j, RegionId& v) { j.get_to(v.value); }

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Pos, x, y)

void to_json(nlohmann::json& j, const Node& v) {
    j = {{"pos", v.pos}, {"kind", v.kind}, {"name", nullptr}};
    if (v.name) j["name"] = *v.name;
}
void from_json(const nlohmann::json& j, Node& v) {
    j.at("pos").get_to(v.pos);
    j.at("kind").get_to(v.kind);
    const auto& name = j.at("name");
    v.name = name.is_null() ? std::nullopt : std::optional<std::string>(name.get<std::string>());
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TerrainGrid, width, height, cell_m, cells)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Edge, a, b, kind, gauge, length_m, capacity_t_per_day)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TransportGraph, nodes, edges)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Region, name, daily_output)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(MapData, name, terrain, graph, regions, region_cells)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SideNode, side, node)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(UnitSpec, side, name, pos)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(CorpsSpec, side, name, units)
// WITH_DEFAULT: files saved before corps existed have no "corps" key.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ScenarioData, legal, control, bases, depots, units, corps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Baked, map, scenario)

namespace {

template <typename T>
T parse_json(const std::string& text) {
    try {
        return nlohmann::json::parse(text).get<T>();
    } catch (const nlohmann::json::exception& e) {
        throw MapError(MapErrorKind::Parse, e.what());
    }
}

bool positive_finite(float v) { return std::isfinite(v) && v > 0.0f; }

}  // namespace

// ---------- MapError ----------

MapError::MapError(MapErrorKind kind, const std::string& message, std::size_t index)
    : std::runtime_error(message), kind_(kind), index_(index) {}

// ---------- TerrainGrid ----------

TerrainGrid TerrainGrid::filled(std::uint32_t width, std::uint32_t height, float cell_m, Terrain t) {
    return TerrainGrid{width, height, cell_m, std::vector<Terrain>(std::size_t(width) * height, t)};
}

std::pair<float, float> TerrainGrid::size_m() const {
    return {float(width) * cell_m, float(height) * cell_m};
}

std::optional<Cell> TerrainGrid::cell_at(Pos pos) const {
    const float cx = std::floor(pos.x / cell_m);
    const float cy = std::floor(pos.y / cell_m);
    if (cx >= 0.0f && cy >= 0.0f && cx < float(width) && cy < float(height)) {
        return Cell{std::uint32_t(cx), std::uint32_t(cy)};
    }
    return std::nullopt;
}

Terrain TerrainGrid::get(std::uint32_t cx, std::uint32_t cy) const {
    return cells[std::size_t(cy) * width + cx];
}

void TerrainGrid::set(std::uint32_t cx, std::uint32_t cy, Terrain t) {
    cells[std::size_t(cy) * width + cx] = t;
}

// ---------- MapData ----------

MapData MapData::single_region(const std::string& name, TerrainGrid terrain, TransportGraph graph) {
    const std::size_t n = terrain.cells.size();
    return MapData{
        name,
        std::move(terrain),
        std::move(graph),
        {Region{name, {}}},
        std::vector<RegionId>(n, RegionId{0}),
    };
}

RegionId MapData::region_at(std::uint32_t cx, std::uint32_t cy) const {
    return region_cells[std::size_t(cy) * terrain.width + cx];
}

MapData MapData::from_json(const std::string& text) {
    MapData map = parse_json<MapData>(text);
    map.validate();
    return map;
}

std::string MapData::to_json() const { return nlohmann::json(*this).dump(); }

void MapData::validate() const {
    const TerrainGrid& t = terrain;
    const std::size_t expected = std::size_t(t.width) * t.height;
    if (t.cells.size() != expected) {
        throw MapError(MapErrorKind::CellCount, "terrain cell count " + std::to_string(t.cells.size()) +
                                                    ", expected " + std::to_string(expected));
    }
    if (!positive_finite(t.cell_m)) {
        throw MapError(MapErrorKind::BadCellSize, "cell size must be positive");
    }
    if (region_cells.size() != expected) {
        throw MapError(MapErrorKind::RegionCellCount, "region cell count " + std::to_string(region_cells.size()) +
                                                          ", expected " + std::to_string(expected));
    }
    for (std::size_t cell = 0; cell < region_cells.size(); ++cell) {
        if (region_cells[cell].value >= regions.size()) {
            throw MapError(MapErrorKind::BadRegionId, "bad region id at cell " + std::to_string(cell), cell);
        }
    }
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (!t.cell_at(graph.nodes[i].pos)) {
            throw MapError(MapErrorKind::NodeOutside, "node " + std::to_string(i) + " outside the map", i);
        }
    }
    const std::size_t n = graph.nodes.size();
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        const Edge& e = graph.edges[i];
        if (e.a.value >= n || e.b.value >= n) {
            throw MapError(MapErrorKind::EdgeNode, "edge " + std::to_string(i) + " uses a missing node", i);
        }
        if (e.a == e.b) {
            throw MapError(MapErrorKind::SelfLoop, "edge " + std::to_string(i) + " is a self loop", i);
        }
        if (!positive_finite(e.length_m) || !positive_finite(e.capacity_t_per_day)) {
            throw MapError(MapErrorKind::BadEdgeValue, "edge " + std::to_string(i) + " has bad length/capacity", i);
        }
    }
}

// ---------- Baked ----------

Baked Baked::from_json(const std::string& text) {
    Baked b = parse_json<Baked>(text);
    b.validate();
    return b;
}

std::string Baked::to_json() const { return nlohmann::json(*this).dump(); }

void Baked::validate() const {
    map.validate();
    const ScenarioData& s = scenario;
    const std::size_t region_count = map.regions.size();
    if (s.legal.size() != region_count || s.control.size() != region_count) {
        throw MapError(MapErrorKind::Scenario, "one legal owner and controller per region");
    }
    const auto bad_side = [](std::uint8_t side) { return side >= 8; };
    if (std::ranges::any_of(s.legal, bad_side) || std::ranges::any_of(s.control, bad_side) ||
        std::ranges::any_of(s.units, [&](const UnitSpec& u) { return bad_side(u.side); })) {
        throw MapError(MapErrorKind::Scenario, "side ids must be below 8");
    }
    const std::size_t nodes = map.graph.nodes.size();
    const auto bad_ref = [&](const SideNode& sn) { return sn.node.value >= nodes || bad_side(sn.side); };
    if (std::ranges::any_of(s.bases, bad_ref) || std::ranges::any_of(s.depots, bad_ref)) {
        throw MapError(MapErrorKind::Scenario, "base or depot on a missing node");
    }
    for (const UnitSpec& u : s.units) {
        if (!map.terrain.cell_at(u.pos)) {
            throw MapError(MapErrorKind::Scenario, "unit " + u.name + " outside the map");
        }
    }
    std::vector<bool> in_corps(s.units.size());
    for (const CorpsSpec& c : s.corps) {
        if (c.units.size() < CORPS_MIN || c.units.size() > CORPS_MAX) {
            throw MapError(MapErrorKind::Scenario, "corps " + c.name + " must have 2 to 5 divisions");
        }
        for (const std::uint32_t i : c.units) {
            if (i >= s.units.size() || in_corps[i] || s.units[i].side != c.side) {
                throw MapError(MapErrorKind::Scenario,
                               "corps " + c.name + ": missing, repeated or other-side division");
            }
            in_corps[i] = true;
        }
    }
}

// ---------- 테스트 지도 ----------

namespace {

Pos km(float x, float y) { return Pos{x * 1000.0f, y * 1000.0f}; }

// Triangle wave in -amp..=amp with the given period (cells). Integer-only so the map is identical on every machine.
std::int64_t wave(std::uint32_t v, std::uint32_t period, std::int64_t amp) {
    const std::int64_t phase = v % period;
    const std::int64_t half = std::int64_t(period) / 2;
    const std::int64_t dist = phase < half ? phase : std::int64_t(period) - phase;
    return 2 * amp * dist / half - amp;
}

}  // namespace

Baked test_baked() {
    MapData map = test_map();
    std::vector<std::uint8_t> legal;
    for (std::size_t r = 0; r < map.regions.size(); ++r) legal.push_back(r % 3 == 2 ? 1 : 0);
    std::vector<std::uint8_t> control = legal;
    control[7] = 1;
    ScenarioData scenario{
        legal,
        control,
        {SideNode{0, NodeId{0}}, SideNode{1, NodeId{3}}},
        {SideNode{0, NodeId{1}}, SideNode{1, NodeId{3}}},
        {
            UnitSpec{0, "1st Inf Div", km(20.0f, 150.0f)},
            UnitSpec{0, "2nd Armd Div", km(20.0f, 120.0f)},
            UnitSpec{1, "Red 10th Div", km(270.0f, 150.0f)},
        },
    };
    return Baked{std::move(map), std::move(scenario)};
}

MapData test_map() {
    TerrainGrid terrain = TerrainGrid::filled(300, 300, 1000.0f, Terrain::Plains);
    for (std::uint32_t cy = 0; cy < 300; ++cy) {
        terrain.set(150, cy, Terrain::Water);  // river, north-south
        for (std::uint32_t cx = 60; cx < 90; ++cx) terrain.set(cx, cy, Terrain::Forest);
    }
    for (std::uint32_t cy = 140; cy < 160; ++cy)
        for (std::uint32_t cx = 200; cx < 215; ++cx) terrain.set(cx, cy, Terrain::Urban);
    for (std::uint32_t cy = 250; cy < 300; ++cy)
        for (std::uint32_t cx = 230; cx < 300; ++cx) terrain.set(cx, cy, Terrain::Mountain);

    std::vector<Node> nodes{
        Node{km(10.5f, 150.5f), NodeKind::Station, "West Base"},
        Node{km(100.5f, 150.5f), NodeKind::Junction, "Forest Junction"},
        Node{km(207.5f, 150.5f), NodeKind::Station, "East City"},
        Node{km(290.5f, 150.5f), NodeKind::Station, "Front Railhead"},
        Node{km(100.5f, 260.5f), NodeKind::Junction, "North Village"},
    };
    const auto edge = [&](std::uint32_t a, std::uint32_t b, EdgeKind kind, std::uint8_t gauge) {
        const Pos pa = nodes[a].pos;
        const Pos pb = nodes[b].pos;
        const float capacity = kind == EdgeKind::Rail ? 3000.0f : kind == EdgeKind::Road ? 600.0f : 1500.0f;
        const float dx = pa.x - pb.x;
        const float dy = pa.y - pb.y;
        return Edge{NodeId{a}, NodeId{b}, kind, gauge, std::sqrt(dx * dx + dy * dy), capacity};
    };
    std::vector<Edge> edges{
        edge(0, 1, EdgeKind::Rail, 0),
        edge(1, 2, EdgeKind::Rail, 0),
        edge(2, 3, EdgeKind::Rail, 1),  // gauge break east of the city
        edge(1, 4, EdgeKind::Road, 0),
    };

    // 3x3 regions with wavy borders, like architectImages/region_area_border.svg. Index = row * 3 + col,
    // row 0 = south.
    std::vector<RegionId> region_cells;
    region_cells.reserve(90'000);
    for (std::uint32_t cy = 0; cy < 300; ++cy) {
        for (std::uint32_t cx = 0; cx < 300; ++cx) {
            const std::int64_t x = cx;
            const std::int64_t y = cy;
            const int col = int(x >= 100 + wave(cy, 80, 12)) + int(x >= 205 + wave(cy, 110, 12));
            const int row = int(y >= 105 + wave(cx, 90, 10)) + int(y >= 195 + wave(cx, 70, 10));
            region_cells.push_back(RegionId{std::uint16_t(row * 3 + col)});
        }
    }
    // Farmland everywhere, oil in the south, industry in the west and east cities.
    const std::array<std::pair<const char*, std::array<float, 4>>, 9> outputs{{
        {"SW", {40, 0, 60, 0}},
        {"S", {40, 0, 60, 0}},
        {"SE", {40, 0, 60, 0}},
        {"W", {0, 40, 40, 20}},
        {"Center", {0, 20, 40, 10}},
        {"E", {0, 40, 40, 20}},
        {"NW", {0, 0, 50, 0}},
        {"N", {0, 0, 50, 0}},
        {"NE", {0, 0, 50, 0}},
    }};
    std::vector<Region> regions;
    for (const auto& [n, daily_output] : outputs) regions.push_back(Region{std::string(n) + " Province", daily_output});

    return MapData{
        "test",
        std::move(terrain),
        TransportGraph{std::move(nodes), std::move(edges)},
        std::move(regions),
        std::move(region_cells),
    };
}

}  // namespace mapdata
