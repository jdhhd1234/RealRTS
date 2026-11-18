//! Supply (architect.md 4.7). Runs hourly in `SimSet::Supply`, in this order:
//! consume -> receive arrived shipments -> dispatch base->depot over the graph -> deliver depot->units by truck.
//!
//! Conservation invariant: bases + pipeline + depots + units + ledger(consumed + destroyed) is constant
//! until production (M7) adds to bases.

use crate::front::Front;
use crate::territory::Territory;
use crate::units::{Map, MoveOrder, Position, Side, UnitId};
use crate::SimClock;
use bevy_ecs::prelude::*;
use mapdata::{EdgeKind, NodeId, Pos, TransportGraph};
use std::cmp::Reverse;
use std::collections::BinaryHeap;

pub const FUEL: usize = 0;
pub const AMMO: usize = 1;
pub const FOOD: usize = 2;
/// Tons of fuel, ammo, food (E1).
pub type Goods = [f32; 3];

// shortcut: fixed numbers for every division/depot; per-unit values come from Lua later.
pub const DIVISION_TARGET: Goods = [400.0, 600.0, 300.0];
pub const DEPOT_TARGET: Goods = [3000.0, 3000.0, 3000.0];
pub const IDLE_USE_T_PER_DAY: Goods = [10.0, 5.0, 30.0];
pub const MOVING_FUEL_T_PER_DAY: f32 = 150.0;
/// Trucks deliver fully up to this distance from a depot, falling linearly to 0 at max range (E4).
pub const TRUCK_FULL_RANGE_M: f32 = 50_000.0;
pub const TRUCK_MAX_RANGE_M: f32 = 150_000.0;
/// Per unit, per good, per hour, at full truck efficiency.
pub const TRUCK_T_PER_HOUR: f32 = 60.0;

#[derive(Component, Debug, Clone, Copy, PartialEq)]
pub struct Stocks(pub Goods);

#[derive(Component, Debug, Clone, Copy, PartialEq)]
pub struct StockTarget(pub Goods);

/// Higher is served first.
#[derive(Component, Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct SupplyPriority(pub u8);

/// Field depot on a graph node (player-placed in E2; placement command not yet added).
#[derive(Component, Debug, Clone, Copy)]
pub struct Depot {
    pub side: u8,
    pub node: NodeId,
}

pub fn depot(side: u8, node: NodeId) -> impl Bundle {
    (Depot { side, node }, Stocks([0.0; 3]))
}

/// Rear base where national stock enters the network. M7 production adds to `stock`.
#[derive(Debug, Clone)]
pub struct SupplyBase {
    pub side: u8,
    pub node: NodeId,
    pub stock: Goods,
}

#[derive(Resource, Default)]
pub struct SupplyBases(pub Vec<SupplyBase>);

#[derive(Debug, Clone)]
pub struct Shipment {
    pub depot: Entity,
    pub goods: Goods,
    pub arrive_tick: u64,
}

/// Goods in transit between base and depot.
#[derive(Resource, Default)]
pub struct Pipeline(pub Vec<Shipment>);

/// Runtime edge state, indexed like `TransportGraph::edges`.
#[derive(Resource, Default)]
pub struct EdgeState {
    /// 0 = intact, 1 = cut. Partisans raise it in M6.
    pub damage: Vec<f32>,
    /// Tons sent over each edge in the last supply hour.
    pub last_flow: Vec<f32>,
}

impl EdgeState {
    pub fn new(edges: usize) -> Self {
        Self { damage: vec![0.0; edges], last_flow: vec![0.0; edges] }
    }
}

#[derive(Resource, Default, Debug)]
pub struct Ledger {
    /// Added by production and requisition (M7).
    pub produced: Goods,
    pub consumed: Goods,
    /// Lost in transit (e.g. destination depot gone).
    pub destroyed: Goods,
}

pub fn capacity_t_per_hour(graph: &TransportGraph, edges: &EdgeState, e: usize) -> f32 {
    graph.edges[e].capacity_t_per_day / 24.0 * (1.0 - edges.damage[e].clamp(0.0, 1.0))
}

