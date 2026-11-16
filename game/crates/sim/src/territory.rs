//! Territory in three layers (architect.md 12): fixed regions (mapdata), legal ownership per region,
//! military control per 1 km cell.

use crate::combat::Battalions;
use crate::deploy::{Deployment, layout, line_ends, point_segment_dist};
use crate::partisan::Partisan;
use crate::supply::Depot;
use crate::units::{Map, Position, Side, UnitId};
use bevy_ecs::prelude::*;
use mapdata::{MapData, Pos};

/// A unit controls cells within this distance of its line (12.3, 15).
pub const CONTROL_RADIUS_M: f32 = 10_000.0;
/// A side controls a region militarily when it holds at least this share of its cells (12.2).
pub const REGION_CONTROL_SHARE: f32 = 0.8;
/// shortcut: side ids are bit positions in a u8 mask; raise when scenarios need more sides.
pub const MAX_SIDES: u8 = 8;

#[derive(Resource, Debug, Clone)]
pub struct Territory {
    /// Legal owner per region (de jure). Changes only by events (annexation, treaty, scenario).
    pub legal: Vec<u8>,
    /// Controlling side per terrain cell (de facto). Starts equal to `legal`.
    pub military: Vec<u8>,
}

impl Territory {
    pub fn from_legal(map: &MapData, legal: Vec<u8>) -> Self {
        assert_eq!(legal.len(), map.regions.len(), "one legal owner per region");
        assert!(legal.iter().all(|&s| s < MAX_SIDES));
        let military = map.region_cells.iter().map(|r| legal[r.0 as usize]).collect();
        Self { legal, military }
    }

