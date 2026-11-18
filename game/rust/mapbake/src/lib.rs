//! QGIS data -> `mapdata::Baked` (architect.md 5.4). Input is one GeoPackage, or a folder of
//! GeoJSON / Shapefile files named after the layers (`terrain.shp`, `rail.geojson`, ...).
//!
//! Layer contract (table names, case-insensitive; every layer optional, unknown layers ignored):
//! - `terrain`  polygons, `kind` = plains | forest | marsh | mountain | urban | water (default plains)
//! - `regions`  polygons, `name`, `side` (legal owner, default 0), `control` (holder at start, default side),
//!   `fuel` `ammo` `food` `equipment` (t/day output, default 0)
//! - `nodes`    points, `name`, `kind` = junction | station | port, `base_side`, `depot_side`
//! - `rail`     lines, `gauge` (default 0), `capacity` (t/day, default 3000)
//! - `roads`    lines, `capacity` (default 600)
//! - `rivers`   lines, `capacity` (default 1500)
//! - `units`    points, `name`, `side` (default 0)
//!
//! All layers must share one projected (metric) CRS; geographic CRS (degrees) is rejected because
//! projection is done in QGIS, never in the game. Line ends snap to the nearest node within 2 km,
//! otherwise a junction node is created there.

pub mod folder;
pub mod geom;
pub use folder::bake_folder;

use geom::{Geom, P, parse_gpb, raster_polygon};
use mapdata::{
    Baked, Edge, EdgeKind, MapData, Node, NodeId, NodeKind, Pos, Region, RegionId, ScenarioData, Terrain, TerrainGrid,
    TransportGraph, UnitSpec,
};
use rusqlite::types::ValueRef;
use rusqlite::{Connection, OpenFlags};
use std::path::Path;

pub const CELL_M: f64 = 1000.0;
/// shortcut: caps the grid at 2000 x 2000 km (16 MB of cells); raise with chunked storage if needed.
pub const MAX_CELLS_PER_SIDE: u32 = 2000;
pub const SNAP_M: f64 = 2000.0;
const LAYERS: [&str; 7] = ["terrain", "regions", "nodes", "rail", "roads", "rivers", "units"];

enum Attr {
    Num(f64),
    Text(String),
    Null,
}

struct Feature {
    geom: Geom,
    attrs: Vec<(String, Attr)>,
}

impl Feature {
    fn get(&self, key: &str) -> Option<&Attr> {
        self.attrs.iter().find(|(k, _)| k.eq_ignore_ascii_case(key)).map(|(_, v)| v)
    }
    fn text(&self, key: &str) -> Option<String> {
        match self.get(key)? {
            Attr::Text(s) if !s.trim().is_empty() => Some(s.trim().to_string()),
            Attr::Num(n) => Some(n.to_string()),
            _ => None,
        }
    }
    fn num(&self, key: &str) -> Result<Option<f64>, String> {
        match self.get(key) {
            None | Some(Attr::Null) => Ok(None),
            Some(Attr::Num(n)) => Ok(Some(*n)),
            Some(Attr::Text(s)) if s.trim().is_empty() => Ok(None),
            Some(Attr::Text(s)) => s.trim().parse().map(Some).map_err(|_| format!("field {key}: '{s}' is not a number")),
        }
    }
    fn side(&self, key: &str) -> Result<Option<u8>, String> {
        match self.num(key)? {
            None => Ok(None),
            Some(n) if n.fract() == 0.0 && (0.0..8.0).contains(&n) => Ok(Some(n as u8)),
            Some(n) => Err(format!("field {key}: side {n} must be a whole number 0..7")),
        }
    }
}

fn quote(name: &str) -> String {
    format!("\"{}\"", name.replace('"', "\"\""))
}