fn transit_minutes(kind: EdgeKind, length_m: f32) -> u64 {
    let speed_m_per_h = match kind {
        EdgeKind::Rail { .. } => 30_000.0,
        EdgeKind::Road => 20_000.0,
        EdgeKind::River => 10_000.0,
    };
    (length_m / speed_m_per_h * 60.0).ceil() as u64
}

/// Fastest route using only edges with spare capacity whose both end nodes `side` controls
/// (architect.md 12.4). Returns (edge indices, minutes).
/// shortcut: rebuilds adjacency per call and ignores gauge (E3); fine for small graphs.
fn route(
    graph: &TransportGraph,
    spare: &[f32],
    node_owner: &[u8],
    side: u8,
    from: NodeId,
    to: NodeId,
) -> Option<(Vec<usize>, u64)> {
    let n = graph.nodes.len();
    let mut best = vec![u64::MAX; n];
    let mut via = vec![usize::MAX; n];
    let mut open = BinaryHeap::new();
    best[from.0 as usize] = 0;
    open.push(Reverse((0u64, from.0)));
    while let Some(Reverse((t, node))) = open.pop() {
        if node == to.0 {
            break;
        }
        if t > best[node as usize] {
            continue;
        }
        for (e, edge) in graph.edges.iter().enumerate() {
            if spare[e] <= 1e-3 || node_owner[edge.a.0 as usize] != side || node_owner[edge.b.0 as usize] != side {
                continue;
            }
            let next = if edge.a.0 == node {
                edge.b.0
            } else if edge.b.0 == node {
                edge.a.0
            } else {
                continue;
            };
            let nt = t + transit_minutes(edge.kind, edge.length_m);
            if nt < best[next as usize] {
                best[next as usize] = nt;
                via[next as usize] = e;
                open.push(Reverse((nt, next)));
            }
        }
    }
    if best[to.0 as usize] == u64::MAX {
        return None;
    }
    let mut path = Vec::new();
    let mut node = to.0;
    while node != from.0 {
        let e = via[node as usize];
        path.push(e);
        let edge = &graph.edges[e];
        node = if edge.a.0 == node { edge.b.0 } else { edge.a.0 };
    }
    path.reverse();
    Some((path, best[to.0 as usize]))
}

pub fn consume(mut units: Query<(&mut Stocks, Has<MoveOrder>), With<UnitId>>, mut ledger: ResMut<Ledger>) {
    for (mut stocks, moving) in &mut units {
        let mut per_day = IDLE_USE_T_PER_DAY;
        if moving {
            per_day[FUEL] = MOVING_FUEL_T_PER_DAY;
        }
        for g in 0..3 {
            let used = (per_day[g] / 24.0).min(stocks.0[g]);
            stocks.0[g] -= used;
            ledger.consumed[g] += used;
        }
    }
}

pub fn receive(
    clock: Res<SimClock>,
    mut pipeline: ResMut<Pipeline>,
    mut depots: Query<&mut Stocks, With<Depot>>,
    mut ledger: ResMut<Ledger>,
) {
    pipeline.0.retain(|s| {
        if s.arrive_tick > clock.tick {
            return true;
        }
        match depots.get_mut(s.depot) {
            Ok(mut stocks) => (0..3).for_each(|g| stocks.0[g] += s.goods[g]),
            Err(_) => (0..3).for_each(|g| ledger.destroyed[g] += s.goods[g]),
        }
        false
    });
}

