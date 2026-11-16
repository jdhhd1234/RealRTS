# RealRTS

2D real-time wargame built on Rust + Bevy. Design: `architect.md`.

## Requirements

- Rust (stable)
- **Bevy 0.19.0 source in `bevy/`** at the repository root. It is not tracked by git; the game uses it as a path dependency.

```
git clone --branch v0.19.0 --depth 1 https://github.com/bevyengine/bevy.git bevy
```

- `milsymbol/` (JS, APP-6 symbol generator) at the repository root, also untracked. Not used by the build yet.

```
git clone https://github.com/spatialillusions/milsymbol.git milsymbol
```

## Build / run

```
cd game
cargo run -p app     # game window
cargo test -p sim    # headless simulation tests
```
