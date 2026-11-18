#include "mapbake/folder.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <nlohmann/json.hpp>
#include <shapefil.h>

#include "mapbake/mapbake.hpp"

namespace mapbake {

// Isolated .cpg code page -> UTF-8 conversion function.
std::string decode_cpg(std::string_view bytes, std::string_view cpg) {
    if (bytes.empty()) {
        return {};
    }
    const std::string spec = to_ascii_uppercase(trim(cpg));
    if (spec.empty() || spec == "UTF-8" || spec == "UTF8" || spec == "65001" || spec == "CP65001" ||
        spec == "UTF_8") {
        return std::string(bytes);
    }

    unsigned int code_page = 0;
    if (spec == "CP949" || spec == "949" || spec == "EUC-KR" || spec == "EUCKR" || spec == "EUC_KR" ||
        spec == "KS_C_5601-1987" || spec == "51949" || spec == "LDID/121") {
        code_page = 949;
    } else if (spec == "CP1252" || spec == "1252" || spec == "WINDOWS-1252" || spec == "ANSI" ||
               spec == "LDID/87") {
        code_page = 1252;
    } else if (spec == "ISO-8859-1" || spec == "ISO8859-1" || spec == "ISO_8859_1" || spec == "LATIN1" ||
               spec == "28591") {
        code_page = 28591;
    } else if (spec == "CP932" || spec == "932" || spec == "SHIFT_JIS" || spec == "SHIFT-JIS" || spec == "SJIS") {
        code_page = 932;
    } else if (spec == "CP936" || spec == "936" || spec == "GBK" || spec == "GB2312") {
        code_page = 936;
    } else if (spec == "CP950" || spec == "950" || spec == "BIG5") {
        code_page = 950;
    } else {
        std::string_view num_part = spec;
        if (num_part.starts_with("CP")) {
            num_part.remove_prefix(2);
        } else if (num_part.starts_with("WINDOWS-")) {
            num_part.remove_prefix(8);
        }
        unsigned int parsed = 0;
        const auto [ptr, ec] = std::from_chars(num_part.data(), num_part.data() + num_part.size(), parsed);
        if (ec == std::errc{} && ptr == num_part.data() + num_part.size()) {
            code_page = parsed;
        }
    }

    if (code_page == 0 || code_page == 65001) {
        return std::string(bytes);
    }

#ifdef _WIN32
    const int wide_len = MultiByteToWideChar(code_page, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    if (wide_len <= 0) {
        return std::string(bytes);
    }
    std::wstring wide(std::size_t(wide_len), L'\0');
    MultiByteToWideChar(code_page, 0, bytes.data(), int(bytes.size()), wide.data(), wide_len);

    const int utf8_len =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) {
        return std::string(bytes);
    }
    std::string utf8(std::size_t(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, utf8.data(), utf8_len, nullptr, nullptr);
    return utf8;
#else
    // QUESTION: On Linux builds (architect.md 11 B4), should non-UTF-8 .cpg encodings use iconv or a table?
    return std::string(bytes);
#endif
}

namespace {

struct Crs {
    bool geographic = false;
    std::optional<std::string> epsg;
};

// shortcut: files without a CRS are judged by extent; a projected map inside +-180 x +-90 m is too small to matter.
bool looks_like_degrees(std::span<const Feature> feats) {
    bool any = false;
    for (const Feature& f : feats) {
        for (const P& p : f.geom.all_points()) {
            any = true;
            if (!(std::abs(p.first) <= 180.0 && std::abs(p.second) <= 90.0)) {
                return false;
            }
        }
    }
    return any;
}

std::optional<std::string> read_file_optional(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string read_file_required(const std::filesystem::path& path) {
    auto content = read_file_optional(path);
    if (!content) {
        throw std::runtime_error("cannot read file " + path_to_utf8(path));
    }
    return std::move(*content);
}

// ---- GeoJSON ----

Attr json_attr(const nlohmann::json& v) {
    if (v.is_null()) {
        return Attr::null();
    }
    if (v.is_number()) {
        const double d = v.get<double>();
        return std::isfinite(d) ? Attr::num(d) : Attr::null();
    }
    if (v.is_boolean()) {
        return Attr::num(v.get<bool>() ? 1.0 : 0.0);
    }
    if (v.is_string()) {
        return Attr::text(v.get<std::string>());
    }
    return Attr::text(v.dump());
}

P json_pos(const nlohmann::json& v) {
    if (!v.is_array()) {
        throw std::runtime_error("position must be an array");
    }
    if (v.size() < 2 || !v[0].is_number() || !v[1].is_number()) {
        throw std::runtime_error("position needs two numbers");
    }
    return {v[0].get<double>(), v[1].get<double>()};
}

template <typename F>
auto json_list(const nlohmann::json& v, F&& f) {
    using Elem = decltype(f(v));
    if (!v.is_array()) {
        throw std::runtime_error("coordinates must be arrays");
    }
    std::vector<Elem> out;
    out.reserve(v.size());
    for (const auto& item : v) {
        out.push_back(f(item));
    }
    return out;
}

void json_geom(const nlohmann::json& g, Geom& out, std::uint32_t depth) {
    if (depth > 8) {
        throw std::runtime_error("geometry nested too deeply");
    }
    if (!g.is_object() || !g.contains("type") || !g["type"].is_string()) {
        throw std::runtime_error("geometry without type");
    }
    const std::string kind = g["type"].get<std::string>();
    if (kind == "GeometryCollection") {
        if (!g.contains("geometries") || !g["geometries"].is_array()) {
            throw std::runtime_error("GeometryCollection without geometries");
        }
        for (const auto& part : g["geometries"]) {
            json_geom(part, out, depth + 1);
        }
        return;
    }
    if (!g.contains("coordinates")) {
        throw std::runtime_error("geometry without coordinates");
    }
    const auto& c = g["coordinates"];
    if (kind == "Point") {
        out.points.push_back(json_pos(c));
    } else if (kind == "MultiPoint") {
        const auto pts = json_list(c, json_pos);
        out.points.insert(out.points.end(), pts.begin(), pts.end());
    } else if (kind == "LineString") {
        out.lines.push_back(json_list(c, json_pos));
    } else if (kind == "MultiLineString") {
        auto lines = json_list(c, [](const nlohmann::json& l) { return json_list(l, json_pos); });
        for (auto& line : lines) {
            out.lines.push_back(std::move(line));
        }
    } else if (kind == "Polygon") {
        out.polygons.push_back(json_list(c, [](const nlohmann::json& r) { return json_list(r, json_pos); }));
    } else if (kind == "MultiPolygon") {
        auto polys = json_list(c, [](const nlohmann::json& p) {
            return json_list(p, [](const nlohmann::json& r) { return json_list(r, json_pos); });
        });
        for (auto& poly : polys) {
            out.polygons.push_back(std::move(poly));
        }
    } else {
        throw std::runtime_error("unsupported geometry type " + kind);
    }
}

std::pair<std::vector<Feature>, Crs> read_geojson(const std::filesystem::path& path) {
    const std::string text = read_file_required(path);
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("invalid JSON: ") + e.what());
    }

    Crs crs;
    // Legacy "crs" member written by QGIS for non-WGS84 exports, e.g. "urn:ogc:def:crs:EPSG::32635".
    const nlohmann::json::json_pointer crs_ptr("/crs/properties/name");
    if (root.contains(crs_ptr) && root[crs_ptr].is_string()) {
        const std::string upper = to_ascii_uppercase(root[crs_ptr].get<std::string>());
        crs.geographic = upper.find("CRS84") != std::string::npos || upper.ends_with(":4326");
        if (upper.find("EPSG") != std::string::npos) {
            const auto pos = upper.rfind(':');
            const std::string code = pos == std::string::npos ? upper : upper.substr(pos + 1);
            if (!code.empty()) {
                crs.epsg = code;
            }
        }
    }

    std::vector<const nlohmann::json*> feats;
    if (!root.is_object() || !root.contains("type") || !root["type"].is_string()) {
        throw std::runtime_error("expected a GeoJSON FeatureCollection or Feature");
    }
    const std::string rtype = root["type"].get<std::string>();
    if (rtype == "FeatureCollection") {
        if (!root.contains("features") || !root["features"].is_array()) {
            throw std::runtime_error("FeatureCollection without features");
        }
        for (const auto& f : root["features"]) {
            feats.push_back(&f);
        }
    } else if (rtype == "Feature") {
        feats.push_back(&root);
    } else {
        throw std::runtime_error("expected a GeoJSON FeatureCollection or Feature");
    }

    std::vector<Feature> out;
    out.reserve(feats.size());
    for (std::size_t i = 0; i < feats.size(); ++i) {
        const nlohmann::json& f = *feats[i];
        Geom geom;
        if (f.is_object() && f.contains("geometry") && !f["geometry"].is_null()) {
            try {
                json_geom(f["geometry"], geom, 0);
            } catch (const std::exception& e) {
                throw std::runtime_error(std::format("feature {}: {}", i + 1, e.what()));
            }
        }
        std::vector<std::pair<std::string, Attr>> attrs;
        if (f.is_object() && f.contains("properties") && f["properties"].is_object()) {
            for (const auto& [k, v] : f["properties"].items()) {
                attrs.emplace_back(k, json_attr(v));
            }
        }
        out.push_back(Feature{std::move(geom), std::move(attrs)});
    }
    return {std::move(out), std::move(crs)};
}

// ---- Shapefile ----

class ShpFileGuard {
public:
    explicit ShpFileGuard(SHPHandle h) : h_(h) {}
    ~ShpFileGuard() {
        if (h_) {
            SHPClose(h_);
        }
    }
    ShpFileGuard(const ShpFileGuard&) = delete;
    ShpFileGuard& operator=(const ShpFileGuard&) = delete;
    SHPHandle get() const { return h_; }

private:
    SHPHandle h_;
};

class DbfFileGuard {
public:
    explicit DbfFileGuard(DBFHandle h) : h_(h) {}
    ~DbfFileGuard() {
        if (h_) {
            DBFClose(h_);
        }
    }
    DbfFileGuard(const DbfFileGuard&) = delete;
    DbfFileGuard& operator=(const DbfFileGuard&) = delete;
    DBFHandle get() const { return h_; }

private:
    DBFHandle h_;
};

class ShpObjGuard {
public:
    explicit ShpObjGuard(SHPObject* o) : o_(o) {}
    ~ShpObjGuard() {
        if (o_) {
            SHPDestroyObject(o_);
        }
    }
    ShpObjGuard(const ShpObjGuard&) = delete;
    ShpObjGuard& operator=(const ShpObjGuard&) = delete;
    SHPObject* get() const { return o_; }

private:
    SHPObject* o_;
};

std::vector<std::vector<P>> extract_parts(const SHPObject* s) {
    std::vector<std::vector<P>> parts;
    if (s->nVertices <= 0) {
        return parts;
    }
    if (s->nParts <= 0 || !s->panPartStart) {
        std::vector<P> pts;
        pts.reserve(std::size_t(s->nVertices));
        for (int v = 0; v < s->nVertices; ++v) {
            pts.emplace_back(s->padfX[v], s->padfY[v]);
        }
        parts.push_back(std::move(pts));
        return parts;
    }
    parts.reserve(std::size_t(s->nParts));
    for (int part = 0; part < s->nParts; ++part) {
        const int start = std::max(s->panPartStart[part], 0);
        const int end =
            (part + 1 < s->nParts) ? std::min(s->panPartStart[part + 1], s->nVertices) : s->nVertices;
        std::vector<P> pts;
        if (end > start) {
            pts.reserve(std::size_t(end - start));
            for (int v = start; v < end; ++v) {
                pts.emplace_back(s->padfX[v], s->padfY[v]);
            }
        }
        parts.push_back(std::move(pts));
    }
    return parts;
}

Geom shape_geom(const SHPObject* s) {
    Geom g;
    if (!s) {
        return g;
    }
    switch (s->nSHPType) {
        case SHPT_POINT:
        case SHPT_POINTM:
        case SHPT_POINTZ:
            if (s->nVertices >= 1) {
                g.points.emplace_back(s->padfX[0], s->padfY[0]);
            }
            break;
        case SHPT_MULTIPOINT:
        case SHPT_MULTIPOINTM:
        case SHPT_MULTIPOINTZ:
            g.points.reserve(std::size_t(s->nVertices));
            for (int v = 0; v < s->nVertices; ++v) {
                g.points.emplace_back(s->padfX[v], s->padfY[v]);
            }
            break;
        case SHPT_ARC:
        case SHPT_ARCM:
        case SHPT_ARCZ: {
            auto parts = extract_parts(s);
            for (auto& p : parts) {
                g.lines.push_back(std::move(p));
            }
            break;
        }
        case SHPT_POLYGON:
        case SHPT_POLYGONM:
        case SHPT_POLYGONZ: {
            // One shapefile polygon record may hold several outer rings; even-odd filling handles them together.
            auto rings = extract_parts(s);
            if (!rings.empty()) {
                g.polygons.push_back(std::move(rings));
            }
            break;
        }
        case SHPT_NULL:
        case SHPT_MULTIPATCH:
        default:
            break;
    }
    return g;
}

std::pair<std::vector<Feature>, Crs> read_shapefile(const std::filesystem::path& path) {
    Crs crs;
    std::filesystem::path prj_path = path;
    prj_path.replace_extension("prj");
    if (const auto wkt = read_file_optional(prj_path)) {
        const std::string head = to_ascii_uppercase(trim_start(*wkt));
        crs.geographic = head.starts_with("GEOGCS") || head.starts_with("GEOGCRS");
        // Last AUTHORITY["EPSG","n"] belongs to the outermost CRS; ESRI-style .prj often has none.
        constexpr std::string_view needle = "AUTHORITY[\"EPSG\",\"";
        if (const auto pos = head.rfind(needle); pos != std::string::npos) {
            std::string code;
            for (std::size_t i = pos + needle.size(); i < head.size(); ++i) {
                if (std::isdigit(static_cast<unsigned char>(head[i])) != 0) {
                    code.push_back(head[i]);
                } else {
                    break;
                }
            }
            if (!code.empty()) {
                crs.epsg = std::move(code);
            }
        }
    }

    std::filesystem::path cpg_path = path;
    cpg_path.replace_extension("cpg");
    std::string cpg = read_file_optional(cpg_path).value_or(std::string{});

    // Check that .shp, .shx, and .dbf all exist before opening (shapelib requires all three for complete reading).
    std::filesystem::path shx_path = path;
    shx_path.replace_extension("shx");
    std::filesystem::path dbf_path = path;
    dbf_path.replace_extension("dbf");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::exists(shx_path, ec) ||
        !std::filesystem::exists(dbf_path, ec)) {
        throw std::runtime_error("cannot read shapefile (.shp/.shx/.dbf): missing component file");
    }

    const std::string path_str = path.string();
    const ShpFileGuard shp(SHPOpen(path_str.c_str(), "rb"));
    const DbfFileGuard dbf(DBFOpen(path_str.c_str(), "rb"));
    if (!shp.get() || !dbf.get()) {
        throw std::runtime_error("cannot read shapefile (.shp/.shx/.dbf): failed to open");
    }

    if (cpg.empty()) {
        if (const char* dbf_cp = DBFGetCodePage(dbf.get())) {
            cpg = dbf_cp;
        }
    }

    int n_entities = 0;
    int shape_type = 0;
    SHPGetInfo(shp.get(), &n_entities, &shape_type, nullptr, nullptr);
    const int n_records = DBFGetRecordCount(dbf.get());
    if (n_entities < 0 || n_entities != n_records) {
        throw std::runtime_error("cannot read shapefile (.shp/.shx/.dbf): record count mismatch");
    }

    const int n_fields = DBFGetFieldCount(dbf.get());
    struct FieldInfo {
        std::string name;
        DBFFieldType type = FTInvalid;
    };
    std::vector<FieldInfo> fields;
    fields.reserve(std::size_t(std::max(n_fields, 0)));
    for (int f = 0; f < n_fields; ++f) {
        char title[16]{};
        int width = 0;
        int decimals = 0;
        const DBFFieldType ft = DBFGetFieldInfo(dbf.get(), f, title, &width, &decimals);
        fields.push_back(FieldInfo{title, ft});
    }

    std::vector<Feature> out;
    out.reserve(std::size_t(n_entities));
    for (int rec = 0; rec < n_entities; ++rec) {
        const ShpObjGuard shape(SHPReadObject(shp.get(), rec));
        if (!shape.get()) {
            throw std::runtime_error(std::format("feature {}: failed to read shape", rec + 1));
        }
        std::vector<std::pair<std::string, Attr>> attrs;
        attrs.reserve(fields.size());
        for (int f = 0; f < n_fields; ++f) {
            Attr val = Attr::null();
            if (!DBFIsAttributeNULL(dbf.get(), rec, f)) {
                switch (fields[std::size_t(f)].type) {
                    case FTString: {
                        const char* s = DBFReadStringAttribute(dbf.get(), rec, f);
                        val = Attr::text(decode_cpg(s ? s : "", cpg));
                        break;
                    }
                    case FTInteger:
                        val = Attr::num(double(DBFReadIntegerAttribute(dbf.get(), rec, f)));
                        break;
                    case FTDouble:
                        val = Attr::num(DBFReadDoubleAttribute(dbf.get(), rec, f));
                        break;
                    case FTLogical: {
                        const char* l = DBFReadLogicalAttribute(dbf.get(), rec, f);
                        if (l && (*l == 'T' || *l == 't' || *l == 'Y' || *l == 'y')) {
                            val = Attr::num(1.0);
                        } else if (l && (*l == 'F' || *l == 'f' || *l == 'N' || *l == 'n')) {
                            val = Attr::num(0.0);
                        }
                        break;
                    }
                    case FTDate: {
                        const char* s = DBFReadStringAttribute(dbf.get(), rec, f);
                        val = Attr::text(s ? s : "");
                        break;
                    }
                    case FTInvalid:
                    default:
                        break;
                }
            }
            attrs.emplace_back(fields[std::size_t(f)].name, std::move(val));
        }
        out.push_back(Feature{shape_geom(shape.get()), std::move(attrs)});
    }
    return {std::move(out), std::move(crs)};
}

}  // namespace