    pub fn military_at(&self, map: &MapData, pos: Pos) -> Option<u8> {
        let (cx, cy) = map.terrain.cell_at(pos)?;
        Some(self.military[(cy * map.terrain.width + cx) as usize])
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RegionStatus {
    /// Military owner is the legal owner.
    Home,
    /// Another side holds the region militarily.
    Occupied { by: u8 },
    /// No side holds `REGION_CONTROL_SHARE` of the cells.
    Contested,
}

pub fn region_status(map: &MapData, t: &Territory) -> Vec<RegionStatus> {
    let mut counts = vec![[0u32; MAX_SIDES as usize]; map.regions.len()];
    for (cell, r) in map.region_cells.iter().enumerate() {
        counts[r.0 as usize][t.military[cell] as usize] += 1;
    }
    counts
        .iter()
        .enumerate()
        .map(|(r, c)| {
            let total: u32 = c.iter().sum();
            let (side, &n) = c.iter().enumerate().max_by_key(|(s, n)| (**n, std::cmp::Reverse(*s))).unwrap();
            if total == 0 || (n as f32) < REGION_CONTROL_SHARE * total as f32 {
                RegionStatus::Contested
            } else if side as u8 == t.legal[r] {
                RegionStatus::Home
            } else {
                RegionStatus::Occupied { by: side as u8 }
            }
        })
        .collect()
}

/// Cells reached by exactly one side's units flip to that side; contested or empty cells keep their owner.
/// Partisan bands are excluded: they raid but do not hold ground.
pub fn update_military(
    map: Res<Map>,
    mut territory: ResMut<Territory>,
    units: Query<(&Side, &Position, Option<&Deployment>, Option<&Battalions>), (With<UnitId>, Without<Partisan>)>,
) {
    let grid = &map.0.terrain;
    let mut touched = vec![0u8; grid.cells.len()];
    for (side, pos, dep, bats) in &units {
        if side.0 >= MAX_SIDES {
            continue;
        }
        let frontage = match (dep, bats) {
            (Some(d), Some(b)) => layout(b, d.posture).frontage_m,
            _ => 0.0,
        };
        let (a, b) = line_ends(pos.0, dep.map_or((1.0, 0.0), |d| d.facing), frontage);
        let reach = CONTROL_RADIUS_M + frontage / 2.0;
        let (Some((ux, uy)), r_cells) = (grid.cell_at(pos.0), (reach / grid.cell_m).ceil() as i64) else { continue };
        for cy in (uy as i64 - r_cells).max(0)..=(uy as i64 + r_cells).min(grid.height as i64 - 1) {
            for cx in (ux as i64 - r_cells).max(0)..=(ux as i64 + r_cells).min(grid.width as i64 - 1) {
                let (x, y) = ((cx as f32 + 0.5) * grid.cell_m, (cy as f32 + 0.5) * grid.cell_m);
                if point_segment_dist(Pos { x, y }, a, b) <= CONTROL_RADIUS_M {
                    touched[(cy * grid.width as i64 + cx) as usize] |= 1 << side.0;
                }
            }
        }
    }
    // Only flag a change when a cell really flips, so the map overlay redraws only then.
    let t = territory.bypass_change_detection();
    let mut changed = false;
    for (cell, mask) in touched.iter().enumerate() {
        if mask.count_ones() == 1 {
            let side = mask.trailing_zeros() as u8;
            if t.military[cell] != side {
                t.military[cell] = side;
                changed = true;
            }
        }
    }
    if changed {
        territory.set_changed();
    }
}

/// A depot standing in enemy-controlled ground is captured with its stock (E5).
pub fn capture_depots(map: Res<Map>, territory: Res<Territory>, mut depots: Query<&mut Depot>) {
    for mut d in &mut depots {
        let pos = map.0.graph.node(d.node).pos;
        if let Some(owner) = territory.military_at(&map.0, pos)
            && owner != d.side
        {
            d.side = owner;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{configure, division, init_world, depot};
    use mapdata::{NodeId, RegionId, test_map};

    fn km(x: f32, y: f32) -> Pos {
        Pos { x: x * 1000.0, y: y * 1000.0 }
    }

    /// West and middle columns Blue (0), east column Red (1), like law_area.svg.
    fn legal() -> Vec<u8> {
        (0..9).map(|r| if r % 3 == 2 { 1 } else { 0 }).collect()
    }

    fn setup() -> (World, Schedule) {
        let mut world = World::new();
        let map = test_map();
        let t = Territory::from_legal(&map, legal());
        init_world(&mut world, map);
        world.insert_resource(t);
        let mut schedule = Schedule::default();
        configure(&mut schedule);
        (world, schedule)
    }

    fn owner(world: &World, p: Pos) -> u8 {
        world.resource::<Territory>().military_at(&world.resource::<Map>().0, p).unwrap()
    }

    #[test]
    fn unit_takes_cells_within_radius() {
        let (mut world, mut schedule) = setup();
        assert_eq!(owner(&world, km(260.0, 150.0)), 1);
        world.spawn(division(1, 0, "raid", km(260.0, 150.0)));
        for _ in 0..5 {
            schedule.run(&mut world);
        }
        assert_eq!(owner(&world, km(260.0, 150.0)), 0);
        assert_eq!(owner(&world, km(268.0, 150.0)), 0, "inside 10 km");
        assert_eq!(owner(&world, km(275.0, 150.0)), 1, "outside 10 km stays Red");
    }

    #[test]
    fn contested_cell_keeps_owner_and_control_persists() {
        let (mut world, mut schedule) = setup();
        world.spawn(division(1, 0, "blue", km(255.0, 150.0)));
        world.spawn(division(2, 1, "red", km(265.0, 150.0)));
        for _ in 0..5 {
            schedule.run(&mut world);
        }
        assert_eq!(owner(&world, km(260.0, 150.0)), 1, "both reach it: unchanged");
        assert_eq!(owner(&world, km(247.0, 150.0)), 0, "only Blue reaches it");

        // Blue leaves: its cells stay Blue until Red arrives.
        let blue = world.query::<(Entity, &UnitId)>().iter(&world).find(|(_, u)| u.0 == 1).unwrap().0;
        world.despawn(blue);
        for _ in 0..5 {
            schedule.run(&mut world);
        }
        assert_eq!(owner(&world, km(247.0, 150.0)), 0);
    }

    #[test]
    fn region_status_home_occupied_contested() {
        let map = test_map();
        let mut t = Territory::from_legal(&map, legal());
        assert!(region_status(&map, &t).iter().all(|s| *s == RegionStatus::Home));

        let cells: Vec<usize> = (0..map.region_cells.len()).filter(|&c| map.region_cells[c] == RegionId(5)).collect();
        for &c in &cells {
            t.military[c] = 0;
        }
        assert_eq!(region_status(&map, &t)[5], RegionStatus::Occupied { by: 0 });

        for &c in &cells[..cells.len() / 2] {
            t.military[c] = 1;
        }
        assert_eq!(region_status(&map, &t)[5], RegionStatus::Contested);
    }

    #[test]
    fn depot_in_enemy_ground_is_captured() {
        let (mut world, mut schedule) = setup();
        let d = world.spawn(depot(1, NodeId(3))).id(); // Red depot at Front Railhead
        world.spawn(division(1, 0, "blue", km(285.0, 150.0)));
        for _ in 0..5 {
            schedule.run(&mut world);
        }
        assert_eq!(world.get::<Depot>(d).unwrap().side, 0);
    }
}
