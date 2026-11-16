//! Combat (architect.md 4.6, D2): continuous Lanchester-style attrition every 5 minutes between
//! enemy divisions in contact. Ammo stock scales firepower; defender terrain divides incoming fire.

use crate::front::*;
use crate::supply::{AMMO, DIVISION_TARGET, Depot, FOOD, Ledger, Stocks};
use crate::units::{Map, MoveOrder, Position, Side, UnitId};
use crate::{TICKS_PER_DAY, cadence};
use bevy_ecs::prelude::*;
use mapdata::{Pos, Terrain};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Arm {
    Infantry,
    Armor,
    Artillery,
}

/// One battalion inside a division. `strength` 1.0 = full.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Battalion {
    pub arm: Arm,
    pub strength: f32,
}

/// Battalion list of a division (D1: the division is the only entity).
#[derive(Component, Debug, Clone, PartialEq)]
pub struct Battalions(pub Vec<Battalion>);

impl Battalions {
    /// shortcut: one fixed composition (6 inf, 2 armor, 2 artillery); per-division ORBAT from Lua later.
    pub fn standard() -> Self {
        let b = |arm, n| std::iter::repeat_n(Battalion { arm, strength: 1.0 }, n);
        Self(b(Arm::Infantry, 6).chain(b(Arm::Armor, 2)).chain(b(Arm::Artillery, 2)).collect())
    }

    /// Battalion-equivalents left.
    pub fn total(&self) -> f32 {
        self.0.iter().map(|b| b.strength).sum()
    }

    /// Raw firepower before ammo and terrain.
    pub fn firepower(&self) -> f32 {
        self.0.iter().map(|b| arm_firepower(b.arm) * b.strength).sum()
    }

    /// Spread a loss (battalion-equivalents) over battalions in proportion to their strength.
    fn take_loss(&mut self, loss: f32) {
        let total = self.total();
        if total <= 0.0 {
            return;
        }
        let ratio = (loss / total).min(1.0);
        for b in &mut self.0 {
            b.strength -= b.strength * ratio;
        }
    }
}

// shortcut: fixed tables; equipment and arm values come from Lua (D3) later.
pub fn arm_firepower(arm: Arm) -> f32 {
    match arm {
        Arm::Infantry => 1.0,
        Arm::Armor => 2.0,
        Arm::Artillery => 1.5,
    }
}

/// Incoming fire is divided by the defender's terrain factor.
pub fn terrain_defense(t: Terrain) -> f32 {
    match t {
        Terrain::Plains | Terrain::Water => 1.0,
        Terrain::Marsh => 1.2,
        Terrain::Forest => 1.5,
        Terrain::Urban | Terrain::Mountain => 2.0,
    }
}

/// Divisions closer than this are in contact.
pub const CONTACT_RANGE_M: f32 = 15_000.0;
/// Losses (battalion-equivalents) per point of effective firepower per 5-minute step.
/// Two standard divisions on plains lose roughly 10% a day.
pub const COMBAT_K: f32 = 2.5e-4;
/// Ammo burned per 5-minute combat step (600 t per day of fighting).
pub const AMMO_PER_STEP_T: f32 = 600.0 / 288.0;
/// Firepower with no ammo left (small arms, scavenging).
pub const NO_AMMO_FACTOR: f32 = 0.1;
/// Battalions below this are dissolved; a division with none left is destroyed.
pub const MIN_BATTALION_STRENGTH: f32 = 0.05;

/// 0.1 at empty ammo, 1.0 at full.
pub fn ammo_factor(ammo: f32) -> f32 {
    NO_AMMO_FACTOR + (1.0 - NO_AMMO_FACTOR) * (ammo / DIVISION_TARGET[AMMO]).clamp(0.0, 1.0)
}

/// Pairs fighting in the last combat step (for UI and events).
#[derive(Resource, Default, Debug)]
pub struct Engagements(pub Vec<(Entity, Entity)>);

/// 0.5 with no food, 1.0 at full.
pub fn food_factor(food: f32) -> f32 {
    0.5 + 0.5 * (food / DIVISION_TARGET[FOOD]).clamp(0.0, 1.0)
}

