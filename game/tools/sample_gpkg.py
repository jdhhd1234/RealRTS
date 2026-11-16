"""Writes samples/sample.gpkg and samples/geojson/*.geojson: a small fictional map that follows the RealRTS layer contract
(see crates/mapbake/src/lib.rs). Open it in QGIS as a template for your own maps.

Standard library only. Run from game/:  python tools/sample_gpkg.py
CRS: EPSG:32635 (WGS 84 / UTM zone 35N), meters. The geography is invented.
"""
import json
import os
import sqlite3
import struct

SRS = 32635
UTM35N = (
    'PROJCS["WGS 84 / UTM zone 35N",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],'
    'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],PROJECTION["Transverse_Mercator"],'
    'PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",27],PARAMETER["scale_factor",0.9996],'
    'PARAMETER["false_easting",500000],PARAMETER["false_northing",0],UNIT["metre",1],AUTHORITY["EPSG","32635"]]'
)
OX, OY = 300_000.0, 5_350_000.0  # map origin (south-west corner), meters


def p(x_km, y_km):
    return (OX + x_km * 1000.0, OY + y_km * 1000.0)


def gpb(wkb):
    # GeoPackage binary header: magic, version 0, flags (little-endian, no envelope), srs id.
    return b"GP" + bytes([0, 1]) + struct.pack("<i", SRS) + wkb


# Each geometry helper returns (GeoPackage blob, GeoJSON geometry).
def point(x, y):
    return gpb(struct.pack("<BIdd", 1, 1, *p(x, y))), {"type": "Point", "coordinates": list(p(x, y))}


def line(*pts):
    body = struct.pack("<BII", 1, 2, len(pts))
    for x, y in pts:
        body += struct.pack("<dd", *p(x, y))
    return gpb(body), {"type": "LineString", "coordinates": [list(p(x, y)) for x, y in pts]}


def polygon(*rings):
    body = struct.pack("<BII", 1, 3, len(rings))
    closed = [list(ring) + [ring[0]] for ring in rings]
    for ring in closed:
        body += struct.pack("<I", len(ring))
        for x, y in ring:
            body += struct.pack("<dd", *p(x, y))
    return gpb(body), {"type": "Polygon", "coordinates": [[list(p(x, y)) for x, y in r] for r in closed]}


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


LAYERS = {
    "terrain": ("POLYGON", [("kind", "TEXT")], [
        (polygon(rect(40, 0, 70, 150)), ["forest"]),
        (polygon([(118, 0), (124, 0), (128, 75), (122, 150), (116, 150), (122, 75)]), ["water"]),
        (polygon(rect(150, 60, 165, 80)), ["urban"]),
        (polygon(rect(170, 110, 220, 150)), ["mountain"]),
        (polygon(rect(80, 10, 100, 40)), ["marsh"]),
    ]),
    "regions": ("POLYGON", [("name", "TEXT"), ("side", "INTEGER"), ("control", "INTEGER"),
                            ("fuel", "REAL"), ("ammo", "REAL"), ("food", "REAL"), ("equipment", "REAL")], [
        (polygon(rect(0, 0, 110, 75)), ["Westmark South", 0, 0, 40, 20, 60, 10]),
        (polygon(rect(0, 75, 110, 150)), ["Westmark North", 0, 0, 0, 30, 50, 20]),
        (polygon(rect(110, 0, 220, 75)), ["Ostland South", 1, 1, 30, 20, 50, 10]),
        (polygon(rect(110, 75, 220, 150)), ["Ostland North", 1, 0, 0, 20, 40, 20]),  # held by Blue at start
    ]),
    "nodes": ("POINT", [("name", "TEXT"), ("kind", "TEXT"), ("base_side", "INTEGER"), ("depot_side", "INTEGER")], [
        (point(8, 70), ["Westport", "port", 0, None]),
        (point(85, 72), ["Kreuzberg Junction", "junction", None, 0]),
        (point(157, 70), ["Ostburg", "station", None, None]),
        (point(212, 68), ["Ostbahnhof", "station", 1, 1]),
        (point(140, 120), ["Hochdorf", "junction", None, 0]),
    ]),
    "rail": ("LINESTRING", [("gauge", "INTEGER"), ("capacity", "REAL")], [
        (line((8, 70), (50, 74), (85, 72)), [0, 3000]),
        (line((85, 72), (120, 73), (157, 70)), [0, 3000]),
        (line((157, 70), (190, 66), (212, 68)), [1, 2500]),  # gauge break at Ostburg
    ]),
    "roads": ("LINESTRING", [("capacity", "REAL")], [
        (line((85, 72), (110, 100), (140, 120)), [600]),
        (line((140, 120), (175, 100), (157, 70)), [500]),
    ]),
    "rivers": ("LINESTRING", [("capacity", "REAL")], [
        (line((121, 5), (125, 75), (119, 145)), [1500]),
    ]),
    "units": ("POINT", [("name", "TEXT"), ("side", "INTEGER")], [
        (point(95, 60), ["1st Infantry Division", 0]),
        (point(100, 85), ["3rd Infantry Division", 0]),
        (point(138, 115), ["5th Infantry Division", 0]),
        (point(175, 62), ["Ostland 1st Division", 1]),
        (point(190, 80), ["Ostland 2nd Division", 1]),
    ]),
}


