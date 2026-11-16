//! Rear area and partisans (architect.md 4.8, 12.2; F1-F3). Runs every 6 hours in `SimSet::Partisan`.
//!
//! Partisans live only in occupied regions (legal owner != military controller). Each region keeps a
//! hidden pool (battalion-equivalents). The pool grows by recruitment, is held down by occupier
//! garrisons, and acts: sabotage (edge damage), convoy raids, garrison attacks. Spotted partisans
//! appear on the map as units of the legal owner. The player sees only a risk grade (D5).

use crate::combat::{Arm, Battalion, Battalions};
use crate::supply::{Depot, EdgeState, Ledger, Pipeline, Stocks};
use crate::territory::{RegionStatus, Territory, region_status};
use crate::units::{Map, Position, Side, UnitId, UnitName};
use crate::SimClock;
use bevy_ecs::prelude::*;
use mapdata::{MapData, Pos, RegionId};

/// Deterministic RNG (splitmix64). Same seed + same commands = same game (architect.md principle 4).
#[derive(Resource, Debug, Clone)]
pub struct SimRng(pub u64);

impl Default for SimRng {
    fn default() -> Self {
        Self(0x5EED_2026)
    }
}

impl SimRng {
    pub fn next_u64(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    /// Uniform in [0, 1).
    pub fn next_f32(&mut self) -> f32 {
        (self.next_u64() >> 40) as f32 / (1u64 << 24) as f32
    }
}

/// Marks a spotted partisan band. It fights like a unit but does not hold ground or draw supply.
#[derive(Component, Debug, Clone, Copy)]
pub struct Partisan {
    pub region: RegionId,
}

#[derive(Resource, Debug, Clone)]
pub struct PartisanState {
    /// Hidden partisan strength per region (battalion-equivalents).
    pub pool: Vec<f32>,
    /// Population acceptance of the occupier, 0..1. Falls with harsh reprisals (and requisition, M7).
    pub support: Vec<f32>,
    pub next_unit_id: u32,
}

impl PartisanState {
    pub fn new(regions: usize) -> Self {
        Self { pool: vec![0.0; regions], support: vec![INITIAL_SUPPORT; regions], next_unit_id: PARTISAN_ID_BASE }
    }
}

/// Text events for the player (F3), newest last.
#[derive(Resource, Debug, Default)]
pub struct EventLog(pub Vec<(u64, String)>);

impl EventLog {
    pub fn push(&mut self, tick: u64, text: String) {
        self.0.push((tick, text));
        if self.0.len() > 100 {
            self.0.remove(0);
        }
    }
}

// shortcut: fixed tuning; move to Lua scenario data with population (G4) later.
pub const INITIAL_SUPPORT: f32 = 0.3;
pub const PARTISAN_ID_BASE: u32 = 100_000;
/// Pool gained per 6 h at zero support.
pub const RECRUIT_PER_STEP: f32 = 0.15;
/// Pool removed per 6 h per battalion of occupier garrison in the region.
pub const SUPPRESS_PER_BN: f32 = 0.05;
pub const SABOTAGE_DAMAGE: f32 = 0.3;
pub const RAID_LOSS: f32 = 0.3;
pub const GARRISON_ATTACK_LOSS: f32 = 0.2;
/// Crews repair this much damage per 6 h. shortcut: automatic; a RepairEdge command (E2) comes later.
pub const REPAIR_PER_STEP: f32 = 0.05;
/// Pool needed before a band can be spotted, and the most a single band takes from the pool.
pub const SPOT_THRESHOLD: f32 = 1.0;
pub const BAND_MAX: f32 = 2.0;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RiskGrade {
    None,
    Low,
    Medium,
    High,
}

/// What the player is allowed to see of a region's hidden pool (D5).
pub fn risk_grade(pool: f32) -> RiskGrade {
    match pool {
        p if p < 0.1 => RiskGrade::None,
        p if p < 0.75 => RiskGrade::Low,
        p if p < 2.0 => RiskGrade::Medium,
        _ => RiskGrade::High,
    }
}

fn region_of(map: &MapData, pos: Pos) -> Option<RegionId> {
    map.terrain.cell_at(pos).map(|(cx, cy)| map.region_at(cx, cy))
}

/// Anti-partisan sweep in a region. Harsh reprisals remove more partisans now but cut support and
/// push civilians into the partisans (F3).
pub fn anti_partisan(
    state: &mut PartisanState,
    log: &mut EventLog,
    map: &MapData,
    tick: u64,
    region: RegionId,
    harsh: bool,
) {
    let r = region.0 as usize;
    if r >= state.pool.len() {
        return;
    }
    let name = &map.regions[r].name;
    if harsh {
        state.pool[r] *= 0.4;
        state.support[r] = (state.support[r] - 0.2).max(0.0);
        let joined = 0.5 * (1.0 - state.support[r]);
        state.pool[r] += joined;
        log.push(tick, format!("Harsh reprisals in {name}: support falls, civilians join the partisans."));
    } else {
        state.pool[r] *= 0.7;
        log.push(tick, format!("Anti-partisan sweep in {name}."));
    }
}

#[allow(clippy::too_many_arguments)]
pub fn partisan_step(
    mut commands: Commands,
    clock: Res<SimClock>,
    map: Res<Map>,
    territory: Res<Territory>,
    mut rng: ResMut<SimRng>,
    mut state: ResMut<PartisanState>,
    mut edges: ResMut<EdgeState>,
    mut pipeline: ResMut<Pipeline>,
    mut ledger: ResMut<Ledger>,
    mut log: ResMut<EventLog>,
    depots: Query<(Entity, &Depot)>,
    mut units: Query<(&UnitId, &Side, &Position, &mut Battalions), Without<Partisan>>,
) {
    let m = &map.0;
    let tick = clock.tick;

    for d in &mut edges.damage {
        *d = (*d - REPAIR_PER_STEP).max(0.0);
    }

    let status = region_status(m, &territory);
    for (r, st) in status.iter().enumerate() {
        let RegionStatus::Occupied { by: occupier } = *st else { continue };
        let legal = territory.legal[r];
        let rid = RegionId(r as u16);
        let name = m.regions[r].name.clone();

        // Garrison: occupier battalions standing in the region, in id order for determinism.
        let mut garrison: Vec<(u32, f32)> = units
            .iter()
            .filter(|(_, s, p, _)| s.0 == occupier && region_of(m, p.0) == Some(rid))
            .map(|(id, _, _, b)| (id.0, b.total()))
            .collect();
        garrison.sort_by_key(|g| g.0);
        let security: f32 = garrison.iter().map(|g| g.1).sum();

        // Recruitment minus suppression (F2).
        let recruit = RECRUIT_PER_STEP * (1.0 - state.support[r]);
        let pool = (state.pool[r] + recruit - SUPPRESS_PER_BN * security).max(0.0);
        state.pool[r] = pool;

        // Activity.
        if rng.next_f32() < pool / (pool + 1.0 + 0.3 * security) {
            let roll = rng.next_f32();
            let region_edges: Vec<usize> = (0..m.graph.edges.len())
                .filter(|&e| {
                    let (a, b) = (m.graph.node(m.graph.edges[e].a).pos, m.graph.node(m.graph.edges[e].b).pos);
                    let mid = Pos { x: (a.x + b.x) / 2.0, y: (a.y + b.y) / 2.0 };
                    [a, b, mid].iter().any(|p| region_of(m, *p) == Some(rid))
                })
                .collect();
            let raid_target = pipeline.0.iter().position(|s| {
                depots.get(s.depot).is_ok_and(|(_, d)| region_of(m, m.graph.node(d.node).pos) == Some(rid))
            });

            if roll < 0.25 && let Some(i) = raid_target {
                let s = &mut pipeline.0[i];
                for g in 0..3 {
                    let lost = s.goods[g] * RAID_LOSS;
                    s.goods[g] -= lost;
                    ledger.destroyed[g] += lost;
                }
                log.push(tick, format!("Partisans ambushed a supply convoy in {name}."));
            } else if roll < 0.5 && let Some(&(target, _)) = garrison.first() {
                if let Some((.., mut b)) = units.iter_mut().find(|(id, ..)| id.0 == target) {
                    let total = b.total();
                    let ratio = (GARRISON_ATTACK_LOSS / total).min(1.0);
                    b.0.iter_mut().for_each(|x| x.strength -= x.strength * ratio);
                }
                log.push(tick, format!("Partisans attacked a garrison in {name}."));
            } else if !region_edges.is_empty() {
                let e = region_edges[(rng.next_u64() % region_edges.len() as u64) as usize];
                edges.damage[e] = (edges.damage[e] + SABOTAGE_DAMAGE).min(1.0);
                let n = |id| m.graph.node(id).name.clone().unwrap_or_default();
                let edge = &m.graph.edges[e];
                log.push(tick, format!("Sabotage on the line {} - {} ({name}).", n(edge.a), n(edge.b)));
            }
        }

        // Spotting (F1): part of the pool shows up as a band on the map.
        let spot_chance = (0.2 + 0.1 * security).min(0.9);
        if pool >= SPOT_THRESHOLD && rng.next_f32() < spot_chance {
            let cells: Vec<usize> = (0..m.region_cells.len()).filter(|&c| m.region_cells[c] == rid).collect();
            let c = cells[(rng.next_u64() % cells.len() as u64) as usize] as u32;
            let (w, cm) = (m.terrain.width, m.terrain.cell_m);
            let pos = Pos { x: ((c % w) as f32 + 0.5) * cm, y: ((c / w) as f32 + 0.5) * cm };
            let size = pool.min(BAND_MAX);
            state.pool[r] -= size;
            let id = state.next_unit_id;
            state.next_unit_id += 1;
            commands.spawn((
                Partisan { region: rid },
                UnitId(id),
                Side(legal),
                UnitName(format!("Partisans ({name})")),
                Position(pos),
                Battalions(vec![Battalion { arm: Arm::Infantry, strength: size }]),
                Stocks([0.0, 0.0, 0.0]),
            ));
            log.push(tick, format!("Partisan band spotted in {name}."));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        SupplyBase, SupplyBases, TICKS_PER_DAY, cadence, configure, depot, division, init_world,
    };
    use mapdata::{NodeId, test_map};

    /// West Province (region 3, holds most of rail edge 0) is legally Red but held by Blue.
    fn setup(pool: f32) -> (World, Schedule) {
        let mut world = World::new();
        let map = test_map();
        let mut legal = vec![0u8; 9];
        legal[3] = 1;
        let mut t = Territory::from_legal(&map, legal);
        for (c, r) in map.region_cells.iter().enumerate() {
            if r.0 == 3 {
                t.military[c] = 0;
            }
        }
        init_world(&mut world, map);
        world.insert_resource(t);
        world.resource_mut::<PartisanState>().pool[3] = pool;
        world.resource_mut::<SupplyBases>().0.push(SupplyBase { side: 0, node: NodeId(0), stock: [50_000.0; 3] });
        world.spawn(depot(0, NodeId(2)));
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        (world, schedule)
    }

    fn run_days(world: &mut World, schedule: &mut Schedule, days: u64) {
        for _ in 0..days * TICKS_PER_DAY {
            schedule.run(world);
        }
    }

    fn depot_stock(world: &mut World) -> f32 {
        world.query::<(&Depot, &Stocks)>().iter(world).map(|(_, s)| s.0.iter().sum::<f32>()).sum()
    }

    #[test]
    fn sabotage_cuts_supply_chain() {
        let (mut quiet, mut s1) = setup(0.0);
        // Quiet run: support 1.0 means no recruitment.
        quiet.resource_mut::<PartisanState>().support[3] = 1.0;
        let (mut hot, mut s2) = setup(20.0);
        hot.resource_mut::<PartisanState>().support[3] = 0.0;
        run_days(&mut quiet, &mut s1, 3);
        run_days(&mut hot, &mut s2, 3);

        let log = &hot.resource::<EventLog>().0;
        assert!(log.iter().any(|(_, t)| t.starts_with("Sabotage")), "expected sabotage, log: {log:?}");
        assert!(quiet.resource::<EventLog>().0.is_empty());
        let (q, h) = (depot_stock(&mut quiet), depot_stock(&mut hot));
        assert!(h < q - 100.0, "sabotage should starve the depot: quiet {q}, hot {h}");
    }

    #[test]
    fn home_regions_grow_no_partisans() {
        let mut world = World::new();
        init_world(&mut world, test_map());
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        run_days(&mut world, &mut schedule, 5);
        assert!(world.resource::<PartisanState>().pool.iter().all(|p| *p == 0.0));
        assert!(world.resource::<EventLog>().0.is_empty());
    }

    #[test]
    fn garrison_suppresses_growth() {
        let (mut empty, mut s1) = setup(0.0);
        let (mut held, mut s2) = setup(0.0);
        held.spawn(division(1, 0, "garrison", Pos { x: 40_000.0, y: 150_000.0 }));
        // Keep both runs free of random spotting by stepping only to the first partisan tick.
        for _ in 0..cadence::PARTISAN {
            s1.run(&mut empty);
            s2.run(&mut held);
        }
        let (a, b) = (empty.resource::<PartisanState>().pool[3], held.resource::<PartisanState>().pool[3]);
        assert!(a > 0.0 && b < a, "empty {a}, garrisoned {b}");
    }

    #[test]
    fn harsh_reprisal_cuts_support_and_feeds_partisans() {
        let map = test_map();
        let mut state = PartisanState::new(9);
        let mut log = EventLog::default();
        state.pool[3] = 2.0;
        let mut normal = state.clone();
        anti_partisan(&mut normal, &mut log, &map, 0, RegionId(3), false);
        anti_partisan(&mut state, &mut log, &map, 0, RegionId(3), true);
        assert!((normal.pool[3] - 1.4).abs() < 1e-5);
        assert!(state.support[3] < INITIAL_SUPPORT);
        // 2.0 * 0.4 + 0.5 * (1 - 0.1) = 1.25: fewer now, but support is gone for good.
        assert!((state.pool[3] - 1.25).abs() < 1e-5);
        assert_eq!(log.0.len(), 2);
    }

    #[test]
    fn spotted_band_appears_for_legal_owner_in_region() {
        let (mut world, mut schedule) = setup(5.0);
        run_days(&mut world, &mut schedule, 5);
        let mut q = world.query::<(&Partisan, &Side, &Position)>();
        let bands: Vec<_> = q.iter(&world).map(|(p, s, pos)| (p.region, s.0, pos.0)).collect();
        assert!(!bands.is_empty(), "a band should be spotted with a pool of 5");
        let map = test_map();
        for (r, side, pos) in bands {
            assert_eq!(side, 1, "partisans fight for the legal owner");
            assert_eq!(region_of(&map, pos), Some(r));
        }
    }

    #[test]
    fn same_seed_same_outcome() {
        let (mut a, mut s1) = setup(10.0);
        let (mut b, mut s2) = setup(10.0);
        run_days(&mut a, &mut s1, 4);
        run_days(&mut b, &mut s2, 4);
        assert_eq!(a.resource::<EdgeState>().damage, b.resource::<EdgeState>().damage);
        assert_eq!(a.resource::<EventLog>().0, b.resource::<EventLog>().0);
    }
}
