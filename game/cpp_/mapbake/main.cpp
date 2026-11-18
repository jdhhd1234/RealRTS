// Offline baking: `mapbake_cli <input.gpkg | layer folder | layer.geojson/.shp> <output.json>`

#include <fstream>
#include <iostream>
#include <string>

#include "mapbake/mapbake.hpp"

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: mapbake <input.gpkg | folder of .geojson/.shp layers> <output.json>\n";
        return 2;
    }
    const std::string input = argv[1];
    const std::string output = argv[2];
    try {
        const mapdata::Baked b = mapbake::bake_path(input);
        std::ofstream out(output, std::ios::binary);
        if (!out) {
            std::cerr << "cannot write " << output << "\n";
            return 1;
        }
        const std::string json = b.to_json();
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out) {
            std::cerr << "cannot write " << output << "\n";
            return 1;
        }
        const auto& t = b.map.terrain;
        std::cout << output << ": " << t.width << " x " << t.height << " km, " << b.map.regions.size() << " regions, "
                  << b.map.graph.nodes.size() << " nodes, " << b.map.graph.edges.size() << " edges, "
                  << b.scenario.units.size() << " units\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
