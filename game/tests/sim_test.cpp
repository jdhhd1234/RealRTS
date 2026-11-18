#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "sim/sim.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

using namespace mapdata;
using namespace sim;

namespace {

Pos km(float x, float y) { return {x * 1000.0f, y * 1000.0f}; }

entt::registry world_with(TerrainGrid grid) {
    entt::registry world;
    init_world(world, MapData::single_region("t", std::move(grid), TransportGraph{}));
    return world;
}

TerrainGrid plains() { return TerrainGrid::filled(40, 10, 1000.0f, Terrain::Plains); }

void run(entt::registry& world, std::uint64_t ticks) {
    for (std::uint64_t i = 0; i < ticks; ++i) step(world);
}

void days(entt::registry& world, std::uint64_t n) { run(world, n * TICKS_PER_DAY); }

void queue(entt::registry& world, SimCommand command) {
    world.ctx().get<CommandQueue>().value.push_back(std::move(command));
}

entt::entity find_unit(const entt::registry& world, std::uint32_t id) {
    for (const auto [entity, unit_id] : world.view<const UnitId>().each()) {
        if (unit_id.value == id) return entity;
    }
    return entt::null;
}

template <typename T>
const T& get(const entt::registry& world, std::uint32_t id) {
    const auto entity = find_unit(world, id);
    REQUIRE((entity != entt::null));
    return world.get<T>(entity);
}

Pos pos_of(const entt::registry& world, std::uint32_t id) { return get<Position>(world, id).value; }

float strength(const entt::registry& world, std::uint32_t id) {
    const auto entity = find_unit(world, id);
    return entity != entt::null ? world.get<Battalions>(entity).total() : 0.0f;
}

template <typename T>
std::size_t count(const entt::registry& world) {
    return world.view<const T>().size();
}

using Log = std::vector<std::pair<std::uint64_t, SimSet>>;

template <SimSet set>
void log_stage(entt::registry& world) {
    world.ctx().get<Log>().emplace_back(world.ctx().get<SimClock>().tick, set);
}

Log run_ticks(std::uint64_t n) {
    entt::registry world;
    init_world(world, test_map());
    world.ctx().emplace<Log>();
    Schedule schedule;
    configure(schedule);
    using enum SimSet;
    for (auto& stage : schedule.stages) {
        switch (stage.set) {
        case Clock: break;
        case Commands: stage.systems.push_back(log_stage<Commands>); break;
        case Movement: stage.systems.push_back(log_stage<Movement>); break;
        case Combat: stage.systems.push_back(log_stage<Combat>); break;
        case Supply: stage.systems.push_back(log_stage<Supply>); break;
        case Partisan: stage.systems.push_back(log_stage<Partisan>); break;
        case Economy: stage.systems.push_back(log_stage<Economy>); break;
        case Cleanup: stage.systems.push_back(log_stage<Cleanup>); break;
        case Events: stage.systems.push_back(log_stage<Events>); break;
        }
    }
    for (std::uint64_t i = 0; i < n; ++i) step(world, schedule);
    CHECK(world.ctx().get<SimClock>().tick == n);
    return world.ctx().get<Log>();
}

}  // namespace

// lib.rs

TEST_CASE("load_scenario_replaces_previous_game") {
    entt::registry world;
    load_scenario(world, test_baked());
    run(world, TICKS_PER_HOUR);
    load_scenario(world, test_baked());
    const auto& ctx = world.ctx();
    CHECK(ctx.get<SimClock>().tick == 0);
    CHECK(count<UnitId>(world) == 3);
    CHECK(count<Depot>(world) == 2);
    CHECK(ctx.get<SupplyBases>().value.size() == 2);
    CHECK(region_status(ctx.get<Map>().value, ctx.get<Territory>())[7] == RegionStatus{RegionStatus::Occupied{1}});
}

TEST_CASE("cadence_counts_over_two_days") {
    constexpr auto n = 2 * TICKS_PER_DAY;
    const auto log = run_ticks(n);
    const auto count_set = [&](SimSet set) { return std::ranges::count(log, set, &Log::value_type::second); };
    CHECK(count_set(SimSet::Commands) == n);
    CHECK(count_set(SimSet::Movement) == n);
    CHECK(count_set(SimSet::Combat) == n / 5);
    CHECK(count_set(SimSet::Supply) == 48);
    CHECK(count_set(SimSet::Partisan) == 8);
    CHECK(count_set(SimSet::Economy) == 2);
    CHECK(count_set(SimSet::Events) == n);
}

TEST_CASE("order_fixed_within_a_tick") {
    const auto log = run_ticks(TICKS_PER_DAY);
    std::vector<SimSet> at_day_end;
    for (const auto& [t, set] : log) {
        if (t == TICKS_PER_DAY) at_day_end.push_back(set);
    }
    using enum SimSet;
    CHECK(at_day_end == std::vector{Commands, Movement, Combat, Supply, Partisan, Economy, Cleanup, Events});
}

TEST_CASE("clock_calendar") {
    const SimClock c{3 * TICKS_PER_DAY + 14 * TICKS_PER_HOUR + 5};
    CHECK(std::tuple{c.day(), c.hour(), c.minute()} == std::tuple{3u, 14u, 5u});
}

