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
