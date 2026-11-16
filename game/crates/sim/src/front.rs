//! Front movement (architect.md 13): advance when winning, retreat when losing, pockets.
//!
//! Combat writes each unit's `Front` (ratio, advance speed, retreat direction); Movement reads it;
//! `detect_pockets` (hourly, before supply) marks units cut off from every own depot and base.

use crate::supply::{Depot, SupplyBases};
use crate::territory::{MAX_SIDES, Territory};
use crate::units::{Map, Position, Side};
use bevy_ecs::prelude::*;
use mapdata::Terrain;

// shortcut: fixed tuning (architect.md 13 [가정] values); move to Lua scenario data later.
/// Superiority ratio at or above which a unit with a move order keeps advancing.
pub const ADVANCE_RATIO: f32 = 1.5;
/// Ratio at or below which a unit falls back.
pub const RETREAT_RATIO: f32 = 0.67;
/// Organization below this forces a retreat.
pub const ORG_BREAK: f32 = 0.25;
/// A retreating unit keeps falling back until organization is back above this.
pub const ORG_RALLY: f32 = 0.5;
pub const ORG_RECOVER_PER_DAY: f32 = 0.2;
/// Organization lost per battalion-equivalent of losses.
pub const ORG_PER_BN_LOST: f32 = 0.8;
/// Extra organization loss while retreating.
pub const RETREAT_ORG_FACTOR: f32 = 1.5;
/// 1 km per 5 minutes.
pub const RETREAT_M_PER_TICK: f32 = 200.0;
pub const RETREAT_FIRE: f32 = 0.5;
/// Loss multiplier for a held unit that would otherwise retreat.
pub const HOLD_LOSS: f32 = 1.3;
/// Loss multiplier inside a pocket.
pub const POCKET_LOSS: f32 = 1.5;

/// Per-division combat state. Partisan bands have none.
#[derive(Component, Debug, Clone, Copy, PartialEq)]
pub struct Front {
    /// 0..1 (Readiness, 4.4).
    pub org: f32,
    /// Inflicted / taken losses in the last combat step; `None` when not in contact.
    pub ratio: Option<f32>,
    /// Multiplier on move-order speed: 1 out of contact, 0 in stalemate, 0.2..0.6 when winning.
    pub speed: f32,
    /// Unit vector to fall back along; `Some` while retreating.
    pub retreat: Option<(f32, f32)>,
    /// Player order: never retreat, take more losses instead.
    pub hold: bool,
    /// Cut off from every own depot and base (set hourly).
    pub encircled: bool,
}

impl Default for Front {
    fn default() -> Self {
        Self { org: 1.0, ratio: None, speed: 1.0, retreat: None, hold: false, encircled: false }
    }
}

impl Front {
    /// Organization and retreat scale firepower.
    pub fn fire_factor(&self) -> f32 {
        (0.3 + 0.7 * self.org) * if self.retreat.is_some() { RETREAT_FIRE } else { 1.0 }
    }
}

/// Move-order speed multiplier for a unit in contact with superiority ratio `r`.
pub fn advance_speed(r: f32) -> f32 {
    if r >= ADVANCE_RATIO { (0.2 + 0.2 * (r - ADVANCE_RATIO)).min(0.6) } else { 0.0 }
}

/// Retreating units walk 1 km / 5 min along their retreat direction, only into own-controlled land.
/// If straight back is blocked they veer up to 90 degrees; with no way out they stay put.
/// shortcut: no fuel use and no terrain slowdown while retreating.
pub fn retreat_units(map: Res<Map>, territory: Res<Territory>, mut units: Query<(&Side, &mut Position, &Front)>) {
    let grid = &map.0.terrain;
    for (side, mut pos, front) in &mut units {
        let Some((dx, dy)) = front.retreat else { continue };
        for deg in [0.0f32, 45.0, -45.0, 90.0, -90.0] {
            let (s, c) = deg.to_radians().sin_cos();
            let next = mapdata::Pos {
                x: pos.0.x + (dx * c - dy * s) * RETREAT_M_PER_TICK,
                y: pos.0.y + (dx * s + dy * c) * RETREAT_M_PER_TICK,
            };
            let Some((cx, cy)) = grid.cell_at(next) else { continue };
            if grid.get(cx, cy) != Terrain::Water && territory.military[(cy * grid.width + cx) as usize] == side.0 {
                pos.0 = next;
                break;
            }
        }
    }
}

