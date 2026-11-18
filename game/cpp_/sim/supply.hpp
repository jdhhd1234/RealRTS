#pragma once

#include "mapdata/mapdata.hpp"

#include <entt/entity/fwd.hpp>

#include <tuple>

namespace sim {

inline constexpr std::size_t FUEL = 0;
inline constexpr std::size_t AMMO = 1;
inline constexpr std::size_t FOOD = 2;
using Goods = std::array<float, 3>;

inline constexpr Goods DIVISION_TARGET{400.0f, 600.0f, 300.0f};
inline constexpr Goods DEPOT_TARGET{3000.0f, 3000.0f, 3000.0f};
inline constexpr Goods IDLE_USE_T_PER_DAY{10.0f, 5.0f, 30.0f};
inline constexpr float MOVING_FUEL_T_PER_DAY = 150.0f;
inline constexpr float TRUCK_FULL_RANGE_M = 50'000.0f;
inline constexpr float TRUCK_MAX_RANGE_M = 150'000.0f;
inline constexpr float TRUCK_T_PER_HOUR = 60.0f;

struct Stocks {
    Goods value{};
    bool operator==(const Stocks&) const = default;
};
struct StockTarget {
    Goods value{};
    bool operator==(const StockTarget&) const = default;
};
struct SupplyPriority {
    std::uint8_t value = 0;
    bool operator==(const SupplyPriority&) const = default;
};

struct Depot {
    std::uint8_t side = 0;
    mapdata::NodeId node;
};

std::tuple<Depot, Stocks> depot(std::uint8_t side, mapdata::NodeId node);
entt::entity depot(entt::registry& world, std::uint8_t side, mapdata::NodeId node);

struct SupplyBase {
    std::uint8_t side = 0;
    mapdata::NodeId node;
    Goods stock{};
};
struct SupplyBases { std::vector<SupplyBase> value; };

struct Shipment {
    entt::entity depot{};
    Goods goods{};
    std::uint64_t arrive_tick = 0;
};
struct Pipeline { std::vector<Shipment> value; };

struct EdgeState {
    std::vector<float> damage;
    std::vector<float> last_flow;

    // QUESTION: Should Rust's new() factories have a project-wide C++ spelling?
    static EdgeState make(std::size_t edges);
};

struct Ledger {
    Goods produced{};
    Goods consumed{};
    Goods destroyed{};
};

float capacity_t_per_hour(const mapdata::TransportGraph& graph, const EdgeState& edges, std::size_t e);
float truck_efficiency(float dist_m);
std::optional<std::pair<float, entt::entity>> nearest_depot(
    const mapdata::TransportGraph& graph, const entt::registry& world, std::uint8_t side, mapdata::Pos pos);
void consume(entt::registry& world);
void receive(entt::registry& world);
void dispatch(entt::registry& world);
void deliver(entt::registry& world);

}  // namespace sim
