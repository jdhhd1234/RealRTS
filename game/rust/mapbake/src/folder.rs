//! A folder of single-layer files: each `.geojson` / `.json` / `.shp` whose name is a layer name
//! (`terrain`, `regions`, `nodes`, `rail`, `roads`, `rivers`, `units`) becomes that layer.

use crate::geom::{Geom, P};
use crate::{Attr, Feature, LAYERS, bake_layers};
use mapdata::Baked;
use serde_json::Value;
use shapefile::dbase::FieldValue;
use shapefile::record::traits::HasXY;
use shapefile::Shape;
use std::path::Path;

/// Bakes every layer file in `dir`. Files with other names are ignored.
pub fn bake_folder(dir: &Path) -> Result<Baked, String> {
    let mut found: Vec<(String, std::path::PathBuf)> = Vec::new();
    let entries = std::fs::read_dir(dir).map_err(|e| format!("cannot read folder {}: {e}", dir.display()))?;
    for entry in entries.flatten() {
        let path = entry.path();
        let (Some(stem), Some(ext)) = (path.file_stem(), path.extension()) else { continue };
        let (stem, ext) = (stem.to_string_lossy().to_ascii_lowercase(), ext.to_string_lossy().to_ascii_lowercase());
        if !LAYERS.contains(&stem.as_str()) || !matches!(ext.as_str(), "geojson" | "json" | "shp") {
            continue;
        }
        if let Some((_, other)) = found.iter().find(|(s, _)| *s == stem) {
            return Err(format!("two files for layer {stem}: {} and {}", other.display(), path.display()));
        }
        found.push((stem, path));
    }
    if found.is_empty() {
        return Err(format!(
            "no layer files in {}; name them after the layers ({}) with .geojson or .shp",
            dir.display(),
            LAYERS.join(", ")
        ));
    }
    found.sort(); // deterministic layer order

    let mut layers = Vec::new();
    let mut crs_seen: Option<(String, String)> = None; // (EPSG code, file)
    for (stem, path) in &found {
        let file = path.file_name().unwrap_or_default().to_string_lossy().into_owned();
        let is_shp = path.extension().is_some_and(|e| e.eq_ignore_ascii_case("shp"));
        let (feats, crs) = if is_shp { read_shapefile(path) } else { read_geojson(path) }.map_err(|e| format!("{file}: {e}"))?;
        if crs.geographic || looks_like_degrees(&feats) {
            return Err(format!(
                "{file}: coordinates are geographic (degrees). In QGIS, export the layers to a projected metric CRS (e.g. a UTM zone) first"
            ));
        }
        if let Some(code) = crs.epsg {
            match &crs_seen {
                Some((c, f)) if *c != code => return Err(format!("{file} uses EPSG:{code} but {f} uses EPSG:{c}: reproject all layers to one CRS")),
                Some(_) => {}
                None => crs_seen = Some((code, file.clone())),
            }
        }
        layers.push((stem.clone(), feats));
    }
    let name = dir.file_name().map_or("map".into(), |n| n.to_string_lossy().into_owned());
    bake_layers(&layers, &name)
}

#[derive(Default)]
struct Crs {
    geographic: bool,
    epsg: Option<String>,
}

/// shortcut: files without a CRS are judged by extent; a projected map inside +-180 x +-90 m is too small to matter.
fn looks_like_degrees(feats: &[Feature]) -> bool {
    let mut pts = feats.iter().flat_map(|f| f.geom.all_points()).peekable();
    pts.peek().is_some() && pts.all(|(x, y)| x.abs() <= 180.0 && y.abs() <= 90.0)
}

// ---- GeoJSON ----

