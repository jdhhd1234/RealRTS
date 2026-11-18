# RealRTS

2D real-time wargame built on C++20 + EnTT + Raylib 6.0. Design: `architect.md`.

The earlier Rust + Bevy implementation is kept only as a backup: git tag `rust-backup`
(`git checkout rust-backup -- game/rust game/Cargo.toml game/Cargo.lock` restores it).

## Requirements

- CMake 3.24+, Ninja, a C++20 compiler (MSVC on Windows first). Libraries (EnTT, Raylib, raygui,
  tinyfiledialogs, SQLite, shapelib, nlohmann_json, doctest) are fetched by CMake at pinned versions.
- Optional, only to regenerate APP-6 symbols: Node.js, `milsymbol/` at the repository root (untracked), `@resvg/resvg-js`.

```
git clone https://github.com/spatialillusions/milsymbol.git milsymbol
```

## Build / run

```
cmake -S game -B game/build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build game/build
ctest --test-dir game/build --output-on-failure
game/build/cpp_/app/app [map.gpkg | layer.geojson | baked.json]
```

`mapbake_cli` (in `game/build/cpp_/mapbake/`) bakes a QGIS map to JSON ahead of time.
