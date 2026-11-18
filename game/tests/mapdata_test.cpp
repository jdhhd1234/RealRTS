#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>

#include "mapdata/mapdata.hpp"

using namespace mapdata;

namespace {

// validate()가 던진 MapError의 종류. 던지지 않으면 nullopt.
template <typename F>
std::optional<MapErrorKind> error_kind(F&& f) {
    try {
        f();
    } catch (const MapError& e) {
        return e.kind();
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("test map is valid") {
    const MapData m = test_map();
    CHECK_NOTHROW(m.validate());
    CHECK(m.terrain.size_m() == std::pair{300'000.0f, 300'000.0f});
}

TEST_CASE("json roundtrip") {
    const MapData m = test_map();
    CHECK(MapData::from_json(m.to_json()) == m);
}

TEST_CASE("baked roundtrip and scenario checks") {
    const Baked b = test_baked();
    CHECK(Baked::from_json(b.to_json()) == b);

    Baked bad = b;
    bad.scenario.depots.push_back(SideNode{0, NodeId{99}});
    CHECK(error_kind([&] { bad.validate(); }) == MapErrorKind::Scenario);

    bad = b;
    bad.scenario.units[0].pos = Pos{-1.0f, 0.0f};
    CHECK(error_kind([&] { bad.validate(); }) == MapErrorKind::Scenario);
}

TEST_CASE("cell lookup") {
    const MapData m = test_map();
    const TerrainGrid& t = m.terrain;
    CHECK(t.cell_at(Pos{150'500.0f, 3'000.0f}) == Cell{150, 3});
    CHECK(t.get(150, 3) == Terrain::Water);
    CHECK(t.get(207, 150) == Terrain::Urban);
    CHECK_FALSE(t.cell_at(Pos{-1.0f, 0.0f}).has_value());
    CHECK_FALSE(t.cell_at(Pos{300'000.0f, 0.0f}).has_value());
}

TEST_CASE("rejects bad maps") {
    MapData m = test_map();
    m.terrain.cells.pop_back();
    CHECK(error_kind([&] { m.validate(); }) == MapErrorKind::CellCount);

    m = test_map();
    m.graph.edges[0].b = NodeId{99};
    CHECK(error_kind([&] { m.validate(); }) == MapErrorKind::EdgeNode);

    m = test_map();
    m.graph.edges[1].b = m.graph.edges[1].a;
    CHECK(error_kind([&] { m.validate(); }) == MapErrorKind::SelfLoop);

    m = test_map();
    m.graph.edges[2].capacity_t_per_day = 0.0f;
    CHECK(error_kind([&] { m.validate(); }) == MapErrorKind::BadEdgeValue);

    m = test_map();
    m.graph.nodes[3].pos.x = 400'000.0f;
    try {
        m.validate();
        FAIL("expected NodeOutside");
    } catch (const MapError& e) {
        CHECK(e.kind() == MapErrorKind::NodeOutside);
        CHECK(e.index() == 3);
    }

    CHECK(error_kind([] { MapData::from_json("not json"); }) == MapErrorKind::Parse);

    m = test_map();
    m.region_cells.pop_back();
    CHECK(error_kind([&] { m.validate(); }) == MapErrorKind::RegionCellCount);

    m = test_map();
    m.region_cells[5] = RegionId{9};
    try {
        m.validate();
        FAIL("expected BadRegionId");
    } catch (const MapError& e) {
        CHECK(e.kind() == MapErrorKind::BadRegionId);
        CHECK(e.index() == 5);
    }
}

TEST_CASE("test map has nine regions all used") {
    const MapData m = test_map();
    CHECK(m.regions.size() == 9);
    for (std::uint16_t r = 0; r < 9; ++r) {
        CHECK_MESSAGE(std::ranges::find(m.region_cells, RegionId{r}) != m.region_cells.end(), "region ", r, " empty");
    }
    CHECK(m.region_at(0, 0) == RegionId{0});
    CHECK(m.region_at(299, 299) == RegionId{8});
    CHECK(m.region_at(150, 150) == RegionId{4});
}

TEST_CASE("corps_need_2_to_5_divisions_of_their_side") {
    Baked b = test_baked();
    b.scenario.corps.push_back({0, "I Corps", {0, 1}});
    CHECK_NOTHROW(b.validate());
    CHECK(Baked::from_json(b.to_json()) == b);

    Baked one = b;
    one.scenario.corps[0].units = {0};
    CHECK_THROWS_AS(one.validate(), MapError);
    Baked enemy = b;
    enemy.scenario.corps[0].units = {0, 2};  // unit 2 is red
    CHECK_THROWS_AS(enemy.validate(), MapError);
    Baked twice = b;
    twice.scenario.corps.push_back({0, "II Corps", {1, 0}});
    CHECK_THROWS_AS(twice.validate(), MapError);

    // Files saved before corps existed have no "corps" key.
    std::string old = test_baked().to_json();
    const auto at = old.find(",\"corps\":[]");
    REQUIRE(at != std::string::npos);
    old.erase(at, std::string(",\"corps\":[]").size());
    CHECK(Baked::from_json(old).scenario.corps.empty());
}