/// Reads every known feature layer. Returns (lowercase layer name, features).
fn read_layers(conn: &Connection) -> Result<Vec<(String, Vec<Feature>)>, String> {
    let sql = |e: rusqlite::Error| format!("GeoPackage read error: {e}");
    let mut stmt =
        conn.prepare("SELECT table_name, column_name, srs_id FROM gpkg_geometry_columns").map_err(|e| format!("not a GeoPackage ({e})"))?;
    let cols: Vec<(String, String, i64)> =
        stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?))).map_err(sql)?.collect::<Result<_, _>>().map_err(sql)?;

    let mut srs_seen: Option<i64> = None;
    let mut out = Vec::new();
    for (table, geom_col, srs) in cols {
        let lower = table.to_ascii_lowercase();
        if !LAYERS.contains(&lower.as_str()) {
            continue;
        }
        match srs_seen {
            Some(s) if s != srs => return Err(format!("layer {table} uses CRS {srs}, others use {s}: reproject all layers to one CRS")),
            _ => srs_seen = Some(srs),
        }
        let mut stmt = conn.prepare(&format!("SELECT * FROM {}", quote(&table))).map_err(sql)?;
        let names: Vec<String> = stmt.column_names().iter().map(|s| s.to_string()).collect();
        let gi = names.iter().position(|n| n.eq_ignore_ascii_case(&geom_col)).ok_or(format!("layer {table}: no geometry column"))?;
        let mut rows = stmt.query([]).map_err(sql)?;
        let mut feats = Vec::new();
        while let Some(row) = rows.next().map_err(sql)? {
            let geom = match row.get_ref(gi).map_err(sql)? {
                ValueRef::Blob(b) => parse_gpb(b).map_err(|e| format!("layer {table}, feature {}: {e}", feats.len() + 1))?,
                _ => Geom::default(),
            };
            let mut attrs = Vec::new();
            for (i, n) in names.iter().enumerate() {
                if i == gi {
                    continue;
                }
                let v = match row.get_ref(i).map_err(sql)? {
                    ValueRef::Integer(v) => Attr::Num(v as f64),
                    ValueRef::Real(v) => Attr::Num(v),
                    ValueRef::Text(t) => Attr::Text(String::from_utf8_lossy(t).into_owned()),
                    _ => Attr::Null,
                };
                attrs.push((n.clone(), v));
            }
            feats.push(Feature { geom, attrs });
        }
        out.push((lower, feats));
    }
    if out.is_empty() {
        return Err(format!("no known layers found; expected some of: {}", LAYERS.join(", ")));
    }
    if let Some(srs) = srs_seen {
        check_projected(conn, srs)?;
    }
    Ok(out)
}

fn check_projected(conn: &Connection, srs: i64) -> Result<(), String> {
    if srs == 0 {
        return Err("layers have an undefined geographic CRS; set a projected metric CRS in QGIS".into());
    }
    if srs == -1 {
        return Ok(()); // undefined Cartesian: taken as meters
    }
    let def: Option<String> = conn
        .query_row("SELECT definition FROM gpkg_spatial_ref_sys WHERE srs_id = ?1", [srs], |r| r.get(0))
        .map_err(|_| format!("CRS {srs} missing from gpkg_spatial_ref_sys"))?;
    let def = def.unwrap_or_default();
    let head = def.trim_start().to_ascii_uppercase();
    if srs == 4326 || head.starts_with("GEOGCS") || head.starts_with("GEOGCRS") || head.starts_with("GEODCRS") {
        return Err(format!(
            "CRS {srs} is geographic (degrees). In QGIS, export the layers to a projected metric CRS (e.g. a UTM zone) first"
        ));
    }
    Ok(())
}

/// A folder, or any file in it with a GeoJSON/Shapefile extension, bakes the whole folder;
/// anything else is opened as a GeoPackage.
pub fn bake_path(path: &Path) -> Result<Baked, String> {
    let ext = path.extension().map(|e| e.to_string_lossy().to_ascii_lowercase());
    match ext.as_deref() {
        _ if path.is_dir() => bake_folder(path),
        Some("geojson" | "json" | "shp" | "dbf" | "shx" | "prj" | "cpg") => bake_folder(path.parent().unwrap_or(Path::new("."))),
        _ => bake_file(path),
    }
}

pub fn bake_file(path: &Path) -> Result<Baked, String> {
    let conn = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY).map_err(|e| format!("cannot open {}: {e}", path.display()))?;
    let name = path.file_stem().map_or("map".into(), |s| s.to_string_lossy().into_owned());
    bake(&conn, &name)
}

