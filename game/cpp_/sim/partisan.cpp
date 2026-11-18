#include "sim/sim.hpp"

#include <algorithm>
#include <bit>
#include <format>

namespace sim {

namespace {

// PCG-XSH-RR 64/32 with a fixed stream. All state updates wrap in unsigned arithmetic.
std::uint32_t next_u32(std::uint64_t& state) {
    const auto old = state;
    state = old * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
    const auto bits = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
    return std::rotr(bits, static_cast<int>(old >> 59u));
}

std::optional<mapdata::RegionId> region_of(const mapdata::MapData& map, mapdata::Pos pos) {
    if (const auto c = map.terrain.cell_at(pos)) return map.region_at(c->x, c->y);
    return std::nullopt;
}

}  // namespace

// QUESTION: Do saved Rust seeds need replay compatibility? The C++ architecture specifies
// PCG32, so the same seed generates a different stream than Rust's SplitMix64.
std::uint64_t SimRng::next_u64() {
    const auto high = static_cast<std::uint64_t>(next_u32(value));
    return (high << 32u) | next_u32(value);
}

float SimRng::next_f32() {
    return static_cast<float>(next_u64() >> 40u) / 16'777'216.0f;
}

PartisanState PartisanState::make(std::size_t regions) {
    return {std::vector<float>(regions), std::vector<float>(regions, INITIAL_SUPPORT), PARTISAN_ID_BASE};
}

void EventLog::push(std::uint64_t tick, std::string text) {
    value.emplace_back(tick, std::move(text));
    if (value.size() > 100) value.erase(value.begin());
}

RiskGrade risk_grade(float pool) {
    if (pool < 0.1f) return RiskGrade::None;
    if (pool < 0.75f) return RiskGrade::Low;
    if (pool < 2.0f) return RiskGrade::Medium;
    return RiskGrade::High;
}

void anti_partisan(PartisanState& state, EventLog& log, const mapdata::MapData& map,
                   std::uint64_t tick, mapdata::RegionId region, bool harsh) {
    const auto r = region.value;
    if (r >= state.pool.size()) return;
    const auto& name = map.regions[r].name;
    if (harsh) {
        state.pool[r] *= 0.4f;
        state.support[r] = std::max(state.support[r] - 0.2f, 0.0f);
        state.pool[r] += 0.5f * (1.0f - state.support[r]);
        log.push(tick, std::format("Harsh reprisals in {}: support falls, civilians join the partisans.", name));
    } else {
        state.pool[r] *= 0.7f;
        log.push(tick, std::format("Anti-partisan sweep in {}.", name));
    }
}

void partisan_step(entt::registry& world) {
    auto& ctx = world.ctx();
    const auto& m = ctx.get<Map>().value;
    const auto& territory = ctx.get<Territory>();
    const auto tick = ctx.get<SimClock>().tick;
    auto& rng = ctx.get<SimRng>();
    auto& state = ctx.get<PartisanState>();
    auto& edges = ctx.get<EdgeState>();
    auto& pipeline = ctx.get<Pipeline>().value;
    auto& ledger = ctx.get<Ledger>();
    auto& log = ctx.get<EventLog>();

    for (auto& d : edges.damage) d = std::max(d - REPAIR_PER_STEP, 0.0f);

    const auto units = world.view<const UnitId, const Side, const Position, Battalions>(entt::exclude<Partisan>);
    const auto status = region_status(m, territory);
    for (std::size_t r = 0; r < status.size(); ++r) {
        const auto* occupied = std::get_if<RegionStatus::Occupied>(&status[r].value);
        if (!occupied) continue;
        const auto legal = territory.legal[r];
        const mapdata::RegionId rid{static_cast<std::uint16_t>(r)};
        const auto& name = m.regions[r].name;

        std::vector<entt::entity> garrison;
        for (const auto [entity, id, side, pos, bats] : units.each()) {
            if (side.value == occupied->by && region_of(m, pos.value) == rid) garrison.push_back(entity);
        }
        std::ranges::sort(garrison, {}, [&](auto e) { return units.get<const UnitId>(e).value; });
        float security = 0.0f;
        for (const auto entity : garrison) security += units.get<Battalions>(entity).total();

        const float recruit = RECRUIT_PER_STEP * (1.0f - state.support[r]);
        const float pool = std::max(state.pool[r] + recruit - SUPPRESS_PER_BN * security, 0.0f);
        state.pool[r] = pool;

        if (rng.next_f32() < pool / (pool + 1.0f + 0.3f * security)) {
            const float roll = rng.next_f32();
            const auto raid_target = std::ranges::find_if(pipeline, [&](const Shipment& s) {
                const auto* d = world.try_get<Depot>(s.depot);
                return d && region_of(m, m.graph.node(d->node).pos) == rid;
            });

            if (roll < 0.25f && raid_target != pipeline.end()) {
                for (std::size_t g = 0; g < raid_target->goods.size(); ++g) {
                    const float lost = raid_target->goods[g] * RAID_LOSS;
                    raid_target->goods[g] -= lost;
                    ledger.destroyed[g] += lost;
                }
                log.push(tick, std::format("Partisans ambushed a supply convoy in {}.", name));
            } else if (roll < 0.5f && !garrison.empty()) {
                auto& b = units.get<Battalions>(garrison.front());
                const float ratio = std::min(GARRISON_ATTACK_LOSS / b.total(), 1.0f);
                for (auto& bat : b.value) bat.strength -= bat.strength * ratio;
                log.push(tick, std::format("Partisans attacked a garrison in {}.", name));
            } else {
                std::vector<std::size_t> region_edges;
                for (std::size_t e = 0; e < m.graph.edges.size(); ++e) {
                    const auto& edge = m.graph.edges[e];
                    const auto a = m.graph.node(edge.a).pos;
                    const auto b = m.graph.node(edge.b).pos;
                    const mapdata::Pos mid{(a.x + b.x) / 2.0f, (a.y + b.y) / 2.0f};
                    if (region_of(m, a) == rid || region_of(m, b) == rid || region_of(m, mid) == rid) region_edges.push_back(e);
                }
                if (!region_edges.empty()) {
                    const auto e = region_edges[rng.next_u64() % region_edges.size()];
                    edges.damage[e] = std::min(edges.damage[e] + SABOTAGE_DAMAGE, 1.0f);
                    const auto& edge = m.graph.edges[e];
                    log.push(tick, std::format("Sabotage on the line {} - {} ({}).",
                                               m.graph.node(edge.a).name.value_or(""),
                                               m.graph.node(edge.b).name.value_or(""), name));
                }
            }
        }

        const float spot_chance = std::min(0.2f + 0.1f * security, 0.9f);
        if (pool >= SPOT_THRESHOLD && rng.next_f32() < spot_chance) {
            std::vector<std::size_t> cells;
            for (std::size_t c = 0; c < m.region_cells.size(); ++c) {
                if (m.region_cells[c] == rid) cells.push_back(c);
            }
            const auto c = cells[rng.next_u64() % cells.size()];
            const auto w = m.terrain.width;
            const auto cm = m.terrain.cell_m;
            const mapdata::Pos pos{(static_cast<float>(c % w) + 0.5f) * cm, (static_cast<float>(c / w) + 0.5f) * cm};
            const float size = std::min(pool, BAND_MAX);
            state.pool[r] -= size;

            const auto entity = world.create();
            world.emplace<Partisan>(entity, rid);
            world.emplace<UnitId>(entity, state.next_unit_id++);
            world.emplace<Side>(entity, legal);
            world.emplace<UnitName>(entity, std::format("Partisans ({})", name));
            world.emplace<Position>(entity, pos);
            world.emplace<Battalions>(entity, std::vector{Battalion{Arm::Infantry, size}});
            world.emplace<Stocks>(entity);
            log.push(tick, std::format("Partisan band spotted in {}.", name));
        }
    }
}

}  // namespace sim
