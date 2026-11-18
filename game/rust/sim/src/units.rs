//! Units, commands and movement (architect.md 4.4, 4.5, 4.11).

use crate::path::{Cell, find_path, terrain_cost};
use crate::combat::Battalions;
use crate::deploy::{Deployment, unit};
use crate::front::Front;
use crate::supply::{DIVISION_TARGET, FUEL, StockTarget, Stocks, SupplyPriority};
use crate::economy::{EconomyState, Nations, order_division};
use crate::partisan::{EventLog, PartisanState, anti_partisan};
use crate::supply::SupplyBases;
use crate::SimClock;
use bevy_ecs::prelude::*;
use mapdata::{MapData, Pos, RegionId};

/// Static map, loaded once per game.
#[derive(Resource)]
pub struct Map(pub MapData);

#[derive(Component, Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct UnitId(pub u32);

#[derive(Component, Debug, Clone, Copy, PartialEq, Eq)]
pub struct Side(pub u8);

#[derive(Component, Debug, Clone)]
pub struct UnitName(pub String);

#[derive(Component, Debug, Clone, Copy, PartialEq)]
pub struct Position(pub Pos);

/// Remaining cells to walk through. Removed when the unit arrives.
#[derive(Component, Debug, Clone)]
pub struct MoveOrder {
    pub path: Vec<Cell>,
    pub next: usize,
}

/// Cross-country speed on plains. shortcut: one speed for all units; per-unit speed from Lua later.
pub const DIVISION_SPEED_M_PER_TICK: f32 = 20_000.0 / 60.0; // 20 km/h

/// Division entity with a standard battalion list (D1).
pub fn division(id: u32, side: u8, name: &str, pos: Pos) -> impl Bundle {
    (
        Battalions::standard(),
        UnitId(id),
        Side(side),
        UnitName(name.to_string()),
        Position(pos),
        Stocks(DIVISION_TARGET),
        StockTarget(DIVISION_TARGET),
        SupplyPriority::default(),
        Front::default(),
        crate::deploy::Deployment::default(),
    )
}

/// The only way anything outside the sim changes sim state.
#[derive(Debug, Clone, PartialEq)]
pub enum SimCommand {
    /// Also the attack order: meeting the enemy on the way turns it into an assault (13.2).
    Move { unit: UnitId, goal: Pos },
    /// Hold the line: never retreat, take more losses (13.7).
    SetHold { unit: UnitId, hold: bool },
    SetSupplyPriority { unit: UnitId, priority: u8 },
    /// Sweep a region for partisans; `harsh` = reprisals (F3).
    AntiPartisan { region: RegionId, harsh: bool },
    /// Take food from an occupied region (cuts support).
    SetRequisition { region: RegionId, on: bool },
    /// Raise a new division at the side's base (G3).
    BuildDivision { side: u8, name: String },
}

/// Commands queued for the next tick. shortcut: not logged yet; the (tick, command) log comes in M10.
#[derive(Resource, Default)]
pub struct CommandQueue(pub Vec<SimCommand>);

pub fn apply_commands(
    mut queue: ResMut<CommandQueue>,
    map: Res<Map>,
    units: Query<(Entity, &UnitId, &Position)>,
    mut fronts: Query<&mut Front>,
    mut commands: Commands,
    clock: Res<SimClock>,
    mut partisans: ResMut<PartisanState>,
    mut log: ResMut<EventLog>,
    mut bases: ResMut<SupplyBases>,
    mut nations: ResMut<Nations>,
    mut econ: ResMut<EconomyState>,
) {
    let grid = &map.0.terrain;
    for cmd in queue.0.drain(..) {
        match cmd {
            SimCommand::Move { unit, goal } => {
                let Some((entity, _, pos)) = units.iter().find(|(_, id, _)| **id == unit) else {
                    continue; // unknown or destroyed unit
                };
                let (Some(start), Some(goal)) = (grid.cell_at(pos.0), grid.cell_at(goal)) else {
                    continue; // goal outside the map
                };
                if let Some(path) = find_path(grid, start, goal) {
                    commands.entity(entity).insert(MoveOrder { path, next: 0 });
                }
            }
            SimCommand::SetHold { unit, hold } => {
                if let Some((entity, ..)) = units.iter().find(|(_, id, _)| **id == unit)
                    && let Ok(mut f) = fronts.get_mut(entity)
                {
                    f.hold = hold;
                    if hold {
                        f.retreat = None;
                    }
                }
            }
            SimCommand::SetSupplyPriority { unit, priority } => {
                if let Some((entity, ..)) = units.iter().find(|(_, id, _)| **id == unit) {
                    commands.entity(entity).insert(SupplyPriority(priority));
                }
            }
            SimCommand::AntiPartisan { region, harsh } => {
                anti_partisan(&mut partisans, &mut log, &map.0, clock.tick, region, harsh);
            }
            SimCommand::SetRequisition { region, on } => {
                if let Some(r) = econ.requisition.get_mut(region.0 as usize) {
                    *r = on;
                }
            }
            SimCommand::BuildDivision { side, name } => {
                order_division(side, name, clock.tick, &mut bases, &mut nations, &mut econ, &mut log);
            }
        }
    }
}