// units.rs

TEST_CASE("division_marches_10km_on_plains_in_30_ticks") {
    auto world = world_with(TerrainGrid::filled(20, 3, 1000.0f, Terrain::Plains));
    division(world, 1, 0, "1st", km(0.5f, 1.5f));
    queue(world, {SimCommand::Move{UnitId{1}, km(10.5f, 1.5f)}});
    run(world, 29);
    CHECK(pos_of(world, 1).x < 10'500.0f);
    CHECK(count<MoveOrder>(world) == 1);
    run(world, 2);
    CHECK(pos_of(world, 1) == km(10.5f, 1.5f));
    CHECK(count<MoveOrder>(world) == 0);
}

TEST_CASE("forest_is_twice_as_slow") {
    auto world = world_with(TerrainGrid::filled(20, 1, 1000.0f, Terrain::Forest));
    division(world, 1, 0, "1st", km(0.5f, 0.5f));
    queue(world, {SimCommand::Move{UnitId{1}, km(10.5f, 0.5f)}});
    run(world, 31);
    CHECK(std::abs(pos_of(world, 1).x - 5500.0f) < 400.0f);
}

TEST_CASE("bad_commands_are_ignored") {
    auto world = world_with(TerrainGrid::filled(5, 5, 1000.0f, Terrain::Plains));
    division(world, 1, 0, "1st", km(0.5f, 0.5f));
    queue(world, {SimCommand::Move{UnitId{99}, km(2.5f, 0.5f)}});
    queue(world, {SimCommand::Move{UnitId{1}, Pos{-10.0f, 500.0f}}});
    step(world);
    CHECK(count<MoveOrder>(world) == 0);
    CHECK(world.ctx().get<CommandQueue>().value.empty());
}

// path.rs

TEST_CASE("straight_line_on_plains") {
    const auto grid = TerrainGrid::filled(10, 10, 1000.0f, Terrain::Plains);
    const auto path = find_path(grid, {0, 0}, {9, 0});
    REQUIRE(path);
    REQUIRE(path->size() == 9);
    CHECK(path->back() == Cell{9, 0});
    CHECK(std::ranges::all_of(*path, [](Cell c) { return c.y == 0; }));
}

TEST_CASE("detours_around_forest_when_cheaper") {
    auto grid = TerrainGrid::filled(10, 3, 1000.0f, Terrain::Plains);
    for (std::uint32_t x = 1; x < 9; ++x) grid.set(x, 1, Terrain::Forest);
    const auto path = find_path(grid, {0, 1}, {9, 1});
    REQUIRE(path);
    REQUIRE_FALSE(path->empty());
    CHECK(path->back() == Cell{9, 1});
    CHECK(std::ranges::all_of(*path, [&](Cell c) { return grid.get(c.x, c.y) != Terrain::Forest; }));
}

TEST_CASE("same_cell_is_empty_path") {
    const auto grid = TerrainGrid::filled(3, 3, 1000.0f, Terrain::Plains);
    const auto path = find_path(grid, {1, 1}, {1, 1});
    REQUIRE(path);
    CHECK(path->empty());
}

// deploy.rs

TEST_CASE("posture_sets_frontage_and_roles") {
    const auto b = Battalions::standard();
    const auto d = layout(b, Posture::Defend);
    CHECK(d.strength == Goods{6.0f, 2.0f, 2.0f});
    CHECK(d.frontage_m == 18'000.0f);
    const auto a = layout(b, Posture::Attack);
    CHECK(a.strength == Goods{8.0f, 0.0f, 2.0f});
    CHECK(a.frontage_m == 12'000.0f);
    CHECK(a.line_fire > d.line_fire);
    const auto m = layout(b, Posture::March);
    CHECK(m.frontage_m == MARCH_FRONTAGE_M);
    CHECK(m.depth_m == 15'000.0f);
}

TEST_CASE("segment_geometry") {
    const auto [a, b] = line_ends({0.0f, 0.0f}, {1.0f, 0.0f}, 10.0f);
    CHECK(std::abs(a.x) < 1e-4f);
    CHECK(std::abs(a.y + 5.0f) < 1e-4f);
    CHECK(std::abs(b.y - 5.0f) < 1e-4f);
    CHECK(segment_dist({{0.0f, -5.0f}, {0.0f, 5.0f}}, {{3.0f, -1.0f}, {3.0f, 1.0f}}) == 3.0f);
    CHECK(segment_dist({{-1.0f, 0.0f}, {1.0f, 0.0f}}, {{0.0f, -1.0f}, {0.0f, 1.0f}}) == 0.0f);
    const auto t = turn_toward({1.0f, 0.0f}, {0.0f, 1.0f}, 0.5f);
    CHECK(std::abs(std::atan2(t.second, t.first) - 0.5f) < 1e-4f);
}

// combat.rs

TEST_CASE("equal_divisions_trade_losses_evenly") {
    auto world = world_with(plains());
    division(world, 1, 0, "blue", km(10.5f, 5.5f));
    division(world, 2, 1, "red", km(20.5f, 5.5f));
    days(world, 1);
    const auto a = strength(world, 1);
    const auto b = strength(world, 2);
    CHECK(std::abs(a - b) < 1e-3f);
    CHECK(a > 9.3f);
    CHECK(a < 9.7f);
}

TEST_CASE("unit_without_ammo_fights_much_worse") {
    auto world = world_with(plains());
    division(world, 1, 0, "blue", km(10.5f, 5.5f));
    const auto red = division(world, 2, 1, "red", km(20.5f, 5.5f));
    world.get<Stocks>(red).value = {400.0f, 0.0f, 300.0f};
    days(world, 1);
    const auto blue_loss = 10.0f - strength(world, 1);
    const auto red_loss = 10.0f - strength(world, 2);
    CHECK(blue_loss < red_loss * 0.3f);
}

TEST_CASE("forest_defender_loses_less") {
    auto grid = plains();
    for (std::uint32_t y = 0; y < 10; ++y) grid.set(20, y, Terrain::Forest);
    auto world = world_with(std::move(grid));
    division(world, 1, 0, "blue", km(10.5f, 5.5f));
    division(world, 2, 1, "red in forest", km(20.5f, 5.5f));
    days(world, 1);
    CHECK(10.0f - strength(world, 2) < (10.0f - strength(world, 1)) * 0.75f);
}

TEST_CASE("hit_on_the_flank_costs_more_than_from_the_front") {
    const auto loss_with_second_enemy_at = [](Pos pos) {
        auto world = world_with(TerrainGrid::filled(100, 100, 1000.0f, Terrain::Plains));
        division(world, 1, 0, "blue", km(50.5f, 50.5f));
        division(world, 2, 1, "red front", km(60.5f, 50.5f));
        run(world, 30);
        division(world, 3, 1, "red second", pos);
        run(world, 60);
        return 10.0f - strength(world, 1);
    };
    const auto front = loss_with_second_enemy_at(km(61.5f, 45.5f));
    const auto flank = loss_with_second_enemy_at(km(50.5f, 61.5f));
    CHECK(flank > front * 1.2f);
}

TEST_CASE("reserves_are_spared_by_the_line") {
    auto b = Battalions::standard();
    b.take_loss(1.0f, Posture::Defend);
    const auto line = std::ranges::find(b.value, Arm::Infantry, &Battalion::arm);
    const auto reserve = std::ranges::find(b.value, Arm::Armor, &Battalion::arm);
    REQUIRE(line != b.value.end());
    REQUIRE(reserve != b.value.end());
    CHECK(reserve->strength > line->strength);
    CHECK(std::abs(10.0f - b.total() - 1.0f) < 1e-4f);
}

TEST_CASE("no_contact_no_combat") {
    auto world = world_with(plains());
    division(world, 1, 0, "blue", km(0.5f, 5.5f));
    division(world, 2, 1, "red", km(39.5f, 5.5f));
    run(world, 60);
    CHECK(strength(world, 1) == 10.0f);
    CHECK(world.ctx().get<Engagements>().value.empty());
}

TEST_CASE("crushed_division_is_destroyed_and_stocks_written_off") {
    auto world = world_with(plains());
    division(world, 1, 0, "blue", km(10.5f, 5.5f));
    const auto red = division(world, 2, 1, "red", km(20.5f, 5.5f));
    world.get<Battalions>(red).value = {{Arm::Infantry, 0.06f}};
    run(world, 60);
    CHECK(strength(world, 2) == 0.0f);
    CHECK_FALSE(world.valid(red));
    CHECK(world.ctx().get<Ledger>().destroyed[FOOD] > DIVISION_TARGET[FOOD] - 5.0f);
}

TEST_CASE("no_fuel_no_movement") {
    auto world = world_with(plains());
    const auto dry = division(world, 1, 0, "dry", km(0.5f, 5.5f));
    world.get<Stocks>(dry).value = {0.0f, 600.0f, 300.0f};
    queue(world, {SimCommand::Move{UnitId{1}, km(10.5f, 5.5f)}});
    run(world, 30);
    CHECK(count<Position>(world) == 1);
    CHECK(pos_of(world, 1) == km(0.5f, 5.5f));
}

// territory.rs

namespace {

std::vector<std::uint8_t> legal() { return {0, 0, 1, 0, 0, 1, 0, 0, 1}; }

entt::registry territory_world() {
    entt::registry world;
    auto map = test_map();
    auto t = Territory::from_legal(map, legal());
    init_world(world, std::move(map));
    world.ctx().insert_or_assign(std::move(t));
    return world;
}

std::uint8_t owner(const entt::registry& world, Pos p) {
    const auto& ctx = world.ctx();
    return ctx.get<Territory>().military_at(ctx.get<Map>().value, p).value();
}

void set_region_owner(const MapData& map, Territory& t, RegionId region, std::uint8_t side) {
    for (std::size_t c = 0; c < map.region_cells.size(); ++c) {
        if (map.region_cells[c] == region) t.military[c] = side;
    }
}

}  // namespace

TEST_CASE("unit_takes_cells_within_radius") {
    auto world = territory_world();
    CHECK(owner(world, km(260.0f, 150.0f)) == 1);
    division(world, 1, 0, "raid", km(260.0f, 150.0f));
    run(world, 5);
    CHECK(owner(world, km(260.0f, 150.0f)) == 0);
    CHECK(owner(world, km(268.0f, 150.0f)) == 0);
    CHECK(owner(world, km(275.0f, 150.0f)) == 1);
}

TEST_CASE("contested_cell_keeps_owner_and_control_persists") {
    auto world = territory_world();
    const auto blue = division(world, 1, 0, "blue", km(255.0f, 150.0f));
    division(world, 2, 1, "red", km(265.0f, 150.0f));
    run(world, 5);
    CHECK(owner(world, km(260.0f, 150.0f)) == 1);
    CHECK(owner(world, km(247.0f, 150.0f)) == 0);
    world.destroy(blue);
    run(world, 5);
    CHECK(owner(world, km(247.0f, 150.0f)) == 0);
}

TEST_CASE("region_status_home_occupied_contested") {
    const auto map = test_map();
    auto t = Territory::from_legal(map, legal());
    CHECK(std::ranges::all_of(region_status(map, t), [](const RegionStatus& s) {
        return s == RegionStatus{RegionStatus::Home{}};
    }));

    std::vector<std::size_t> cells;
    for (std::size_t c = 0; c < map.region_cells.size(); ++c) {
        if (map.region_cells[c] == RegionId{5}) cells.push_back(c);
    }
    for (const auto c : cells) t.military[c] = 0;
    CHECK(region_status(map, t)[5] == RegionStatus{RegionStatus::Occupied{0}});

    for (std::size_t i = 0; i < cells.size() / 2; ++i) t.military[cells[i]] = 1;
    CHECK(region_status(map, t)[5] == RegionStatus{RegionStatus::Contested{}});
}

TEST_CASE("depot_in_enemy_ground_is_captured") {
    auto world = territory_world();
    const auto d = depot(world, 1, NodeId{3});
    division(world, 1, 0, "blue", km(285.0f, 150.0f));
    run(world, 5);
    CHECK(world.get<Depot>(d).side == 0);
}

// front.rs

namespace {

// Cells east of 40 km start under Red control.
entt::registry strip() {
    auto world = world_with(TerrainGrid::filled(80, 10, 1000.0f, Terrain::Plains));
    auto& military = world.ctx().get<Territory>().military;
    for (std::size_t c = 0; c < military.size(); ++c) {
        if (c % 80 >= 40) military[c] = 1;
    }
    return world;
}

Battalions weak() { return {std::vector<Battalion>(3, Battalion{Arm::Infantry, 1.0f})}; }

Battalions strong() {
    auto b = Battalions::standard();
    const auto more = Battalions::standard();
    b.value.insert(b.value.end(), more.value.begin(), more.value.end());
    return b;
}

}  // namespace

TEST_CASE("strong_attacker_advances_and_front_moves") {
    auto world = strip();
    const auto blue = division(world, 1, 0, "blue", km(20.5f, 5.5f));
    world.replace<Battalions>(blue, strong());
    division(world, 2, 1, "red", km(45.5f, 5.5f));
    queue(world, {SimCommand::Move{UnitId{1}, km(75.5f, 5.5f)}});
    run(world, 2 * TICKS_PER_HOUR);
    const auto& red = get<Front>(world, 2);
    CHECK((red.retreat.has_value() || !red.ratio.has_value()));
    CHECK(pos_of(world, 2).x > 50'000.0f);
    CHECK(pos_of(world, 1).x > 40'000.0f);
    CHECK(owner(world, km(45.5f, 5.5f)) == 0);
}

TEST_CASE("weak_defender_retreats") {
    auto world = strip();
    division(world, 1, 0, "blue", km(35.5f, 5.5f));
    world.replace<Battalions>(division(world, 2, 1, "red", km(45.5f, 5.5f)), weak());
    run(world, 10);
    CHECK(get<Front>(world, 2).retreat.has_value());
    run(world, TICKS_PER_HOUR);
    CHECK(pos_of(world, 2).x > 50'500.0f);
    CHECK_FALSE(get<Front>(world, 1).retreat.has_value());
}

TEST_CASE("equal_forces_stall") {
    auto world = strip();
    division(world, 1, 0, "blue", km(20.5f, 5.5f));
    division(world, 2, 1, "red", km(45.5f, 5.5f));
    queue(world, {SimCommand::Move{UnitId{1}, km(75.5f, 5.5f)}});
    run(world, 6 * TICKS_PER_HOUR);
    const auto gap = pos_of(world, 2).x - pos_of(world, 1).x;
    CHECK(pos_of(world, 2).x == 45'500.0f);
    CHECK(gap > 13'000.0f);
    CHECK(gap <= 15'000.0f);
    CHECK(get<Front>(world, 1).speed == 0.0f);
    CHECK(count<MoveOrder>(world) == 1);
}

TEST_CASE("held_unit_does_not_retreat_and_loses_more") {
    const auto result = [](bool hold) {
        auto world = strip();
        division(world, 1, 0, "blue", km(35.5f, 5.5f));
        world.replace<Battalions>(division(world, 2, 1, "red", km(45.5f, 5.5f)), weak());
        if (hold) queue(world, {SimCommand::SetHold{UnitId{2}, true}});
        run(world, 2 * TICKS_PER_HOUR);
        return std::pair{pos_of(world, 2).x, strength(world, 2)};
    };
    const auto [held_x, held_str] = result(true);
    const auto [free_x, free_str] = result(false);
    CHECK(held_x == 45'500.0f);
    CHECK(free_x > held_x);
    CHECK(held_str < free_str);
}

TEST_CASE("encircled_unit_gets_no_supply_and_cannot_retreat") {
    entt::registry world;
    init_world(world, test_map());
    auto& ctx = world.ctx();
    ctx.get<SupplyBases>().value.push_back({0, NodeId{0}, {20'000.0f, 20'000.0f, 20'000.0f}});
    world.get<Stocks>(depot(world, 0, NodeId{1})).value = DEPOT_TARGET;
    const auto& grid = ctx.get<Map>().value.terrain;
    auto& military = ctx.get<Territory>().military;
    for (std::size_t y = 0; y < grid.height; ++y) {
        for (std::size_t x = 170; x < 175; ++x) military[y * grid.width + x] = 1;
    }
    const auto pocket = division(world, 1, 0, "pocket", km(200.5f, 150.5f));
    world.replace<Battalions>(pocket, weak());
    world.get<Stocks>(pocket).value = {400.0f, 600.0f, 100.0f};
    division(world, 2, 1, "red", km(212.5f, 150.5f));
    run(world, TICKS_PER_HOUR);
    CHECK(get<Front>(world, 1).encircled);
    run(world, 5);
    CHECK_FALSE(get<Front>(world, 1).retreat.has_value());
    const auto at = pos_of(world, 1);
    run(world, 3 * TICKS_PER_HOUR);
    CHECK(pos_of(world, 1) == at);
    CHECK(get<Stocks>(world, 1).value[FOOD] < 100.0f);
}

TEST_CASE("unit_in_reach_is_not_encircled") {
    entt::registry world;
    init_world(world, test_map());
    depot(world, 0, NodeId{1});
    division(world, 1, 0, "home", km(120.5f, 150.5f));
    run(world, TICKS_PER_HOUR);
    CHECK_FALSE(get<Front>(world, 1).encircled);
}

// supply.rs

namespace {

Goods goods(float amount) {
    Goods stock;
    stock.fill(amount);
    return stock;
}

entt::registry supply_world(float base_stock) {
    entt::registry world;
    init_world(world, test_map());
    world.ctx().get<SupplyBases>().value.push_back({0, NodeId{0}, goods(base_stock)});
    return world;
}

float total(const entt::registry& world) {
    const auto& ctx = world.ctx();
    const auto sum = [](const Goods& a) { return std::accumulate(a.begin(), a.end(), 0.0f); };
    float result = 0.0f;
    for (const auto& b : ctx.get<SupplyBases>().value) result += sum(b.stock);
    for (const auto& s : ctx.get<Pipeline>().value) result += sum(s.goods);
    for (const auto& b : ctx.get<EconomyState>().builds) result += sum(b.stocks);
    for (const auto [entity, stocks] : world.view<const Stocks>().each()) result += sum(stocks.value);
    const auto& ledger = ctx.get<Ledger>();
    return result + sum(ledger.consumed) + sum(ledger.destroyed) - sum(ledger.produced);
}

}  // namespace

TEST_CASE("goods_are_conserved_over_three_days") {
    auto world = supply_world(20'000.0f);
    depot(world, 0, NodeId{2});
    division(world, 1, 0, "A", km(210.0f, 160.0f));
    division(world, 2, 0, "B", km(260.0f, 150.0f));
    queue(world, {SimCommand::Move{UnitId{2}, km(200.0f, 100.0f)}});
    const auto start = total(world);
    days(world, 3);
    CHECK(std::abs(start - total(world)) < 0.5f);
    CHECK(world.ctx().get<Ledger>().consumed[FOOD] > 0.0f);
}

TEST_CASE("edges_never_exceed_capacity_and_cut_edge_blocks") {
    auto world = supply_world(50'000.0f);
    depot(world, 0, NodeId{2});
    auto& edges = world.ctx().get<EdgeState>();
    const auto& graph = world.ctx().get<Map>().value.graph;
    float max_seen = 0.0f;
    for (std::uint64_t tick = 0; tick < TICKS_PER_DAY; ++tick) {
        step(world);
        for (std::size_t e = 0; e < edges.last_flow.size(); ++e) {
            const auto flow = edges.last_flow[e];
            CAPTURE(e);
            CHECK(flow <= capacity_t_per_hour(graph, edges, e) + 1e-3f);
            max_seen = std::max(max_seen, flow);
        }
    }
    CHECK(std::abs(max_seen - 125.0f) < 1e-3f);
    edges.damage[0] = 1.0f;
    const auto& pipeline = world.ctx().get<Pipeline>().value;
    const auto before = pipeline.size();
    run(world, TICKS_PER_HOUR);
    CHECK(edges.last_flow[0] == 0.0f);
    CHECK(pipeline.size() <= before);
}

TEST_CASE("rail_through_enemy_ground_is_blocked") {
    auto world = supply_world(20'000.0f);
    depot(world, 0, NodeId{2});
    const auto& map = world.ctx().get<Map>().value;
    const auto cell = map.terrain.cell_at(map.graph.node(NodeId{1}).pos);
    REQUIRE(cell);
    world.ctx().get<Territory>().military[cell->y * map.terrain.width + cell->x] = 1;
    run(world, TICKS_PER_HOUR);
    CHECK(world.ctx().get<Pipeline>().value.empty());
    CHECK(std::ranges::all_of(world.ctx().get<EdgeState>().last_flow, [](float f) { return f == 0.0f; }));
}

TEST_CASE("isolated_unit_gets_nothing") {
    auto world = supply_world(20'000.0f);
    world.get<Stocks>(depot(world, 0, NodeId{0})).value = DEPOT_TARGET;
    division(world, 1, 0, "far", km(250.0f, 250.0f));
    division(world, 2, 0, "near", km(20.0f, 150.0f));
    days(world, 1);
    const auto& far_stocks = get<Stocks>(world, 1).value;
    const auto& near_stocks = get<Stocks>(world, 2).value;
    CHECK(far_stocks[FOOD] < DIVISION_TARGET[FOOD] - 25.0f);
    CHECK(std::abs(near_stocks[FOOD] - DIVISION_TARGET[FOOD]) < 2.0f);
}

TEST_CASE("priority_unit_served_first_when_depot_short") {
    auto world = supply_world(0.0f);
    world.get<Stocks>(depot(world, 0, NodeId{0})).value = {0.0f, 0.0f, 10.0f};
    for (const auto& [id, prio] : {std::pair{1u, std::uint8_t{0}}, std::pair{2u, std::uint8_t{5}}}) {
        const auto entity = division(world, id, 0, "u", km(15.0f, 150.0f));
        world.get<Stocks>(entity).value = {400.0f, 600.0f, 200.0f};
        world.get<SupplyPriority>(entity).value = prio;
    }
    run(world, TICKS_PER_HOUR);
    const float hour_use = IDLE_USE_T_PER_DAY[FOOD] / 24.0f;
    CHECK(std::abs(get<Stocks>(world, 2).value[FOOD] - (200.0f - hour_use + 10.0f)) < 1e-3f);
    CHECK(std::abs(get<Stocks>(world, 1).value[FOOD] - (200.0f - hour_use)) < 1e-3f);
}

TEST_CASE("truck_efficiency_curve") {
    CHECK(truck_efficiency(0.0f) == 1.0f);
    CHECK(truck_efficiency(TRUCK_FULL_RANGE_M) == 1.0f);
    CHECK(truck_efficiency(100'000.0f) == 0.5f);
    CHECK(truck_efficiency(TRUCK_MAX_RANGE_M) == 0.0f);
}

// economy.rs

namespace {

entt::registry economy_world() {
    auto world = territory_world();
    world.ctx().get<SupplyBases>().value = {{0, NodeId{0}, goods(10'000.0f)}, {1, NodeId{3}, goods(10'000.0f)}};
    return world;
}

float equipment_total(const entt::registry& world) {
    const auto& n = world.ctx().get<Nations>();
    return std::accumulate(n.equipment.begin(), n.equipment.end(), 0.0f) + n.equipment_used - n.equipment_produced;
}

const Goods& base_stock(const entt::registry& world, std::uint8_t side) {
    const auto& bases = world.ctx().get<SupplyBases>().value;
    const auto base = std::ranges::find(bases, side, &SupplyBase::side);
    REQUIRE(base != bases.end());
    return base->stock;
}

}  // namespace

TEST_CASE("home_regions_feed_their_base_daily") {
    auto world = economy_world();
    days(world, 1);
    const auto& map = world.ctx().get<Map>().value;
    const auto expect = [&](std::uint8_t side, std::size_t g) {
        float sum = 0.0f;
        for (std::size_t r = 0; r < map.regions.size(); ++r) {
            if ((r % 3 == 2) == (side == 1)) sum += map.regions[r].daily_output[g];
        }
        return sum;
    };
    for (std::size_t g = 0; g < Goods{}.size(); ++g) {
        CHECK(std::abs(base_stock(world, 0)[g] - (10'000.0f + expect(0, g))) < 1e-2f);
        CHECK(std::abs(base_stock(world, 1)[g] - (10'000.0f + expect(1, g))) < 1e-2f);
    }
    CHECK(std::abs(world.ctx().get<Nations>().equipment[0] - expect(0, EQUIPMENT)) < 1e-3f);
}

TEST_CASE("occupied_region_produces_nothing_for_owner_but_can_be_requisitioned") {
    auto world = economy_world();
    auto& ctx = world.ctx();
    set_region_owner(ctx.get<Map>().value, ctx.get<Territory>(), RegionId{3}, 1);
    queue(world, {SimCommand::SetRequisition{RegionId{3}, true}});
    const auto support_before = ctx.get<PartisanState>().support[3];
    days(world, 1);
    const auto blue_food = base_stock(world, 0)[FOOD];
    const auto red_food = base_stock(world, 1)[FOOD];
    CHECK(std::abs(blue_food - (10'000.0f + 60.0f * 2.0f + 40.0f + 50.0f * 2.0f)) < 1e-2f);
    CHECK(std::abs(red_food - (10'000.0f + 60.0f + 40.0f + 50.0f + 20.0f)) < 1e-2f);
    CHECK(ctx.get<PartisanState>().support[3] < support_before);
}

TEST_CASE("production_and_consumption_balance_over_ten_days") {
    auto world = economy_world();
    depot(world, 0, NodeId{1});
    depot(world, 1, NodeId{3});
    division(world, 1, 0, "blue", km(150.0f, 150.0f));
    division(world, 2, 1, "red", km(162.0f, 150.0f));
    division(world, 3, 0, "reserve", km(30.0f, 140.0f));
    const auto start = total(world);
    const auto eq_start = equipment_total(world);
    days(world, 10);
    CHECK(std::abs(start - total(world)) < 1.0f);
    CHECK(std::abs(eq_start - equipment_total(world)) < 1e-2f);
    const auto& ledger = world.ctx().get<Ledger>();
    CHECK(ledger.produced[FOOD] > 0.0f);
    CHECK(ledger.consumed[AMMO] > 0.0f);
}

TEST_CASE("replacements_need_depot_and_equipment") {
    auto world = economy_world();
    depot(world, 0, NodeId{0});
    world.ctx().get<Nations>().equipment[0] = 1000.0f;
    const auto worn = [] { return Battalions{{Battalion{Arm::Infantry, 0.5f}}}; };
    world.replace<Battalions>(division(world, 1, 0, "near", km(20.0f, 150.0f)), worn());
    world.replace<Battalions>(division(world, 2, 0, "cut off", km(250.0f, 280.0f)), worn());
    days(world, 2);
    CHECK(std::abs(strength(world, 1) - 0.6f) < 1e-4f);
    CHECK(strength(world, 2) == 0.5f);
}

TEST_CASE("new_division_costs_supplies_and_appears_after_build_time") {
    auto world = economy_world();
    auto& ctx = world.ctx();
    ctx.get<Nations>().equipment[0] = DIVISION_EQUIPMENT;
    queue(world, {SimCommand::BuildDivision{0, "3rd Div"}});
    step(world);
    CHECK(ctx.get<EconomyState>().builds.size() == 1);
    CHECK(base_stock(world, 0)[FOOD] == 10'000.0f - DIVISION_TARGET[FOOD]);

    queue(world, {SimCommand::BuildDivision{0, "4th Div"}});
    step(world);
    CHECK(ctx.get<EconomyState>().builds.size() == 1);

    days(world, BUILD_DAYS + 1);
    const auto names = world.view<const UnitName>();
    CHECK(std::ranges::any_of(names, [&](auto entity) {
        return names.get<const UnitName>(entity).value == "3rd Div";
    }));
    CHECK(ctx.get<EconomyState>().builds.empty());
}

// partisan.rs

namespace {

// West Province is legally Red and militarily Blue.
entt::registry partisan_world(float pool) {
    auto world = supply_world(50'000.0f);
    auto& ctx = world.ctx();
    auto owners = std::vector<std::uint8_t>(9);
    owners[3] = 1;
    const auto& map = ctx.get<Map>().value;
    auto t = Territory::from_legal(map, std::move(owners));
    set_region_owner(map, t, RegionId{3}, 0);
    ctx.insert_or_assign(std::move(t));
    ctx.get<PartisanState>().pool[3] = pool;
    depot(world, 0, NodeId{2});
    return world;
}

float depot_stock(const entt::registry& world) {
    float result = 0.0f;
    for (const auto [entity, depot_component, stocks] : world.view<const Depot, const Stocks>().each()) {
        result += std::accumulate(stocks.value.begin(), stocks.value.end(), 0.0f);
    }
    return result;
}

}  // namespace

TEST_CASE("sabotage_cuts_supply_chain") {
    auto quiet = partisan_world(0.0f);
    quiet.ctx().get<PartisanState>().support[3] = 1.0f;
    auto hot = partisan_world(20.0f);
    hot.ctx().get<PartisanState>().support[3] = 0.0f;
    days(quiet, 3);
    days(hot, 3);

    const auto& log = hot.ctx().get<EventLog>().value;
    CHECK(std::ranges::any_of(log, [](const auto& entry) { return entry.second.starts_with("Sabotage"); }));
    CHECK(quiet.ctx().get<EventLog>().value.empty());
    CHECK(depot_stock(hot) < depot_stock(quiet) - 100.0f);
}

TEST_CASE("home_regions_grow_no_partisans") {
    entt::registry world;
    init_world(world, test_map());
    days(world, 5);
    CHECK(std::ranges::all_of(world.ctx().get<PartisanState>().pool, [](float p) { return p == 0.0f; }));
    CHECK(world.ctx().get<EventLog>().value.empty());
}

TEST_CASE("garrison_suppresses_growth") {
    auto empty = partisan_world(0.0f);
    auto held = partisan_world(0.0f);
    division(held, 1, 0, "garrison", km(40.0f, 150.0f));
    run(empty, cadence::PARTISAN);
    run(held, cadence::PARTISAN);
    const auto a = empty.ctx().get<PartisanState>().pool[3];
    const auto b = held.ctx().get<PartisanState>().pool[3];
    CHECK(a > 0.0f);
    CHECK(b < a);
}

TEST_CASE("harsh_reprisal_cuts_support_and_feeds_partisans") {
    const auto map = test_map();
    auto state = PartisanState::make(9);
    EventLog log;
    state.pool[3] = 2.0f;
    auto normal = state;
    anti_partisan(normal, log, map, 0, RegionId{3}, false);
    anti_partisan(state, log, map, 0, RegionId{3}, true);
    CHECK(std::abs(normal.pool[3] - 1.4f) < 1e-5f);
    CHECK(state.support[3] < INITIAL_SUPPORT);
    CHECK(std::abs(state.pool[3] - 1.25f) < 1e-5f);
    CHECK(log.value.size() == 2);
}

TEST_CASE("spotted_band_appears_for_legal_owner_in_region") {
    auto world = partisan_world(5.0f);
    days(world, 5);
    const auto bands = world.view<const Partisan, const Side, const Position>();
    REQUIRE(bands.begin() != bands.end());
    const auto& map = world.ctx().get<Map>().value;
    for (const auto [entity, partisan, side, pos] : bands.each()) {
        CHECK(side.value == 1);
        const auto c = map.terrain.cell_at(pos.value);
        REQUIRE(c);
        CHECK(map.region_at(c->x, c->y) == partisan.region);
    }
}

TEST_CASE("same_seed_same_outcome") {
    auto a = partisan_world(10.0f);
    auto b = partisan_world(10.0f);
    days(a, 4);
    days(b, 4);
    CHECK(a.ctx().get<EdgeState>().damage == b.ctx().get<EdgeState>().damage);
    CHECK(a.ctx().get<EventLog>().value == b.ctx().get<EventLog>().value);
}

namespace {

// Two blue divisions in a corps west of x = 15 km; red holds x >= 20 km with one division far east.
entt::registry corps_world() {
    auto world = world_with(plains());
    auto& t = world.ctx().get<Territory>();
    for (std::uint32_t cy = 0; cy < 10; ++cy) {
        for (std::uint32_t cx = 20; cx < 40; ++cx) t.military[cy * 40 + cx] = 1;
    }
    division(world, 1, 0, "A", km(5, 2));
    division(world, 2, 0, "B", km(5, 8));
    division(world, 3, 1, "R", km(38, 5));
    world.ctx().get<CorpsList>().value.push_back({1, 0, "I Corps", {UnitId{2}, UnitId{1}}, std::nullopt});
    return world;
}

Pos pos_of(entt::registry& world, std::uint32_t id) {
    for (auto [e, uid, pos] : world.view<const UnitId, const Position>().each()) {
        if (uid.value == id) return pos.value;
    }
    return {};
}

}  // namespace

TEST_CASE("front_slots_split_the_line_evenly") {
    const auto s = front_slots({km(0, 0), km(0, 4), km(4, 4)}, 4, false);
    REQUIRE(s.size() == 4);
    CHECK(s[0].pos == km(0, 1));
    CHECK(s[1].pos == km(0, 3));
    CHECK(s[2].pos == km(1, 4));
    CHECK(s[0].normal == std::pair{-1.0f, 0.0f});  // left of north
    CHECK(s[3].normal == std::pair{0.0f, 1.0f});   // left of east
}

TEST_CASE("corps_deploys_along_its_front_facing_the_enemy") {
    auto world = corps_world();
    world.ctx().get<CommandQueue>().value.push_back({SimCommand::SetFront{1, {km(15, 1), km(15, 9)}}});
    run(world, 120);
    const auto& c = world.ctx().get<CorpsList>().value[0];
    REQUIRE(c.front);
    CHECK(c.front->flip);  // red ground is east, to the right of a line drawn northward
    CHECK(c.front->formed);
    CHECK(c.members[0] == UnitId{1});  // re-ordered: A stands at the south end
    const Pos a = pos_of(world, 1), b = pos_of(world, 2);
    CHECK(std::abs(a.x - 15'000.0f) < 1'500.0f);
    CHECK(std::abs(a.y - 3'000.0f) < 1'500.0f);
    CHECK(std::abs(b.y - 7'000.0f) < 1'500.0f);
    for (auto [e, uid, dep] : world.view<const UnitId, const Deployment>().each()) {
        if (uid.value != 3) CHECK(dep.facing.first > 0.9f);
    }
}

TEST_CASE("attacking_corps_moves_its_front_forward") {
    auto world = corps_world();
    auto& q = world.ctx().get<CommandQueue>().value;
    q.push_back({SimCommand::SetFront{1, {km(15, 1), km(15, 9)}}});
    q.push_back({SimCommand::SetFrontAttack{1, true}});
    run(world, 150);
    const auto& f = *world.ctx().get<CorpsList>().value[0].front;
    CHECK(f.formed);
    CHECK(pos_of(world, 1).x > 18'000.0f);
    CHECK(f.points[1].x == pos_of(world, 1).x);  // the line follows the divisions

    world.ctx().get<CommandQueue>().value.push_back({SimCommand::SetFront{1, {}}});
    run(world, 1);
    CHECK_FALSE(world.ctx().get<CorpsList>().value[0].front);
}
