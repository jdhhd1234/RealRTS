// Symbol PNG checks live in app_symbols_test.cpp: this file includes windows.h (doctest main), which clashes with raylib.h.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/editor.hpp"
#include "app/files.hpp"

TEST_CASE("node_toggle_and_side_flip") {
    std::vector<mapdata::SideNode> list{{0, mapdata::NodeId{1}}};
    app::toggle_at(list, 1, mapdata::NodeId{2});
    app::toggle_at(list, 1, mapdata::NodeId{1});
    CHECK(list == std::vector<mapdata::SideNode>{{1, mapdata::NodeId{2}}});

    app::Editor ed(mapdata::test_baked());
    ed.on = true;
    const auto side = ed.baked.scenario.units[0].side;
    ed.flip_side(0);
    CHECK(ed.baked.scenario.units[0].side != side);
    CHECK(ed.reload);
    std::optional<sim::UnitId> sel = sim::UnitId{1};
    CHECK(ed.selected_index(sel) == std::optional<std::size_t>{0});
    ed.delete_unit(0, sel);
    CHECK(ed.baked.scenario.units.size() == 2);
    CHECK_FALSE(sel.has_value());
    ed.baked.validate();
}

TEST_CASE("baked_maps_load_and_bad_files_report_errors") {
    const auto dir = std::filesystem::temp_directory_path();
    const auto good = dir / "realrts_app_test.json";
    std::ofstream(good, std::ios::binary) << mapdata::test_baked().to_json();
    const auto [b, label] = app::read_map(good);
    CHECK(b.scenario.units.size() == 3);
    CHECK(label.ends_with(".json"));
    std::ofstream(good, std::ios::binary) << "not a map";
    CHECK_THROWS_AS(app::read_map(good), app::LoadError);
    std::filesystem::remove(good);
    CHECK_THROWS_AS(app::read_map(dir / "does_not_exist.gpkg"), app::LoadError);
}

TEST_CASE("drawn_region_takes_cells_and_its_share_of_output") {
    auto b = mapdata::test_baked();
    const auto sum_output = [](const mapdata::MapData& m) {
        float t = 0.0f;
        for (const auto& r : m.regions) t += r.daily_output[2];  // food: every test region makes some
        return t;
    };
    const float before = sum_output(b.map);
    const std::size_t regions = b.map.regions.size();
    const auto [w_m, h_m] = b.map.terrain.size_m();
    // Square in the middle of the map, all given to side 1.
    const std::vector<mapdata::Pos> square{
        {w_m * 0.4f, h_m * 0.4f}, {w_m * 0.6f, h_m * 0.4f}, {w_m * 0.6f, h_m * 0.6f}, {w_m * 0.4f, h_m * 0.6f}};
    CHECK(app::carve_polygon(b, square, 1) > 0);
    CHECK(b.map.regions.size() == regions + 1);
    CHECK(b.map.regions.back().name == "Drawn region 1");
    CHECK(b.scenario.legal.back() == 1);
    CHECK(b.map.regions.back().daily_output[2] > 0.0f);
    CHECK(sum_output(b.map) == doctest::Approx(before));
    const auto mid = b.map.terrain.cell_at({w_m * 0.5f, h_m * 0.5f});
    CHECK(b.map.region_at(mid->x, mid->y).value == regions);
    b.validate();

    // A circle over the whole map leaves one region; every emptied region is removed.
    const mapdata::Pos center{w_m / 2, h_m / 2};
    CHECK(app::carve_circle(b, center, w_m + h_m, 0) == b.map.region_cells.size());
    CHECK(b.map.regions.size() == 1);
    CHECK(sum_output(b.map) == doctest::Approx(before));
    b.validate();
    const std::vector<mapdata::Pos> line{{0, 0}, {1, 0}};
    CHECK(app::carve_polygon(b, line, 0) == 0);  // not a polygon
}

TEST_CASE("drawn_line_snaps_to_nodes_and_adds_edges") {
    auto m = mapdata::test_baked().map;
    const std::size_t nodes = m.graph.nodes.size(), edges = m.graph.edges.size();
    const mapdata::Pos start = m.graph.nodes[0].pos;
    const mapdata::Pos east{start.x + 5000.0f, start.y};
    // First point lands near node 0, the second makes a new junction, the repeat adds nothing.
    const std::vector<mapdata::Pos> pts{{start.x + 10.0f, start.y}, east, east};
    CHECK(app::add_line(m, pts, mapdata::EdgeKind::Rail, 100.0f) == 1);
    CHECK(m.graph.nodes.size() == nodes + 1);
    CHECK(m.graph.edges.size() == edges + 1);
    const auto& e = m.graph.edges.back();
    CHECK(e.a == mapdata::NodeId{0});
    CHECK(e.capacity_t_per_day == 3000.0f);
    CHECK(e.length_m == doctest::Approx(5000.0f));
    const std::vector<mapdata::Pos> again{start, east};
    CHECK(app::add_line(m, again, mapdata::EdgeKind::Rail, 100.0f) == 0);  // same edge again
    CHECK(app::add_line(m, again, mapdata::EdgeKind::Road, 100.0f) == 1);
    m.validate();
}
