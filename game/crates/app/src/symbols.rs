//! NATO APP-6 (D) symbols for units and depots (architect.md 7, M9).
//! SVGs come from milsymbol via `tools/symbols/gen.mjs`, are compiled into the binary, and are
//! rasterized with resvg at startup. Symbols keep a constant on-screen size at every zoom.

use crate::PLAYER_SIDE;
use bevy::asset::RenderAssetUsages;
use bevy::prelude::*;
use bevy::render::render_resource::{Extent3d, TextureDimension, TextureFormat};
use sim::{Arm, Battalions, Depot, Partisan, Position, Side, UnitId};

/// On-screen symbol height in pixels.
pub const SYMBOL_PX: f32 = 34.0;
/// Rasterized texture height; larger than shown so zoomed-in or HiDPI views stay sharp.
const RASTER_PX: f32 = 96.0;

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Kind {
    Infantry,
    Armor,
    Artillery,
    Partisan,
    Depot,
}

macro_rules! svg {
    ($side:literal, $kind:literal) => {
        include_str!(concat!("../assets/symbols/", $side, "_", $kind, ".svg"))
    };
}

const SVGS: [(bool, Kind, &str); 10] = [
    (true, Kind::Infantry, svg!("friend", "infantry")),
    (true, Kind::Armor, svg!("friend", "armor")),
    (true, Kind::Artillery, svg!("friend", "artillery")),
    (true, Kind::Partisan, svg!("friend", "partisan")),
    (true, Kind::Depot, svg!("friend", "depot")),
    (false, Kind::Infantry, svg!("hostile", "infantry")),
    (false, Kind::Armor, svg!("hostile", "armor")),
    (false, Kind::Artillery, svg!("hostile", "artillery")),
    (false, Kind::Partisan, svg!("hostile", "partisan")),
    (false, Kind::Depot, svg!("hostile", "depot")),
];

/// (friendly, kind) -> texture and on-screen size in pixels.
#[derive(Resource, Default)]
pub struct Symbols(Vec<((bool, Kind), Handle<Image>, Vec2)>);

impl Symbols {
    fn get(&self, side: u8, kind: Kind) -> (Handle<Image>, Vec2) {
        let key = (side == PLAYER_SIDE, kind);
        let (_, h, s) = self.0.iter().find(|(k, ..)| *k == key).expect("every symbol is generated");
        (h.clone(), *s)
    }
}

/// Marks sprites drawn at a constant screen size.
#[derive(Component)]
pub struct SymbolSprite;

pub struct SymbolsPlugin;

impl Plugin for SymbolsPlugin {
    fn build(&self, app: &mut App) {
        app.init_resource::<Symbols>()
            .add_systems(Startup, load_symbols)
            .add_systems(Update, (attach_unit_symbols, attach_depot_symbols, keep_screen_size));
    }
}

/// Main arm by battalion count, for the symbol and the detail panel.
pub fn main_arm(b: &Battalions) -> Arm {
    let n = |a: Arm| b.0.iter().filter(|x| x.arm == a).count();
    let (inf, arm, art) = (n(Arm::Infantry), n(Arm::Armor), n(Arm::Artillery));
    if arm > inf && arm >= art {
        Arm::Armor
    } else if art > inf {
        Arm::Artillery
    } else {
        Arm::Infantry
    }
}

fn rasterize(svg: &str) -> (Image, Vec2) {
    let tree = resvg::usvg::Tree::from_str(svg, &resvg::usvg::Options::default()).expect("generated SVG parses");
    let size = tree.size();
    let scale = RASTER_PX / size.height();
    let (w, h) = ((size.width() * scale).ceil() as u32, RASTER_PX as u32);
    let mut pixmap = resvg::tiny_skia::Pixmap::new(w, h).expect("non-zero size");
    resvg::render(&tree, resvg::tiny_skia::Transform::from_scale(scale, scale), &mut pixmap.as_mut());
    let data = pixmap
        .pixels()
        .iter()
        .flat_map(|p| {
            let c = p.demultiply();
            [c.red(), c.green(), c.blue(), c.alpha()]
        })
        .collect();
    let image = Image::new(
        Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        TextureDimension::D2,
        data,
        TextureFormat::Rgba8UnormSrgb,
        RenderAssetUsages::RENDER_WORLD,
    );
    (image, Vec2::new(SYMBOL_PX * w as f32 / h as f32, SYMBOL_PX))
}

fn load_symbols(mut symbols: ResMut<Symbols>, mut images: ResMut<Assets<Image>>) {
    for (friendly, kind, svg) in SVGS {
        let (image, size) = rasterize(svg);
        symbols.0.push(((friendly, kind), images.add(image), size));
    }
}

fn attach_unit_symbols(
    mut commands: Commands,
    symbols: Res<Symbols>,
    new_units: Query<(Entity, &Side, &Position, &Battalions, Has<Partisan>), Added<UnitId>>,
) {
    for (entity, side, pos, bats, partisan) in &new_units {
        let kind = match (partisan, main_arm(bats)) {
            (true, _) => Kind::Partisan,
            (_, Arm::Armor) => Kind::Armor,
            (_, Arm::Artillery) => Kind::Artillery,
            _ => Kind::Infantry,
        };
        let (image, size) = symbols.get(side.0, kind);
        let size = if partisan { size * 0.75 } else { size };
        commands.entity(entity).insert((
            Sprite { image, custom_size: Some(size), ..default() },
            Transform::from_xyz(pos.0.x, pos.0.y, 2.0),
            SymbolSprite,
        ));
    }
}

/// Depots get a supply symbol; it switches sides when the depot is captured.
fn attach_depot_symbols(
    mut commands: Commands,
    symbols: Res<Symbols>,
    map: Res<sim::Map>,
    depots: Query<(Entity, &Depot), Changed<Depot>>,
) {
    for (entity, d) in &depots {
        let (image, size) = symbols.get(d.side, Kind::Depot);
        let p = map.0.graph.node(d.node).pos;
        commands.entity(entity).insert((
            Sprite { image, custom_size: Some(size * 0.8), ..default() },
            Transform::from_xyz(p.x, p.y, 1.0),
            SymbolSprite,
        ));
    }
}

/// World units per screen pixel of the 2D camera.
pub fn world_per_px(proj: &Projection) -> f32 {
    match proj {
        Projection::Orthographic(o) => o.scale,
        _ => 1.0,
    }
}

fn keep_screen_size(
    camera: Single<&Projection, With<Camera2d>>,
    mut sprites: Query<&mut Transform, With<SymbolSprite>>,
) {
    let s = world_per_px(&camera);
    for mut t in &mut sprites {
        if t.scale.x != s {
            t.scale = Vec3::new(s, s, 1.0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_symbol_rasterizes_with_visible_pixels() {
        for (_, kind, svg) in SVGS {
            let (img, size) = rasterize(svg);
            let opaque = img.data.as_ref().unwrap().chunks(4).filter(|p| p[3] > 0).count();
            assert!(opaque > 500, "{kind:?} has only {opaque} visible pixels");
            assert_eq!(size.y, SYMBOL_PX);
        }
    }
}
