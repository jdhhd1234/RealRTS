//! RealRTS simulation core. Headless: depends on `bevy_ecs` only.

use bevy_ecs::prelude::*;

pub mod combat;
pub mod deploy;
pub use deploy::*;
pub mod economy;
pub use economy::*;
pub mod front;
pub use front::*;
pub mod partisan;
pub mod path;
pub use combat::*;
pub use partisan::*;
pub mod supply;
pub mod territory;
pub mod units;
pub use supply::*;
pub use territory::*;
pub use units::*;

pub const TICKS_PER_HOUR: u64 = 60;
pub const TICKS_PER_DAY: u64 = 24 * TICKS_PER_HOUR;

/// How often each system group runs, in ticks (architect.md 4.2).
pub mod cadence {
    pub const COMMANDS: u64 = 1;
    pub const MOVEMENT: u64 = 1;
    pub const COMBAT: u64 = 5;
    pub const SUPPLY: u64 = super::TICKS_PER_HOUR;
    pub const PARTISAN: u64 = 6 * super::TICKS_PER_HOUR;
    pub const ECONOMY: u64 = super::TICKS_PER_DAY;
}

/// Game time. 1 tick = 1 game minute. Seasons come from Lua scenario data, not here.
#[derive(Resource, Default, Debug, Clone, Copy, PartialEq, Eq)]
pub struct SimClock {
    pub tick: u64,
}

impl SimClock {
    /// Day index, starting at 0.
    pub fn day(&self) -> u64 {
        self.tick / TICKS_PER_DAY
    }
    pub fn hour(&self) -> u64 {
        self.tick % TICKS_PER_DAY / TICKS_PER_HOUR
    }
    pub fn minute(&self) -> u64 {
        self.tick % TICKS_PER_HOUR
    }
}

/// Fixed per-tick order. Systems of later milestones go into these sets.
#[derive(SystemSet, Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SimSet {
    Clock,
    Commands,
    Movement,
    Combat,
    Supply,
    Partisan,
    Economy,
    Cleanup,
    Events,
}

/// Run condition: true on ticks that are a multiple of `n`.
pub fn every(n: u64) -> impl Fn(Res<SimClock>) -> bool + Clone {
    move |clock: Res<SimClock>| clock.tick % n == 0
}

pub fn advance_clock(mut clock: ResMut<SimClock>) {
    clock.tick += 1;
}

/// Installs the sim into a schedule that runs once per tick (Bevy `FixedUpdate`, or a plain
/// `Schedule` in headless tests). Clock advances first, so tick 1 is the first simulated minute.
pub fn configure(schedule: &mut Schedule) {
    use SimSet::*;
    schedule.configure_sets(
        (
            Clock,
            Commands.run_if(every(cadence::COMMANDS)),
            Movement.run_if(every(cadence::MOVEMENT)),
            Combat.run_if(every(cadence::COMBAT)),
            Supply.run_if(every(cadence::SUPPLY)),
            Partisan.run_if(every(cadence::PARTISAN)),
            Economy.run_if(every(cadence::ECONOMY)),
            Cleanup,
            Events,
        )
            .chain(),
    );
    schedule.add_systems((
        advance_clock.in_set(Clock),
        apply_commands.in_set(Commands),
        (move_units, retreat_units).chain().in_set(Movement),
        (resolve_combat, update_military, capture_depots).chain().in_set(Combat),
        partisan_step.in_set(Partisan),
        economy_step.in_set(Economy),
        remove_destroyed.in_set(Cleanup),
        (detect_pockets, consume, receive, dispatch, deliver).chain().in_set(Supply),
    ));
}

/// Inserts every resource the sim systems need, replacing any earlier game's state.
/// Despawning old units and depots is the caller's job.
pub fn init_world(world: &mut World, map: mapdata::MapData) {
    world.insert_resource(SimClock::default());
    world.insert_resource(CommandQueue::default());
    world.insert_resource(SupplyBases::default());
    world.insert_resource(Pipeline::default());
    world.insert_resource(Ledger::default());
    world.insert_resource(Engagements::default());
    world.insert_resource(SimRng::default());
    world.insert_resource(EventLog::default());
    world.insert_resource(PartisanState::new(map.regions.len()));
    world.insert_resource(Nations::default());
    world.insert_resource(EconomyState::new(map.regions.len()));
    world.insert_resource(EdgeState::new(map.graph.edges.len()));
    // shortcut: every region legally side 0 until a scenario (Lua) sets owners via Territory::from_legal.
    world.insert_resource(Territory::from_legal(&map, vec![0; map.regions.len()]));
    world.insert_resource(Map(map));
}

/// Starting stock of each scenario supply base, per good.
/// shortcut: one fixed value until Lua scenarios set stocks.
pub const SCENARIO_BASE_STOCK: f32 = 20_000.0;