/// Losses, ammo use, and the front outcome (architect.md 13.1): superiority ratio R = losses inflicted
/// / losses taken, where fire already reflects organization, supply (ammo, food) and retreat.
/// R >= 1.5 advances, R <= 0.67 or broken organization retreats, otherwise stalemate.
pub fn resolve_combat(
    map: Res<Map>,
    mut commands: Commands,
    mut engagements: ResMut<Engagements>,
    mut ledger: ResMut<Ledger>,
    depots: Query<&Depot>,
    mut units: Query<(Entity, &UnitId, &Side, &Position, &mut Battalions, &mut Stocks, Option<&mut Front>)>,
) {
    let grid = &map.0.terrain;
    let mut list: Vec<_> = units
        .iter()
        .map(|(e, id, s, p, b, st, f)| {
            let front = f.copied().unwrap_or_default();
            let fire = b.firepower() * ammo_factor(st.0[AMMO]) * food_factor(st.0[FOOD]) * front.fire_factor();
            (e, id.0, s.0, p.0, fire, front)
        })
        .collect();
    list.sort_by_key(|u| u.1);

    // Contacts per unit, so firepower is split between all enemies it faces.
    let mut contacts: Vec<Vec<usize>> = vec![Vec::new(); list.len()];
    engagements.0.clear();
    for i in 0..list.len() {
        for j in i + 1..list.len() {
            let (a, b) = (&list[i], &list[j]);
            let d2 = (a.3.x - b.3.x).powi(2) + (a.3.y - b.3.y).powi(2);
            if a.2 != b.2 && d2 <= CONTACT_RANGE_M * CONTACT_RANGE_M {
                contacts[i].push(j);
                contacts[j].push(i);
                engagements.0.push((a.0, b.0));
            }
        }
    }

    // Compute every loss first, then apply, so order never matters.
    let mut loss = vec![0.0f32; list.len()];
    let mut inflicted = vec![0.0f32; list.len()];
    for (i, targets) in contacts.iter().enumerate() {
        if targets.is_empty() {
            continue;
        }
        let fp = list[i].4 / targets.len() as f32;
        for &j in targets {
            let defense = grid.cell_at(list[j].3).map_or(1.0, |(cx, cy)| terrain_defense(grid.get(cx, cy)));
            loss[j] += COMBAT_K * fp / defense;
            inflicted[i] += COMBAT_K * fp / defense;
        }
    }

    for (i, u) in list.iter().enumerate() {
        let Ok((.., mut bats, mut stocks, front)) = units.get_mut(u.0) else { continue };
        let mut f = u.5;
        if contacts[i].is_empty() {
            f.ratio = None;
            f.speed = 1.0;
            f.retreat = None;
            if !f.encircled {
                f.org = (f.org + ORG_RECOVER_PER_DAY * cadence::COMBAT as f32 / TICKS_PER_DAY as f32).min(1.0);
            }
        } else {
            let r = if loss[i] > 0.0 { inflicted[i] / loss[i] } else { f32::INFINITY };
            let losing = r <= RETREAT_RATIO || f.org < ORG_BREAK || (f.retreat.is_some() && f.org < ORG_RALLY);
            let mult = if f.encircled {
                POCKET_LOSS
            } else if f.hold && losing {
                HOLD_LOSS
            } else {
                1.0
            };
            let taken = loss[i] * mult;
            bats.take_loss(taken);
            let org_loss = taken * ORG_PER_BN_LOST * if f.retreat.is_some() { RETREAT_ORG_FACTOR } else { 1.0 };
            f.org = (f.org - org_loss).max(0.0);
            f.ratio = Some(r);
            f.speed = advance_speed(r);
            f.retreat = (losing && !f.hold && !f.encircled).then(|| retreat_dir(u.3, &contacts[i], &list, &depots, &map, u.2));
            if f.retreat.is_some() {
                commands.entity(u.0).remove::<MoveOrder>();
            }
            let used = AMMO_PER_STEP_T.min(stocks.0[AMMO]);
            stocks.0[AMMO] -= used;
            ledger.consumed[AMMO] += used;
        }
        if let Some(mut front) = front
            && *front != f
        {
            *front = f;
        }
    }
}

/// Away from the enemies in contact, bent toward the nearest own depot.
fn retreat_dir(
    me: Pos,
    contacts: &[usize],
    list: &[(Entity, u32, u8, Pos, f32, Front)],
    depots: &Query<&Depot>,
    map: &Map,
    side: u8,
) -> (f32, f32) {
    let unit = |x: f32, y: f32| {
        let len = (x * x + y * y).sqrt();
        if len > 1e-3 { (x / len, y / len) } else { (0.0, 0.0) }
    };
    let (mut ax, mut ay) = (0.0, 0.0);
    for &j in contacts {
        let (x, y) = unit(me.x - list[j].3.x, me.y - list[j].3.y);
        ax += x;
        ay += y;
    }
    let (ax, ay) = unit(ax, ay);
    let home = depots
        .iter()
        .filter(|d| d.side == side)
        .map(|d| (d.node, map.0.graph.node(d.node).pos))
        .min_by(|a, b| {
            let da = (a.1.x - me.x).powi(2) + (a.1.y - me.y).powi(2);
            let db = (b.1.x - me.x).powi(2) + (b.1.y - me.y).powi(2);
            da.total_cmp(&db).then(a.0.cmp(&b.0))
        });
    let (hx, hy) = home.map_or((0.0, 0.0), |(_, p)| unit(p.x - me.x, p.y - me.y));
    let dir = unit(ax + 0.5 * hx, ay + 0.5 * hy);
    if dir == (0.0, 0.0) { (ax, ay) } else { dir }
}

