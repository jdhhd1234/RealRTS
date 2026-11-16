# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

RealRTS: 2D real-time operational wargame (Rust + Bevy). Graphics are only QGIS-derived maps and NATO APP-6 symbols; depth comes from interacting simulation systems (frontline, supply network, rear-area partisans, partial economy).

- `chat.md` — original design discussion (Korean).
- `architect.md` — design draft derived from `chat.md`. **Draft under human review**; items marked `[가정]` and section 9 are undecided. Do not treat them as final.
- `ai_ask.md` — user's answers to design questions; summarized in `architect.md` section 11, which overrides the rest of that doc.
- `bevy/` — Bevy **0.19.0** engine source, used as a **path dependency** by `game/`. Edit engine code only when unavoidable. Use it and `bevy/examples/` to check API usage instead of guessing; Bevy APIs change between versions. Untracked by git.
- `milsymbol/` — JS APP-6 symbol library clone, untracked. Planned for offline symbol generation.
- `game/` — Cargo workspace (`crates/sim`, `crates/app`, `crates/mapdata`, `crates/mapbake`). Layout plan in `architect.md` section 3.
- Workflow: one milestone (architect.md section 8) at a time; stop for user review and approval before starting the next.

## Architecture rules (from architect.md)

- `sim` crate depends on `bevy_ecs` only, no rendering; must run headless in tests. `app` reads sim state read-only and writes only via `SimCommand`.
- Sim runs in `FixedUpdate`, 1 tick = 1 game minute. Speed-up = more ticks per frame, never a larger tick. Each system runs on its own cadence (move 1, combat 5, supply 60, partisan 360, economy 1440 ticks) in a fixed order.
- Systems never call each other; they interact through shared state (e.g. partisans raise `Edge.damage`, supply reads effective capacity).
- Units/depots are ECS entities; terrain grid, transport graph, regions, nations are plain Rust `Resource`s. Cross-reference by index IDs (`NodeId`, `RegionId`), not `Entity`.
- Deterministic: seeded `rand_chacha`; save/replay = seed + scenario + `(tick, SimCommand)` log.
- Game coordinates: one projected metric CRS per scenario (meters, x east, y north). Projection happens only offline in `tools/mapbake`.
- Work in milestone-sized steps (architect.md section 8), each with tests (supply conservation and capacity invariants are mandatory).

## Commands

Run from `game/`:

```
cargo run -p app                         # game (built-in test map)
cargo run -p app -- samples/sample.gpkg  # game on a QGIS map (.gpkg, layer folder of .geojson/.shp, or baked .ron)
cargo test --workspace                   # all tests
cargo test -p sim <name>                 # single test
cargo run -p mapbake -- in.gpkg out.ron  # bake offline (input: .gpkg or a .geojson/.shp layer folder)
python tools/sample_gpkg.py              # regenerate samples/sample.gpkg and samples/geojson/
node tools/symbols/gen.mjs               # regenerate APP-6 SVGs (needs ../milsymbol)
```

QGIS layer contract and symbol pipeline: `architect.md` 5.4 (overrides 5.2/5.3).
