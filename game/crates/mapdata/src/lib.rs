//! Static map data (architect.md 4.1, 5). Only what never changes during a game lives here;
//! runtime state such as edge damage or region control belongs to `sim`.
//!
//! Coordinates: meters in the scenario's projected CRS. Origin at the map's bottom-left,
//! x east, y north.

use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Debug, Clone, Copy, PartialEq, Eq)]
pub enum Terrain {
    Plains,
    Forest,
    Marsh,
    Mountain,
    Urban,
    Water,
}

/// Square grid, row-major from the bottom-left cell. Rendering is continuous; only sim math uses cells.
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct TerrainGrid {
    pub width: u32,
    pub height: u32,
    pub cell_m: f32,
    pub cells: Vec<Terrain>,
}

impl TerrainGrid {
    pub fn filled(width: u32, height: u32, cell_m: f32, t: Terrain) -> Self {
        Self { width, height, cell_m, cells: vec![t; (width * height) as usize] }
    }

    pub fn size_m(&self) -> (f32, f32) {
        (self.width as f32 * self.cell_m, self.height as f32 * self.cell_m)
    }

    /// Cell containing a point, or `None` outside the map.
    pub fn cell_at(&self, pos: Pos) -> Option<(u32, u32)> {
        let (cx, cy) = ((pos.x / self.cell_m).floor(), (pos.y / self.cell_m).floor());
        (cx >= 0.0 && cy >= 0.0 && cx < self.width as f32 && cy < self.height as f32)
            .then_some((cx as u32, cy as u32))
    }

    pub fn get(&self, cx: u32, cy: u32) -> Terrain {
        self.cells[(cy * self.width + cx) as usize]
    }

    pub fn set(&mut self, cx: u32, cy: u32, t: Terrain) {
        self.cells[(cy * self.width + cx) as usize] = t;
    }
}

#[derive(Serialize, Deserialize, Debug, Clone, Copy, PartialEq)]
pub struct Pos {
    pub x: f32,
    pub y: f32,
}

#[derive(Serialize, Deserialize, Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct NodeId(pub u32);

#[derive(Serialize, Deserialize, Debug, Clone, Copy, PartialEq, Eq)]
pub enum NodeKind {
    Junction,
    Station,
    Port,
}

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct Node {
    pub pos: Pos,
    pub kind: NodeKind,
    pub name: Option<String>,
}

#[derive(Serialize, Deserialize, Debug, Clone, Copy, PartialEq, Eq)]
pub enum EdgeKind {
    /// Gauge id (meaning set by scenario Lua). Different gauges need engineers to convert (E3).
    Rail { gauge: u8 },
    Road,
    River,
}

/// Undirected transport link.
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct Edge {
    pub a: NodeId,
    pub b: NodeId,
    pub kind: EdgeKind,
    pub length_m: f32,
    pub capacity_t_per_day: f32,
}

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq, Default)]
pub struct TransportGraph {
    pub nodes: Vec<Node>,
    pub edges: Vec<Edge>,
}

impl TransportGraph {
    pub fn node(&self, id: NodeId) -> &Node {
        &self.nodes[id.0 as usize]
    }
}

#[derive(Serialize, Deserialize, Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct RegionId(pub u16);

/// Administrative region (architect.md 12): fixed unit for economy, population, partisans, legal ownership.
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct Region {
    pub name: String,
    /// Tons per day of fuel, ammo, food, equipment at full output (G1). mapbake will sum industry points.
    pub daily_output: [f32; 4],
}

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct MapData {
    pub name: String,
    pub terrain: TerrainGrid,
    pub graph: TransportGraph,
    pub regions: Vec<Region>,
    /// Region of each terrain cell, same layout as `TerrainGrid::cells`.
    pub region_cells: Vec<RegionId>,
}

/// Starting situation that comes with a map (QGIS attributes today; Lua scenarios later).
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq, Default)]
pub struct ScenarioData {
    /// Legal owner per region (same order as `MapData::regions`).
    pub legal: Vec<u8>,
    /// Side holding each region militarily at start (usually equal to `legal`).
    pub control: Vec<u8>,
    /// (side, node) supply bases.
    pub bases: Vec<(u8, NodeId)>,
    /// (side, node) field depots.
    pub depots: Vec<(u8, NodeId)>,
    pub units: Vec<UnitSpec>,
}

#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct UnitSpec {
    pub side: u8,
    pub name: String,
    pub pos: Pos,
}

