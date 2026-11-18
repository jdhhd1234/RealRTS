#pragma once
// NATO APP-6 (D) symbols for units and depots (architect.md 7, M9).
// PNGs come from milsymbol via `tools/symbols/gen.mjs` + `png.mjs` and live in `game/assets/symbols/`.
// Symbols keep a constant on-screen size at every zoom.

#include <array>
#include <string>
#include <vector>

#include "raylib.h"
#include "sim/sim.hpp"

namespace app {

// On-screen symbol height in pixels.
inline constexpr float SYMBOL_PX = 34.0f;

enum class Kind { Infantry, Armor, Artillery, Partisan, Depot };

inline constexpr std::array<const char*, 5> KIND_FILES{"infantry", "armor", "artillery", "partisan", "depot"};

// Owns one GPU texture. Move-only so the texture is unloaded exactly once.
class Texture {
public:
    Texture() = default;
    explicit Texture(const std::string& path);
    explicit Texture(const Image& image);
    ~Texture();
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    const Texture2D& get() const { return tex; }
    bool loaded() const { return tex.id != 0; }

private:
    Texture2D tex{};
};

// (friendly, kind) -> texture. Index = friendly * 5 + kind.
class Symbols {
public:
    // Loads every PNG from `dir`. Throws std::runtime_error if one is missing.
    void load(const std::string& dir);
    // Draws a symbol centered on a screen point, `height` pixels tall.
    void draw(std::uint8_t side, Kind kind, Vector2 center, float height) const;

private:
    std::vector<Texture> textures;
};

// Main arm by battalion count, for the symbol and the detail panel.
sim::Arm main_arm(const sim::Battalions& b);

}  // namespace app