/// Flood fill own-controlled cells from each side's depots and bases; units outside are encircled.
/// A side with no depot or base at all has no network to be cut from, so it is never encircled.
pub fn detect_pockets(
    map: Res<Map>,
    territory: Res<Territory>,
    bases: Res<SupplyBases>,
    depots: Query<&Depot>,
    mut units: Query<(&Side, &Position, &mut Front)>,
) {
    let m = &map.0;
    let (w, h) = (m.terrain.width as usize, m.terrain.height as usize);
    let mut reach: Vec<Option<Option<Vec<bool>>>> = vec![None; MAX_SIDES as usize];
    for (side, pos, mut front) in &mut units {
        let s = side.0;
        if s >= MAX_SIDES {
            continue;
        }
        let filled = reach[s as usize].get_or_insert_with(|| {
            let sources: Vec<_> = depots
                .iter()
                .filter(|d| d.side == s)
                .map(|d| d.node)
                .chain(bases.0.iter().filter(|b| b.side == s).map(|b| b.node))
                .collect();
            if sources.is_empty() {
                return None;
            }
            let mut seen = vec![false; w * h];
            let mut stack: Vec<usize> = sources
                .iter()
                .filter_map(|&n| m.terrain.cell_at(m.graph.node(n).pos))
                .map(|(x, y)| y as usize * w + x as usize)
                .filter(|&c| territory.military[c] == s)
                .collect();
            stack.iter().for_each(|&c| seen[c] = true);
            while let Some(c) = stack.pop() {
                let (x, y) = (c % w, c / w);
                let near = [(x > 0, c.wrapping_sub(1)), (x + 1 < w, c + 1), (y > 0, c.wrapping_sub(w)), (y + 1 < h, c + w)];
                for (ok, n) in near {
                    if ok && !seen[n] && territory.military[n] == s {
                        seen[n] = true;
                        stack.push(n);
                    }
                }
            }
            Some(seen)
        });
        let encircled = match (filled, m.terrain.cell_at(pos.0)) {
            (Some(seen), Some((x, y))) => !seen[y as usize * w + x as usize],
            _ => false,
        };
        if front.encircled != encircled {
            front.encircled = encircled;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::combat::{Arm, Battalion, Battalions};
    use crate::supply::{FOOD, Stocks, SupplyBase, depot};
    use crate::units::{CommandQueue, MoveOrder, SimCommand, UnitId, division};
    use crate::{TICKS_PER_HOUR, configure, init_world};
    use mapdata::{MapData, NodeId, Pos, TerrainGrid, TransportGraph, test_map};

    fn km(x: f32, y: f32) -> Pos {
        Pos { x: x * 1000.0, y: y * 1000.0 }
    }

    /// 80 x 10 km plains; cells east of x = 40 km held by Red (1).
    fn strip() -> (World, Schedule) {
        let mut world = World::new();
        init_world(&mut world, MapData::single_region("t", TerrainGrid::filled(80, 10, 1000.0, Terrain::Plains), TransportGraph::default()));
        let mut t = world.resource_mut::<Territory>();
        for c in 0..t.military.len() {
            if c % 80 >= 40 {
                t.military[c] = 1;
            }
        }
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        (world, schedule)
    }

    fn run(world: &mut World, schedule: &mut Schedule, ticks: u64) {
        for _ in 0..ticks {
            schedule.run(world);
        }
    }

    fn get<T: Component + Clone>(world: &mut World, id: u32) -> T {
        let mut q = world.query::<(&UnitId, &T)>();
        q.iter(world).find(|(u, _)| u.0 == id).unwrap().1.clone()
    }

    fn x_of(world: &mut World, id: u32) -> f32 {
        get::<Position>(world, id).0.x
    }

    fn strength(world: &mut World, id: u32) -> f32 {
        get::<Battalions>(world, id).total()
    }

    fn weak() -> Battalions {
        Battalions(vec![Battalion { arm: Arm::Infantry, strength: 1.0 }; 3])
    }

    fn strong() -> Battalions {
        let mut b = Battalions::standard();
        b.0.extend(Battalions::standard().0);
        b
    }

    #[test]
    fn strong_attacker_advances_and_front_moves() {
        let (mut world, mut schedule) = strip();
        world.spawn(division(1, 0, "blue", km(20.5, 5.5))).insert(strong());
        world.spawn(division(2, 1, "red", km(45.5, 5.5)));
        world.resource_mut::<CommandQueue>().0.push(SimCommand::Move { unit: UnitId(1), goal: km(75.5, 5.5) });
        run(&mut world, &mut schedule, 2 * TICKS_PER_HOUR);
        let red = get::<Front>(&mut world, 2);
        assert!(red.retreat.is_some() || red.ratio.is_none(), "red falls back: {red:?}");
        assert!(x_of(&mut world, 2) > 50_000.0, "red pushed east: {}", x_of(&mut world, 2));
        assert!(x_of(&mut world, 1) > 40_000.0, "blue advanced past the old line: {}", x_of(&mut world, 1));
        let owner = world.resource::<Territory>().military_at(&world.resource::<Map>().0, km(45.5, 5.5));
        assert_eq!(owner, Some(0), "the front moved east");
    }

    #[test]
    fn weak_defender_retreats() {
        let (mut world, mut schedule) = strip();
        world.spawn(division(1, 0, "blue", km(35.5, 5.5)));
        world.spawn(division(2, 1, "red", km(45.5, 5.5))).insert(weak());
        run(&mut world, &mut schedule, 10);
        assert!(get::<Front>(&mut world, 2).retreat.is_some());
        run(&mut world, &mut schedule, TICKS_PER_HOUR);
        assert!(x_of(&mut world, 2) > 50_500.0, "red fell back east: {}", x_of(&mut world, 2));
        assert!(get::<Front>(&mut world, 1).retreat.is_none(), "winner holds its ground");
    }

    #[test]
    fn equal_forces_stall() {
        let (mut world, mut schedule) = strip();
        world.spawn(division(1, 0, "blue", km(20.5, 5.5)));
        world.spawn(division(2, 1, "red", km(45.5, 5.5)));
        world.resource_mut::<CommandQueue>().0.push(SimCommand::Move { unit: UnitId(1), goal: km(75.5, 5.5) });
        run(&mut world, &mut schedule, 6 * TICKS_PER_HOUR);
        let (bx, rx) = (x_of(&mut world, 1), x_of(&mut world, 2));
        assert_eq!(rx, 45_500.0, "red did not budge");
        assert!(rx - bx > 13_000.0 && rx - bx <= 15_000.0, "blue stopped at contact: gap {}", rx - bx);
        assert!(get::<Front>(&mut world, 1).speed == 0.0);
        assert_eq!(world.query::<&MoveOrder>().iter(&world).count(), 1, "attack order kept for later");
    }

    #[test]
    fn held_unit_does_not_retreat_and_loses_more() {
        let result = |hold: bool| {
            let (mut world, mut schedule) = strip();
            world.spawn(division(1, 0, "blue", km(35.5, 5.5)));
            world.spawn(division(2, 1, "red", km(45.5, 5.5))).insert(weak());
            if hold {
                world.resource_mut::<CommandQueue>().0.push(SimCommand::SetHold { unit: UnitId(2), hold: true });
            }
            run(&mut world, &mut schedule, 2 * TICKS_PER_HOUR);
            (x_of(&mut world, 2), strength(&mut world, 2))
        };
        let (held_x, held_str) = result(true);
        let (free_x, free_str) = result(false);
        assert_eq!(held_x, 45_500.0, "held in place");
        assert!(free_x > held_x);
        assert!(held_str < free_str, "holding costs men: {held_str} vs {free_str}");
    }

    #[test]
    fn encircled_unit_gets_no_supply_and_cannot_retreat() {
        let mut world = World::new();
        init_world(&mut world, test_map());
        world.resource_mut::<SupplyBases>().0.push(SupplyBase { side: 0, node: NodeId(0), stock: [20_000.0; 3] });
        world.spawn(depot(0, NodeId(1))).insert(Stocks([3000.0; 3])); // Forest Junction, 100 km west
        // Red holds a north-south band at x 170..175 km: Blue's unit east of it is cut off.
        {
            let map = test_map();
            let mut t = world.resource_mut::<Territory>();
            for y in 0..map.terrain.height {
                for x in 170..175 {
                    t.military[(y * map.terrain.width + x) as usize] = 1;
                }
            }
        }
        world.spawn(division(1, 0, "pocket", km(200.5, 150.5))).insert((weak(), Stocks([400.0, 600.0, 100.0])));
        world.spawn(division(2, 1, "red", km(212.5, 150.5)));
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        run(&mut world, &mut schedule, TICKS_PER_HOUR);
        let f = get::<Front>(&mut world, 1);
        assert!(f.encircled, "{f:?}");
        // Pockets are checked hourly; from then on the unit stays put.
        run(&mut world, &mut schedule, 5);
        assert!(get::<Front>(&mut world, 1).retreat.is_none(), "no retreat out of a pocket");
        let at = get::<Position>(&mut world, 1).0;
        run(&mut world, &mut schedule, 3 * TICKS_PER_HOUR);
        assert_eq!(get::<Position>(&mut world, 1).0, at);
        assert!(get::<Stocks>(&mut world, 1).0[FOOD] < 100.0, "no trucks into the pocket");
    }

    #[test]
    fn unit_in_reach_is_not_encircled() {
        let mut world = World::new();
        init_world(&mut world, test_map());
        world.spawn(depot(0, NodeId(1)));
        world.spawn(division(1, 0, "home", km(120.5, 150.5)));
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        run(&mut world, &mut schedule, TICKS_PER_HOUR);
        assert!(!get::<Front>(&mut world, 1).encircled);
    }
}
