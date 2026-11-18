#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 5285)
#endif
#include <doctest/doctest.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <shapefil.h>
#include <sqlite3.h>

#include "mapbake/mapbake.hpp"

using namespace mapbake;
using namespace mapdata;

namespace {

void append_u32_le(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(std::uint8_t(x & 0xFFu));
    v.push_back(std::uint8_t((x >> 8) & 0xFFu));
    v.push_back(std::uint8_t((x >> 16) & 0xFFu));
    v.push_back(std::uint8_t((x >> 24) & 0xFFu));
}

void append_f64_le(std::vector<std::uint8_t>& v, double d) {
    const auto bits = std::bit_cast<std::uint64_t>(d);
    for (std::size_t i = 0; i < 8; ++i) {
        v.push_back(std::uint8_t((bits >> (i * 8)) & 0xFFu));
    }
}

// Little-endian WKB writer for tests.
std::vector<std::uint8_t> wkb_polygon(std::span<const std::vector<P>> rings) {
    std::vector<std::uint8_t> v{1};
    append_u32_le(v, 3);
    append_u32_le(v, std::uint32_t(rings.size()));
    for (const auto& r : rings) {
        append_u32_le(v, std::uint32_t(r.size()));
        for (const auto& [x, y] : r) {
            append_f64_le(v, x);
            append_f64_le(v, y);
        }
    }
    return v;
}

std::vector<std::uint8_t> wkb_line(std::span<const P> pts) {
    std::vector<std::uint8_t> v{1};
    append_u32_le(v, 2);
    append_u32_le(v, std::uint32_t(pts.size()));
    for (const auto& [x, y] : pts) {
        append_f64_le(v, x);
        append_f64_le(v, y);
    }
    return v;
}

std::vector<std::uint8_t> wkb_point(P p) {
    std::vector<std::uint8_t> v{1};
    append_u32_le(v, 1);
    append_f64_le(v, p.first);
    append_f64_le(v, p.second);
    return v;
}

// GP header (little-endian, no envelope) around a WKB body.
std::vector<std::uint8_t> gpb(std::int32_t srs, const std::vector<std::uint8_t>& wkb) {
    std::vector<std::uint8_t> v{'G', 'P', 0, 1};
    append_u32_le(v, std::uint32_t(srs));
    v.insert(v.end(), wkb.begin(), wkb.end());
    return v;
}

std::vector<P> square(double x0, double y0, double x1, double y1) {
    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}, {x0, y0}};
}

std::vector<P> sq(double x0, double y0, double x1, double y1) {
    return square(x0, y0, x1, y1);
}

