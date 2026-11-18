#include "mapbake/mapbake.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>

#include <sqlite3.h>

namespace mapbake {

std::string to_ascii_lowercase(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
    }
    return out;
}

std::string to_ascii_uppercase(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = char(c - 'a' + 'A');
        }
    }
    return out;
}

bool eq_ignore_ascii_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::string_view trim_start(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0) {
        ++i;
    }
    return s.substr(i);
}

std::string_view trim(std::string_view s) {
    s = trim_start(s);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// ---------- Feature ----------

const Attr* Feature::get(std::string_view key) const {
    for (const auto& [k, v] : attrs) {
        if (eq_ignore_ascii_case(k, key)) {
            return &v;
        }
    }
    return nullptr;
}

std::optional<std::string> Feature::text(std::string_view key) const {
    const Attr* a = get(key);
    if (!a) {
        return std::nullopt;
    }
    if (a->kind == AttrKind::Text) {
        const std::string_view t = trim(a->text_val);
        if (!t.empty()) {
            return std::string(t);
        }
        return std::nullopt;
    }
    if (a->kind == AttrKind::Num) {
        return std::format("{}", a->num_val);
    }
    return std::nullopt;
}

std::optional<double> Feature::num(std::string_view key) const {
    const Attr* a = get(key);
    if (!a || a->kind == AttrKind::Null) {
        return std::nullopt;
    }
    if (a->kind == AttrKind::Num) {
        return a->num_val;
    }
    const std::string_view t = trim(a->text_val);
    if (t.empty()) {
        return std::nullopt;
    }
    double v = 0.0;
    const auto [ptr, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (ec != std::errc{} || ptr != t.data() + t.size()) {
        throw std::runtime_error(std::format("field {}: '{}' is not a number", key, a->text_val));
    }
    return v;
}

std::optional<std::uint8_t> Feature::side(std::string_view key) const {
    const auto val = num(key);
    if (!val) {
        return std::nullopt;
    }
    const double n = *val;
    if (std::isfinite(n) && std::trunc(n) == n && n >= 0.0 && n < 8.0) {
        return std::uint8_t(n);
    }
    throw std::runtime_error(std::format("field {}: side {} must be a whole number 0..7", key, n));
}

namespace {

class SqliteConn {
public:
    explicit SqliteConn(sqlite3* db) : db_(db) {}
    ~SqliteConn() {
        if (db_) {
            sqlite3_close(db_);
        }
    }
    SqliteConn(const SqliteConn&) = delete;
    SqliteConn& operator=(const SqliteConn&) = delete;
    sqlite3* get() const { return db_; }

private:
    sqlite3* db_;
};

class SqliteStmt {
public:
    explicit SqliteStmt(sqlite3_stmt* stmt) : stmt_(stmt) {}
    ~SqliteStmt() {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
    }
    SqliteStmt(const SqliteStmt&) = delete;
    SqliteStmt& operator=(const SqliteStmt&) = delete;
    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3_stmt* stmt_;
};

std::string quote(std::string_view name) {
    std::string out = "\"";
    for (const char c : name) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

std::string col_text(sqlite3_stmt* stmt, int col) {
    const unsigned char* t = sqlite3_column_text(stmt, col);
    const int n = sqlite3_column_bytes(stmt, col);
    if (!t || n <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(t), std::size_t(n));
}

void check_projected(sqlite3* conn, std::int64_t srs) {
    if (srs == 0) {
        throw std::runtime_error("layers have an undefined geographic CRS; set a projected metric CRS in QGIS");
    }
    if (srs == -1) {
        return;  // undefined Cartesian: taken as meters
    }
    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(conn, "SELECT definition FROM gpkg_spatial_ref_sys WHERE srs_id = ?1", -1, &raw_stmt,
                           nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::format("CRS {} missing from gpkg_spatial_ref_sys", srs));
    }
    const SqliteStmt stmt(raw_stmt);
    sqlite3_bind_int64(stmt.get(), 1, srs);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error(std::format("CRS {} missing from gpkg_spatial_ref_sys", srs));
    }
    const std::string def = col_text(stmt.get(), 0);
    const std::string head = to_ascii_uppercase(trim_start(def));
    if (srs == 4326 || head.starts_with("GEOGCS") || head.starts_with("GEOGCRS") || head.starts_with("GEODCRS")) {
        throw std::runtime_error(std::format(
            "CRS {} is geographic (degrees). In QGIS, export the layers to a projected metric CRS (e.g. a UTM zone) first",
            srs));
    }
}

// Reads every known feature layer. Returns (lowercase layer name, features).
std::vector<LayerFeatures> read_layers(sqlite3* conn) {
    const auto sql_err = [conn]() { return std::string("GeoPackage read error: ") + sqlite3_errmsg(conn); };

    sqlite3_stmt* raw_cols = nullptr;
    if (sqlite3_prepare_v2(conn, "SELECT table_name, column_name, srs_id FROM gpkg_geometry_columns", -1, &raw_cols,
                           nullptr) != SQLITE_OK) {
        throw std::runtime_error(std::string("not a GeoPackage (") + sqlite3_errmsg(conn) + ")");
    }
    const SqliteStmt cols_stmt(raw_cols);

    struct ColRow {
        std::string table;
        std::string geom_col;
        std::int64_t srs = 0;
    };
    std::vector<ColRow> cols;
    while (true) {
        const int rc = sqlite3_step(cols_stmt.get());
        if (rc == SQLITE_ROW) {
            cols.push_back(ColRow{
                col_text(cols_stmt.get(), 0),
                col_text(cols_stmt.get(), 1),
                sqlite3_column_int64(cols_stmt.get(), 2),
            });
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            throw std::runtime_error(sql_err());
        }
    }

    std::optional<std::int64_t> srs_seen;
    std::vector<LayerFeatures> out;
    for (const auto& [table, geom_col, srs] : cols) {
        const std::string lower = to_ascii_lowercase(table);
        if (std::ranges::find(LAYERS, lower) == LAYERS.end()) {
            continue;
        }
        if (srs_seen && *srs_seen != srs) {
            throw std::runtime_error(
                std::format("layer {} uses CRS {}, others use {}: reproject all layers to one CRS", table, srs, *srs_seen));
        }
        srs_seen = srs;

        const std::string query = "SELECT * FROM " + quote(table);
        sqlite3_stmt* raw_q = nullptr;
        if (sqlite3_prepare_v2(conn, query.c_str(), -1, &raw_q, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sql_err());
        }
        const SqliteStmt stmt(raw_q);
        const int col_count = sqlite3_column_count(stmt.get());
        std::vector<std::string> names;
        names.reserve(std::size_t(col_count));
        std::optional<int> gi;
        for (int i = 0; i < col_count; ++i) {
            const char* n = sqlite3_column_name(stmt.get(), i);
            names.emplace_back(n ? n : "");
            if (!gi && eq_ignore_ascii_case(names.back(), geom_col)) {
                gi = i;
            }
        }
        if (!gi) {
            throw std::runtime_error("layer " + table + ": no geometry column");
        }

        std::vector<Feature> feats;
        while (true) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) {
                break;
            }
            if (rc != SQLITE_ROW) {
                throw std::runtime_error(sql_err());
            }
            Geom geom;
            if (sqlite3_column_type(stmt.get(), *gi) == SQLITE_BLOB) {
                const auto* blob_ptr = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt.get(), *gi));
                const int blob_len = sqlite3_column_bytes(stmt.get(), *gi);
                const std::span<const std::uint8_t> span =
                    (blob_ptr && blob_len > 0) ? std::span<const std::uint8_t>(blob_ptr, std::size_t(blob_len))
                                               : std::span<const std::uint8_t>{};
                try {
                    geom = parse_gpb(span);
                } catch (const std::exception& e) {
                    throw std::runtime_error(
                        std::format("layer {}, feature {}: {}", table, feats.size() + 1, e.what()));
                }
            }
            std::vector<std::pair<std::string, Attr>> attrs;
            for (int i = 0; i < col_count; ++i) {
                if (i == *gi) {
                    continue;
                }
                Attr v = Attr::null();
                switch (sqlite3_column_type(stmt.get(), i)) {
                    case SQLITE_INTEGER:
                        v = Attr::num(double(sqlite3_column_int64(stmt.get(), i)));
                        break;
                    case SQLITE_FLOAT:
                        v = Attr::num(sqlite3_column_double(stmt.get(), i));
                        break;
                    case SQLITE_TEXT:
                        v = Attr::text(col_text(stmt.get(), i));
                        break;
                    default:
                        v = Attr::null();
                        break;
                }
                attrs.emplace_back(names[std::size_t(i)], std::move(v));
            }
            feats.push_back(Feature{std::move(geom), std::move(attrs)});
        }
        out.emplace_back(lower, std::move(feats));
    }
    if (out.empty()) {
        throw std::runtime_error(
            "no known layers found; expected some of: terrain, regions, nodes, rail, roads, rivers, units");
    }
    if (srs_seen) {
        check_projected(conn, *srs_seen);
    }
    return out;
}

}  // namespace