fn read_geojson(path: &Path) -> Result<(Vec<Feature>, Crs), String> {
    let text = std::fs::read_to_string(path).map_err(|e| e.to_string())?;
    let root: Value = serde_json::from_str(&text).map_err(|e| format!("invalid JSON: {e}"))?;
    let mut crs = Crs::default();
    // Legacy "crs" member written by QGIS for non-WGS84 exports, e.g. "urn:ogc:def:crs:EPSG::32635".
    if let Some(name) = root.pointer("/crs/properties/name").and_then(Value::as_str) {
        let upper = name.to_ascii_uppercase();
        crs.geographic = upper.contains("CRS84") || upper.ends_with(":4326");
        crs.epsg = upper.contains("EPSG").then(|| upper.rsplit(':').next().unwrap_or("").to_string()).filter(|c| !c.is_empty());
    }
    let feats = match root.get("type").and_then(Value::as_str) {
        Some("FeatureCollection") => root.get("features").and_then(Value::as_array).ok_or("FeatureCollection without features")?.iter().collect(),
        Some("Feature") => vec![&root],
        _ => return Err("expected a GeoJSON FeatureCollection or Feature".into()),
    };
    let mut out = Vec::new();
    for (i, f) in feats.into_iter().enumerate() {
        let mut geom = Geom::default();
        if let Some(g) = f.get("geometry").filter(|g| !g.is_null()) {
            json_geom(g, &mut geom, 0).map_err(|e| format!("feature {}: {e}", i + 1))?;
        }
        let attrs = f
            .get("properties")
            .and_then(Value::as_object)
            .map(|props| props.iter().map(|(k, v)| (k.clone(), json_attr(v))).collect())
            .unwrap_or_default();
        out.push(Feature { geom, attrs });
    }
    Ok((out, crs))
}

fn json_attr(v: &Value) -> Attr {
    match v {
        Value::Null => Attr::Null,
        Value::Number(n) => n.as_f64().map_or(Attr::Null, Attr::Num),
        Value::Bool(b) => Attr::Num(*b as u8 as f64),
        Value::String(s) => Attr::Text(s.clone()),
        other => Attr::Text(other.to_string()),
    }
}

fn json_geom(g: &Value, out: &mut Geom, depth: u32) -> Result<(), String> {
    if depth > 8 {
        return Err("geometry nested too deeply".into());
    }
    let kind = g.get("type").and_then(Value::as_str).ok_or("geometry without type")?;
    if kind == "GeometryCollection" {
        for part in g.get("geometries").and_then(Value::as_array).ok_or("GeometryCollection without geometries")? {
            json_geom(part, out, depth + 1)?;
        }
        return Ok(());
    }
    let c = g.get("coordinates").ok_or("geometry without coordinates")?;
    match kind {
        "Point" => out.points.push(pos(c)?),
        "MultiPoint" => out.points.extend(list(c, pos)?),
        "LineString" => out.lines.push(list(c, pos)?),
        "MultiLineString" => out.lines.extend(list(c, |l| list(l, pos))?),
        "Polygon" => out.polygons.push(list(c, |r| list(r, pos))?),
        "MultiPolygon" => out.polygons.extend(list(c, |p| list(p, |r| list(r, pos)))?),
        k => return Err(format!("unsupported geometry type {k}")),
    }
    Ok(())
}

fn list<T>(v: &Value, f: impl Fn(&Value) -> Result<T, String>) -> Result<Vec<T>, String> {
    v.as_array().ok_or("coordinates must be arrays")?.iter().map(f).collect()
}

fn pos(v: &Value) -> Result<P, String> {
    let a = v.as_array().ok_or("position must be an array")?;
    match (a.first().and_then(Value::as_f64), a.get(1).and_then(Value::as_f64)) {
        (Some(x), Some(y)) => Ok((x, y)),
        _ => Err("position needs two numbers".into()),
    }
}

// ---- Shapefile ----