class MemoryDb {
public:
    MemoryDb() { REQUIRE(sqlite3_open(":memory:", &db_) == SQLITE_OK); }
    ~MemoryDb() {
        if (db_) {
            sqlite3_close(db_);
        }
    }
    MemoryDb(const MemoryDb&) = delete;
    MemoryDb& operator=(const MemoryDb&) = delete;
    MemoryDb(MemoryDb&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}
    MemoryDb& operator=(MemoryDb&& other) noexcept {
        if (this != &other) {
            if (db_) sqlite3_close(db_);
            db_ = std::exchange(other.db_, nullptr);
        }
        return *this;
    }
    sqlite3* get() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

struct LayerRow {
    std::vector<std::uint8_t> wkb;
    std::vector<std::string> vals;
};

struct LayerDef {
    std::string name;
    std::vector<std::string> cols;
    std::vector<LayerRow> rows;
};

constexpr const char* UTM = R"(PROJCS["WGS 84 / UTM zone 35N",GEOGCS["WGS 84"],UNIT["metre",1]])";
constexpr const char* WGS84 = R"(GEOGCS["WGS 84",DATUM["WGS_1984"],UNIT["degree",0.0174532925199433]])";

// Minimal GeoPackage in memory: (layer, extra column names, rows of (geometry wkb, values)).
MemoryDb gpkg(std::int32_t srs, const char* def, const std::vector<LayerDef>& layers) {
    MemoryDb c;
    char* errmsg = nullptr;
    REQUIRE(sqlite3_exec(c.get(),
                         "CREATE TABLE gpkg_spatial_ref_sys (srs_name TEXT, srs_id INTEGER, organization TEXT,"
                         " organization_coordsys_id INTEGER, definition TEXT);"
                         "CREATE TABLE gpkg_geometry_columns (table_name TEXT, column_name TEXT, geometry_type_name TEXT,"
                         " srs_id INTEGER, z INTEGER, m INTEGER);",
                         nullptr, nullptr, &errmsg) == SQLITE_OK);

    {
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(c.get(), "INSERT INTO gpkg_spatial_ref_sys VALUES ('x', ?1, 'EPSG', ?1, ?2)", -1,
                                   &stmt, nullptr) == SQLITE_OK);
        sqlite3_bind_int(stmt, 1, srs);
        sqlite3_bind_text(stmt, 2, def, -1, SQLITE_TRANSIENT);
        REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    for (const auto& layer : layers) {
        std::string extra;
        for (const auto& col : layer.cols) {
            extra += ", " + col + " TEXT";
        }
        const std::string create_sql = "CREATE TABLE " + layer.name + " (fid INTEGER PRIMARY KEY, geom BLOB" + extra + ");";
        REQUIRE(sqlite3_exec(c.get(), create_sql.c_str(), nullptr, nullptr, &errmsg) == SQLITE_OK);

        {
            sqlite3_stmt* stmt = nullptr;
            REQUIRE(sqlite3_prepare_v2(c.get(), "INSERT INTO gpkg_geometry_columns VALUES (?1, 'geom', 'GEOMETRY', ?2, 0, 0)",
                                       -1, &stmt, nullptr) == SQLITE_OK);
            sqlite3_bind_text(stmt, 1, layer.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, srs);
            REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }

        for (const auto& row : layer.rows) {
            std::string col_list;
            std::string marks;
            for (std::size_t i = 0; i < layer.cols.size(); ++i) {
                col_list += ", " + layer.cols[i];
                marks += ", ?" + std::to_string(i + 2);
            }
            const std::string ins_sql = "INSERT INTO " + layer.name + " (geom" + col_list + ") VALUES (?1" + marks + ")";
            sqlite3_stmt* stmt = nullptr;
            REQUIRE(sqlite3_prepare_v2(c.get(), ins_sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
            const auto blob = gpb(srs, row.wkb);
            sqlite3_bind_blob(stmt, 1, blob.data(), int(blob.size()), SQLITE_TRANSIENT);
            for (std::size_t i = 0; i < row.vals.size(); ++i) {
                sqlite3_bind_text(stmt, int(i + 2), row.vals[i].c_str(), -1, SQLITE_TRANSIENT);
            }
            REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
            sqlite3_finalize(stmt);
        }
    }
    return c;
}

// 40 x 20 km at UTM-like offsets: west region Blue, east region Red, forest block, rail + road.
MemoryDb sample() {
    const double ox = 500'000.0;
    const double oy = 5'000'000.0;
    const auto p = [&](double x, double y) -> P { return {ox + x * 1000.0, oy + y * 1000.0}; };
    const auto poly = [&](double x0, double y0, double x1, double y1) {
        const std::vector<std::vector<P>> rings{sq(p(x0, y0).first, p(x0, y0).second, p(x1, y1).first, p(x1, y1).second)};
        return wkb_polygon(rings);
    };
    const std::vector<P> rail_pts{p(2.0, 10.0), p(20.0, 12.0), p(38.0, 10.0)};
    const std::vector<P> road_pts{p(2.5, 10.5), p(20.5, 2.5)};
    return gpkg(
        32635, UTM,
        {
            {"terrain", {"kind"}, {{poly(0.0, 0.0, 40.0, 20.0), {"plains"}}, {poly(10.0, 0.0, 15.0, 20.0), {"Forest"}}}},
            {"regions", {"name", "side"}, {{poly(0.0, 0.0, 20.0, 20.0), {"West", "0"}}, {poly(20.0, 0.0, 40.0, 20.0), {"East", "1"}}}},
            {"nodes",
             {"name", "kind", "base_side", "depot_side"},
             {{wkb_point(p(2.5, 10.5)), {"Base", "station", "0", ""}},
              {wkb_point(p(37.5, 10.5)), {"Rail End", "", "", "1"}}}},
            {"rail", {"gauge"}, {{wkb_line(rail_pts), {"1"}}}},
            {"roads", {}, {{wkb_line(road_pts), {}}}},
            {"units", {"name", "side"}, {{wkb_point(p(5.0, 5.0)), {"1st Div", "0"}}}},
        });
}

template <typename F>
std::string expect_error(F&& f) {
    try {
        f();
    } catch (const std::runtime_error& e) {
        return e.what();
    }
    FAIL("expected std::runtime_error");
    return {};
}

std::filesystem::path temp_dir(const std::string& name) {
    const auto d = std::filesystem::temp_directory_path() / ("mapbake_cpp_" + name);
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d);
    return d;
}

void write_text_file(const std::filesystem::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary);
    REQUIRE(bool(out));
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(bool(out));
}

constexpr const char* CRS_JSON = R"("crs":{"type":"name","properties":{"name":"urn:ogc:def:crs:EPSG::32635"}})";

std::string fc(const std::string& features) {
    return std::string(R"({"type":"FeatureCollection",)") + CRS_JSON + R"(,"features":[)" + features + "]}";
}

std::filesystem::path game_dir() {
#ifdef MAPBAKE_GAME_DIR
    return std::filesystem::path(MAPBAKE_GAME_DIR);
#else
    return std::filesystem::current_path();
#endif
}

}  // namespace

