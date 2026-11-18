#include "sim/sim.hpp"

#include <algorithm>
#include <cmath>

namespace sim {

using mapdata::Pos;

Division division(std::uint32_t id, std::uint8_t side, std::string_view name, Pos pos) {
    return {Battalions::standard(), UnitId{id}, Side{side}, UnitName{std::string(name)}, Position{pos},
            Stocks{DIVISION_TARGET}, StockTarget{DIVISION_TARGET}, SupplyPriority{}, Front{}, Deployment{}};
}

entt::entity division(entt::registry& world, std::uint32_t id, std::uint8_t side,
                      std::string_view name, Pos pos) {
    const auto entity = world.create();
    std::apply([&](auto&&... component) {
        (world.emplace<std::decay_t<decltype(component)>>(entity, std::move(component)), ...);
    }, division(id, side, name, pos));
    return entity;
}

void apply_commands(entt::registry& world) {
    auto& ctx = world.ctx();
    const auto& map = ctx.get<Map>().value;
    const auto& grid = map.terrain;
    auto queue = std::exchange(ctx.get<CommandQueue>().value, {});
    const auto units = world.view<const UnitId, const Position>();
    const auto find_unit = [&](UnitId id) {
        const auto it = std::find_if(units.begin(), units.end(), [&](auto e) {
            return units.get<const UnitId>(e) == id;
        });
        return it == units.end() ? entt::entity(entt::null) : *it;
    };

    for (auto& cmd : queue) {
        std::visit([&]<typename Command>(Command& command) {
            if constexpr (std::is_same_v<Command, SimCommand::Move>) {
                const auto entity = find_unit(command.unit);
                if (entity == entt::null) return;
                const auto start = grid.cell_at(units.get<const Position>(entity).value);
                const auto goal = grid.cell_at(command.goal);
                if (start && goal) {
                    if (auto path = find_path(grid, *start, *goal)) {
                        world.emplace_or_replace<MoveOrder>(entity, std::move(*path), 0u);
                    }
                }
            } else if constexpr (std::is_same_v<Command, SimCommand::SetHold>) {
                const auto entity = find_unit(command.unit);
                if (entity != entt::null) {
                    if (auto* f = world.try_get<Front>(entity)) {
                        f->hold = command.hold;
                        if (command.hold) f->retreat.reset();
                    }
                }
            } else if constexpr (std::is_same_v<Command, SimCommand::SetSupplyPriority>) {
                const auto entity = find_unit(command.unit);
                if (entity != entt::null) world.emplace_or_replace<SupplyPriority>(entity, command.priority);
            } else if constexpr (std::is_same_v<Command, SimCommand::AntiPartisan>) {
                anti_partisan(ctx.get<PartisanState>(), ctx.get<EventLog>(), map,
                              ctx.get<SimClock>().tick, command.region, command.harsh);
            } else if constexpr (std::is_same_v<Command, SimCommand::SetRequisition>) {
                auto& requisition = ctx.get<EconomyState>().requisition;
                if (command.region.value < requisition.size()) requisition[command.region.value] = command.on;
            } else if constexpr (std::is_same_v<Command, SimCommand::BuildDivision>) {
                order_division(command.side, std::move(command.name), ctx.get<SimClock>().tick,
                               ctx.get<SupplyBases>(), ctx.get<Nations>(), ctx.get<EconomyState>(),
                               ctx.get<EventLog>());
            } else if constexpr (std::is_same_v<Command, SimCommand::SetFront>) {
                if (auto* corps = find_corps(world, command.corps)) set_front(world, *corps, std::move(command.points));
            } else if constexpr (std::is_same_v<Command, SimCommand::SetFrontAttack>) {
                auto* corps = find_corps(world, command.corps);
                if (!corps || !corps->front) return;
                corps->front->attack = command.attack;
                // Defend: stop where they are instead of finishing the last push.
                if (!command.attack) {
                    for (const UnitId id : corps->members) {
                        if (const auto e = find_unit(id); e != entt::null) world.remove<MoveOrder>(e);
                    }
                }
            }
        }, cmd.value);
    }
}

namespace {

Pos cell_center(const mapdata::TerrainGrid& grid, Cell cell) {
    return {(static_cast<float>(cell.x) + 0.5f) * grid.cell_m,
            (static_cast<float>(cell.y) + 0.5f) * grid.cell_m};
}

}  // namespace

void move_units(entt::registry& world) {
    const auto& grid = world.ctx().get<Map>().value.terrain;
    const auto units = world.view<Position, MoveOrder>();
    std::vector<entt::entity> arrived;
    for (auto [entity, pos, order] : units.each()) {
        const auto* stocks = world.try_get<Stocks>(entity);
        if (stocks && stocks->value[FUEL] <= 0.0f) continue;

        const auto* front = world.try_get<Front>(entity);
        const float pace = front ? front->speed : 1.0f;
        if (pace <= 0.0f) continue;

        const auto start = pos.value;
        float time_left = 1.0f;
        while (time_left > 0.0f && order.next < order.path.size()) {
            const auto cell = order.path[order.next];
            const auto target = cell_center(grid, cell);
            const float dx = target.x - pos.value.x;
            const float dy = target.y - pos.value.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float speed = pace * DIVISION_SPEED_M_PER_TICK / terrain_cost(grid.get(cell.x, cell.y));
            const float reach = speed * time_left;
            if (dist <= reach) {
                pos.value = target;
                time_left -= dist / speed;
                ++order.next;
            } else {
                pos.value.x += dx / dist * reach;
                pos.value.y += dy / dist * reach;
                time_left = 0.0f;
            }
        }

        if (auto* dep = world.try_get<Deployment>(entity); dep && (!front || !front->ratio)) {
            if (const auto dir = unit(pos.value.x - start.x, pos.value.y - start.y)) dep->facing = *dir;
        }
        if (order.next >= order.path.size()) arrived.push_back(entity);
    }
    for (const auto entity : arrived) world.remove<MoveOrder>(entity);
}

}  // namespace sim