/// Dissolve worn-out battalions; destroy divisions with none left. Their stocks are lost.
pub fn remove_destroyed(
    mut commands: Commands,
    mut ledger: ResMut<Ledger>,
    mut units: Query<(Entity, &mut Battalions, &Stocks)>,
) {
    for (entity, mut bats, stocks) in &mut units {
        bats.0.retain(|b| b.strength >= MIN_BATTALION_STRENGTH);
        if bats.0.is_empty() {
            (0..3).for_each(|g| ledger.destroyed[g] += stocks.0[g]);
            commands.entity(entity).despawn();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{TICKS_PER_DAY, configure, division, init_world};
    use mapdata::{MapData, Pos, TerrainGrid, TransportGraph};

    fn setup(grid: TerrainGrid) -> (World, Schedule) {
        let mut world = World::new();
        init_world(&mut world, MapData::single_region("t", grid, TransportGraph::default()));
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        (world, schedule)
    }

    fn plains() -> TerrainGrid {
        TerrainGrid::filled(40, 10, 1000.0, Terrain::Plains)
    }

    fn strength(world: &mut World, id: u32) -> f32 {
        let mut q = world.query::<(&UnitId, &Battalions)>();
        q.iter(world).find(|(u, _)| u.0 == id).map_or(0.0, |(_, b)| b.total())
    }

    fn run(world: &mut World, schedule: &mut Schedule, ticks: u64) {
        for _ in 0..ticks {
            schedule.run(world);
        }
    }

    #[test]
    fn equal_divisions_trade_losses_evenly() {
        let (mut world, mut schedule) = setup(plains());
        world.spawn(division(1, 0, "blue", Pos { x: 10_500.0, y: 5_500.0 }));
        world.spawn(division(2, 1, "red", Pos { x: 20_500.0, y: 5_500.0 }));
        run(&mut world, &mut schedule, TICKS_PER_DAY);
        let (a, b) = (strength(&mut world, 1), strength(&mut world, 2));
        assert!((a - b).abs() < 1e-3, "symmetric: {a} vs {b}");
        // ~10%/day at full ammo; with no resupply ammo runs dry over the day, so about half that.
        assert!(a > 9.3 && a < 9.7, "{a}");
    }

    #[test]
    fn unit_without_ammo_fights_much_worse() {
        let (mut world, mut schedule) = setup(plains());
        world.spawn(division(1, 0, "blue", Pos { x: 10_500.0, y: 5_500.0 }));
        let mut red = world.spawn(division(2, 1, "red", Pos { x: 20_500.0, y: 5_500.0 }));
        red.insert(Stocks([400.0, 0.0, 300.0])); // no ammo
        run(&mut world, &mut schedule, TICKS_PER_DAY);
        let (blue, red) = (strength(&mut world, 1), strength(&mut world, 2));
        let blue_loss = 10.0 - blue;
        let red_loss = 10.0 - red;
        assert!(blue_loss < red_loss * 0.3, "blue lost {blue_loss}, red lost {red_loss}");
    }

    #[test]
    fn forest_defender_loses_less() {
        let mut grid = plains();
        for y in 0..10 {
            grid.set(20, y, Terrain::Forest);
        }
        let (mut world, mut schedule) = setup(grid);
        world.spawn(division(1, 0, "blue", Pos { x: 10_500.0, y: 5_500.0 }));
        world.spawn(division(2, 1, "red in forest", Pos { x: 20_500.0, y: 5_500.0 }));
        run(&mut world, &mut schedule, TICKS_PER_DAY);
        assert!(10.0 - strength(&mut world, 2) < (10.0 - strength(&mut world, 1)) * 0.75);
    }

    #[test]
    fn no_contact_no_combat() {
        let (mut world, mut schedule) = setup(plains());
        world.spawn(division(1, 0, "blue", Pos { x: 500.0, y: 5_500.0 }));
        world.spawn(division(2, 1, "red", Pos { x: 39_500.0, y: 5_500.0 }));
        run(&mut world, &mut schedule, 60);
        assert_eq!(strength(&mut world, 1), 10.0);
        assert!(world.resource::<Engagements>().0.is_empty());
    }

    #[test]
    fn crushed_division_is_destroyed_and_stocks_written_off() {
        let (mut world, mut schedule) = setup(plains());
        world.spawn(division(1, 0, "blue", Pos { x: 10_500.0, y: 5_500.0 }));
        let mut red = world.spawn(division(2, 1, "red", Pos { x: 20_500.0, y: 5_500.0 }));
        red.insert(Battalions(vec![Battalion { arm: Arm::Infantry, strength: 0.06 }]));
        let food = DIVISION_TARGET[crate::FOOD];
        run(&mut world, &mut schedule, 60);
        assert_eq!(strength(&mut world, 2), 0.0, "despawned");
        assert!(world.resource::<Ledger>().destroyed[crate::FOOD] > food - 5.0);
    }

    #[test]
    fn no_fuel_no_movement() {
        let (mut world, mut schedule) = setup(plains());
        let mut u = world.spawn(division(1, 0, "dry", Pos { x: 500.0, y: 5_500.0 }));
        u.insert(Stocks([0.0, 600.0, 300.0]));
        world.resource_mut::<crate::CommandQueue>().0.push(crate::SimCommand::Move {
            unit: UnitId(1),
            goal: Pos { x: 10_500.0, y: 5_500.0 },
        });
        run(&mut world, &mut schedule, 30);
        let mut q = world.query::<&Position>();
        assert_eq!(q.single(&world).unwrap().0, Pos { x: 500.0, y: 5_500.0 });
    }
}
