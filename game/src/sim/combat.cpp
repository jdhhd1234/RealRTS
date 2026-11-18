#include "sim/sim.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim {

using mapdata::Pos;
using mapdata::Terrain;

Battalions Battalions::standard() {
    Battalions b;
    b.value.reserve(10);
    b.value.insert(b.value.end(), 6, Battalion{Arm::Infantry, 1.0f});
    b.value.insert(b.value.end(), 2, Battalion{Arm::Armor, 1.0f});
    b.value.insert(b.value.end(), 2, Battalion{Arm::Artillery, 1.0f});
    return b;
}

float Battalions::total() const {
    float result = 0.0f;
    for (const auto& b : value) result += b.strength;
    return result;
}

float Battalions::firepower() const {
    float result = 0.0f;
    for (const auto& b : value) result += arm_firepower(b.arm) * b.strength;
    return result;
}

void Battalions::take_loss(float loss, Posture posture) {
    std::array<float, 3> have{};
    for (const auto& b : value) have[index(role(b.arm, posture))] += b.strength;
    float share_sum = 0.0f;
    for (std::size_t r = 0; r < have.size(); ++r) if (have[r] > 0.0f) share_sum += LOSS_SHARE[r];
    if (share_sum <= 0.0f) return;
    for (auto& b : value) {
        const auto r = index(role(b.arm, posture));
        const float ratio = std::fmin(loss * LOSS_SHARE[r] / share_sum / have[r], 1.0f);
        b.strength -= b.strength * ratio;
    }
}

float arm_firepower(Arm arm) {
    switch (arm) {
    case Arm::Infantry: return 1.0f;
    case Arm::Armor: return 2.0f;
    case Arm::Artillery: return 1.5f;
    }
    return 1.0f;
}

float terrain_defense(Terrain t) {
    switch (t) {
    case Terrain::Plains:
    case Terrain::Water: return 1.0f;
    case Terrain::Marsh: return 1.2f;
    case Terrain::Forest: return 1.5f;
    case Terrain::Urban:
    case Terrain::Mountain: return 2.0f;
    }
    return 1.0f;
}

float posture_defense(Posture p) {
    switch (p) {
    case Posture::Defend: return 1.0f;
    case Posture::Attack: return 0.8f;
    case Posture::March: return 0.5f;
    }
    return 1.0f;
}

float ammo_factor(float ammo) {
    return NO_AMMO_FACTOR + (1.0f - NO_AMMO_FACTOR) * std::clamp(ammo / DIVISION_TARGET[AMMO], 0.0f, 1.0f);
}

float food_factor(float food) {
    return 0.5f + 0.5f * std::clamp(food / DIVISION_TARGET[FOOD], 0.0f, 1.0f);
}

namespace {

struct Fighter {
    entt::entity entity;
    std::uint32_t id;
    std::uint8_t side;
    Pos pos;
    Front front;
    std::optional<Deployment> dep;
    Layout layout;
    float factor;

    std::pair<Pos, Pos> ends() const {
        return line_ends(pos, dep ? dep->facing : std::pair{1.0f, 0.0f}, layout.frontage_m);
    }