/// Greedy: depots in node order each take what the cheapest route's spare capacity allows.
/// shortcut: greedy, not min-cost max-flow (architect.md 4.7 step 3); swap in later behind this system.
pub fn dispatch(
    clock: Res<SimClock>,
    map: Res<Map>,
    mut bases: ResMut<SupplyBases>,
    mut edges: ResMut<EdgeState>,
    mut pipeline: ResMut<Pipeline>,
    depots: Query<(Entity, &Depot, &Stocks)>,
    territory: Res<Territory>,
) {
    let graph = &map.0.graph;
    let mut spare: Vec<f32> = (0..graph.edges.len()).map(|e| capacity_t_per_hour(graph, &edges, e)).collect();
    let node_owner: Vec<u8> =
        graph.nodes.iter().map(|n| territory.military_at(&map.0, n.pos).expect("validated: nodes inside map")).collect();
    edges.last_flow.iter_mut().for_each(|f| *f = 0.0);

    let mut list: Vec<_> = depots.iter().collect();
    list.sort_by_key(|(e, d, _)| (d.node, *e));
    for (entity, depot, stocks) in list {
        let Some(base) = bases.0.iter_mut().find(|b| b.side == depot.side) else { continue };
        let mut need = [0.0; 3];
        for g in 0..3 {
            let in_transit: f32 = pipeline.0.iter().filter(|s| s.depot == entity).map(|s| s.goods[g]).sum();
            need[g] = (DEPOT_TARGET[g] - stocks.0[g] - in_transit).max(0.0).min(base.stock[g]);
        }
        let total: f32 = need.iter().sum();
        if total <= 0.0 {
            continue;
        }
        let Some((path, minutes)) = route(graph, &spare, &node_owner, depot.side, base.node, depot.node) else {
            continue;
        };
        let bottleneck = path.iter().map(|&e| spare[e]).fold(f32::INFINITY, f32::min);
        let scale = (bottleneck / total).min(1.0);
        let send = need.map(|n| n * scale);
        let sent: f32 = send.iter().sum();
        for g in 0..3 {
            base.stock[g] -= send[g];
        }
        for &e in &path {
            spare[e] -= sent;
            edges.last_flow[e] += sent;
        }
        pipeline.0.push(Shipment { depot: entity, goods: send, arrive_tick: clock.tick + minutes.max(1) });
    }
}

/// Truck efficiency 1..0 by distance from depot to unit.
pub fn truck_efficiency(dist_m: f32) -> f32 {
    ((TRUCK_MAX_RANGE_M - dist_m) / (TRUCK_MAX_RANGE_M - TRUCK_FULL_RANGE_M)).clamp(0.0, 1.0)
}

/// Nearest own depot within truck range: (distance m, depot entity). Ties go to the lower node id.
pub fn nearest_depot<'a>(
    graph: &TransportGraph,
    depots: impl Iterator<Item = (Entity, &'a Depot)>,
    side: u8,
    pos: Pos,
) -> Option<(f32, Entity)> {
    depots
        .filter(|(_, d)| d.side == side)
        .map(|(e, d)| {
            let p = graph.node(d.node).pos;
            (((p.x - pos.x).powi(2) + (p.y - pos.y).powi(2)).sqrt(), d.node, e)
        })
        .filter(|(m, _, _)| *m < TRUCK_MAX_RANGE_M)
        .min_by(|a, b| a.0.total_cmp(&b.0).then(a.1.cmp(&b.1)))
        .map(|(m, _, e)| (m, e))
}

