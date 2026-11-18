#include "sim/sim.hpp"

#include <algorithm>

namespace sim {

std::uint64_t SimClock::day() const { return tick / TICKS_PER_DAY; }
std::uint64_t SimClock::hour() const { return tick % TICKS_PER_DAY / TICKS_PER_HOUR; }
std::uint64_t SimClock::minute() const { return tick % TICKS_PER_HOUR; }

void advance_clock(entt::registry& world) {
    ++world.ctx().get<SimClock>().tick;
}

void configure(Schedule& schedule) {
    using enum SimSet;
    schedule.stages = {
        {Clock, 1, {advance_clock}},
        {Commands, cadence::COMMANDS, {apply_commands}},
        {Movement, cadence::MOVEMENT, {move_units, retreat_units}},
        {Combat, cadence::COMBAT, {resolve_combat, update_military, capture_depots}},
        {Supply, cadence::SUPPLY, {detect_pockets, consume, receive, dispatch, deliver}},
        {Partisan, cadence::PARTISAN, {partisan_step}},
        {Economy, cadence::ECONOMY, {economy_step}},
        {Cleanup, 1, {remove_destroyed}},
        {Events, 1, {}},
    };
}

void step(entt::registry& world, const Schedule& schedule) {
    const auto& clock = world.ctx().get<SimClock>();
    for (const auto& stage : schedule.stages) {
        if (every(stage.interval)(clock)) {
            for (const auto system : stage.systems) system(world);
        }
    }
}

void step(entt::registry& world) {
    static const Schedule schedule = [] {
        Schedule result;
        configure(result);
        return result;
    }();
    step(world, schedule);
}

void init_world(entt::registry& world, mapdata::MapData map) {
    auto& ctx = world.ctx();
    const auto regions = map.regions.size();
    ctx.insert_or_assign(SimClock{});
    ctx.insert_or_assign(CommandQueue{});
    ctx.insert_or_assign(SupplyBases{});
    ctx.insert_or_assign(Pipeline{});
    ctx.insert_or_assign(Ledger{});
    ctx.insert_or_assign(Engagements{});
    ctx.insert_or_assign(SimRng{});
    ctx.insert_or_assign(EventLog{});
    ctx.insert_or_assign(PartisanState::make(regions));
    ctx.insert_or_assign(Nations{});
    ctx.insert_or_assign(EconomyState::make(regions));
    ctx.insert_or_assign(EdgeState::make(map.graph.edges.size()));
    ctx.insert_or_assign(Territory::from_legal(map, std::vector<std::uint8_t>(regions)));
    ctx.insert_or_assign(Map{std::move(map)});
}

void load_scenario(entt::registry& world, mapdata::Baked baked) {
    baked.validate();
    auto& [map, scenario] = baked;
    auto territory = Territory::from_legal(map, scenario.legal);
    for (std::size_t cell = 0; cell < map.region_cells.size(); ++cell) {
        territory.military[cell] = scenario.control[map.region_cells[cell].value];
    }

    std::vector<entt::entity> old;
    for (const auto e : world.view<UnitId>()) old.push_back(e);
    for (const auto e : world.view<Depot>(entt::exclude<UnitId>)) old.push_back(e);
    for (const auto e : old) world.destroy(e);

    init_world(world, std::move(map));
    auto& ctx = world.ctx();
    ctx.insert_or_assign(std::move(territory));
    auto& bases = ctx.get<SupplyBases>().value;
    for (const auto& [side, node] : scenario.bases) {
        Goods stock;
        stock.fill(SCENARIO_BASE_STOCK);
        bases.push_back({side, node, stock});
    }
    for (const auto& [side, node] : scenario.depots) depot(world, side, node);

    std::uint32_t id = 1;
    for (const auto& u : scenario.units) {
        const auto distance = [&](mapdata::Pos pos) {
            const float dx = pos.x - u.pos.x;
            const float dy = pos.y - u.pos.y;
            return dx * dx + dy * dy;
        };
        const mapdata::UnitSpec* enemy = nullptr;
        for (const auto& other : scenario.units) {
            if (other.side != u.side && (!enemy || distance(other.pos) < distance(enemy->pos))) {
                enemy = &other;
            }
        }
        const auto entity = division(world, id++, u.side, u.name, u.pos);
        if (enemy) {
            if (const auto facing = unit(enemy->pos.x - u.pos.x, enemy->pos.y - u.pos.y)) {
                world.get<Deployment>(entity).facing = *facing;
            }
        }
    }
}

}  // namespace sim