fn cell_center(grid: &mapdata::TerrainGrid, (cx, cy): Cell) -> Pos {
    Pos { x: (cx as f32 + 0.5) * grid.cell_m, y: (cy as f32 + 0.5) * grid.cell_m }
}

/// Walk along the path; entering a cell costs `terrain_cost` times the plains time.
pub fn move_units(
    map: Res<Map>,
    mut units: Query<(Entity, &mut Position, &mut MoveOrder, Option<&Stocks>, Option<&Front>, Option<&mut Deployment>)>,
    mut commands: Commands,
) {
    let grid = &map.0.terrain;
    for (entity, mut pos, mut order, stocks, front, dep) in &mut units {
        if stocks.is_some_and(|s| s.0[FUEL] <= 0.0) {
            continue; // out of fuel: order kept, unit waits for resupply
        }
        // In contact: slowed advance when winning, halt (order kept) in stalemate.
        let pace = front.map_or(1.0, |f| f.speed);
        if pace <= 0.0 {
            continue;
        }
        let start = pos.0;
        let mut time_left = 1.0f32; // fraction of this tick
        while time_left > 0.0 && order.next < order.path.len() {
            let cell = order.path[order.next];
            let target = cell_center(grid, cell);
            let (dx, dy) = (target.x - pos.0.x, target.y - pos.0.y);
            let dist = (dx * dx + dy * dy).sqrt();
            let speed = pace * DIVISION_SPEED_M_PER_TICK / terrain_cost(grid.get(cell.0, cell.1));
            let reach = speed * time_left;
            if dist <= reach {
                pos.0 = target;
                time_left -= dist / speed;
                order.next += 1;
            } else {
                pos.0.x += dx / dist * reach;
                pos.0.y += dy / dist * reach;
                time_left = 0.0;
            }
        }
        // Out of contact the column faces its way; in contact combat sets the facing.
        if let (Some(mut dep), Some(dir)) = (dep, unit(pos.0.x - start.x, pos.0.y - start.y))
            && front.is_none_or(|f| f.ratio.is_none())
        {
            dep.facing = dir;
        }
        if order.next >= order.path.len() {
            commands.entity(entity).remove::<MoveOrder>();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::configure;
    use mapdata::{Terrain, TerrainGrid, TransportGraph};

    fn world_with(grid: TerrainGrid) -> (World, Schedule) {
        let mut world = World::new();
        crate::init_world(&mut world, MapData::single_region("t", grid, TransportGraph::default()));
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        (world, schedule)
    }

    fn pos_of(world: &mut World, id: u32) -> Pos {
        let mut q = world.query::<(&UnitId, &Position)>();
        q.iter(world).find(|(u, _)| u.0 == id).unwrap().1.0
    }

    fn moving(world: &mut World) -> usize {
        world.query::<&MoveOrder>().iter(world).count()
    }

    #[test]
    fn division_marches_10km_on_plains_in_30_ticks() {
        let (mut world, mut schedule) = world_with(TerrainGrid::filled(20, 3, 1000.0, Terrain::Plains));
        world.spawn(division(1, 0, "1st", Pos { x: 500.0, y: 1500.0 }));
        world.resource_mut::<CommandQueue>().0.push(SimCommand::Move {
            unit: UnitId(1),
            goal: Pos { x: 10_500.0, y: 1500.0 },
        });
        for _ in 0..29 {
            schedule.run(&mut world);
        }
        assert!(pos_of(&mut world, 1).x < 10_500.0);
        assert_eq!(moving(&mut world), 1);
        for _ in 0..2 {
            schedule.run(&mut world);
        }
        assert_eq!(pos_of(&mut world, 1), Pos { x: 10_500.0, y: 1500.0 });
        assert_eq!(moving(&mut world), 0, "order removed on arrival");
    }

    #[test]
    fn forest_is_twice_as_slow() {
        let (mut world, mut schedule) = world_with(TerrainGrid::filled(20, 1, 1000.0, Terrain::Forest));
        world.spawn(division(1, 0, "1st", Pos { x: 500.0, y: 500.0 }));
        world.resource_mut::<CommandQueue>().0.push(SimCommand::Move {
            unit: UnitId(1),
            goal: Pos { x: 10_500.0, y: 500.0 },
        });
        for _ in 0..31 {
            schedule.run(&mut world);
        }
        let x = pos_of(&mut world, 1).x;
        assert!((x - 5_500.0).abs() < 400.0, "about halfway after 31 move ticks, got {x}");
    }

    #[test]
    fn bad_commands_are_ignored() {
        let (mut world, mut schedule) = world_with(TerrainGrid::filled(5, 5, 1000.0, Terrain::Plains));
        world.spawn(division(1, 0, "1st", Pos { x: 500.0, y: 500.0 }));
        let mut q = world.resource_mut::<CommandQueue>();
        q.0.push(SimCommand::Move { unit: UnitId(99), goal: Pos { x: 2500.0, y: 500.0 } });
        q.0.push(SimCommand::Move { unit: UnitId(1), goal: Pos { x: -10.0, y: 500.0 } });
        schedule.run(&mut world);
        assert_eq!(moving(&mut world), 0);
        assert!(world.resource::<CommandQueue>().0.is_empty());
    }
}
