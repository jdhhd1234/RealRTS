#pragma once
// File menu: open a QGIS map (.gpkg / GeoJSON / Shapefile folder, baked on the fly by `mapbake`)
// or a baked map (.json), go back to the built-in test map, or save the edited scenario as .json.
// The dialog and baking run on a worker thread; the result is applied at the start of a frame.

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "mapdata/mapdata.hpp"

namespace app {

// Shown in the top bar and the File menu.
struct FileStatus {
    std::string text = "Map: built-in test map";
    bool error = false;
    bool busy = false;
};

// Worker result: a map to load, or (no map, status text) after a save.
// `ok == false` with empty `text` means the dialog was cancelled.
struct LoadMsg {
    bool ok = false;
    std::optional<mapdata::Baked> baked;
    std::string text;
};

// Loader errors (bad file, bake failure). Message is shown to the player as is.
class LoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Loader {
public:
    // Native file dialog on a worker thread, then bake or parse there too.
    void open_dialog(FileStatus& status);
    void load_test_map();
    // Save dialog on a worker thread; writes map + scenario as baked JSON that File > Open reads back.
    void save_dialog(const mapdata::Baked& baked, FileStatus& status);
    // Takes the finished result, if any.
    std::optional<LoadMsg> try_recv();

private:
    void send(LoadMsg msg);

    std::mutex mutex;
    std::optional<LoadMsg> inbox;
    // Declared last: joined first on destruction, while the mutex still exists.
    std::jthread worker;
};

// .json = baked map; .geojson/.shp = the whole folder of layer files; anything else = GeoPackage.
// Returns the map and a label for the status bar. Throws LoadError.
std::pair<mapdata::Baked, std::string> read_map(const std::filesystem::path& path);

// UTF-8 text <-> filesystem path (dialogs, command line and the UI all use UTF-8).
std::filesystem::path path_from_utf8(const std::string& s);
std::string utf8(const std::filesystem::path& p);

// Status bar text for a freshly loaded map.
std::string loaded_text(const mapdata::Baked& baked, const std::string& label);

}  // namespace app
