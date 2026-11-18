#include "sim/sim.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <queue>

namespace sim {

using namespace mapdata;

std::tuple<Depot, Stocks> depot(std::uint8_t side, NodeId node) {
    return {Depot{side, node}, Stocks{}};
}

entt::entity depot(entt::registry& world, std::uint8_t side, NodeId node) {
    const auto entity = world.create();
    const auto [component, stock] = depot(side, node);
    world.emplace<Depot>(entity, component);
    world.emplace<Stocks>(entity, stock);
    return entity;
}

EdgeState EdgeState::make(std::size_t edges) {
    return {std::vector<float>(edges), std::vector<float>(edges)};
}

float capacity_t_per_hour(const TransportGraph& graph, const EdgeState& edges, std::size_t e) {
    return graph.edges[e].capacity_t_per_day / 24.0f * (1.0f - std::clamp(edges.damage[e], 0.0f, 1.0f));
}

namespace {

std::uint64_t transit_minutes(EdgeKind kind, float length_m) {
    float speed_m_per_h = 10'000.0f;
    if (kind == EdgeKind::Rail) speed_m_per_h = 30'000.0f;
    if (kind == EdgeKind::Road) speed_m_per_h = 20'000.0f;
    return static_cast<std::uint64_t>(std::ceil(length_m / speed_m_per_h * 60.0f));
}

// Fastest route over edges with spare capacity and two friendly endpoints.
std::optional<std::pair<std::vector<std::size_t>, std::uint64_t>> route(
    const TransportGraph& graph, const std::vector<float>& spare, const std::vector<std::uint8_t>& node_owner,
    std::uint8_t side, NodeId from, NodeId to) {
    constexpr auto unreachable = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint64_t> best(graph.nodes.size(), unreachable);
    std::vector<std::size_t> via(graph.nodes.size());
    using Entry = std::pair<std::uint64_t, std::uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    best[from.value] = 0;
    open.emplace(0, from.value);
    while (!open.empty()) {
        const auto [time, node] = open.top();
        open.pop();
        if (node == to.value) break;
        if (time > best[node]) continue;
        for (std::size_t e = 0; e < graph.edges.size(); ++e) {
            const auto& edge = graph.edges[e];
            if (spare[e] <= 1e-3f || node_owner[edge.a.value] != side || node_owner[edge.b.value] != side) continue;
            if (edge.a.value != node && edge.b.value != node) continue;
            const auto next = edge.a.value == node ? edge.b.value : edge.a.value;
            const auto arrival = time + transit_minutes(edge.kind, edge.length_m);
            if (arrival < best[next]) {
                best[next] = arrival;
                via[next] = e;
                open.emplace(arrival, next);
            }
        }
    }
    if (best[to.value] == unreachable) return std::nullopt;
    std::vector<std::size_t> path;
    for (auto node = to.value; node != from.value;) {
        const auto e = via[node];
        path.push_back(e);
        const auto& edge = graph.edges[e];
        node = edge.a.value == node ? edge.b.value : edge.a.value;
    }
    std::reverse(path.begin(), path.end());
    return std::pair{std::move(path), best[to.value]};
}

template <typename Depots>
std::optional<std::pair<float, entt::entity>> nearest_depot(
    const TransportGraph& graph, const entt::registry& world, const Depots& depots,
    std::uint8_t side, Pos pos) {
    std::optional<std::tuple<float, std::uint32_t, entt::entity>> best;
    for (const auto entity : depots) {
        const auto& d = world.get<Depot>(entity);
        if (d.side != side) continue;
        const auto p = graph.node(d.node).pos;
        const float dx = p.x - pos.x;
        const float dy = p.y - pos.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        const auto candidate = std::tuple{dist, d.node.value, entity};
        if (dist < TRUCK_MAX_RANGE_M && (!best || candidate < *best)) best = candidate;
    }
    if (best) return std::pair{std::get<0>(*best), std::get<2>(*best)};
    return std::nullopt;
}

}  // namespace

void consume(entt::registry& world) {
    auto& ledger = world.ctx().get<Ledger>();
    const auto units = world.view<const UnitId, Stocks>();
    std::vector<entt::entity> list(units.begin(), units.end());
    std::ranges::sort(list, {}, [&](auto entity) { return units.get<const UnitId>(entity).value; });
    for (const auto entity : list) {
        auto& stocks = units.get<Stocks>(entity).value;
        auto per_day = IDLE_USE_T_PER_DAY;
        if (world.all_of<MoveOrder>(entity)) per_day[FUEL] = MOVING_FUEL_T_PER_DAY;
        for (std::size_t g = 0; g < stocks.size(); ++g) {
            const float used = std::min(per_day[g] / 24.0f, stocks[g]);
            stocks[g] -= used;
            ledger.consumed[g] += used;
        }
    }
}

void receive(entt::registry& world) {
    auto& ctx = world.ctx();
    const auto tick = ctx.get<SimClock>().tick;
    auto& destroyed = ctx.get<Ledger>().destroyed;
    std::erase_if(ctx.get<Pipeline>().value, [&](const Shipment& s) {
        if (s.arrive_tick > tick) return false;
        auto& target = world.all_of<Depot, Stocks>(s.depot) ? world.get<Stocks>(s.depot).value : destroyed;
        for (std::size_t g = 0; g < target.size(); ++g) target[g] += s.goods[g];
        return true;
    });
}

void dispatch(entt::registry& world) {
    auto& ctx = world.ctx();
    const auto& map = ctx.get<Map>().value;
    const auto& graph = map.graph;
    const auto& territory = ctx.get<Territory>();
    auto& bases = ctx.get<SupplyBases>().value;
    auto& edges = ctx.get<EdgeState>();
    auto& pipeline = ctx.get<Pipeline>().value;
    const auto tick = ctx.get<SimClock>().tick;
    std::vector<float> spare(graph.edges.size());
    for (std::size_t e = 0; e < spare.size(); ++e) spare[e] = capacity_t_per_hour(graph, edges, e);
    std::vector<std::uint8_t> node_owner;
    node_owner.reserve(graph.nodes.size());
    for (const auto& node : graph.nodes) node_owner.push_back(territory.military_at(map, node.pos).value());
    std::ranges::fill(edges.last_flow, 0.0f);

    const auto depots = world.view<const Depot, const Stocks>();
    std::vector<entt::entity> list(depots.begin(), depots.end());
    std::ranges::sort(list, {}, [&](auto entity) {
        return std::pair{depots.get<const Depot>(entity).node.value, entity};
    });
    for (const auto entity : list) {
        const auto& d = depots.get<const Depot>(entity);
        const auto& stocks = depots.get<const Stocks>(entity).value;
        auto base = std::ranges::find(bases, d.side, &SupplyBase::side);
        if (base == bases.end()) continue;
        Goods need{};
        for (std::size_t g = 0; g < need.size(); ++g) {
            float in_transit = 0.0f;
            for (const auto& s : pipeline) if (s.depot == entity) in_transit += s.goods[g];
            need[g] = std::min(std::max(DEPOT_TARGET[g] - stocks[g] - in_transit, 0.0f), base->stock[g]);
        }
        const float total = std::accumulate(need.begin(), need.end(), 0.0f);
        if (total <= 0.0f) continue;
        const auto connection = route(graph, spare, node_owner, d.side, base->node, d.node);
        if (!connection) continue;

        const auto& [path, minutes] = *connection;
        float bottleneck = std::numeric_limits<float>::infinity();
        for (const auto e : path) bottleneck = std::min(bottleneck, spare[e]);
        const float scale = std::min(bottleneck / total, 1.0f);
        Goods send{};
        for (std::size_t g = 0; g < send.size(); ++g) send[g] = need[g] * scale;
        pipeline.push_back({entity, send, tick + std::max<std::uint64_t>(minutes, 1)});

        const float sent = std::accumulate(send.begin(), send.end(), 0.0f);
        for (std::size_t g = 0; g < send.size(); ++g) base->stock[g] -= send[g];
        for (const auto e : path) {
            spare[e] -= sent;
            edges.last_flow[e] += sent;
        }
    }
}

float truck_efficiency(float dist_m) {
    return std::clamp((TRUCK_MAX_RANGE_M - dist_m) / (TRUCK_MAX_RANGE_M - TRUCK_FULL_RANGE_M), 0.0f, 1.0f);
}

std::optional<std::pair<float, entt::entity>> nearest_depot(
    const TransportGraph& graph, const entt::registry& world, std::uint8_t side, Pos pos) {
    return nearest_depot(graph, world, world.view<const Depot>(), side, pos);
}

void deliver(entt::registry& world) {
    const auto& graph = world.ctx().get<Map>().value.graph;
    const auto depots = world.view<const Depot, Stocks>(entt::exclude<UnitId>);
    const auto units = world.view<const UnitId, const Side, const Position, Stocks, const StockTarget,
                                  const SupplyPriority>(entt::exclude<Depot>);

    struct Req {
        entt::entity unit;
        entt::entity depot;
        std::uint8_t prio;
        std::uint32_t id;
        Goods want;
        Goods got{};
    };
    std::vector<Req> reqs;
    for (const auto [entity, id, side, pos, stocks, target, prio] : units.each()) {
        if (const auto* front = world.try_get<Front>(entity); front && front->encircled) continue;
        const auto nearest = nearest_depot(graph, world, depots, side.value, pos.value);
        if (!nearest) continue;
        const auto [m, depot_entity] = *nearest;
        const float cap = TRUCK_T_PER_HOUR * truck_efficiency(m);
        Goods want{};
        for (std::size_t g = 0; g < want.size(); ++g) {
            want[g] = std::clamp(target.value[g] - stocks.value[g], 0.0f, cap);
        }
        reqs.push_back({entity, depot_entity, prio.value, id.value, want});
    }
    std::ranges::sort(reqs, {}, [](const Req& r) { return std::tuple{r.depot, -static_cast<int>(r.prio), r.id}; });

    for (std::size_t i = 0; i < reqs.size();) {
        std::size_t j = i + 1;
        while (j < reqs.size() && reqs[j].depot == reqs[i].depot && reqs[j].prio == reqs[i].prio) ++j;
        auto& stock = depots.get<Stocks>(reqs[i].depot).value;
        for (std::size_t g = 0; g < stock.size(); ++g) {
            float asked = 0.0f;
            for (auto k = i; k < j; ++k) asked += reqs[k].want[g];
            if (asked <= 0.0f || stock[g] <= 0.0f) continue;
            const float ratio = std::min(stock[g] / asked, 1.0f);
            for (auto k = i; k < j; ++k) {
                const float give = std::min(reqs[k].want[g] * ratio, stock[g]);
                reqs[k].got[g] = give;
                stock[g] -= give;
            }
        }
        i = j;
    }

    for (const auto& r : reqs) {
        auto& stock = units.get<Stocks>(r.unit).value;
        for (std::size_t g = 0; g < stock.size(); ++g) stock[g] += r.got[g];
    }
}

}  // namespace sim