mapdata::Baked bake_path(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return bake_folder(path);
    }
    std::string ext = path.has_extension() ? path_to_utf8(path.extension()) : std::string{};
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(ext.begin());
    }
    ext = to_ascii_lowercase(ext);
    if (ext == "geojson" || ext == "json" || ext == "shp" || ext == "dbf" || ext == "shx" || ext == "prj" ||
        ext == "cpg") {
        const auto parent = path.parent_path();
        return bake_folder(parent.empty() ? std::filesystem::path(".") : parent);
    }
    return bake_file(path);
}

mapdata::Baked bake_file(const std::filesystem::path& path) {
    const std::string utf8 = path_to_utf8(path);
    sqlite3* raw_db = nullptr;
    const int rc = sqlite3_open_v2(utf8.c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr);
    const SqliteConn conn(raw_db);
    if (rc != SQLITE_OK || !raw_db) {
        const std::string msg = raw_db ? sqlite3_errmsg(raw_db) : "sqlite3_open_v2 failed";
        throw std::runtime_error(std::format("cannot open {}: {}", utf8, msg));
    }
    const std::string name = path.has_stem() ? path_to_utf8(path.stem()) : "map";
    return bake(conn.get(), name);
}

mapdata::Baked bake(sqlite3* conn, std::string_view name) {
    const auto layers = read_layers(conn);
    return bake_layers(layers, name);
}