def main():
    out = os.path.join(os.path.dirname(__file__), "..", "samples", "sample.gpkg")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.remove(out)
    db = sqlite3.connect(out)
    db.execute("PRAGMA application_id = 1196444487")  # 'GPKG'
    db.execute("PRAGMA user_version = 10300")
    db.executescript("""
        CREATE TABLE gpkg_spatial_ref_sys (srs_name TEXT NOT NULL, srs_id INTEGER PRIMARY KEY,
            organization TEXT NOT NULL, organization_coordsys_id INTEGER NOT NULL, definition TEXT NOT NULL,
            description TEXT);
        CREATE TABLE gpkg_contents (table_name TEXT PRIMARY KEY, data_type TEXT NOT NULL, identifier TEXT UNIQUE,
            description TEXT DEFAULT '', last_change DATETIME NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
            min_x DOUBLE, min_y DOUBLE, max_x DOUBLE, max_y DOUBLE, srs_id INTEGER);
        CREATE TABLE gpkg_geometry_columns (table_name TEXT NOT NULL, column_name TEXT NOT NULL,
            geometry_type_name TEXT NOT NULL, srs_id INTEGER NOT NULL, z TINYINT NOT NULL, m TINYINT NOT NULL,
            PRIMARY KEY (table_name, column_name));
    """)
    db.executemany("INSERT INTO gpkg_spatial_ref_sys VALUES (?,?,?,?,?,?)", [
        ("Undefined cartesian SRS", -1, "NONE", -1, "undefined", None),
        ("Undefined geographic SRS", 0, "NONE", 0, "undefined", None),
        ("WGS 84 geodetic", 4326, "EPSG", 4326,
         'GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],'
         'UNIT["degree",0.0174532925199433]]', None),
        ("WGS 84 / UTM zone 35N", SRS, "EPSG", SRS, UTM35N, None),
    ])
    (x0, y0), (x1, y1) = p(0, 0), p(220, 150)
    for name, (gtype, cols, rows) in LAYERS.items():
        coldefs = "".join(f", {c} {t}" for c, t in cols)
        db.execute(f"CREATE TABLE {name} (fid INTEGER PRIMARY KEY AUTOINCREMENT, geom {gtype}{coldefs})")
        db.execute("INSERT INTO gpkg_contents (table_name, data_type, identifier, min_x, min_y, max_x, max_y, srs_id) "
                   "VALUES (?, 'features', ?, ?, ?, ?, ?, ?)", (name, name, x0, y0, x1, y1, SRS))
        db.execute("INSERT INTO gpkg_geometry_columns VALUES (?, 'geom', ?, ?, 0, 0)", (name, gtype, SRS))
        names = ", ".join(["geom"] + [c for c, _ in cols])
        marks = ", ".join("?" * (len(cols) + 1))
        db.executemany(f"INSERT INTO {name} ({names}) VALUES ({marks})", [[g[0]] + vals for g, vals in rows])
    db.commit()
    db.close()
    print("wrote", os.path.normpath(out))

    # Same map as one GeoJSON file per layer, with the legacy "crs" member QGIS writes for projected exports.
    folder = os.path.join(os.path.dirname(out), "geojson")
    os.makedirs(folder, exist_ok=True)
    for name, (_, cols, rows) in LAYERS.items():
        fc = {
            "type": "FeatureCollection",
            "crs": {"type": "name", "properties": {"name": f"urn:ogc:def:crs:EPSG::{SRS}"}},
            "features": [
                {"type": "Feature", "properties": dict(zip([c for c, _ in cols], vals)), "geometry": g[1]}
                for g, vals in rows
            ],
        }
        with open(os.path.join(folder, f"{name}.geojson"), "w", encoding="utf-8") as f:
            json.dump(fc, f)
    print("wrote", os.path.normpath(folder))


if __name__ == "__main__":
    main()
