//! Partial economy (architect.md 4.9, G1-G4). Runs daily in `SimSet::Economy`:
//! regional production -> side's rear base (supply entry), requisition in occupied regions,
//! battalion replacements from national equipment, and new divisions (G3).
//!
//! Goods conservation (fuel/ammo/food): bases + pipeline + depots + units + builds in progress
//! + consumed + destroyed - produced is constant. Equipment: stock + used - produced is constant.

use crate::combat::Battalions;
use crate::partisan::{EventLog, PartisanState};
use crate::supply::{DIVISION_TARGET, Depot, Goods, Ledger, StockTarget, SupplyBases, nearest_depot, FOOD};
use crate::territory::{MAX_SIDES, RegionStatus, Territory, region_status};
use crate::units::{Map, Position, Side, UnitId, division};
use crate::SimClock;
use crate::TICKS_PER_DAY;
use bevy_ecs::prelude::*;

/// Index of equipment in `Region::daily_output`.
pub const EQUIPMENT: usize = 3;
/// Output share the legal owner gets from a contested region (12.2).
pub const CONTESTED_OUTPUT: f32 = 0.5;
pub const REQUISITION_SHARE: f32 = 0.5;
pub const REQUISITION_SUPPORT_LOSS: f32 = 0.02;
/// Equipment tons per 1.0 battalion strength.
pub const EQUIPMENT_PER_BN: f32 = 50.0;
/// Most strength a battalion regains per day.
pub const REPLACE_PER_DAY: f32 = 0.05;
pub const BUILD_DAYS: u64 = 14;
/// A new division costs its starting stocks plus equipment for 10 battalions.
pub const DIVISION_EQUIPMENT: f32 = 10.0 * EQUIPMENT_PER_BN;
pub const BUILT_ID_BASE: u32 = 10_000;

/// National equipment stock per side (not shipped; replacements are abstract).
#[derive(Resource, Debug, Clone)]
pub struct Nations {
    pub equipment: Vec<f32>,
    pub equipment_produced: f32,
    pub equipment_used: f32,
}

impl Default for Nations {
    fn default() -> Self {
        Self { equipment: vec![0.0; MAX_SIDES as usize], equipment_produced: 0.0, equipment_used: 0.0 }
    }
}

#[derive(Debug, Clone)]
pub struct Build {
    pub side: u8,
    pub name: String,
    pub ready_tick: u64,
    /// Starting stocks, already taken from the base.
    pub stocks: Goods,
}

#[derive(Resource, Debug, Clone)]
pub struct EconomyState {
    /// Requisition on/off per region; applies when the region is occupied (by the occupier).
    pub requisition: Vec<bool>,
    pub builds: Vec<Build>,
    pub next_unit_id: u32,
}

impl EconomyState {
    pub fn new(regions: usize) -> Self {
        Self { requisition: vec![false; regions], builds: Vec::new(), next_unit_id: BUILT_ID_BASE }
    }
}

/// Order a new division (G3). Pays now; it appears at the side's base after `BUILD_DAYS`.
/// Returns false (and logs why) if the side cannot pay.
pub fn order_division(
    side: u8,
    name: String,
    tick: u64,
    bases: &mut SupplyBases,
    nations: &mut Nations,
    econ: &mut EconomyState,
    log: &mut EventLog,
) -> bool {
    let Some(base) = bases.0.iter_mut().find(|b| b.side == side) else { return false };
    let eq = nations.equipment.get_mut(side as usize);
    let affordable = (0..3).all(|g| base.stock[g] >= DIVISION_TARGET[g]) && eq.as_ref().is_some_and(|e| **e >= DIVISION_EQUIPMENT);
    if !affordable {
        log.push(tick, format!("Cannot raise {name}: not enough supplies or equipment."));
        return false;
    }
    (0..3).for_each(|g| base.stock[g] -= DIVISION_TARGET[g]);
    *eq.unwrap() -= DIVISION_EQUIPMENT;
    nations.equipment_used += DIVISION_EQUIPMENT;
    log.push(tick, format!("Raising {name}: ready in {BUILD_DAYS} days."));
    econ.builds.push(Build { side, name, ready_tick: tick + BUILD_DAYS * TICKS_PER_DAY, stocks: DIVISION_TARGET });
    true
}

