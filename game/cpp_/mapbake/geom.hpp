#pragma once
// GeoPackage geometry blobs (GP header + WKB) and scanline rasterization.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace mapbake {

// (x, y) in the layer's CRS units.
using P = std::pair<double, double>;

// Geometry flattened to what the baker needs.
struct Geom {
    std::vector<P> points;
    std::vector<std::vector<P>> lines;
    // Each polygon is a list of rings (outer first, then holes).
    std::vector<std::vector<std::vector<P>>> polygons;

    std::vector<P> all_points() const;
    bool operator==(const Geom&) const = default;
};

// Parse a GeoPackage geometry blob (http://www.geopackage.org/spec/#gpb_format).
// Throws std::runtime_error on invalid or unsupported blobs.
Geom parse_gpb(std::span<const std::uint8_t> b);

// Cells (cx, cy) whose centers lie inside the polygon (even-odd rule, so holes work).
// Coordinates are in cell units with (0, 0) at the grid's bottom-left corner.
template <typename F>
void raster_polygon(std::span<const std::vector<P>> rings, std::uint32_t w, std::uint32_t h, F&& f) {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (const auto& ring : rings) {
        for (const P& p : ring) {
            lo = std::min(lo, p.second);
            hi = std::max(hi, p.second);
        }
    }
    if (!std::isfinite(lo)) {
        return;
    }
    const double first_d = std::max(std::ceil(lo - 0.5), 0.0);
    const double last_d = std::min(std::floor(hi - 0.5), double(h) - 1.0);
    if (last_d < 0.0 || first_d > last_d) {
        return;
    }
    const auto first = std::uint32_t(first_d);
    const auto last = std::uint32_t(last_d);
    std::vector<double> xs;
    for (std::uint32_t cy = first; cy <= last; ++cy) {
        const double y = double(cy) + 0.5;
        xs.clear();
        for (const auto& ring : rings) {
            if (ring.empty()) {
                continue;
            }
            for (std::size_t i = 0; i < ring.size(); ++i) {
                const auto [x0, y0] = ring[i];
                const auto [x1, y1] = ring[(i + 1) % ring.size()];
                if ((y0 <= y) != (y1 <= y)) {
                    xs.push_back(x0 + (y - y0) / (y1 - y0) * (x1 - x0));
                }
            }
        }
        std::ranges::sort(xs);
        for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
            const double a = std::max(std::ceil(xs[k] - 0.5), 0.0);
            const double b = std::min(std::floor(xs[k + 1] - 0.5), double(w) - 1.0);
            if (b >= a) {
                for (std::uint32_t cx = std::uint32_t(a); cx <= std::uint32_t(b); ++cx) {
                    f(cx, cy);
                }
            }
        }
    }
}

namespace geom {
using mapbake::Geom;
using mapbake::P;
using mapbake::parse_gpb;
using mapbake::raster_polygon;
}  // namespace geom

}  // namespace mapbake