fn read_shapefile(path: &Path) -> Result<(Vec<Feature>, Crs), String> {
    let mut crs = Crs::default();
    if let Ok(wkt) = std::fs::read_to_string(path.with_extension("prj")) {
        let head = wkt.trim_start().to_ascii_uppercase();
        crs.geographic = head.starts_with("GEOGCS") || head.starts_with("GEOGCRS");
        // Last AUTHORITY["EPSG","n"] belongs to the outermost CRS; ESRI-style .prj often has none.
        crs.epsg = head
            .rfind("AUTHORITY[\"EPSG\",\"")
            .map(|i| head[i + 18..].chars().take_while(char::is_ascii_digit).collect::<String>())
            .filter(|c| !c.is_empty());
    }
    let mut reader = shapefile::Reader::from_path(path).map_err(|e| format!("cannot read shapefile (.shp/.shx/.dbf): {e}"))?;
    let mut out = Vec::new();
    for item in reader.iter_shapes_and_records() {
        let (shape, record) = item.map_err(|e| format!("feature {}: {e}", out.len() + 1))?;
        let attrs = record.into_iter().map(|(k, v)| (k, dbf_attr(v))).collect();
        out.push(Feature { geom: shape_geom(&shape), attrs });
    }
    Ok((out, crs))
}

fn dbf_attr(v: FieldValue) -> Attr {
    match v {
        FieldValue::Character(Some(s)) | FieldValue::Memo(s) => Attr::Text(s),
        FieldValue::Numeric(Some(n)) | FieldValue::Double(n) | FieldValue::Currency(n) => Attr::Num(n),
        FieldValue::Float(Some(n)) => Attr::Num(n as f64),
        FieldValue::Integer(n) => Attr::Num(n as f64),
        FieldValue::Logical(Some(b)) => Attr::Num(b as u8 as f64),
        FieldValue::Date(Some(d)) => Attr::Text(d.to_string()),
        _ => Attr::Null,
    }
}

fn xy<T: HasXY>(pts: &[T]) -> Vec<P> {
    pts.iter().map(|p| (p.x(), p.y())).collect()
}

fn shape_geom(shape: &Shape) -> Geom {
    let mut g = Geom::default();
    macro_rules! poly {
        ($p:expr) => {
            g.polygons.push($p.rings().iter().map(|r| xy(r.points())).collect())
        };
    }
    match shape {
        Shape::Point(p) => g.points.push((p.x, p.y)),
        Shape::PointM(p) => g.points.push((p.x, p.y)),
        Shape::PointZ(p) => g.points.push((p.x, p.y)),
        Shape::Multipoint(m) => g.points.extend(xy(m.points())),
        Shape::MultipointM(m) => g.points.extend(xy(m.points())),
        Shape::MultipointZ(m) => g.points.extend(xy(m.points())),
        Shape::Polyline(l) => g.lines.extend(l.parts().iter().map(|p| xy(p))),
        Shape::PolylineM(l) => g.lines.extend(l.parts().iter().map(|p| xy(p))),
        Shape::PolylineZ(l) => g.lines.extend(l.parts().iter().map(|p| xy(p))),
        // One shapefile polygon record may hold several outer rings; even-odd filling handles them together.
        Shape::Polygon(p) => poly!(p),
        Shape::PolygonM(p) => poly!(p),
        Shape::PolygonZ(p) => poly!(p),
        Shape::NullShape | Shape::Multipatch(_) => {}
    }
    g
}

#[cfg(test)]
mod tests {
    use super::*;
    use mapdata::{EdgeKind, NodeId, Pos, RegionId, Terrain};
    use shapefile::dbase::{FieldName, Record, TableWriterBuilder};

