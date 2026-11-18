#include "sim/sim.hpp"

#include <algorithm>
#include <format>

namespace sim {

Nations::Nations() : equipment(MAX_SIDES) {}

EconomyState EconomyState::make(std::size_t regions) {
    return {std::vector<bool>(regions), {}, BUILT_ID_BASE};
}

bool order_division(std::uint8_t side, std::string name, std::uint64_t tick, SupplyBases& bases,
                    Nations& nations, EconomyState& econ, EventLog& log) {
    const auto base = std::ranges::find(bases.value, side, &SupplyBase::side);
    if (base == bases.value.end()) return false;

    bool affordable = side < nations.equipment.size() && nations.equipment[side] >= DIVISION_EQUIPMENT;
    for (std::size_t g = 0; g < DIVISION_TARGET.size(); ++g) affordable = affordable && base->stock[g] >= DIVISION_TARGET[g];
    if (!affordable) {
        log.push(tick, std::format("Cannot raise {}: not enough supplies or equipment.", name));
        return false;
    }
    const auto message = std::format("Raising {}: ready in {} days.", name, BUILD_DAYS);
    econ.builds.push_back({side, std::move(name), tick + BUILD_DAYS * TICKS_PER_DAY, DIVISION_TARGET});
    for (std::size_t g = 0; g < DIVISION_TARGET.size(); ++g) base->stock[g] -= DIVISION_TARGET[g];
    nations.equipment[side] -= DIVISION_EQUIPMENT;
    nations.equipment_used += DIVISION_EQUIPMENT;
    log.push(tick, message);
    return true;
}

void economy_step(entt::registry& world) {
    auto& ctx = world.ctx();
    const auto& m = ctx.get<Map>().value;
    const auto& territory = ctx.get<Territory>();
    const auto tick = ctx.get<SimClock>().tick;
    auto& bases = ctx.get<SupplyBases>().value;
    auto& nations = ctx.get<Nations>();
    auto& econ = ctx.get<EconomyState>();
    auto& ledger = ctx.get<Ledger>();
    auto& partisans = ctx.get<PartisanState>();
    auto& log = ctx.get<EventLog>();

    const auto status = region_status(m, territory);
    for (std::size_t r = 0; r < status.size(); ++r) {
        const auto& out = m.regions[r].daily_output;
        const auto legal = territory.legal[r];
        if (const auto* occupied = std::get_if<RegionStatus::Occupied>(&status[r].value)) {
            const auto base = std::ranges::find(bases, occupied->by, &SupplyBase::side);
            if (econ.requisition[r] && base != bases.end()) {
                const float food = out[FOOD] * REQUISITION_SHARE;
                base->stock[FOOD] += food;
                ledger.produced[FOOD] += food;
                partisans.support[r] = std::max(partisans.support[r] - REQUISITION_SUPPORT_LOSS, 0.0f);
            }
        } else {
            const auto base = std::ranges::find(bases, legal, &SupplyBase::side);
            if (base == bases.end()) continue;
            const float share = std::holds_alternative<RegionStatus::Home>(status[r].value) ? 1.0f : CONTESTED_OUTPUT;
            for (std::size_t g = 0; g < base->stock.size(); ++g) {
                base->stock[g] += out[g] * share;
                ledger.produced[g] += out[g] * share;
            }
            nations.equipment[legal] += out[EQUIPMENT] * share;
            nations.equipment_produced += out[EQUIPMENT] * share;
        }
    }

    const auto units = world.view<const UnitId, const Side, const Position, Battalions, const StockTarget>();
    std::vector<entt::entity> list(units.begin(), units.end());
    std::ranges::sort(list, {}, [&](auto e) { return units.get<const UnitId>(e).value; });
    for (const auto entity : list) {
        const auto side = units.get<const Side>(entity).value;
        const auto pos = units.get<const Position>(entity).value;
        const auto* front = world.try_get<Front>(entity);
        if ((front && front->encircled) || !nearest_depot(m.graph, world, side, pos)) continue;
        auto& stock = nations.equipment[side];
        for (auto& b : units.get<Battalions>(entity).value) {
            const float gain = std::min(std::clamp(1.0f - b.strength, 0.0f, REPLACE_PER_DAY), stock / EQUIPMENT_PER_BN);
            b.strength += gain;
            stock -= gain * EQUIPMENT_PER_BN;
            nations.equipment_used += gain * EQUIPMENT_PER_BN;
        }
    }

    std::vector<Build> waiting;
    for (auto& b : econ.builds) {
        if (b.ready_tick > tick) {
            waiting.push_back(std::move(b));
            continue;
        }
        const auto base = std::ranges::find(bases, b.side, &SupplyBase::side);
        if (base == bases.end()) continue;
        const auto entity = division(world, econ.next_unit_id, b.side, b.name, m.graph.node(base->node).pos);
        world.get<Stocks>(entity).value = b.stocks;
        ++econ.next_unit_id;
        log.push(tick, std::format("{} is ready.", b.name));
    }
    econ.builds = std::move(waiting);
}

}  // namespace sim