    bool frontal(Pos other) const {
        if (!dep) return true;
        const auto dir = unit(other.x - pos.x, other.y - pos.y);
        return !dir || dir->first * dep->facing.first + dir->second * dep->facing.second >= FLANK_COS;
    }
};

std::pair<float, float> retreat_dir(std::size_t me, const std::vector<std::size_t>& contacts,
                                    const std::vector<Fighter>& list, const entt::registry& world,
                                    const Map& map) {
    const auto pos = list[me].pos;
    const auto side = list[me].side;
    constexpr std::pair<float, float> zero{0.0f, 0.0f};
    const auto dir = [](float x, float y) { return unit(x, y).value_or(std::pair{0.0f, 0.0f}); };
    float away_x = 0.0f;
    float away_y = 0.0f;
    for (const auto j : contacts) {
        const auto [x, y] = dir(pos.x - list[j].pos.x, pos.y - list[j].pos.y);
        away_x += x;
        away_y += y;
    }
    const auto [ax, ay] = dir(away_x, away_y);
    std::optional<std::pair<float, std::uint32_t>> home;
    for (const auto [entity, d] : world.view<const Depot>().each()) {
        if (d.side != side) continue;
        const auto p = map.value.graph.node(d.node).pos;
        const float dx = p.x - pos.x;
        const float dy = p.y - pos.y;
        const auto candidate = std::pair{dx * dx + dy * dy, d.node.value};
        if (!home || candidate < *home) home = candidate;
    }
    auto heading = zero;
    if (home) {
        const auto p = map.value.graph.node(mapdata::NodeId{home->second}).pos;
        heading = dir(p.x - pos.x, p.y - pos.y);
    }
    const auto d = dir(ax + 0.5f * heading.first, ay + 0.5f * heading.second);
    return d == zero ? std::pair{ax, ay} : d;
}

}  // namespace