/// A map plus its starting situation; what `mapbake` produces and the game opens.
#[derive(Serialize, Deserialize, Debug, Clone, PartialEq)]
pub struct Baked {
    pub map: MapData,
    pub scenario: ScenarioData,
}

impl Baked {
    pub fn from_ron(s: &str) -> Result<Self, MapError> {
        let b: Baked = ron::from_str(s).map_err(|e| MapError::Parse(e.to_string()))?;
        b.validate()?;
        Ok(b)
    }

    pub fn to_ron(&self) -> String {
        ron::ser::to_string(self).expect("Baked is always serializable")
    }

    /// Map checks plus scenario references (untrusted file input).
    pub fn validate(&self) -> Result<(), MapError> {
        self.map.validate()?;
        let s = &self.scenario;
        let regions = self.map.regions.len();
        if s.legal.len() != regions || s.control.len() != regions {
            return Err(MapError::Scenario("one legal owner and controller per region".into()));
        }
        if s.legal.iter().chain(&s.control).chain(s.units.iter().map(|u| &u.side)).any(|&side| side >= 8) {
            return Err(MapError::Scenario("side ids must be below 8".into()));
        }
        let nodes = self.map.graph.nodes.len() as u32;
        if s.bases.iter().chain(&s.depots).any(|(side, n)| n.0 >= nodes || *side >= 8) {
            return Err(MapError::Scenario("base or depot on a missing node".into()));
        }
        if let Some(u) = s.units.iter().find(|u| self.map.terrain.cell_at(u.pos).is_none()) {
            return Err(MapError::Scenario(format!("unit {} outside the map", u.name)));
        }
        Ok(())
    }
}

#[derive(Debug, PartialEq)]
pub enum MapError {
    Scenario(String),
    Parse(String),
    CellCount { expected: usize, got: usize },
    BadCellSize,
    EdgeNode { edge: usize },
    SelfLoop { edge: usize },
    BadEdgeValue { edge: usize },
    NodeOutside { node: usize },
    RegionCellCount { expected: usize, got: usize },
    BadRegionId { cell: usize },
}

impl MapData {
    /// Map with a single region covering everything (for tests and quick setups).
    pub fn single_region(name: &str, terrain: TerrainGrid, graph: TransportGraph) -> Self {
        let n = terrain.cells.len();
        Self {
            name: name.into(),
            terrain,
            graph,
            regions: vec![Region { name: name.into(), daily_output: [0.0; 4] }],
            region_cells: vec![RegionId(0); n],
        }
    }

    pub fn region_at(&self, cx: u32, cy: u32) -> RegionId {
        self.region_cells[(cy * self.terrain.width + cx) as usize]
    }

    /// Parse and validate. Map files are untrusted input (user/Lua-made), so always validate.
    pub fn from_ron(s: &str) -> Result<Self, MapError> {
        let map: MapData = ron::from_str(s).map_err(|e| MapError::Parse(e.to_string()))?;
        map.validate()?;
        Ok(map)
    }

    pub fn to_ron(&self) -> String {
        ron::ser::to_string(self).expect("MapData is always serializable")
    }

    pub fn validate(&self) -> Result<(), MapError> {
        let t = &self.terrain;
        let expected = t.width as usize * t.height as usize;
        if t.cells.len() != expected {
            return Err(MapError::CellCount { expected, got: t.cells.len() });
        }
        if !(t.cell_m.is_finite() && t.cell_m > 0.0) {
            return Err(MapError::BadCellSize);
        }
        if self.region_cells.len() != expected {
            return Err(MapError::RegionCellCount { expected, got: self.region_cells.len() });
        }
        if let Some(cell) = self.region_cells.iter().position(|r| r.0 as usize >= self.regions.len()) {
            return Err(MapError::BadRegionId { cell });
        }
        for (i, n) in self.graph.nodes.iter().enumerate() {
            if t.cell_at(n.pos).is_none() {
                return Err(MapError::NodeOutside { node: i });
            }
        }
        let n = self.graph.nodes.len() as u32;
        for (i, e) in self.graph.edges.iter().enumerate() {
            if e.a.0 >= n || e.b.0 >= n {
                return Err(MapError::EdgeNode { edge: i });
            }
            if e.a == e.b {
                return Err(MapError::SelfLoop { edge: i });
            }
            let ok = |v: f32| v.is_finite() && v > 0.0;
            if !ok(e.length_m) || !ok(e.capacity_t_per_day) {
                return Err(MapError::BadEdgeValue { edge: i });
            }
        }
        Ok(())
    }
}