// ---------- geom.rs tests ----------

TEST_CASE("parses_polygon_with_envelope_skipped") {
    const std::vector<std::vector<P>> poly{square(0.0, 0.0, 4.0, 4.0)};
    std::vector<std::uint8_t> blob{'G', 'P', 0, 0b0000'0011};  // little-endian, 32-byte envelope
    append_u32_le(blob, 3857);
    blob.insert(blob.end(), 32, 0);
    const auto wkb = wkb_polygon(poly);
    blob.insert(blob.end(), wkb.begin(), wkb.end());
    CHECK(parse_gpb(blob).polygons == std::vector<std::vector<std::vector<P>>>{poly});
}

TEST_CASE("rejects_garbage") {
    const std::vector<std::uint8_t> bad{'X', 'X', '1', '2', '3', '4', '5', '6'};
    CHECK_THROWS_AS(parse_gpb(bad), std::runtime_error);
    const std::vector<P> line{{0.0, 0.0}, {1.0, 1.0}};
    auto blob = gpb(1, wkb_line(line));
    blob.resize(blob.size() - 3);
    CHECK_THROWS_AS(parse_gpb(blob), std::runtime_error);
}

TEST_CASE("raster_fills_cell_centers_and_respects_holes") {
    const std::vector<std::vector<P>> rings{square(0.0, 0.0, 6.0, 6.0), square(2.0, 2.0, 4.0, 4.0)};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cells;
    raster_polygon(rings, 10, 10, [&](std::uint32_t x, std::uint32_t y) { cells.emplace_back(x, y); });
    CHECK(cells.size() == 36 - 4);
    const auto has = [&](std::uint32_t x, std::uint32_t y) {
        return std::ranges::find(cells, std::pair{x, y}) != cells.end();
    };
    CHECK(!has(2, 2));
    CHECK(!has(3, 3));
    CHECK(has(0, 0));
    CHECK(has(5, 5));
    CHECK(!has(6, 0));
}

TEST_CASE("raster_clips_to_grid") {
    int n = 0;
    const std::vector<std::vector<P>> rings{square(-5.0, -5.0, 50.0, 50.0)};
    raster_polygon(rings, 3, 2, [&](std::uint32_t, std::uint32_t) { ++n; });
    CHECK(n == 6);
}

// ---------- lib.rs tests ----------

TEST_CASE("bakes_terrain_regions_graph_and_units") {
    const MemoryDb db = sample();
    const Baked b = bake(db.get(), "sample");
    const MapData& m = b.map;
    CHECK(m.terrain.width == 41);
    CHECK(m.terrain.height == 21);
    CHECK(m.terrain.get(12, 5) == Terrain::Forest);
    CHECK(m.terrain.get(5, 5) == Terrain::Plains);
    CHECK(m.regions.size() == 3);
    CHECK(m.region_at(5, 5) == RegionId{0});
    CHECK(m.region_at(30, 5) == RegionId{1});
    CHECK(b.scenario.legal[0] == 0);
    CHECK(b.scenario.legal[1] == 1);
    CHECK(m.graph.nodes.size() == 3);
    CHECK(m.graph.edges.size() == 2);
    CHECK(m.graph.edges[0].kind == EdgeKind::Rail);
    CHECK(m.graph.edges[0].gauge == 1);
    CHECK(std::abs(m.graph.edges[0].length_m - 36'221.0f) < 10.0f);
    CHECK(b.scenario.bases == std::vector<SideNode>{{0, NodeId{0}}});
    CHECK(b.scenario.depots == std::vector<SideNode>{{1, NodeId{1}}});
    CHECK(b.scenario.units[0].pos == Pos{5'000.0f, 5'000.0f});
}

TEST_CASE("rejects_geographic_crs") {
    const MemoryDb c = gpkg(4326, WGS84, {{"nodes", {}, {{wkb_point({27.0, 47.0}), {}}}}});
    const std::string e = expect_error([&] { bake(c.get(), "x"); });
    CHECK(e.find("geographic") != std::string::npos);
}

TEST_CASE("rejects_unknown_terrain_and_bad_side") {
    const std::vector<std::vector<P>> rings{sq(0.0, 0.0, 5000.0, 5000.0)};
    const auto poly = wkb_polygon(rings);
    {
        const MemoryDb c = gpkg(3857, UTM, {{"terrain", {"kind"}, {{poly, {"lava"}}}}});
        CHECK(expect_error([&] { bake(c.get(), "x"); }).find("lava") != std::string::npos);
    }
    {
        const MemoryDb c = gpkg(3857, UTM, {{"regions", {"side"}, {{poly, {"9"}}}}});
        CHECK(expect_error([&] { bake(c.get(), "x"); }).find("side") != std::string::npos);
    }
}

TEST_CASE("rejects_empty_or_unrelated_packages") {
    const MemoryDb c = gpkg(3857, UTM, {{"buildings", {}, {}}});
    CHECK(expect_error([&] { bake(c.get(), "x"); }).find("no known layers") != std::string::npos);
}

// ---------- folder.rs tests ----------

TEST_CASE("geojson_folder_bakes") {
    const auto d = temp_dir("geojson");
    write_text_file(
        d / "terrain.geojson",
        fc(R"({"type":"Feature","properties":{"kind":"forest"},"geometry":{"type":"MultiPolygon","coordinates":[[[[500000,5000000],[505000,5000000],[505000,5005000],[500000,5005000],[500000,5000000]]]]}})"));
    write_text_file(
        d / "nodes.geojson",
        fc(R"({"type":"Feature","properties":{"name":"A","base_side":0},"geometry":{"type":"Point","coordinates":[500000,5000000]}},)"
           R"({"type":"Feature","properties":{"name":"B","depot_side":0,"kind":null},"geometry":{"type":"Point","coordinates":[520000,5010000]}})"));
    write_text_file(d / "notes.geojson", "ignored, not a layer name");

    const Baked b = bake_folder(d);
    CHECK(b.map.terrain.width == 21);
    CHECK(b.map.terrain.height == 11);
    CHECK(b.map.terrain.get(2, 2) == Terrain::Forest);
    CHECK(b.map.graph.nodes[1].pos == Pos{20'000.0f, 10'000.0f});
    CHECK(b.scenario.bases == std::vector<SideNode>{{0, NodeId{0}}});
    CHECK(b.map.region_at(0, 0) == RegionId{0});

    std::filesystem::remove_all(d);
}

TEST_CASE("geojson_in_degrees_is_rejected") {
    const auto d = temp_dir("degrees");
    const std::string plain =
        R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[27.1,47.5]}}]})";
    write_text_file(d / "nodes.geojson", plain);
    CHECK(expect_error([&] { bake_folder(d); }).find("geographic") != std::string::npos);
    std::filesystem::remove_all(d);
}