mapdata::Baked bake_folder(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) {
        throw std::runtime_error(std::format("cannot read folder {}: {}", path_to_utf8(dir), ec.message()));
    }

    std::vector<std::pair<std::string, std::filesystem::path>> found;
    for (const std::filesystem::directory_entry& entry : it) {
        const std::filesystem::path& path = entry.path();
        if (!path.has_stem() || !path.has_extension()) {
            continue;
        }
        const std::string stem = to_ascii_lowercase(path_to_utf8(path.stem()));
        std::string ext = path_to_utf8(path.extension());
        if (!ext.empty() && ext.front() == '.') {
            ext.erase(ext.begin());
        }
        ext = to_ascii_lowercase(ext);
        if (std::ranges::find(LAYERS, stem) == LAYERS.end()) {
            continue;
        }
        if (ext != "geojson" && ext != "json" && ext != "shp") {
            continue;
        }
        for (const auto& [s, other] : found) {
            if (s == stem) {
                throw std::runtime_error(
                    std::format("two files for layer {}: {} and {}", stem, path_to_utf8(other), path_to_utf8(path)));
            }
        }
        found.emplace_back(stem, path);
    }

    if (found.empty()) {
        throw std::runtime_error(std::format(
            "no layer files in {}; name them after the layers (terrain, regions, nodes, rail, roads, rivers, units) with .geojson or .shp",
            path_to_utf8(dir)));
    }
    std::ranges::sort(found, [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return path_to_utf8(a.second) < path_to_utf8(b.second);
    });

    std::vector<LayerFeatures> layers;
    std::optional<std::pair<std::string, std::string>> crs_seen;  // (EPSG code, file)
    for (const auto& [stem, path] : found) {
        const std::string file = path.has_filename() ? path_to_utf8(path.filename()) : std::string{};
        std::string ext = path.has_extension() ? path_to_utf8(path.extension()) : std::string{};
        if (!ext.empty() && ext.front() == '.') {
            ext.erase(ext.begin());
        }
        const bool is_shp = eq_ignore_ascii_case(ext, "shp");
        std::vector<Feature> feats;
        Crs crs;
        try {
            std::tie(feats, crs) = is_shp ? read_shapefile(path) : read_geojson(path);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::format("{}: {}", file, e.what()));
        }
        if (crs.geographic || looks_like_degrees(feats)) {
            throw std::runtime_error(std::format(
                "{}: coordinates are geographic (degrees). In QGIS, export the layers to a projected metric CRS (e.g. a UTM zone) first",
                file));
        }
        if (crs.epsg) {
            if (crs_seen && crs_seen->first != *crs.epsg) {
                throw std::runtime_error(std::format("{} uses EPSG:{} but {} uses EPSG:{}: reproject all layers to one CRS",
                                                     file, *crs.epsg, crs_seen->second, crs_seen->first));
            }
            if (!crs_seen) {
                crs_seen = std::pair{*crs.epsg, file};
            }
        }
        layers.emplace_back(stem, std::move(feats));
    }

    const std::filesystem::path norm = dir.filename().empty() ? dir.parent_path() : dir;
    const std::string name =
        (norm.has_filename() && norm.filename() != "." && norm.filename() != "..") ? path_to_utf8(norm.filename())
                                                                                   : "map";
    return bake_layers(layers, name);
}

}  // namespace mapbake
