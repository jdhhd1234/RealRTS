#pragma once
// A folder of single-layer files: each `.geojson` / `.json` / `.shp` whose name is a layer name
// (`terrain`, `regions`, `nodes`, `rail`, `roads`, `rivers`, `units`) becomes that layer.

#include <filesystem>
#include <string>
#include <string_view>

#include "mapdata/mapdata.hpp"

namespace mapbake {

// Bakes every layer file in `dir`. Files with other names are ignored.
// Throws std::runtime_error on invalid inputs.
mapdata::Baked bake_folder(const std::filesystem::path& dir);

// Converts DBF string bytes encoded in the code page specified by a `.cpg` file
// (e.g. "CP949", "EUC-KR", "UTF-8", "CP1252") into UTF-8.
std::string decode_cpg(std::string_view bytes, std::string_view cpg);

namespace folder {
using mapbake::bake_folder;
using mapbake::decode_cpg;
}  // namespace folder

}  // namespace mapbake