    fn temp_dir(name: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("mapbake_{name}_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    const CRS: &str = r#""crs":{"type":"name","properties":{"name":"urn:ogc:def:crs:EPSG::32635"}}"#;

    fn fc(features: &str) -> String {
        format!(r#"{{"type":"FeatureCollection",{CRS},"features":[{features}]}}"#)
    }

    #[test]
    fn geojson_folder_bakes() {
        let d = temp_dir("geojson");
        let (x, y) = (500_000.0, 5_000_000.0);
        std::fs::write(
            d.join("terrain.geojson"),
            fc(&format!(
                r#"{{"type":"Feature","properties":{{"kind":"forest"}},"geometry":{{"type":"MultiPolygon","coordinates":[[[[{x},{y}],[{},{y}],[{},{}],[{x},{}],[{x},{y}]]]]}}}}"#,
                x + 5000.0,
                x + 5000.0,
                y + 5000.0,
                y + 5000.0
            )),
        )
        .unwrap();
        std::fs::write(
            d.join("nodes.geojson"),
            fc(&format!(
                r#"{{"type":"Feature","properties":{{"name":"A","base_side":0}},"geometry":{{"type":"Point","coordinates":[{x},{y}]}}}},
                   {{"type":"Feature","properties":{{"name":"B","depot_side":0,"kind":null}},"geometry":{{"type":"Point","coordinates":[{},{}]}}}}"#,
                x + 20_000.0,
                y + 10_000.0
            )),
        )
        .unwrap();
        std::fs::write(d.join("notes.geojson"), "ignored, not a layer name").unwrap();
        let b = bake_folder(&d).unwrap();
        assert_eq!((b.map.terrain.width, b.map.terrain.height), (21, 11));
        assert_eq!(b.map.terrain.get(2, 2), Terrain::Forest);
        assert_eq!(b.map.graph.nodes[1].pos, Pos { x: 20_000.0, y: 10_000.0 });
        assert_eq!(b.scenario.bases, vec![(0, NodeId(0))]);
        assert_eq!(b.map.region_at(0, 0), RegionId(0));
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn geojson_in_degrees_is_rejected() {
        let d = temp_dir("degrees");
        let plain = r#"{"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[27.1,47.5]}}]}"#;
        std::fs::write(d.join("nodes.geojson"), plain).unwrap();
        assert!(bake_folder(&d).unwrap_err().contains("geographic"));
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn duplicate_layer_files_and_empty_folders_are_errors() {
        let d = temp_dir("dupes");
        assert!(bake_folder(&d).unwrap_err().contains("no layer files"));
        std::fs::write(d.join("rail.geojson"), fc("")).unwrap();
        std::fs::write(d.join("RAIL.json"), fc("")).unwrap();
        assert!(bake_folder(&d).unwrap_err().contains("two files"));
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn shapefile_folder_bakes_with_attributes() {
        let d = temp_dir("shp");
        let (x, y) = (300_000.0, 5_300_000.0);
        let p = |dx: f64, dy: f64| shapefile::Point::new(x + dx, y + dy);
        // Rail line with gauge + capacity in the .dbf.
        let table = TableWriterBuilder::new()
            .add_numeric_field(FieldName::try_from("gauge").unwrap(), 4, 0)
            .add_numeric_field(FieldName::try_from("capacity").unwrap(), 10, 1);
        let mut w = shapefile::Writer::from_path(d.join("rail.shp"), table).unwrap();
        let mut rec = Record::default();
        rec.insert("gauge".into(), FieldValue::Numeric(Some(1.0)));
        rec.insert("capacity".into(), FieldValue::Numeric(Some(2500.0)));
        w.write_shape_and_record(&shapefile::Polyline::new(vec![p(0.0, 0.0), p(30_000.0, 0.0), p(30_000.0, 40_000.0)]), &rec)
            .unwrap();
        drop(w);
        std::fs::write(d.join("rail.prj"), r#"PROJCS["WGS 84 / UTM zone 35N",GEOGCS["WGS 84"],AUTHORITY["EPSG","32635"]]"#).unwrap();
        let b = bake_folder(&d).unwrap();
        let e = &b.map.graph.edges[0];
        assert_eq!(e.kind, EdgeKind::Rail { gauge: 1 });
        assert_eq!(e.capacity_t_per_day, 2500.0);
        assert!((e.length_m - 70_000.0).abs() < 1.0);
        assert_eq!(b.map.graph.nodes.len(), 2, "both ends become junctions");

        std::fs::write(d.join("rail.prj"), r#"GEOGCS["WGS 84",DATUM["WGS_1984"]]"#).unwrap();
        assert!(bake_folder(&d).unwrap_err().contains("geographic"));
        std::fs::remove_dir_all(&d).unwrap();
    }
}
