#pragma once

#include "mapdata/mapdata.hpp"

namespace sim {

using Cell = mapdata::Cell;

float terrain_cost(mapdata::Terrain t);
std::optional<std::vector<Cell>> find_path(const mapdata::TerrainGrid& grid, Cell start, Cell goal);

}  // namespace sim