/// Each unit draws from its nearest own depot in truck range. Higher priority first; equal priority
/// shares a shortfall in proportion to request.
pub fn deliver(
    map: Res<Map>,
    mut depots: Query<(Entity, &Depot, &mut Stocks), Without<UnitId>>,
    mut units: Query<
        (Entity, &UnitId, &Side, &Position, &mut Stocks, &StockTarget, &SupplyPriority, Option<&Front>),
        Without<Depot>,
    >,
) {
    let graph = &map.0.graph;

    struct Req {
        unit: Entity,
        depot: Entity,
        prio: u8,
        id: u32,
        want: Goods,
        got: Goods,
    }
    let mut reqs = Vec::new();
    for (unit, id, side, pos, stocks, target, prio, front) in &units {
        if front.is_some_and(|f| f.encircled) {
            continue; // trucks cannot cross enemy ground into a pocket (13.4)
        }
        let nearest = nearest_depot(graph, depots.iter().map(|(e, d, _)| (e, d)), side.0, pos.0);
        let Some((m, depot)) = nearest else { continue }; // isolated: no supply
        let cap = TRUCK_T_PER_HOUR * truck_efficiency(m);
        let want = std::array::from_fn(|g| (target.0[g] - stocks.0[g]).max(0.0).min(cap));
        reqs.push(Req { unit, depot, prio: prio.0, id: id.0, want, got: [0.0; 3] });
    }
    reqs.sort_by_key(|r| (r.depot, Reverse(r.prio), r.id));

    let mut i = 0;
    while i < reqs.len() {
        // Group by priority within one depot.
        let j = (i..reqs.len()).find(|&k| reqs[k].depot != reqs[i].depot || reqs[k].prio != reqs[i].prio).unwrap_or(reqs.len());
        let Ok((_, _, mut stock)) = depots.get_mut(reqs[i].depot) else { unreachable!("depot came from this query") };
        for g in 0..3 {
            let asked: f32 = reqs[i..j].iter().map(|r| r.want[g]).sum();
            if asked <= 0.0 || stock.0[g] <= 0.0 {
                continue;
            }
            let ratio = (stock.0[g] / asked).min(1.0);
            for r in &mut reqs[i..j] {
                let give = (r.want[g] * ratio).min(stock.0[g]);
                r.got[g] = give;
                stock.0[g] -= give;
            }
        }
        i = j;
    }

    for r in reqs {
        if let Ok((.., mut stocks, _, _, _)) = units.get_mut(r.unit) {
            (0..3).for_each(|g| stocks.0[g] += r.got[g]);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{configure, division, init_world, CommandQueue, SimCommand, TICKS_PER_DAY, TICKS_PER_HOUR};
    use mapdata::test_map;

    /// Everything that holds goods, plus what left the system.
    fn total(world: &mut World) -> f32 {
        let mut sum = 0.0;
        sum += world.resource::<SupplyBases>().0.iter().flat_map(|b| b.stock).sum::<f32>();
        sum += world.resource::<Pipeline>().0.iter().flat_map(|s| s.goods).sum::<f32>();
        let l = world.resource::<Ledger>();
        sum += l.consumed.iter().chain(&l.destroyed).sum::<f32>();
        sum -= l.produced.iter().sum::<f32>();
        sum += world.query::<&Stocks>().iter(world).flat_map(|s| s.0).sum::<f32>();
        sum
    }

    fn setup(base_stock: f32) -> (World, Schedule) {
        let mut world = World::new();
        init_world(&mut world, test_map());
        world.resource_mut::<SupplyBases>().0.push(SupplyBase { side: 0, node: NodeId(0), stock: [base_stock; 3] });
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        (world, schedule)
    }

    fn km(x: f32, y: f32) -> Pos {
        Pos { x: x * 1000.0, y: y * 1000.0 }
    }

    fn stocks_of(world: &mut World, id: u32) -> Goods {
        let mut q = world.query::<(&UnitId, &Stocks)>();
        q.iter(world).find(|(u, _)| u.0 == id).unwrap().1.0
    }

    #[test]
    fn goods_are_conserved_over_three_days() {
        let (mut world, mut schedule) = setup(20_000.0);
        world.spawn(depot(0, NodeId(2))); // East City, 197 km of rail from base
        world.spawn(division(1, 0, "A", km(210.0, 160.0)));
        world.spawn(division(2, 0, "B", km(260.0, 150.0)));
        world.resource_mut::<CommandQueue>().0.push(SimCommand::Move { unit: UnitId(2), goal: km(200.0, 100.0) });
        let start = total(&mut world);
        for _ in 0..3 * TICKS_PER_DAY {
            schedule.run(&mut world);
        }
        let end = total(&mut world);
        assert!((start - end).abs() < 0.5, "start {start}, end {end}");
        assert!(world.resource::<Ledger>().consumed[FOOD] > 0.0);
    }

    #[test]
    fn edges_never_exceed_capacity_and_cut_edge_blocks() {
        let (mut world, mut schedule) = setup(50_000.0);
        world.spawn(depot(0, NodeId(2)));
        let mut max_seen = 0.0f32;
        for _ in 0..TICKS_PER_DAY {
            schedule.run(&mut world);
            let edges = world.resource::<EdgeState>();
            let graph = &world.resource::<Map>().0.graph;
            for (e, f) in edges.last_flow.iter().enumerate() {
                assert!(*f <= capacity_t_per_hour(graph, edges, e) + 1e-3, "edge {e} flow {f}");
                max_seen = max_seen.max(*f);
            }
        }
        // Demand (3000 t x 3) exceeds rail capacity (125 t/h), so the line ran full.
        assert!((max_seen - 125.0).abs() < 1e-3, "max flow {max_seen}");

        // Cut the first rail edge: nothing new can be dispatched.
        world.resource_mut::<EdgeState>().damage[0] = 1.0;
        let before = world.resource::<Pipeline>().0.len();
        for _ in 0..TICKS_PER_HOUR {
            schedule.run(&mut world);
        }
        assert_eq!(world.resource::<EdgeState>().last_flow[0], 0.0);
        assert!(world.resource::<Pipeline>().0.len() <= before);
    }

    #[test]
    fn rail_through_enemy_ground_is_blocked() {
        let (mut world, mut schedule) = setup(20_000.0);
        world.spawn(depot(0, NodeId(2)));
        // Red holds the ground around Forest Junction (node 1), cutting West Base -> East City.
        {
            let map = test_map();
            let mut t = world.resource_mut::<Territory>();
            let (cx, cy) = map.terrain.cell_at(map.graph.node(NodeId(1)).pos).unwrap();
            t.military[(cy * map.terrain.width + cx) as usize] = 1;
        }
        for _ in 0..TICKS_PER_HOUR {
            schedule.run(&mut world);
        }
        assert!(world.resource::<Pipeline>().0.is_empty());
        assert!(world.resource::<EdgeState>().last_flow.iter().all(|f| *f == 0.0));
    }

    #[test]
    fn isolated_unit_gets_nothing() {
        let (mut world, mut schedule) = setup(20_000.0);
        let mut d = world.spawn(depot(0, NodeId(0)));
        d.insert(Stocks([3000.0; 3]));
        world.spawn(division(1, 0, "far", km(250.0, 250.0))); // > 150 km from West Base
        world.spawn(division(2, 0, "near", km(20.0, 150.0)));
        for _ in 0..TICKS_PER_DAY {
            schedule.run(&mut world);
        }
        let far = stocks_of(&mut world, 1);
        let near = stocks_of(&mut world, 2);
        assert!(far[FOOD] < DIVISION_TARGET[FOOD] - 25.0, "far unit only consumed: {far:?}");
        assert!((near[FOOD] - DIVISION_TARGET[FOOD]).abs() < 2.0, "near unit kept full: {near:?}");
    }

    #[test]
    fn priority_unit_served_first_when_depot_short() {
        let (mut world, mut schedule) = setup(0.0); // no resupply into the depot
        let mut d = world.spawn(depot(0, NodeId(0)));
        d.insert(Stocks([0.0, 0.0, 10.0]));
        for (id, prio) in [(1, 0), (2, 5)] {
            let mut u = world.spawn(division(id, 0, "u", km(15.0, 150.0)));
            u.insert((Stocks([400.0, 600.0, 200.0]), SupplyPriority(prio)));
        }
        for _ in 0..TICKS_PER_HOUR {
            schedule.run(&mut world);
        }
        let low = stocks_of(&mut world, 1)[FOOD];
        let high = stocks_of(&mut world, 2)[FOOD];
        let hour_use = IDLE_USE_T_PER_DAY[FOOD] / 24.0;
        assert!((high - (200.0 - hour_use + 10.0)).abs() < 1e-3, "high got all 10 t: {high}");
        assert!((low - (200.0 - hour_use)).abs() < 1e-3, "low got none: {low}");
    }

    #[test]
    fn truck_efficiency_curve() {
        assert_eq!(truck_efficiency(0.0), 1.0);
        assert_eq!(truck_efficiency(TRUCK_FULL_RANGE_M), 1.0);
        assert_eq!(truck_efficiency(100_000.0), 0.5);
        assert_eq!(truck_efficiency(TRUCK_MAX_RANGE_M), 0.0);
    }
}
