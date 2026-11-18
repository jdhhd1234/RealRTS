#include "mapbake/geom.hpp"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace mapbake {

std::vector<P> Geom::all_points() const {
    std::vector<P> out;
    out.insert(out.end(), points.begin(), points.end());
    for (const auto& line : lines) {
        out.insert(out.end(), line.begin(), line.end());
    }
    for (const auto& poly : polygons) {
        for (const auto& ring : poly) {
            out.insert(out.end(), ring.begin(), ring.end());
        }
    }
    return out;
}

namespace {

struct Reader {
    std::span<const std::uint8_t> b;
    std::size_t i = 0;

    std::span<const std::uint8_t> take(std::size_t n) {
        if (i + n > b.size()) {
            throw std::runtime_error("truncated WKB");
        }
        const auto s = b.subspan(i, n);
        i += n;
        return s;
    }

    std::uint32_t u32(bool le) {
        const auto a = take(4);
        if (le) {
            return std::uint32_t(a[0]) | (std::uint32_t(a[1]) << 8) |
                   (std::uint32_t(a[2]) << 16) | (std::uint32_t(a[3]) << 24);
        }
        return (std::uint32_t(a[0]) << 24) | (std::uint32_t(a[1]) << 16) |
               (std::uint32_t(a[2]) << 8) | std::uint32_t(a[3]);
    }

    double f64(bool le) {
        const auto a = take(8);
        std::uint64_t bits = 0;
        if (le) {
            for (std::size_t k = 0; k < 8; ++k) {
                bits |= std::uint64_t(a[k]) << (k * 8);
            }
        } else {
            for (std::size_t k = 0; k < 8; ++k) {
                bits = (bits << 8) | std::uint64_t(a[k]);
            }
        }
        return std::bit_cast<double>(bits);
    }
};

void read_wkb(Reader& r, Geom& g, std::uint32_t depth) {
    if (depth > 8) {
        throw std::runtime_error("geometry nested too deeply");
    }
    const bool le = r.take(1)[0] == 1;
    const std::uint32_t raw = r.u32(le);
    // ISO (1000/2000/3000 offsets) and EWKB (high flag bits) Z/M variants.
    const bool z = (raw & 0x8000'0000u) != 0;
    const bool m = (raw & 0x4000'0000u) != 0;
    const std::uint32_t iso = raw & 0x0FFF'FFFFu;
    const std::uint32_t kind = iso % 1000;
    const std::uint32_t iso_hi = iso / 1000;
    const bool iz = (iso_hi == 1 || iso_hi == 3);
    const bool im = (iso_hi == 2 || iso_hi == 3);
    const std::size_t dims = 2 + std::size_t(z || iz) + std::size_t(m || im);

    const auto point = [&]() -> P {
        const double x = r.f64(le);
        const double y = r.f64(le);
        for (std::size_t d = 2; d < dims; ++d) {
            (void)r.f64(le);
        }
        return {x, y};
    };

    const auto count = [&]() -> std::size_t {
        const auto n = std::size_t(r.u32(le));
        // Every element needs at least 16 bytes; reject counts the blob cannot hold.
        if (n > r.b.size() / 16 + 1) {
            throw std::runtime_error("bad WKB count");
        }
        return n;
    };

    const auto ring = [&]() -> std::vector<P> {
        const std::size_t n = count();
        std::vector<P> pts;
        pts.reserve(n);
        for (std::size_t k = 0; k < n; ++k) {
            pts.push_back(point());
        }
        return pts;
    };

    switch (kind) {
        case 1: {
            const P p = point();
            if (std::isfinite(p.first)) {
                g.points.push_back(p);  // NaN point = empty
            }
            break;
        }
        case 2: {
            g.lines.push_back(ring());
            break;
        }
        case 3: {
            const std::size_t n = count();
            std::vector<std::vector<P>> rings;
            rings.reserve(n);
            for (std::size_t k = 0; k < n; ++k) {
                rings.push_back(ring());
            }
            g.polygons.push_back(std::move(rings));
            break;
        }
        case 4:
        case 5:
        case 6:
        case 7: {
            const std::size_t n = count();
            for (std::size_t k = 0; k < n; ++k) {
                read_wkb(r, g, depth + 1);
            }
            break;
        }
        default:
            throw std::runtime_error("unsupported geometry type " + std::to_string(kind));
    }
}

}  // namespace

Geom parse_gpb(std::span<const std::uint8_t> b) {
    if (b.size() < 8 || b[0] != 'G' || b[1] != 'P') {
        throw std::runtime_error("not a GeoPackage geometry blob");
    }
    const std::uint8_t flags = b[3];
    if ((flags & 0b0001'0000u) != 0) {
        return Geom{};  // empty geometry
    }
    std::size_t envelope = 0;
    const std::uint8_t env_code = (flags >> 1) & 0b111u;
    switch (env_code) {
        case 0:
            envelope = 0;
            break;
        case 1:
            envelope = 32;
            break;
        case 2:
        case 3:
            envelope = 48;
            break;
        case 4:
            envelope = 64;
            break;
        default:
            throw std::runtime_error("bad envelope code " + std::to_string(env_code));
    }
    const std::size_t start = 8 + envelope;
    if (b.size() < start) {
        throw std::runtime_error("truncated geometry header");
    }
    Geom g;
    Reader r{b.subspan(start), 0};
    read_wkb(r, g, 0);
    return g;
}

}  // namespace mapbake
