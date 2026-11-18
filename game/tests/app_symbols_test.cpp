#include <doctest/doctest.h>

#include <string>

#include "raylib.h"

TEST_CASE("every_symbol_png_has_visible_pixels") {
    for (const char* side : {"friend", "hostile"}) {
        for (const char* kind : {"infantry", "armor", "artillery", "partisan", "depot"}) {
            const std::string path = std::string(ASSETS_DIR) + "/symbols/" + side + "_" + kind + ".png";
            Image img = LoadImage(path.c_str());
            REQUIRE_MESSAGE(img.data != nullptr, path);
            Color* px = LoadImageColors(img);
            int opaque = 0;
            for (int i = 0; i < img.width * img.height; ++i) opaque += px[i].a > 0 ? 1 : 0;
            CHECK_MESSAGE(opaque > 500, path, " has only ", opaque, " visible pixels");
            CHECK(img.height == 96);
            UnloadImageColors(px);
            UnloadImage(img);
        }
    }
}
