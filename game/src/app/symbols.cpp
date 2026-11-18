#include "app/symbols.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "app/common.hpp"

namespace app {

Texture::Texture(const std::string& path) : tex(LoadTexture(path.c_str())) {
    if (tex.id == 0) throw std::runtime_error("cannot load texture " + path);
}

Texture::Texture(const Image& image) : tex(LoadTextureFromImage(image)) {}

Texture::~Texture() {
    if (tex.id != 0) UnloadTexture(tex);
}

Texture::Texture(Texture&& other) noexcept : tex(std::exchange(other.tex, Texture2D{})) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (tex.id != 0) UnloadTexture(tex);
        tex = std::exchange(other.tex, Texture2D{});
    }
    return *this;
}

void Symbols::load(const std::string& dir) {
    textures.clear();
    for (const char* side : {"friend", "hostile"}) {
        for (const char* kind : KIND_FILES) {
            textures.emplace_back(dir + "/" + side + "_" + kind + ".png");
            SetTextureFilter(textures.back().get(), TEXTURE_FILTER_BILINEAR);
        }
    }
}

void Symbols::draw(std::uint8_t side, Kind kind, Vector2 center, float height) const {
    const std::size_t i = (side == PLAYER_SIDE ? 0 : 1) * KIND_FILES.size() + static_cast<std::size_t>(kind);
    const Texture2D& t = textures[i].get();
    const float w = height * static_cast<float>(t.width) / static_cast<float>(t.height);
    const Rectangle src{0, 0, static_cast<float>(t.width), static_cast<float>(t.height)};
    const Rectangle dst{center.x - w / 2, center.y - height / 2, w, height};
    DrawTexturePro(t, src, dst, {0, 0}, 0, WHITE);
}

sim::Arm main_arm(const sim::Battalions& b) {
    const auto n = [&](sim::Arm a) { return std::ranges::count_if(b.value, [&](const auto& x) { return x.arm == a; }); };
    const auto inf = n(sim::Arm::Infantry), arm = n(sim::Arm::Armor), art = n(sim::Arm::Artillery);
    if (arm > inf && arm >= art) return sim::Arm::Armor;
    if (art > inf) return sim::Arm::Artillery;
    return sim::Arm::Infantry;
}

}  // namespace app