pub fn bake(conn: &Connection, name: &str) -> Result<Baked, String> {
    bake_layers(&read_layers(conn)?, name)
}

/// Format-neutral core: features per known layer (already checked to be in one projected CRS).
fn bake_layers(layers: &[(String, Vec<Feature>)], name: &str) -> Result<Baked, String> {
    let layer = |n: &str| layers.iter().find(|(l, _)| l == n).map_or(&[][..], |(_, f)| f.as_slice());

    // Extent over every feature, snapped to whole cells.
    let (mut lo, mut hi) = ((f64::INFINITY, f64::INFINITY), (f64::NEG_INFINITY, f64::NEG_INFINITY));
    for p in layers.iter().flat_map(|(_, f)| f).flat_map(|f| f.geom.all_points()) {
        if !(p.0.is_finite() && p.1.is_finite()) {
            return Err("geometry with non-finite coordinates".into());
        }
        lo = (lo.0.min(p.0), lo.1.min(p.1));
        hi = (hi.0.max(p.0), hi.1.max(p.1));
    }
    if !lo.0.is_finite() {
        return Err("the layers contain no geometry".into());
    }
    let origin = ((lo.0 / CELL_M).floor() * CELL_M, (lo.1 / CELL_M).floor() * CELL_M);
    let (w, h) = (((hi.0 - origin.0) / CELL_M).floor() as u64 + 1, ((hi.1 - origin.1) / CELL_M).floor() as u64 + 1);
    if w > MAX_CELLS_PER_SIDE as u64 || h > MAX_CELLS_PER_SIDE as u64 {
        return Err(format!("map is {w} x {h} km; the limit is {MAX_CELLS_PER_SIDE} km per side (is the CRS in meters?)"));
    }
    let (w, h) = (w as u32, h as u32);
    let cell = |p: P| ((p.0 - origin.0) / CELL_M, (p.1 - origin.1) / CELL_M);
    let meters = |p: P| Pos { x: (p.0 - origin.0) as f32, y: (p.1 - origin.1) as f32 };
    let to_cells = |rings: &Vec<Vec<P>>| -> Vec<Vec<P>> { rings.iter().map(|r| r.iter().map(|&p| cell(p)).collect()).collect() };

    // Terrain: later kinds win (water last).
    let mut terrain = TerrainGrid::filled(w, h, CELL_M as f32, Terrain::Plains);
    let kinds = [
        ("plains", Terrain::Plains),
        ("forest", Terrain::Forest),
        ("marsh", Terrain::Marsh),
        ("urban", Terrain::Urban),
        ("mountain", Terrain::Mountain),
        ("water", Terrain::Water),
    ];
    let terrain_feats = layer("terrain");
    for (i, f) in terrain_feats.iter().enumerate() {
        let k = f.text("kind").unwrap_or("plains".into()).to_ascii_lowercase();
        if !kinds.iter().any(|(n, _)| *n == k) {
            return Err(format!("terrain feature {}: unknown kind '{k}' (use plains, forest, marsh, mountain, urban, water)", i + 1));
        }
    }
    for (kname, t) in kinds {
        for f in terrain_feats.iter().filter(|f| f.text("kind").unwrap_or("plains".into()).eq_ignore_ascii_case(kname)) {
            for poly in &f.geom.polygons {
                raster_polygon(&to_cells(poly), w, h, |x, y| terrain.set(x, y, t));
            }
        }
    }

    // Regions; cells outside every region polygon go to an extra "Unassigned" region.
    let mut regions = Vec::new();
    let (mut legal, mut control) = (Vec::new(), Vec::new());
    let mut region_cells = vec![u16::MAX; (w * h) as usize];
    for (i, f) in layer("regions").iter().enumerate() {
        if i >= u16::MAX as usize - 1 {
            return Err("too many regions".into());
        }
        let out = ["fuel", "ammo", "food", "equipment"].map(|k| f.num(k).map(|v| v.unwrap_or(0.0).max(0.0) as f32));
        let mut daily_output = [0.0; 4];
        for (d, o) in daily_output.iter_mut().zip(out) {
            *d = o.map_err(|e| format!("region {}: {e}", i + 1))?;
        }
        let side = f.side("side").map_err(|e| format!("region {}: {e}", i + 1))?.unwrap_or(0);
        let held = f.side("control").map_err(|e| format!("region {}: {e}", i + 1))?.unwrap_or(side);
        regions.push(Region { name: f.text("name").unwrap_or(format!("Region {}", i + 1)), daily_output });
        legal.push(side);
        control.push(held);
        for poly in &f.geom.polygons {
            raster_polygon(&to_cells(poly), w, h, |x, y| region_cells[(y * w + x) as usize] = i as u16);
        }
    }
    if region_cells.contains(&u16::MAX) {
        let r = regions.len() as u16;
        regions.push(Region { name: if r == 0 { name.to_string() } else { "Unassigned".into() }, daily_output: [0.0; 4] });
        legal.push(0);
        control.push(0);
        region_cells.iter_mut().filter(|c| **c == u16::MAX).for_each(|c| *c = r);
    }

    // Nodes, bases, depots.
    let mut nodes: Vec<Node> = Vec::new();
    let mut raw: Vec<P> = Vec::new(); // node positions in CRS units, for snapping
    let (mut bases, mut depots) = (Vec::new(), Vec::new());
    for (i, f) in layer("nodes").iter().enumerate() {
        let err = |e: String| format!("node {}: {e}", i + 1);
        let kind = match f.text("kind").map(|k| k.to_ascii_lowercase()).as_deref() {
            None | Some("junction") => NodeKind::Junction,
            Some("station") => NodeKind::Station,
            Some("port") => NodeKind::Port,
            Some(k) => return Err(err(format!("unknown kind '{k}' (use junction, station, port)"))),
        };
        for &p in &f.geom.points {
            let id = NodeId(nodes.len() as u32);
            nodes.push(Node { pos: meters(p), kind, name: f.text("name") });
            raw.push(p);
            if let Some(s) = f.side("base_side").map_err(err)? {
                bases.push((s, id));
            }
            if let Some(s) = f.side("depot_side").map_err(err)? {
                depots.push((s, id));
            }
        }
    }

    // Transport lines.
    let mut edges = Vec::new();
    for (layer_name, default_cap) in [("rail", 3000.0), ("roads", 600.0), ("rivers", 1500.0)] {
        for (i, f) in layer(layer_name).iter().enumerate() {
            let err = |e: String| format!("{layer_name} feature {}: {e}", i + 1);
            let kind = match layer_name {
                "rail" => {
                    let g = f.num("gauge").map_err(err)?.unwrap_or(0.0);
                    if !(0.0..=255.0).contains(&g) || g.fract() != 0.0 {
                        return Err(err(format!("gauge {g} must be a whole number 0..255")));
                    }
                    EdgeKind::Rail { gauge: g as u8 }
                }
                "roads" => EdgeKind::Road,
                _ => EdgeKind::River,
            };
            let capacity = f.num("capacity").map_err(err)?.unwrap_or(default_cap);
            if !(capacity.is_finite() && capacity > 0.0) {
                return Err(err(format!("capacity {capacity} must be positive")));
            }
            for line in f.geom.lines.iter().filter(|l| l.len() >= 2) {
                let mut snap = |p: P| -> NodeId {
                    let near = raw
                        .iter()
                        .enumerate()
                        .map(|(i, q)| (i, (q.0 - p.0).hypot(q.1 - p.1)))
                        .filter(|(_, d)| *d <= SNAP_M)
                        .min_by(|a, b| a.1.total_cmp(&b.1));
                    if let Some((i, _)) = near {
                        return NodeId(i as u32);
                    }
                    nodes.push(Node { pos: meters(p), kind: NodeKind::Junction, name: None });
                    raw.push(p);
                    NodeId(nodes.len() as u32 - 1)
                };
                let (a, b) = (snap(line[0]), snap(*line.last().unwrap()));
                if a == b {
                    continue; // loop or too short to connect two nodes
                }
                let length: f64 = line.windows(2).map(|s| (s[1].0 - s[0].0).hypot(s[1].1 - s[0].1)).sum();
                edges.push(Edge { a, b, kind, length_m: length.max(1.0) as f32, capacity_t_per_day: capacity as f32 });
            }
        }
    }

    let mut units = Vec::new();
    for (i, f) in layer("units").iter().enumerate() {
        let side = f.side("side").map_err(|e| format!("unit {}: {e}", i + 1))?.unwrap_or(0);
        for &p in &f.geom.points {
            units.push(UnitSpec { side, name: f.text("name").unwrap_or(format!("Unit {}", units.len() + 1)), pos: meters(p) });
        }
    }

    let baked = Baked {
        map: MapData {
            name: name.to_string(),
            terrain,
            graph: TransportGraph { nodes, edges },
            regions,
            region_cells: region_cells.into_iter().map(RegionId).collect(),
        },
        scenario: ScenarioData { legal, control, bases, depots, units },
    };
    baked.validate().map_err(|e| format!("baked map is invalid: {e:?}"))?;
    Ok(baked)
}