/// Replaces the current game with a baked map and its starting situation: despawns every unit
/// and depot, resets all sim resources, then spawns bases, depots and divisions (ids from 1).
pub fn load_scenario(world: &mut World, baked: mapdata::Baked) {
    let old: Vec<Entity> =
        world.query_filtered::<Entity, Or<(With<UnitId>, With<Depot>)>>().iter(world).collect();
    for e in old {
        world.despawn(e);
    }
    let mapdata::Baked { map, scenario: s } = baked;
    let mut territory = Territory::from_legal(&map, s.legal.clone());
    for (cell, r) in map.region_cells.iter().enumerate() {
        territory.military[cell] = s.control[r.0 as usize];
    }
    init_world(world, map);
    world.insert_resource(territory);
    world.resource_mut::<SupplyBases>().0 =
        s.bases.iter().map(|&(side, node)| SupplyBase { side, node, stock: [SCENARIO_BASE_STOCK; 3] }).collect();
    for &(side, node) in &s.depots {
        world.spawn(depot(side, node));
    }
    for (i, u) in s.units.iter().enumerate() {
        let enemy = s.units.iter().filter(|e| e.side != u.side).min_by(|a, b| {
            let d = |p: mapdata::Pos| (p.x - u.pos.x).powi(2) + (p.y - u.pos.y).powi(2);
            d(a.pos).total_cmp(&d(b.pos))
        });
        let facing = enemy.and_then(|e| unit(e.pos.x - u.pos.x, e.pos.y - u.pos.y)).unwrap_or((1.0, 0.0));
        world.spawn(division(i as u32 + 1, u.side, &u.name, u.pos)).insert(Deployment { facing, ..Default::default() });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn load_scenario_replaces_previous_game() {
        let mut world = World::new();
        load_scenario(&mut world, mapdata::test_baked());
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        for _ in 0..TICKS_PER_HOUR {
            schedule.run(&mut world);
        }
        load_scenario(&mut world, mapdata::test_baked());
        assert_eq!(world.resource::<SimClock>().tick, 0);
        assert_eq!(world.query::<&UnitId>().iter(&world).count(), 3);
        assert_eq!(world.query::<&Depot>().iter(&world).count(), 2);
        assert_eq!(world.resource::<SupplyBases>().0.len(), 2);
        let t = world.resource::<Territory>();
        assert_eq!(region_status(&world.resource::<Map>().0, t)[7], RegionStatus::Occupied { by: 1 });
    }

    #[derive(Resource, Default)]
    struct Log(Vec<(u64, SimSet)>);

    fn logger(set: SimSet) -> impl FnMut(Res<SimClock>, ResMut<Log>) {
        move |clock, mut log| log.0.push((clock.tick, set))
    }

    fn run_ticks(n: u64) -> Vec<(u64, SimSet)> {
        let mut world = World::new();
        init_world(&mut world, mapdata::test_map());
        world.init_resource::<Log>();
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        use SimSet::*;
        for set in [Commands, Movement, Combat, Supply, Partisan, Economy, Cleanup, Events] {
            schedule.add_systems(logger(set).in_set(set));
        }
        for _ in 0..n {
            schedule.run(&mut world);
        }
        assert_eq!(world.resource::<SimClock>().tick, n);
        world.remove_resource::<Log>().unwrap().0
    }

    #[test]
    fn cadence_counts_over_two_days() {
        let log = run_ticks(2 * TICKS_PER_DAY);
        let count = |s: SimSet| log.iter().filter(|(_, x)| *x == s).count() as u64;
        let n = 2 * TICKS_PER_DAY;
        assert_eq!(count(SimSet::Commands), n);
        assert_eq!(count(SimSet::Movement), n);
        assert_eq!(count(SimSet::Combat), n / 5);
        assert_eq!(count(SimSet::Supply), 48);
        assert_eq!(count(SimSet::Partisan), 8);
        assert_eq!(count(SimSet::Economy), 2);
        assert_eq!(count(SimSet::Events), n);
    }

    #[test]
    fn order_fixed_within_a_tick() {
        let log = run_ticks(TICKS_PER_DAY);
        let at_day_end: Vec<SimSet> = log
            .iter()
            .filter(|(t, _)| *t == TICKS_PER_DAY)
            .map(|(_, s)| *s)
            .collect();
        use SimSet::*;
        assert_eq!(
            at_day_end,
            [Commands, Movement, Combat, Supply, Partisan, Economy, Cleanup, Events]
        );
    }

    #[test]
    fn clock_calendar() {
        let c = SimClock { tick: 3 * TICKS_PER_DAY + 14 * TICKS_PER_HOUR + 5 };
        assert_eq!((c.day(), c.hour(), c.minute()), (3, 14, 5));
    }
}