TEST_CASE("duplicate_layer_files_and_empty_folders_are_errors") {
    const auto d = temp_dir("dupes");
    CHECK(expect_error([&] { bake_folder(d); }).find("no layer files") != std::string::npos);
    write_text_file(d / "rail.geojson", fc(""));
    write_text_file(d / "RAIL.json", fc(""));
    CHECK(expect_error([&] { bake_folder(d); }).find("two files") != std::string::npos);
    std::filesystem::remove_all(d);
}

TEST_CASE("shapefile_folder_bakes_with_attributes") {
    const auto d = temp_dir("shp");
    const double x = 300'000.0;
    const double y = 5'300'000.0;
    const std::array<double, 3> xs{x + 0.0, x + 30'000.0, x + 30'000.0};
    const std::array<double, 3> ys{y + 0.0, y + 0.0, y + 40'000.0};

    const std::string shp_base = (d / "rail").string();
    {
        SHPHandle shp = SHPCreate(shp_base.c_str(), SHPT_ARC);
        REQUIRE(shp != nullptr);
        SHPObject* obj = SHPCreateSimpleObject(SHPT_ARC, 3, xs.data(), ys.data(), nullptr);
        REQUIRE(obj != nullptr);
        CHECK(SHPWriteObject(shp, -1, obj) == 0);
        SHPDestroyObject(obj);
        SHPClose(shp);

        DBFHandle dbf = DBFCreate(shp_base.c_str());
        REQUIRE(dbf != nullptr);
        CHECK(DBFAddField(dbf, "gauge", FTDouble, 4, 0) == 0);
        CHECK(DBFAddField(dbf, "capacity", FTDouble, 10, 1) == 1);
        CHECK(DBFWriteDoubleAttribute(dbf, 0, 0, 1.0) != 0);
        CHECK(DBFWriteDoubleAttribute(dbf, 0, 1, 2500.0) != 0);
        DBFClose(dbf);
    }

    write_text_file(d / "rail.prj",
                    R"(PROJCS["WGS 84 / UTM zone 35N",GEOGCS["WGS 84"],AUTHORITY["EPSG","32635"]])");
    const Baked b = bake_folder(d);
    const Edge& e = b.map.graph.edges[0];
    CHECK(e.kind == EdgeKind::Rail);
    CHECK(e.gauge == 1);
    CHECK(e.capacity_t_per_day == 2500.0f);
    CHECK(std::abs(e.length_m - 70'000.0f) < 1.0f);
    CHECK(b.map.graph.nodes.size() == 2);

    write_text_file(d / "rail.prj", R"(GEOGCS["WGS 84",DATUM["WGS_1984"]])");
    CHECK(expect_error([&] { bake_folder(d); }).find("geographic") != std::string::npos);
    std::filesystem::remove_all(d);
}

TEST_CASE("shapefile_cpg_cp949_decodes_to_utf8") {
    const auto d = temp_dir("cpg");
    const double x0 = 300'000.0;
    const double y0 = 5'300'000.0;
    const std::array<double, 2> xs{x0, x0 + 10'000.0};
    const std::array<double, 2> ys{y0, y0 + 10'000.0};

    const std::string shp_base = (d / "nodes").string();
    {
        SHPHandle shp = SHPCreate(shp_base.c_str(), SHPT_POINT);
        REQUIRE(shp != nullptr);
        for (int i = 0; i < 2; ++i) {
            SHPObject* obj = SHPCreateSimpleObject(SHPT_POINT, 1, &xs[std::size_t(i)], &ys[std::size_t(i)], nullptr);
            REQUIRE(obj != nullptr);
            CHECK(SHPWriteObject(shp, -1, obj) == i);
            SHPDestroyObject(obj);
        }
        SHPClose(shp);

        DBFHandle dbf = DBFCreate(shp_base.c_str());
        REQUIRE(dbf != nullptr);
        CHECK(DBFAddField(dbf, "name", FTString, 32, 0) == 0);
        // "서울역" in CP949: BCs-BFE, C0-FC, BF-AA -> 0xBC 0xAD 0xBF 0xEF 0xBF 0xAA
        const char seoul_cp949[] = "\xBC\xAD\xBF\xEF\xBF\xAA";
        CHECK(DBFWriteStringAttribute(dbf, 0, 0, seoul_cp949) != 0);
        CHECK(DBFWriteStringAttribute(dbf, 1, 0, "Junction") != 0);
        DBFClose(dbf);
    }
    write_text_file(d / "nodes.prj",
                    R"(PROJCS["WGS 84 / UTM zone 35N",GEOGCS["WGS 84"],AUTHORITY["EPSG","32635"]])");
    write_text_file(d / "nodes.cpg", "CP949");

    const Baked b = bake_path(d / "nodes.shp");
    REQUIRE(b.map.graph.nodes.size() == 2);
    CHECK(b.map.graph.nodes[0].name == std::string("\xEC\x84\x9C\xEC\x9A\xB8\xEC\x97\xAD"));
    std::filesystem::remove_all(d);
}

TEST_CASE("samples_and_example_geodata") {
    const auto root = game_dir();
    const Baked from_gpkg = bake_path(root / "samples" / "sample.gpkg");
    const Baked from_folder = bake_path(root / "samples" / "geojson");
    const Baked from_layer_file = bake_path(root / "samples" / "geojson" / "terrain.geojson");

    CHECK(from_gpkg.map.terrain == from_folder.map.terrain);
    CHECK(from_gpkg.map.graph == from_folder.map.graph);
    CHECK(from_gpkg.map.regions == from_folder.map.regions);
    CHECK(from_gpkg.map.region_cells == from_folder.map.region_cells);
    CHECK(from_gpkg.scenario == from_folder.scenario);
    CHECK(from_folder == from_layer_file);
    CHECK(Baked::from_json(from_gpkg.to_json()) == from_gpkg);

    const Baked ee = bake_path(root / "example_geodata" / "eastern_europe_game.gpkg");
    CHECK(ee.map.terrain.width == 1601);
    CHECK(ee.map.terrain.height == 1201);

    CHECK(expect_error([&] { bake_path(root / "example_geodata" / "eastern_europe_terrain.gpkg"); }).find("geographic") !=
          std::string::npos);
    CHECK(expect_error([&] { bake_path(root / "example_geodata" / "eastern_europe_land.gpkg"); }).find("no known layers") !=
          std::string::npos);
}
