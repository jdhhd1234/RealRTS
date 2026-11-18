#include "app/files.hpp"

#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "mapbake/mapbake.hpp"
#include "tinyfiledialogs.h"

namespace app {

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool ends_with(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw LoadError("cannot open file");
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// Baked maps and GeoJSON layers can both be .json: a .json with top-level "map" and "scenario" is baked.
bool is_baked_json(const std::filesystem::path& path, const std::string& low) {
    if (!ends_with(low, ".json")) return false;
    const auto j = nlohmann::json::parse(read_text(path), nullptr, false);
    return j.is_object() && j.contains("map") && j.contains("scenario");
}

}  // namespace

std::filesystem::path path_from_utf8(const std::string& s) { return std::u8string(s.begin(), s.end()); }

std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

void Loader::send(LoadMsg msg) {
    std::scoped_lock lock(mutex);
    inbox = std::move(msg);
}

std::optional<LoadMsg> Loader::try_recv() {
    std::scoped_lock lock(mutex);
    std::optional<LoadMsg> out = std::move(inbox);
    inbox.reset();
    return out;
}

void Loader::open_dialog(FileStatus& status) {
    if (status.busy) return;
    status.busy = true;
    status.text = "Opening...";
    worker = std::jthread([this] {
        const char* filters[] = {"*.gpkg", "*.geojson", "*.json", "*.shp"};
        const char* picked = tinyfd_openFileDialog(
            "Open map", "", 4, filters, "QGIS map (GeoPackage, GeoJSON, Shapefile) or baked map", 0);
        if (picked == nullptr) {
            send({});  // cancelled
            return;
        }
        try {
            auto [baked, label] = read_map(path_from_utf8(picked));
            send({true, std::move(baked), std::move(label)});
        } catch (const std::exception& e) {
            send({false, std::nullopt, e.what()});
        }
    });
}

void Loader::load_test_map() { send({true, mapdata::test_baked(), "built-in test map"}); }

void Loader::save_dialog(const mapdata::Baked& baked, FileStatus& status) {
    if (status.busy) return;
    status.busy = true;
    status.text = "Saving...";
    worker = std::jthread([this, text = baked.to_json(), name = baked.map.name + ".json"] {
        const char* filters[] = {"*.json"};
        const char* picked = tinyfd_saveFileDialog("Save scenario", name.c_str(), 1, filters, "Baked map");
        if (picked == nullptr) {
            send({});
            return;
        }
        const std::filesystem::path path = path_from_utf8(picked);
        std::ofstream out(path, std::ios::binary);
        out << text;
        if (!out) {
            send({false, std::nullopt, "Save failed: cannot write " + utf8(path)});
            return;
        }
        send({true, std::nullopt, "Saved: " + utf8(path)});
    });
}

std::pair<mapdata::Baked, std::string> read_map(const std::filesystem::path& path) {
    std::string label = path.filename().empty() ? utf8(path) : utf8(path.filename());
    const std::string low = lower(label);
    mapdata::Baked baked;
    bool baked_file = false;
    try {
        baked_file = is_baked_json(path, low);
        if (baked_file) {
            baked = mapdata::Baked::from_json(read_text(path));
        } else {
            baked = mapbake::bake_path(path);
        }
    } catch (const std::exception& e) {
        throw LoadError(label + ": " + e.what());
    }
    // Folder maps are named after the folder (set by mapbake).
    if (!ends_with(low, ".gpkg") && !baked_file) {
        label = baked.map.name + "/ folder";
    }
    return {std::move(baked), std::move(label)};
}

std::string loaded_text(const mapdata::Baked& baked, const std::string& label) {
    const auto& t = baked.map.terrain;
    return std::format("Map: {} ({} x {} km, {} units)", label,
                       std::round(static_cast<float>(t.width) * t.cell_m / 1000.0f),
                       std::round(static_cast<float>(t.height) * t.cell_m / 1000.0f), baked.scenario.units.size());
}

}  // namespace app