void resolve_combat(entt::registry& world) {
    auto& ctx = world.ctx();
    const auto& map = ctx.get<Map>();
    const auto& grid = map.value.terrain;
    auto& engagements = ctx.get<Engagements>().value;
    auto& ledger = ctx.get<Ledger>();
    const auto units = world.view<const UnitId, const Side, const Position, Battalions, Stocks>();

    std::vector<Fighter> list;
    for (const auto [entity, id, side, pos, bats, stocks] : units.each()) {
        const auto* component = world.try_get<Front>(entity);
        const Front front = component ? *component : Front{};
        std::optional<Deployment> dep;
        if (const auto* d = world.try_get<Deployment>(entity)) {
            dep = *d;
            dep->posture = Posture::Defend;
            if (world.all_of<MoveOrder>(entity)) dep->posture = front.ratio ? Posture::Attack : Posture::March;
        }
        const auto formation = layout(bats, dep ? dep->posture : Posture::Defend);
        const float factor = ammo_factor(stocks.value[AMMO]) * food_factor(stocks.value[FOOD]) * front.fire_factor();
        list.push_back({entity, id.value, side.value, pos.value, front, dep, formation, factor});
    }
    std::ranges::sort(list, {}, &Fighter::id);

    std::vector<std::pair<Pos, Pos>> ends;
    ends.reserve(list.size());
    for (const auto& u : list) ends.push_back(u.ends());
    std::vector<std::vector<std::size_t>> contacts(list.size());
    engagements.clear();
    for (std::size_t i = 0; i < list.size(); ++i) {
        for (std::size_t j = i + 1; j < list.size(); ++j) {
            if (list[i].side != list[j].side && segment_dist(ends[i], ends[j]) <= CONTACT_RANGE_M) {
                contacts[i].push_back(j);
                contacts[j].push_back(i);
                engagements.emplace_back(list[i].entity, list[j].entity);
            }
        }
    }

    // A fresh contact deploys immediately. An engaged line can only turn gradually.
    for (std::size_t i = 0; i < list.size(); ++i) {
        auto& me = list[i];
        if (!me.dep) continue;
        float sx = 0.0f;
        float sy = 0.0f;
        for (const auto j : contacts[i]) {
            sx = sx + list[j].pos.x - me.pos.x;
            sy = sy + list[j].pos.y - me.pos.y;
        }
        if (const auto to = unit(sx, sy)) {
            me.dep->facing = me.front.ratio ? turn_toward(me.dep->facing, *to, TURN_PER_STEP_RAD) : *to;
        }
    }

    // Resolve from the snapshot before applying any losses.
    std::vector<float> loss(list.size());
    std::vector<float> inflicted(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& targets = contacts[i];
        if (targets.empty()) continue;
        const auto& a = list[i];
        const auto ahead = std::ranges::count_if(targets, [&](auto j) { return a.frontal(list[j].pos); });
        const float sector = a.layout.frontage_m / std::max(static_cast<float>(ahead), 1.0f);
        for (const auto j : targets) {
            const auto& b = list[j];
            float fire = a.layout.depth_fire / static_cast<float>(targets.size());
            if (ahead > 0 && a.frontal(b.pos)) {
                fire += a.layout.line_fire / static_cast<float>(ahead) * std::min(b.layout.frontage_m / sector, 1.0f);
            }
            const auto cell = grid.cell_at(b.pos);
            const float terrain = cell ? terrain_defense(grid.get(cell->x, cell->y)) : 1.0f;
            const float posture = b.dep ? posture_defense(b.dep->posture) : 1.0f;
            const float flank = b.frontal(a.pos) ? 1.0f : FLANK_DEFENSE;
            const float hit = COMBAT_K * fire * a.factor / (terrain * posture * flank);
            loss[j] += hit;
            inflicted[i] += hit;
        }
    }

    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto& u = list[i];
        auto f = u.front;
        if (contacts[i].empty()) {
            f.ratio.reset();
            f.speed = 1.0f;
            f.retreat.reset();
            if (!f.encircled) {
                f.org = std::min(f.org + ORG_RECOVER_PER_DAY * static_cast<float>(cadence::COMBAT) /
                                 static_cast<float>(TICKS_PER_DAY), 1.0f);
            }
        } else {
            const float r = loss[i] > 0.0f ? inflicted[i] / loss[i] : std::numeric_limits<float>::infinity();
            const bool losing = r <= RETREAT_RATIO || f.org < ORG_BREAK || (f.retreat && f.org < ORG_RALLY);
            const float mult = f.encircled ? POCKET_LOSS : (f.hold && losing ? HOLD_LOSS : 1.0f);
            const float taken = loss[i] * mult;
            units.get<Battalions>(u.entity).take_loss(taken, u.dep ? u.dep->posture : Posture::Defend);
            const float org_loss = taken * ORG_PER_BN_LOST * (f.retreat ? RETREAT_ORG_FACTOR : 1.0f);
            f.org = std::max(f.org - org_loss, 0.0f);
            f.ratio = r;
            f.speed = advance_speed(r);
            f.retreat.reset();
            if (losing && !f.hold && !f.encircled) {
                f.retreat = retreat_dir(i, contacts[i], list, world, map);
                world.remove<MoveOrder>(u.entity);
            }
            auto& ammo = units.get<Stocks>(u.entity).value[AMMO];
            const float used = std::min(AMMO_PER_STEP_T, ammo);
            ammo -= used;
            ledger.consumed[AMMO] += used;
        }
        if (auto* front = world.try_get<Front>(u.entity); front && *front != f) *front = f;
        if (auto* dep = world.try_get<Deployment>(u.entity); dep && u.dep && *dep != *u.dep) *dep = *u.dep;
    }
}

void remove_destroyed(entt::registry& world) {
    auto& destroyed = world.ctx().get<Ledger>().destroyed;
    std::vector<entt::entity> removed;
    for (auto [entity, bats, stocks] : world.view<Battalions, const Stocks>().each()) {
        std::erase_if(bats.value, [](const auto& b) { return !(b.strength >= MIN_BATTALION_STRENGTH); });
        if (bats.value.empty()) removed.push_back(entity);
    }
    // Ledger accumulation and entity recycling both depend on removal order.
    const auto key = [&](auto e) {
        const auto* id = world.try_get<UnitId>(e);
        return std::pair{id ? id->value : std::numeric_limits<std::uint32_t>::max(), e};
    };
    std::ranges::sort(removed, {}, key);
    for (const auto entity : removed) {
        const auto& stocks = world.get<Stocks>(entity).value;
        for (std::size_t g = 0; g < destroyed.size(); ++g) destroyed[g] += stocks[g];
        world.destroy(entity);
    }
}

}  // namespace sim