mapdata::Baked bake_layers(std::span<const LayerFeatures> layers, std::string_view name) {
    const auto layer = [&](std::string_view n) -> std::span<const Feature> {
        for (const auto& [l, f] : layers) {
            if (l == n) {
                return f;
            }
        }
        return {};
    };

    // Extent over every feature, snapped to whole cells.
    P lo{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    P hi{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const auto& [_, feats] : layers) {
        for (const Feature& f : feats) {
            for (const P& p : f.geom.all_points()) {
                if (!(std::isfinite(p.first) && std::isfinite(p.second))) {
                    throw std::runtime_error("geometry with non-finite coordinates");
                }
                lo.first = std::min(lo.first, p.first);
                lo.second = std::min(lo.second, p.second);
                hi.first = std::max(hi.first, p.first);
                hi.second = std::max(hi.second, p.second);
            }
        }
    }
    if (!std::isfinite(lo.first)) {
        throw std::runtime_error("the layers contain no geometry");
    }
    const P origin{std::floor(lo.first / CELL_M) * CELL_M, std::floor(lo.second / CELL_M) * CELL_M};
    const auto w_u64 = std::uint64_t(std::floor((hi.first - origin.first) / CELL_M)) + 1;
    const auto h_u64 = std::uint64_t(std::floor((hi.second - origin.second) / CELL_M)) + 1;
    if (w_u64 > MAX_CELLS_PER_SIDE || h_u64 > MAX_CELLS_PER_SIDE) {
        throw std::runtime_error(std::format(
            "map is {} x {} km; the limit is {} km per side (is the CRS in meters?)", w_u64, h_u64, MAX_CELLS_PER_SIDE));
    }
    const auto w = std::uint32_t(w_u64);
    const auto h = std::uint32_t(h_u64);
    const auto cell = [&](P p) -> P { return {(p.first - origin.first) / CELL_M, (p.second - origin.second) / CELL_M}; };
    const auto meters = [&](P p) -> mapdata::Pos {
        return mapdata::Pos{float(p.first - origin.first), float(p.second - origin.second)};
    };
    const auto to_cells = [&](const std::vector<std::vector<P>>& rings) {
        std::vector<std::vector<P>> out;
        out.reserve(rings.size());
        for (const auto& r : rings) {
            std::vector<P> cr;
            cr.reserve(r.size());
            for (const P& p : r) {
                cr.push_back(cell(p));
            }
            out.push_back(std::move(cr));
        }
        return out;
    };

    // Terrain: later kinds win (water last).
    mapdata::TerrainGrid terrain = mapdata::TerrainGrid::filled(w, h, float(CELL_M), mapdata::Terrain::Plains);
    constexpr std::array<std::pair<std::string_view, mapdata::Terrain>, 6> kinds{{
        {"plains", mapdata::Terrain::Plains},
        {"forest", mapdata::Terrain::Forest},
        {"marsh", mapdata::Terrain::Marsh},
        {"urban", mapdata::Terrain::Urban},
        {"mountain", mapdata::Terrain::Mountain},
        {"water", mapdata::Terrain::Water},
    }};
    const auto terrain_feats = layer("terrain");
    for (std::size_t i = 0; i < terrain_feats.size(); ++i) {
        const std::string k = to_ascii_lowercase(terrain_feats[i].text("kind").value_or("plains"));
        const bool known = std::ranges::any_of(kinds, [&](const auto& kv) { return kv.first == k; });
        if (!known) {
            throw std::runtime_error(std::format(
                "terrain feature {}: unknown kind '{}' (use plains, forest, marsh, mountain, urban, water)", i + 1, k));
        }
    }
    for (const auto& [kname, t] : kinds) {
        for (const Feature& f : terrain_feats) {
            const std::string k = f.text("kind").value_or("plains");
            if (!eq_ignore_ascii_case(k, kname)) {
                continue;
            }
            for (const auto& poly : f.geom.polygons) {
                const auto cell_rings = to_cells(poly);
                raster_polygon(cell_rings, w, h, [&](std::uint32_t x, std::uint32_t y) { terrain.set(x, y, t); });
            }
        }
    }

    // Regions; cells outside every region polygon go to an extra "Unassigned" region.
    std::vector<mapdata::Region> regions;
    std::vector<std::uint8_t> legal;
    std::vector<std::uint8_t> control;
    constexpr std::uint16_t kUnassigned = std::numeric_limits<std::uint16_t>::max();
    std::vector<std::uint16_t> region_cells(std::size_t(w) * h, kUnassigned);
    const auto region_feats = layer("regions");
    for (std::size_t i = 0; i < region_feats.size(); ++i) {
        if (i >= std::size_t(kUnassigned) - 1) {
            throw std::runtime_error("too many regions");
        }
        const Feature& f = region_feats[i];
        constexpr std::array<std::string_view, 4> res_keys{"fuel", "ammo", "food", "equipment"};
        std::array<float, 4> daily_output{};
        for (std::size_t k = 0; k < 4; ++k) {
            try {
                const double val = f.num(res_keys[k]).value_or(0.0);
                daily_output[k] = float(std::max(val, 0.0));
            } catch (const std::exception& e) {
                throw std::runtime_error(std::format("region {}: {}", i + 1, e.what()));
            }
        }
        std::uint8_t side = 0;
        std::uint8_t held = 0;
        try {
            side = f.side("side").value_or(0);
            held = f.side("control").value_or(side);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::format("region {}: {}", i + 1, e.what()));
        }
        regions.push_back(mapdata::Region{
            f.text("name").value_or(std::format("Region {}", i + 1)),
            daily_output,
        });
        legal.push_back(side);
        control.push_back(held);
        for (const auto& poly : f.geom.polygons) {
            const auto cell_rings = to_cells(poly);
            raster_polygon(cell_rings, w, h, [&](std::uint32_t x, std::uint32_t y) {
                region_cells[std::size_t(y) * w + x] = std::uint16_t(i);
            });
        }
    }
    if (std::ranges::find(region_cells, kUnassigned) != region_cells.end()) {
        const auto r = std::uint16_t(regions.size());
        regions.push_back(mapdata::Region{
            r == 0 ? std::string(name) : std::string("Unassigned"),
            {0.0f, 0.0f, 0.0f, 0.0f},
        });
        legal.push_back(0);
        control.push_back(0);
        for (std::uint16_t& c : region_cells) {
            if (c == kUnassigned) {
                c = r;
            }
        }
    }

    // Nodes, bases, depots.
    std::vector<mapdata::Node> nodes;
    std::vector<P> raw;  // node positions in CRS units, for snapping
    std::vector<mapdata::SideNode> bases;
    std::vector<mapdata::SideNode> depots;
    const auto node_feats = layer("nodes");
    for (std::size_t i = 0; i < node_feats.size(); ++i) {
        const Feature& f = node_feats[i];
        const auto err = [&](std::string_view msg) { return std::format("node {}: {}", i + 1, msg); };
        mapdata::NodeKind kind = mapdata::NodeKind::Junction;
        if (const auto k_opt = f.text("kind")) {
            const std::string k = to_ascii_lowercase(*k_opt);
            if (k == "junction") {
                kind = mapdata::NodeKind::Junction;
            } else if (k == "station") {
                kind = mapdata::NodeKind::Station;
            } else if (k == "port") {
                kind = mapdata::NodeKind::Port;
            } else {
                throw std::runtime_error(err(std::format("unknown kind '{}' (use junction, station, port)", k)));
            }
        }
        for (const P& p : f.geom.points) {
            const mapdata::NodeId id{std::uint32_t(nodes.size())};
            nodes.push_back(mapdata::Node{meters(p), kind, f.text("name")});
            raw.push_back(p);
            try {
                if (const auto s = f.side("base_side")) {
                    bases.push_back(mapdata::SideNode{*s, id});
                }
                if (const auto s = f.side("depot_side")) {
                    depots.push_back(mapdata::SideNode{*s, id});
                }
            } catch (const std::exception& e) {
                throw std::runtime_error(err(e.what()));
            }
        }
    }

    // Transport lines.
    std::vector<mapdata::Edge> edges;
    constexpr std::array<std::pair<std::string_view, double>, 3> line_layers{{
        {"rail", 3000.0},
        {"roads", 600.0},
        {"rivers", 1500.0},
    }};
    for (const auto& [layer_name, default_cap] : line_layers) {
        const auto feats = layer(layer_name);
        for (std::size_t i = 0; i < feats.size(); ++i) {
            const Feature& f = feats[i];
            const auto err = [&](std::string_view msg) {
                return std::format("{} feature {}: {}", layer_name, i + 1, msg);
            };
            mapdata::EdgeKind kind = mapdata::EdgeKind::Road;
            std::uint8_t gauge = 0;
            if (layer_name == "rail") {
                double g = 0.0;
                try {
                    g = f.num("gauge").value_or(0.0);
                } catch (const std::exception& e) {
                    throw std::runtime_error(err(e.what()));
                }
                if (!std::isfinite(g) || g < 0.0 || g > 255.0 || std::trunc(g) != g) {
                    throw std::runtime_error(err(std::format("gauge {} must be a whole number 0..255", g)));
                }
                kind = mapdata::EdgeKind::Rail;
                gauge = std::uint8_t(g);
            } else if (layer_name == "roads") {
                kind = mapdata::EdgeKind::Road;
            } else {
                kind = mapdata::EdgeKind::River;
            }

            double capacity = default_cap;
            try {
                capacity = f.num("capacity").value_or(default_cap);
            } catch (const std::exception& e) {
                throw std::runtime_error(err(e.what()));
            }
            if (!(std::isfinite(capacity) && capacity > 0.0)) {
                throw std::runtime_error(err(std::format("capacity {} must be positive", capacity)));
            }

            for (const auto& line : f.geom.lines) {
                if (line.size() < 2) {
                    continue;
                }
                const auto snap = [&](P p) -> mapdata::NodeId {
                    std::optional<std::size_t> best_i;
                    double best_d = std::numeric_limits<double>::infinity();
                    for (std::size_t idx = 0; idx < raw.size(); ++idx) {
                        const double d = std::hypot(raw[idx].first - p.first, raw[idx].second - p.second);
                        if (d <= SNAP_M && d < best_d) {
                            best_d = d;
                            best_i = idx;
                        }
                    }
                    if (best_i) {
                        return mapdata::NodeId{std::uint32_t(*best_i)};
                    }
                    nodes.push_back(mapdata::Node{meters(p), mapdata::NodeKind::Junction, std::nullopt});
                    raw.push_back(p);
                    return mapdata::NodeId{std::uint32_t(nodes.size() - 1)};
                };
                const mapdata::NodeId a = snap(line.front());
                const mapdata::NodeId b = snap(line.back());
                if (a == b) {
                    continue;  // loop or too short to connect two nodes
                }
                double length = 0.0;
                for (std::size_t k = 0; k + 1 < line.size(); ++k) {
                    length += std::hypot(line[k + 1].first - line[k].first, line[k + 1].second - line[k].second);
                }
                edges.push_back(mapdata::Edge{
                    a,
                    b,
                    kind,
                    gauge,
                    float(std::max(length, 1.0)),
                    float(capacity),
                });
            }
        }
    }

    // Units.
    std::vector<mapdata::UnitSpec> units;
    const auto unit_feats = layer("units");
    for (std::size_t i = 0; i < unit_feats.size(); ++i) {
        const Feature& f = unit_feats[i];
        std::uint8_t side = 0;
        try {
            side = f.side("side").value_or(0);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::format("unit {}: {}", i + 1, e.what()));
        }
        for (const P& p : f.geom.points) {
            units.push_back(mapdata::UnitSpec{
                side,
                f.text("name").value_or(std::format("Unit {}", units.size() + 1)),
                meters(p),
            });
        }
    }

    std::vector<mapdata::RegionId> rc;
    rc.reserve(region_cells.size());
    for (const std::uint16_t r : region_cells) {
        rc.push_back(mapdata::RegionId{r});
    }

    mapdata::Baked baked{
        mapdata::MapData{
            std::string(name),
            std::move(terrain),
            mapdata::TransportGraph{std::move(nodes), std::move(edges)},
            std::move(regions),
            std::move(rc),
        },
        mapdata::ScenarioData{
            std::move(legal),
            std::move(control),
            std::move(bases),
            std::move(depots),
            std::move(units),
        },
    };
    try {
        baked.validate();
    } catch (const mapdata::MapError& e) {
        throw std::runtime_error(std::string("baked map is invalid: ") + e.what());
    }
    return baked;
}

}  // namespace mapbake