#[cfg(test)]
mod tests {
    use super::*;
    use geom::tests::{gpb, wkb_line, wkb_point, wkb_polygon};

    const UTM: &str = r#"PROJCS["WGS 84 / UTM zone 35N",GEOGCS["WGS 84"],UNIT["metre",1]]"#;
    const WGS84: &str = r#"GEOGCS["WGS 84",DATUM["WGS_1984"],UNIT["degree",0.0174532925199433]]"#;

    /// Minimal GeoPackage in memory: (layer, extra column names, rows of (geometry wkb, values)).
    fn gpkg(srs: i32, def: &str, layers: &[(&str, &[&str], Vec<(Vec<u8>, Vec<&str>)>)]) -> Connection {
        let c = Connection::open_in_memory().unwrap();
        c.execute_batch(
            "CREATE TABLE gpkg_spatial_ref_sys (srs_name TEXT, srs_id INTEGER, organization TEXT,
                 organization_coordsys_id INTEGER, definition TEXT);
             CREATE TABLE gpkg_geometry_columns (table_name TEXT, column_name TEXT, geometry_type_name TEXT,
                 srs_id INTEGER, z INTEGER, m INTEGER);",
        )
        .unwrap();
        c.execute("INSERT INTO gpkg_spatial_ref_sys VALUES ('x', ?1, 'EPSG', ?1, ?2)", rusqlite::params![srs, def]).unwrap();
        for (name, cols, rows) in layers {
            let extra: String = cols.iter().map(|c| format!(", {c} TEXT")).collect();
            c.execute_batch(&format!("CREATE TABLE {name} (fid INTEGER PRIMARY KEY, geom BLOB{extra});")).unwrap();
            c.execute("INSERT INTO gpkg_geometry_columns VALUES (?1, 'geom', 'GEOMETRY', ?2, 0, 0)", rusqlite::params![name, srs])
                .unwrap();
            for (wkb, vals) in rows {
                let marks: String = (0..cols.len()).map(|i| format!(", ?{}", i + 2)).collect();
                let mut stmt = c.prepare(&format!("INSERT INTO {name} (geom{}) VALUES (?1{marks})", cols.iter().map(|c| format!(", {c}")).collect::<String>())).unwrap();
                let mut params: Vec<Box<dyn rusqlite::ToSql>> = vec![Box::new(gpb(srs, wkb.clone()))];
                params.extend(vals.iter().map(|v| Box::new(v.to_string()) as Box<dyn rusqlite::ToSql>));
                stmt.execute(rusqlite::params_from_iter(params.iter())).unwrap();
            }
        }
        c
    }

    fn sq(x0: f64, y0: f64, x1: f64, y1: f64) -> Vec<P> {
        vec![(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)]
    }

    /// 40 x 20 km at UTM-like offsets: west region Blue, east region Red, forest block, rail + road.
    fn sample() -> Connection {
        let (ox, oy) = (500_000.0, 5_000_000.0);
        let p = |x: f64, y: f64| (ox + x * 1000.0, oy + y * 1000.0);
        let poly = |x0, y0, x1, y1| wkb_polygon(&[sq(p(x0, y0).0, p(x0, y0).1, p(x1, y1).0, p(x1, y1).1)]);
        gpkg(
            32635,
            UTM,
            &[
                ("terrain", &["kind"], vec![(poly(0.0, 0.0, 40.0, 20.0), vec!["plains"]), (poly(10.0, 0.0, 15.0, 20.0), vec!["Forest"])]),
                (
                    "regions",
                    &["name", "side"],
                    vec![(poly(0.0, 0.0, 20.0, 20.0), vec!["West", "0"]), (poly(20.0, 0.0, 40.0, 20.0), vec!["East", "1"])],
                ),
                (
                    "nodes",
                    &["name", "kind", "base_side", "depot_side"],
                    vec![(wkb_point(p(2.5, 10.5)), vec!["Base", "station", "0", ""]), (wkb_point(p(37.5, 10.5)), vec!["Rail End", "", "", "1"])],
                ),
                ("rail", &["gauge"], vec![(wkb_line(&[p(2.0, 10.0), p(20.0, 12.0), p(38.0, 10.0)]), vec!["1"])]),
                ("roads", &[], vec![(wkb_line(&[p(2.5, 10.5), p(20.5, 2.5)]), vec![])]),
                ("units", &["name", "side"], vec![(wkb_point(p(5.0, 5.0)), vec!["1st Div", "0"])]),
            ],
        )
    }

    #[test]
    fn bakes_terrain_regions_graph_and_units() {
        let b = bake(&sample(), "sample").unwrap();
        let m = &b.map;
        assert_eq!((m.terrain.width, m.terrain.height), (41, 21), "extent snapped to whole km");
        assert_eq!(m.terrain.get(12, 5), Terrain::Forest);
        assert_eq!(m.terrain.get(5, 5), Terrain::Plains);
        assert_eq!(m.regions.len(), 3, "West, East, and Unassigned for the edge row/column");
        assert_eq!(m.region_at(5, 5), RegionId(0));
        assert_eq!(m.region_at(30, 5), RegionId(1));
        assert_eq!(b.scenario.legal[..2], [0, 1]);
        assert_eq!(m.graph.nodes.len(), 3, "road end creates a junction");
        assert_eq!(m.graph.edges.len(), 2);
        assert_eq!(m.graph.edges[0].kind, EdgeKind::Rail { gauge: 1 });
        assert!((m.graph.edges[0].length_m - 36_221.0).abs() < 10.0, "polyline length {}", m.graph.edges[0].length_m);
        assert_eq!((b.scenario.bases.clone(), b.scenario.depots.clone()), (vec![(0, NodeId(0))], vec![(1, NodeId(1))]));
        assert_eq!(b.scenario.units[0].pos, Pos { x: 5_000.0, y: 5_000.0 });
    }

    #[test]
    fn rejects_geographic_crs() {
        let c = gpkg(4326, WGS84, &[("nodes", &[], vec![(wkb_point((27.0, 47.0)), vec![])])]);
        let e = bake(&c, "x").unwrap_err();
        assert!(e.contains("geographic"), "{e}");
    }

    #[test]
    fn rejects_unknown_terrain_and_bad_side() {
        let poly = wkb_polygon(&[sq(0.0, 0.0, 5000.0, 5000.0)]);
        let c = gpkg(3857, UTM, &[("terrain", &["kind"], vec![(poly.clone(), vec!["lava"])])]);
        assert!(bake(&c, "x").unwrap_err().contains("lava"));
        let c = gpkg(3857, UTM, &[("regions", &["side"], vec![(poly, vec!["9"])])]);
        assert!(bake(&c, "x").unwrap_err().contains("side"));
    }

    #[test]
    fn rejects_empty_or_unrelated_packages() {
        let c = gpkg(3857, UTM, &[("buildings", &[], vec![])]);
        assert!(bake(&c, "x").unwrap_err().contains("no known layers"));
    }
}