/// `test_map` with its demo situation: east region column Red, the rest Blue; Red already holds
/// N Province (legally Blue), like the occupied city in military_area.svg.
pub fn test_baked() -> Baked {
    let map = test_map();
    let legal: Vec<u8> = (0..map.regions.len()).map(|r| if r % 3 == 2 { 1 } else { 0 }).collect();
    let mut control = legal.clone();
    control[7] = 1;
    let km = |x: f32, y: f32| Pos { x: x * 1000.0, y: y * 1000.0 };
    let unit = |side, name: &str, pos| UnitSpec { side, name: name.into(), pos };
    Baked {
        map,
        scenario: ScenarioData {
            legal,
            control,
            bases: vec![(0, NodeId(0)), (1, NodeId(3))],
            depots: vec![(0, NodeId(1)), (1, NodeId(3))],
            units: vec![
                unit(0, "1st Inf Div", km(20.0, 150.0)),
                unit(0, "2nd Armd Div", km(20.0, 120.0)),
                unit(1, "Red 10th Div", km(270.0, 150.0)),
            ],
        },
    }
}

/// Hand-made 300 km x 300 km test map (C1 minimum size), no QGIS needed.
/// A rail line runs west to east through a city; a road branches north; a river and forest split the map.
pub fn test_map() -> MapData {
    let mut terrain = TerrainGrid::filled(300, 300, 1000.0, Terrain::Plains);
    for cy in 0..300 {
        terrain.set(150, cy, Terrain::Water); // river, north-south
        for cx in 60..90 {
            terrain.set(cx, cy, Terrain::Forest);
        }
    }
    for cy in 140..160 {
        for cx in 200..215 {
            terrain.set(cx, cy, Terrain::Urban);
        }
    }
    for cy in 250..300 {
        for cx in 230..300 {
            terrain.set(cx, cy, Terrain::Mountain);
        }
    }

    let km = |x: f32, y: f32| Pos { x: x * 1000.0, y: y * 1000.0 };
    let node = |pos, kind, name: &str| Node { pos, kind, name: Some(name.to_string()) };
    let nodes = vec![
        node(km(10.5, 150.5), NodeKind::Station, "West Base"),
        node(km(100.5, 150.5), NodeKind::Junction, "Forest Junction"),
        node(km(207.5, 150.5), NodeKind::Station, "East City"),
        node(km(290.5, 150.5), NodeKind::Station, "Front Railhead"),
        node(km(100.5, 260.5), NodeKind::Junction, "North Village"),
    ];
    let edge = |a: u32, b: u32, kind| {
        let (pa, pb): (Pos, Pos) = (nodes[a as usize].pos, nodes[b as usize].pos);
        Edge {
            a: NodeId(a),
            b: NodeId(b),
            kind,
            length_m: ((pa.x - pb.x).powi(2) + (pa.y - pb.y).powi(2)).sqrt(),
            capacity_t_per_day: match kind {
                EdgeKind::Rail { .. } => 3000.0,
                EdgeKind::Road => 600.0,
                EdgeKind::River => 1500.0,
            },
        }
    };
    let rail = EdgeKind::Rail { gauge: 0 };
    let edges = vec![
        edge(0, 1, rail),
        edge(1, 2, rail),
        edge(2, 3, EdgeKind::Rail { gauge: 1 }), // gauge break east of the city
        edge(1, 4, EdgeKind::Road),
    ];

    // 3x3 regions with wavy borders, like architectImages/region_area_border.svg. Index = row * 3 + col,
    // row 0 = south. Integer-only so the map is identical on every machine.
    // Triangle wave in -amp..=amp with the given period (cells).
    let wave = |v: u32, period: u32, amp: i64| {
        let (phase, half) = ((v % period) as i64, period as i64 / 2);
        let dist = if phase < half { phase } else { period as i64 - phase };
        2 * amp * dist / half - amp
    };
    let mut region_cells = Vec::with_capacity(90_000);
    for cy in 0..300u32 {
        for cx in 0..300u32 {
            let col_border = |base: i64, p| base + wave(cy, p, 12);
            let row_border = |base: i64, p| base + wave(cx, p, 10);
            let col = (cx as i64 >= col_border(100, 80)) as u16 + (cx as i64 >= col_border(205, 110)) as u16;
            let row = (cy as i64 >= row_border(105, 90)) as u16 + (cy as i64 >= row_border(195, 70)) as u16;
            region_cells.push(RegionId(row * 3 + col));
        }
    }
    // Farmland everywhere, oil in the south, industry in the west and east cities.
    let regions = [
        ("SW", [40.0, 0.0, 60.0, 0.0]),
        ("S", [40.0, 0.0, 60.0, 0.0]),
        ("SE", [40.0, 0.0, 60.0, 0.0]),
        ("W", [0.0, 40.0, 40.0, 20.0]),
        ("Center", [0.0, 20.0, 40.0, 10.0]),
        ("E", [0.0, 40.0, 40.0, 20.0]),
        ("NW", [0.0, 0.0, 50.0, 0.0]),
        ("N", [0.0, 0.0, 50.0, 0.0]),
        ("NE", [0.0, 0.0, 50.0, 0.0]),
    ]
    .into_iter()
    .map(|(n, daily_output)| Region { name: format!("{n} Province"), daily_output })
    .collect();

    MapData {
        name: "test".into(),
        terrain,
        graph: TransportGraph { nodes, edges },
        regions,
        region_cells,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_map_is_valid() {
        let m = test_map();
        assert_eq!(m.validate(), Ok(()));
        assert_eq!(m.terrain.size_m(), (300_000.0, 300_000.0));
    }

    #[test]
    fn ron_roundtrip() {
        let m = test_map();
        assert_eq!(MapData::from_ron(&m.to_ron()).unwrap(), m);
    }

    #[test]
    fn baked_roundtrip_and_scenario_checks() {
        let b = test_baked();
        assert_eq!(Baked::from_ron(&b.to_ron()).unwrap(), b);
        let mut bad = b.clone();
        bad.scenario.depots.push((0, NodeId(99)));
        assert!(matches!(bad.validate(), Err(MapError::Scenario(_))));
        let mut bad = b;
        bad.scenario.units[0].pos = Pos { x: -1.0, y: 0.0 };
        assert!(matches!(bad.validate(), Err(MapError::Scenario(_))));
    }

    #[test]
    fn cell_lookup() {
        let m = test_map();
        let t = &m.terrain;
        assert_eq!(t.cell_at(Pos { x: 150_500.0, y: 3_000.0 }), Some((150, 3)));
        assert_eq!(t.get(150, 3), Terrain::Water);
        assert_eq!(t.get(207, 150), Terrain::Urban);
        assert_eq!(t.cell_at(Pos { x: -1.0, y: 0.0 }), None);
        assert_eq!(t.cell_at(Pos { x: 300_000.0, y: 0.0 }), None);
    }

    #[test]
    fn rejects_bad_maps() {
        let mut m = test_map();
        m.terrain.cells.pop();
        assert!(matches!(m.validate(), Err(MapError::CellCount { .. })));

        let mut m = test_map();
        m.graph.edges[0].b = NodeId(99);
        assert_eq!(m.validate(), Err(MapError::EdgeNode { edge: 0 }));

        let mut m = test_map();
        m.graph.edges[1].b = m.graph.edges[1].a;
        assert_eq!(m.validate(), Err(MapError::SelfLoop { edge: 1 }));

        let mut m = test_map();
        m.graph.edges[2].capacity_t_per_day = 0.0;
        assert_eq!(m.validate(), Err(MapError::BadEdgeValue { edge: 2 }));

        let mut m = test_map();
        m.graph.nodes[3].pos.x = 400_000.0;
        assert_eq!(m.validate(), Err(MapError::NodeOutside { node: 3 }));

        assert!(matches!(MapData::from_ron("not ron"), Err(MapError::Parse(_))));

        let mut m = test_map();
        m.region_cells.pop();
        assert!(matches!(m.validate(), Err(MapError::RegionCellCount { .. })));

        let mut m = test_map();
        m.region_cells[5] = RegionId(9);
        assert_eq!(m.validate(), Err(MapError::BadRegionId { cell: 5 }));
    }

    #[test]
    fn test_map_has_nine_regions_all_used() {
        let m = test_map();
        assert_eq!(m.regions.len(), 9);
        for r in 0..9 {
            assert!(m.region_cells.contains(&RegionId(r)), "region {r} empty");
        }
        assert_eq!(m.region_at(0, 0), RegionId(0));
        assert_eq!(m.region_at(299, 299), RegionId(8));
        assert_eq!(m.region_at(150, 150), RegionId(4));
    }
}
