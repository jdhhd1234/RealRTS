//! Grid A* over the terrain grid (architect.md 4.5). Integer costs keep results deterministic.

use mapdata::{Terrain, TerrainGrid};
use std::cmp::Reverse;
use std::collections::BinaryHeap;

pub type Cell = (u32, u32);

/// Movement cost multiplier for entering a cell. shortcut: fixed table, move to Lua/scenario data later.
pub fn terrain_cost(t: Terrain) -> f32 {
    match t {
        Terrain::Plains => 1.0,
        Terrain::Urban => 1.5,
        Terrain::Forest => 2.0,
        Terrain::Marsh => 3.0,
        Terrain::Mountain => 4.0,
        Terrain::Water => 10.0, // river crossing: slow, not blocked (no bridges yet)
    }
}

fn int_cost(t: Terrain) -> u32 {
    (terrain_cost(t) * 10.0) as u32
}

/// Cheapest 8-neighbour path from `start` to `goal`, excluding `start`. `None` if unreachable.
/// shortcut: ignores roads/rail and enemy ZOC; add when movement uses the transport graph.
pub fn find_path(grid: &TerrainGrid, start: Cell, goal: Cell) -> Option<Vec<Cell>> {
    let (w, h) = (grid.width as i64, grid.height as i64);
    let idx = |(x, y): Cell| (y as i64 * w + x as i64) as usize;
    // Octile distance with straight=10, diagonal=14, scaled by the cheapest terrain (1.0 -> 10).
    let heur = |(x, y): Cell| {
        let dx = (x as i64 - goal.0 as i64).unsigned_abs();
        let dy = (y as i64 - goal.1 as i64).unsigned_abs();
        (10 * (dx + dy) - 6 * dx.min(dy)) as u32
    };

    let n = (w * h) as usize;
    let mut g = vec![u32::MAX; n];
    let mut came = vec![u32::MAX; n];
    let mut open = BinaryHeap::new();
    g[idx(start)] = 0;
    open.push(Reverse((heur(start), idx(start) as u32)));

    while let Some(Reverse((_, i))) = open.pop() {
        let cur = (i % grid.width, i / grid.width);
        if cur == goal {
            let mut path = vec![goal];
            let mut c = i as usize;
            while came[c] != u32::MAX {
                c = came[c] as usize;
                path.push((c as u32 % grid.width, c as u32 / grid.width));
            }
            path.pop(); // start
            path.reverse();
            return Some(path);
        }
        for (dx, dy) in [(-1i64, -1i64), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)] {
            let (nx, ny) = (cur.0 as i64 + dx, cur.1 as i64 + dy);
            if nx < 0 || ny < 0 || nx >= w || ny >= h {
                continue;
            }
            let next = (nx as u32, ny as u32);
            let step = if dx != 0 && dy != 0 { 14 } else { 10 };
            let cost = g[i as usize] + step * int_cost(grid.get(next.0, next.1)) / 10;
            let ni = idx(next);
            if cost < g[ni] {
                g[ni] = cost;
                came[ni] = i;
                open.push(Reverse((cost + heur(next), ni as u32)));
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn straight_line_on_plains() {
        let grid = TerrainGrid::filled(10, 10, 1000.0, Terrain::Plains);
        let p = find_path(&grid, (0, 0), (9, 0)).unwrap();
        assert_eq!(p.len(), 9);
        assert_eq!(*p.last().unwrap(), (9, 0));
        assert!(p.iter().all(|&(_, y)| y == 0));
    }

    #[test]
    fn detours_around_forest_when_cheaper() {
        let mut grid = TerrainGrid::filled(10, 3, 1000.0, Terrain::Plains);
        for x in 1..9 {
            grid.set(x, 1, Terrain::Forest);
        }
        let p = find_path(&grid, (0, 1), (9, 1)).unwrap();
        assert_eq!(*p.last().unwrap(), (9, 1));
        assert!(
            p.iter().all(|&(x, y)| grid.get(x, y) != Terrain::Forest),
            "should go around the forest, got {p:?}"
        );
    }

    #[test]
    fn same_cell_is_empty_path() {
        let grid = TerrainGrid::filled(3, 3, 1000.0, Terrain::Plains);
        assert_eq!(find_path(&grid, (1, 1), (1, 1)), Some(vec![]));
    }
}