#[allow(clippy::too_many_arguments)]
pub fn economy_step(
    mut commands: Commands,
    clock: Res<SimClock>,
    map: Res<Map>,
    territory: Res<Territory>,
    mut bases: ResMut<SupplyBases>,
    mut nations: ResMut<Nations>,
    mut econ: ResMut<EconomyState>,
    mut ledger: ResMut<Ledger>,
    mut partisans: ResMut<PartisanState>,
    mut log: ResMut<EventLog>,
    depots: Query<(Entity, &Depot)>,
    mut units: Query<(&UnitId, &Side, &Position, &mut Battalions, Option<&crate::front::Front>), With<StockTarget>>,
) {
    let m = &map.0;
    let tick = clock.tick;

    // Production and requisition.
    // shortcut: output goes straight into the side's first base; no raw-material links or labor yet (G4).
    for (r, status) in region_status(m, &territory).iter().enumerate() {
        let out = m.regions[r].daily_output;
        let legal = territory.legal[r];
        let share = match status {
            RegionStatus::Home => 1.0,
            RegionStatus::Contested => CONTESTED_OUTPUT,
            RegionStatus::Occupied { .. } => 0.0,
        };
        if share > 0.0 && let Some(base) = bases.0.iter_mut().find(|b| b.side == legal) {
            for g in 0..3 {
                base.stock[g] += out[g] * share;
                ledger.produced[g] += out[g] * share;
            }
            nations.equipment[legal as usize] += out[EQUIPMENT] * share;
            nations.equipment_produced += out[EQUIPMENT] * share;
        }
        if let RegionStatus::Occupied { by } = *status
            && econ.requisition[r]
            && let Some(base) = bases.0.iter_mut().find(|b| b.side == by)
        {
            let food = out[FOOD] * REQUISITION_SHARE;
            base.stock[FOOD] += food;
            ledger.produced[FOOD] += food;
            partisans.support[r] = (partisans.support[r] - REQUISITION_SUPPORT_LOSS).max(0.0);
        }
    }

    // Replacements for divisions in reach of an own depot, in id order.
    let mut list: Vec<_> = units.iter_mut().collect();
    list.sort_by_key(|(id, ..)| id.0);
    for (_, side, pos, mut bats, front) in list {
        if front.is_some_and(|f| f.encircled) || nearest_depot(&m.graph, depots.iter(), side.0, pos.0).is_none() {
            continue;
        }
        let nations = &mut *nations;
        for b in &mut bats.0 {
            let stock = &mut nations.equipment[side.0 as usize];
            let gain = (1.0 - b.strength).clamp(0.0, REPLACE_PER_DAY).min(*stock / EQUIPMENT_PER_BN);
            b.strength += gain;
            *stock -= gain * EQUIPMENT_PER_BN;
            nations.equipment_used += gain * EQUIPMENT_PER_BN;
        }
    }

    // Finished divisions appear at the base.
    let (ready, waiting): (Vec<Build>, Vec<Build>) = econ.builds.drain(..).partition(|b| b.ready_tick <= tick);
    econ.builds = waiting;
    for b in ready {
        let Some(base) = bases.0.iter().find(|x| x.side == b.side) else { continue };
        let id = econ.next_unit_id;
        econ.next_unit_id += 1;
        commands.spawn(division(id, b.side, &b.name, m.graph.node(base.node).pos));
        log.push(tick, format!("{} is ready.", b.name));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::supply::{Pipeline, Stocks, SupplyBase};
    use crate::{Arm, Battalion, CommandQueue, SimCommand, configure, depot, init_world};
    use mapdata::{NodeId, Pos, RegionId, test_map};

    fn km(x: f32, y: f32) -> Pos {
        Pos { x: x * 1000.0, y: y * 1000.0 }
    }

    /// Columns 0-1 Blue, column 2 Red (like the app scenario). Bases at West Base and Front Railhead.
    fn setup() -> (World, Schedule) {
        let mut world = World::new();
        let map = test_map();
        let t = Territory::from_legal(&map, (0..9).map(|r| if r % 3 == 2 { 1 } else { 0 }).collect());
        init_world(&mut world, map);
        world.insert_resource(t);
        world.resource_mut::<SupplyBases>().0 = vec![
            SupplyBase { side: 0, node: NodeId(0), stock: [10_000.0; 3] },
            SupplyBase { side: 1, node: NodeId(3), stock: [10_000.0; 3] },
        ];
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        (world, schedule)
    }

    fn days(world: &mut World, schedule: &mut Schedule, n: u64) {
        for _ in 0..n * TICKS_PER_DAY {
            schedule.run(world);
        }
    }

    fn goods_total(world: &mut World) -> f32 {
        let mut sum: f32 = world.resource::<SupplyBases>().0.iter().flat_map(|b| b.stock).sum();
        sum += world.resource::<Pipeline>().0.iter().flat_map(|s| s.goods).sum::<f32>();
        sum += world.resource::<EconomyState>().builds.iter().flat_map(|b| b.stocks).sum::<f32>();
        sum += world.query::<&Stocks>().iter(world).flat_map(|s| s.0).sum::<f32>();
        let l = world.resource::<Ledger>();
        sum + l.consumed.iter().chain(&l.destroyed).sum::<f32>() - l.produced.iter().sum::<f32>()
    }

    fn equipment_total(world: &World) -> f32 {
        let n = world.resource::<Nations>();
        n.equipment.iter().sum::<f32>() + n.equipment_used - n.equipment_produced
    }

    fn base_stock(world: &World, side: u8) -> Goods {
        world.resource::<SupplyBases>().0.iter().find(|b| b.side == side).unwrap().stock
    }

    #[test]
    fn home_regions_feed_their_base_daily() {
        let (mut world, mut schedule) = setup();
        days(&mut world, &mut schedule, 1);
        let map = test_map();
        let expect = |side: u8, g: usize| -> f32 {
            (0..9).filter(|r| (r % 3 == 2) == (side == 1)).map(|r| map.regions[r].daily_output[g]).sum()
        };
        for g in 0..3 {
            assert!((base_stock(&world, 0)[g] - (10_000.0 + expect(0, g))).abs() < 1e-2);
            assert!((base_stock(&world, 1)[g] - (10_000.0 + expect(1, g))).abs() < 1e-2);
        }
        assert!((world.resource::<Nations>().equipment[0] - expect(0, EQUIPMENT)).abs() < 1e-3);
    }

    #[test]
    fn occupied_region_produces_nothing_for_owner_but_can_be_requisitioned() {
        let (mut world, mut schedule) = setup();
        // Red occupies W Province (region 3: food 40) and requisitions it.
        {
            let map = test_map();
            let mut t = world.resource_mut::<Territory>();
            for (c, r) in map.region_cells.iter().enumerate() {
                if r.0 == 3 {
                    t.military[c] = 1;
                }
            }
        }
        world.resource_mut::<CommandQueue>().0.push(SimCommand::SetRequisition { region: RegionId(3), on: true });
        let support_before = world.resource::<PartisanState>().support[3];
        days(&mut world, &mut schedule, 1);
        let blue_food = base_stock(&world, 0)[FOOD];
        let red_food = base_stock(&world, 1)[FOOD];
        assert!((blue_food - (10_000.0 + 60.0 * 2.0 + 40.0 + 50.0 * 2.0)).abs() < 1e-2, "W lost to Blue: {blue_food}");
        assert!((red_food - (10_000.0 + 60.0 + 40.0 + 50.0 + 20.0)).abs() < 1e-2, "Red requisitions 20: {red_food}");
        assert!(world.resource::<PartisanState>().support[3] < support_before);
    }

    #[test]
    fn production_and_consumption_balance_over_ten_days() {
        let (mut world, mut schedule) = setup();
        world.spawn(depot(0, NodeId(1)));
        world.spawn(depot(1, NodeId(3)));
        world.spawn(division(1, 0, "blue", km(150.0, 150.0)));
        world.spawn(division(2, 1, "red", km(162.0, 150.0))); // in contact: combat burns ammo
        world.spawn(division(3, 0, "reserve", km(30.0, 140.0)));
        let start = goods_total(&mut world);
        let eq_start = equipment_total(&world);
        days(&mut world, &mut schedule, 10);
        let end = goods_total(&mut world);
        assert!((start - end).abs() < 1.0, "goods: start {start}, end {end}");
        assert!((eq_start - equipment_total(&world)).abs() < 1e-2);
        let l = world.resource::<Ledger>();
        assert!(l.produced[FOOD] > 0.0 && l.consumed[crate::AMMO] > 0.0);
    }

    #[test]
    fn replacements_need_depot_and_equipment() {
        let (mut world, mut schedule) = setup();
        world.spawn(depot(0, NodeId(0)));
        world.resource_mut::<Nations>().equipment[0] = 1_000.0;
        let worn = || Battalions(vec![Battalion { arm: Arm::Infantry, strength: 0.5 }]);
        world.spawn(division(1, 0, "near", km(20.0, 150.0))).insert(worn());
        world.spawn(division(2, 0, "cut off", km(250.0, 280.0))).insert(worn());
        days(&mut world, &mut schedule, 2);
        let mut q = world.query::<(&UnitId, &Battalions)>();
        let s = |id: u32, q: &mut QueryState<(&UnitId, &Battalions)>, w: &World| {
            q.iter(w).find(|(u, _)| u.0 == id).unwrap().1.total()
        };
        assert!((s(1, &mut q, &world) - 0.6).abs() < 1e-4, "two days of +0.05");
        assert_eq!(s(2, &mut q, &world), 0.5);
    }

    #[test]
    fn new_division_costs_supplies_and_appears_after_build_time() {
        let (mut world, mut schedule) = setup();
        world.resource_mut::<Nations>().equipment[0] = DIVISION_EQUIPMENT;
        world.resource_mut::<CommandQueue>().0.push(SimCommand::BuildDivision { side: 0, name: "3rd Div".into() });
        schedule.run(&mut world);
        assert_eq!(world.resource::<EconomyState>().builds.len(), 1);
        assert_eq!(base_stock(&world, 0)[FOOD], 10_000.0 - DIVISION_TARGET[FOOD]);

        // Second order fails: no equipment left.
        world.resource_mut::<CommandQueue>().0.push(SimCommand::BuildDivision { side: 0, name: "4th Div".into() });
        schedule.run(&mut world);
        assert_eq!(world.resource::<EconomyState>().builds.len(), 1);

        // Ready at the first daily economy step after the build time.
        days(&mut world, &mut schedule, BUILD_DAYS + 1);
        let mut q = world.query::<&crate::UnitName>();
        assert!(q.iter(&world).any(|n| n.0 == "3rd Div"));
        assert!(world.resource::<EconomyState>().builds.is_empty());
    }
}
