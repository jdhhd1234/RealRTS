use bevy::asset::RenderAssetUsages;
use bevy::image::ImageSampler;
use bevy::input::mouse::AccumulatedMouseScroll;
use bevy::prelude::*;
use bevy::render::render_resource::{Extent3d, TextureDimension, TextureFormat};
use bevy::ui::RelativeCursorPosition;
use bevy::window::PrimaryWindow;
use mapdata::{EdgeKind, MapData, Pos, Terrain};
use sim::{
    CommandQueue, EconomyState, EdgeState, Engagements, Front, Map, MoveOrder, Partisan, PartisanState, Position, Side,
    SimCommand, SupplyPriority, Territory, UnitId,
};

/// Speed steps in ticks per real second. Tick size never changes, only how many run per second.
const SPEEDS: [f64; 4] = [1.0, 2.0, 10.0, 60.0];
/// Player controls this side. shortcut: hard-coded until scenario setup exists.
const PLAYER_SIDE: u8 = 0;

mod editor;
mod files;
mod symbols;
mod ui;
use symbols::{SYMBOL_PX, world_per_px};
use ui::{UiBlock, cursor_over_ui};

/// Index into `SPEEDS`. Presentation-only: speed does not affect sim results.
#[derive(Resource, Default)]
struct Speed(usize);

#[derive(Resource, Default)]
struct Selected(Option<UnitId>);

/// Layers tab: draw the transport graph and depots.
#[derive(Resource)]
struct ShowNetwork(bool);

impl Default for ShowNetwork {
    fn default() -> Self {
        Self(true)
    }
}

/// Counter for new division names.
#[derive(Resource, Default)]
struct Raised(u32);

impl Raised {
    fn order(&mut self) -> SimCommand {
        self.0 += 1;
        SimCommand::BuildDivision { side: PLAYER_SIDE, name: format!("New Div #{}", self.0) }
    }
}

fn main() {
    let mut app = App::new();
    app.add_plugins(DefaultPlugins.set(WindowPlugin {
        primary_window: Some(Window { title: "RealRTS".into(), resolution: (1600, 900).into(), ..default() }),        ..default()
    }))
    .add_plugins((ui::UiPlugin, symbols::SymbolsPlugin))
    .insert_resource(ClearColor(Color::srgb(0.05, 0.06, 0.07)))
    .insert_resource(Time::<Fixed>::from_hz(SPEEDS[0]))
    .init_resource::<Speed>()
    .init_resource::<Selected>()
    .init_resource::<ShowNetwork>()
    .init_resource::<Raised>()
    .init_resource::<files::Loader>()
    .init_resource::<files::FileStatus>()
    .edit_schedule(FixedUpdate, sim::configure)
    .add_systems(Startup, setup)
    .init_resource::<MapVersion>()
    .add_systems(PreUpdate, (files::apply_loaded, editor::apply_edits).chain())
    .add_systems(
        Update,
        (
            (speed_controls, mouse_orders, unit_keys, region_keys).run_if(editor::playing),
            (editor::edit_mouse, editor::draw_editor).run_if(editor::editing),
            camera_controls,
            sync_unit_sprites,
            draw_overlay,
            overlay_key,
            (rebuild_map_view, refresh_territory_overlay).chain(),
        ),
    )
    .init_resource::<OverlayMode>();

    // `app [map.gpkg|map.ron]` opens that map; otherwise the built-in test map. File > Open swaps maps later.
    let start = std::env::args().nth(1).map(|p| files::read_map(std::path::Path::new(&p)));
    let baked = match start {
        Some(Ok((baked, label))) => {
            app.world_mut().resource_mut::<files::FileStatus>().text = format!("Map: {label}");
            baked
        }
        Some(Err(e)) => {
            eprintln!("{e}");
            *app.world_mut().resource_mut::<files::FileStatus>() =
                files::FileStatus { text: format!("Load failed: {e}"), error: true, busy: false };
            mapdata::test_baked()
        }
        None => mapdata::test_baked(),
    };
    app.insert_resource(editor::Editor::new(baked.clone()));
    sim::load_scenario(app.world_mut(), baked);
    app.run();
}

/// Territory overlay layer (architect.md 12.5). T cycles.
#[derive(Resource, Default, Clone, Copy, PartialEq, Eq, Debug)]
enum OverlayMode {
    #[default]
    Military,
    Legal,
    Regions,
    /// Regions shaded by partisan risk grade.
    Risk,
    Off,
}

#[derive(Component)]
struct TerritoryOverlay;

fn overlay_key(keys: Res<ButtonInput<KeyCode>>, mut mode: ResMut<OverlayMode>) {
    if keys.just_pressed(KeyCode::KeyT) {
        *mode = match *mode {
            OverlayMode::Military => OverlayMode::Legal,
            OverlayMode::Legal => OverlayMode::Regions,
            OverlayMode::Regions => OverlayMode::Risk,
            OverlayMode::Risk => OverlayMode::Off,
            OverlayMode::Off => OverlayMode::Military,
        };
    }
}

/// Redraws the overlay image only when territory or the mode changes.
fn refresh_territory_overlay(
    mode: Res<OverlayMode>,
    territory: Res<Territory>,
    map: Res<Map>,
    overlay: Single<&Sprite, With<TerritoryOverlay>>,
    mut images: ResMut<Assets<Image>>,
    partisans: Res<PartisanState>,
) {
    let risk_changed = *mode == OverlayMode::Risk && partisans.is_changed();
    if !mode.is_changed() && !territory.is_changed() && !risk_changed {
        return;
    }
    if let Some(mut img) = images.get_mut(&overlay.image) {
        *img = overlay_image(*mode, &territory, &map.0, &partisans);
    }
}

fn overlay_image(mode: OverlayMode, territory: &Territory, m: &MapData, partisans: &PartisanState) -> Image {
    // Amber shade by risk grade; only the grade is visible to the player (D5).
    let risk = |r: usize| -> [u8; 4] {
        match sim::risk_grade(partisans.pool[r]) {
            sim::RiskGrade::None => [0, 0, 0, 0],
            sim::RiskGrade::Low => [0xf2, 0xb3, 0x40, 50],
            sim::RiskGrade::Medium => [0xf2, 0x8c, 0x28, 100],
            sim::RiskGrade::High => [0xe0, 0x40, 0x30, 140],
        }
    };
    let (w, h) = (m.terrain.width, m.terrain.height);
    let idx = |cx: u32, cy: u32| (cy * w + cx) as usize;
    // Concept-diagram colors: blue #3574c1, red #c95049 at 0.35 alpha, border #475569, region border #64748b.
    let fill = |side: u8| if side == PLAYER_SIDE { [0x35, 0x74, 0xc1, 90] } else { [0xc9, 0x50, 0x49, 90] };
    let owner = |cx: u32, cy: u32| -> u32 {
        match mode {
            OverlayMode::Military => territory.military[idx(cx, cy)] as u32,
            OverlayMode::Legal => territory.legal[m.region_at(cx, cy).0 as usize] as u32,
            OverlayMode::Regions | OverlayMode::Risk | OverlayMode::Off => m.region_at(cx, cy).0 as u32,
        }
    };
    let mut data = Vec::with_capacity((w * h * 4) as usize);
    for cy in (0..h).rev() {
        for cx in 0..w {
            let o = owner(cx, cy);
            let border = (cx + 1 < w && owner(cx + 1, cy) != o) || (cy + 1 < h && owner(cx, cy + 1) != o);
            let px = match mode {
                OverlayMode::Off => [0, 0, 0, 0],
                OverlayMode::Regions if border => [0x64, 0x74, 0x8b, 200],
                OverlayMode::Regions => [0, 0, 0, 0],
                OverlayMode::Risk if border => [0x64, 0x74, 0x8b, 200],
                OverlayMode::Risk => risk(o as usize),
                _ if border => [0x47, 0x55, 0x69, 230],
                _ => fill(o as u8),
            };
            data.extend_from_slice(&px);
        }
    }
    rgba_image(w, h, data)
}

fn rgba_image(w: u32, h: u32, data: Vec<u8>) -> Image {
    let mut image = Image::new(
        Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        TextureDimension::D2,
        data,
        TextureFormat::Rgba8UnormSrgb,
        RenderAssetUsages::RENDER_WORLD,
    );
    image.sampler = ImageSampler::nearest();
    image
}

fn setup(mut commands: Commands) {
    commands.spawn((Camera2d, Projection::Orthographic(OrthographicProjection::default_2d())));
}

/// Bumped when a different map is loaded. Editor reloads keep it, so the camera stays put.
#[derive(Resource, Default)]
struct MapVersion(u32);

/// Terrain and territory sprites of the current map.
#[derive(Component)]
struct MapView;

/// Rebuilds the map sprites and fits the camera whenever a (new) map is loaded.
#[allow(clippy::too_many_arguments)]
fn rebuild_map_view(
    mut commands: Commands,
    map: Res<Map>,
    version: Res<MapVersion>,
    old: Query<Entity, With<MapView>>,
    mut images: ResMut<Assets<Image>>,
    camera: Single<(&mut Transform, &mut Projection), With<Camera2d>>,
    window: Single<&Window, With<PrimaryWindow>>,
    (mode, territory, partisans): (Res<OverlayMode>, Res<Territory>, Res<PartisanState>),
) {
    if !version.is_changed() {
        return;
    }
    old.iter().for_each(|e| commands.entity(e).despawn());
    let (w_m, h_m) = map.0.terrain.size_m();
    let center = Vec3::new(w_m / 2.0, h_m / 2.0, 0.0);
    let size = Some(Vec2::new(w_m, h_m));
    commands.spawn((
        Sprite { image: images.add(terrain_image(&map.0.terrain)), custom_size: size, ..default() },
        Transform::from_translation(center.with_z(-10.0)),
        MapView,
    ));
    commands.spawn((
        Sprite { image: images.add(overlay_image(*mode, &territory, &map.0, &partisans)), custom_size: size, ..default() },
        Transform::from_translation(center.with_z(-5.0)),
        TerritoryOverlay,
        MapView,
    ));

    // Fit the whole map into the free area between the panels.
    let free = Vec2::new(window.width() - ui::LEFT_W - ui::RIGHT_W, window.height() - ui::TOP_H - ui::BOTTOM_H).max(Vec2::splat(100.0));
    let scale = (w_m / free.x).max(h_m / free.y) * 1.05;
    let shift = Vec3::new((ui::LEFT_W - ui::RIGHT_W) / 2.0, (ui::BOTTOM_H - ui::TOP_H) / 2.0, 0.0);
    let (mut t, mut proj) = camera.into_inner();
    if let Projection::Orthographic(o) = &mut *proj {
        o.scale = scale;
    }
    t.translation = center - shift * scale;
}

/// P: toggle supply priority (0 / 5) of the selected unit. G: toggle hold (no retreat).
fn unit_keys(
    keys: Res<ButtonInput<KeyCode>>,
    selected: Res<Selected>,
    units: Query<(&UnitId, &SupplyPriority, &Front)>,
    mut queue: ResMut<CommandQueue>,
) {
    let Some(sel) = selected.0 else { return };
    let Some((_, p, f)) = units.iter().find(|(id, ..)| **id == sel) else { return };
    if keys.just_pressed(KeyCode::KeyP) {
        queue.0.push(SimCommand::SetSupplyPriority { unit: sel, priority: if p.0 == 0 { 5 } else { 0 } });
    }
    if keys.just_pressed(KeyCode::KeyG) {
        queue.0.push(SimCommand::SetHold { unit: sel, hold: !f.hold });
    }
}


fn terrain_image(grid: &mapdata::TerrainGrid) -> Image {
    let mut data = Vec::with_capacity((grid.width * grid.height * 4) as usize);
    // Image rows go top-down; grid rows go bottom-up.
    for cy in (0..grid.height).rev() {
        for cx in 0..grid.width {
            data.extend_from_slice(&match grid.get(cx, cy) {
                Terrain::Plains => [196, 208, 160, 255],
                Terrain::Forest => [90, 140, 80, 255],
                Terrain::Marsh => [130, 160, 150, 255],
                Terrain::Mountain => [150, 130, 110, 255],
                Terrain::Urban => [170, 170, 170, 255],
                Terrain::Water => [90, 140, 210, 255],
            });
        }
    }
    rgba_image(grid.width, grid.height, data)
}

/// shortcut: snaps to the sim position each frame, no interpolation between ticks yet.
fn sync_unit_sprites(mut units: Query<(&Position, &mut Transform), Changed<Position>>) {
    for (pos, mut t) in &mut units {
        t.translation.x = pos.0.x;
        t.translation.y = pos.0.y;
    }
}

fn cursor_world(window: &Window, camera: &Camera, cam_t: &GlobalTransform) -> Option<Vec2> {
    camera.viewport_to_world_2d(cam_t, window.cursor_position()?).ok()
}

/// Left click: select own unit. Right click: order the selected unit to move there.
fn mouse_orders(
    buttons: Res<ButtonInput<MouseButton>>,
    window: Single<&Window, With<PrimaryWindow>>,
    camera: Single<(&Camera, &GlobalTransform, &Projection)>,
    // Partisans are not player-controlled yet (A5).
    units: Query<(&UnitId, &Side, &Position), Without<Partisan>>,
    mut selected: ResMut<Selected>,
    mut queue: ResMut<CommandQueue>,
    panels: Query<&RelativeCursorPosition, With<UiBlock>>,
) {
    if cursor_over_ui(&panels) {
        return;
    }
    let (camera, cam_t, proj) = *camera;
    let Some(p) = cursor_world(&window, camera, cam_t) else { return };
    if buttons.just_pressed(MouseButton::Left) {
        // Nearest own unit whose symbol is under the cursor.
        let reach = SYMBOL_PX * 0.7 * world_per_px(proj);
        selected.0 = units
            .iter()
            .filter(|(_, side, _)| side.0 == PLAYER_SIDE)
            .map(|(id, _, pos)| (*id, Vec2::new(pos.0.x, pos.0.y).distance(p)))
            .filter(|(_, d)| *d < reach)
            .min_by(|a, b| a.1.total_cmp(&b.1))
            .map(|(id, _)| id);
    }
    if buttons.just_pressed(MouseButton::Right) && let Some(unit) = selected.0 {
        queue.0.push(SimCommand::Move { unit, goal: Pos { x: p.x, y: p.y } });
    }
}

/// Region keys act on the region under the cursor: K sweep, H harsh reprisals, Q toggle requisition.
/// B raises a new division for the player.
#[allow(clippy::too_many_arguments)]
fn region_keys(
    keys: Res<ButtonInput<KeyCode>>,
    window: Single<&Window, With<PrimaryWindow>>,
    camera: Single<(&Camera, &GlobalTransform)>,
    panels: Query<&RelativeCursorPosition, With<UiBlock>>,
    map: Res<Map>,
    econ: Res<EconomyState>,
    mut raised: ResMut<Raised>,
    mut queue: ResMut<CommandQueue>,
) {
    if keys.just_pressed(KeyCode::KeyB) {
        queue.0.push(raised.order());
    }
    if cursor_over_ui(&panels) {
        return;
    }
    let (camera, cam_t) = *camera;
    let Some(p) = cursor_world(&window, camera, cam_t) else { return };
    let Some((cx, cy)) = map.0.terrain.cell_at(Pos { x: p.x, y: p.y }) else { return };
    let region = map.0.region_at(cx, cy);
    for (key, harsh) in [(KeyCode::KeyK, false), (KeyCode::KeyH, true)] {
        if keys.just_pressed(key) {
            queue.0.push(SimCommand::AntiPartisan { region, harsh });
        }
    }
    if keys.just_pressed(KeyCode::KeyQ) {
        let on = !econ.requisition[region.0 as usize];
        queue.0.push(SimCommand::SetRequisition { region, on });
    }
}

/// WASD / arrows: pan. Mouse wheel: zoom.
fn camera_controls(
    keys: Res<ButtonInput<KeyCode>>,
    scroll: Res<AccumulatedMouseScroll>,
    time: Res<Time<Real>>,
    camera: Single<(&mut Transform, &mut Projection), With<Camera2d>>,
) {
    let (mut t, mut proj) = camera.into_inner();
    let Projection::Orthographic(proj) = &mut *proj else { return };
    if scroll.delta.y != 0.0 {
        proj.scale = (proj.scale * 0.9f32.powf(scroll.delta.y)).clamp(5.0, 5000.0);
    }
    let mut dir = Vec2::ZERO;
    for (keys_for, d) in [
        ([KeyCode::KeyW, KeyCode::ArrowUp], Vec2::Y),
        ([KeyCode::KeyS, KeyCode::ArrowDown], -Vec2::Y),
        ([KeyCode::KeyA, KeyCode::ArrowLeft], -Vec2::X),
        ([KeyCode::KeyD, KeyCode::ArrowRight], Vec2::X),
    ] {
        if keys.any_pressed(keys_for) {
            dir += d;
        }
    }
    t.translation += (dir * proj.scale * 800.0 * time.delta_secs()).extend(0.0);
}

/// Transport graph, selection ring and remaining path.
fn draw_overlay(
    mut gizmos: Gizmos,
    map: Res<Map>,
    selected: Res<Selected>,
    units: Query<(&UnitId, &Position, Option<&MoveOrder>)>,
    edge_state: Res<EdgeState>,
    engagements: Res<Engagements>,
    positions: Query<&Position>,
    fronts: Query<(&Position, &Front)>,
    network: Res<ShowNetwork>,
    camera: Single<&Projection, With<Camera2d>>,
    lines: Query<(&Position, &Side, &sim::Deployment, &sim::Battalions)>,
) {
    let g = &map.0.graph;
    let v = |p: Pos| Vec2::new(p.x, p.y);
    // Marker sizes follow the screen, like the symbols.
    let px = world_per_px(&camera);
    let ring = SYMBOL_PX * 0.75 * px;
    let edges = if network.0 { g.edges.as_slice() } else { &[] };
    for (i, e) in edges.iter().enumerate() {
        // Edges carrying supply this hour are drawn orange.
        // Damaged (sabotaged) edges red, edges carrying supply orange.
        let color = if edge_state.damage[i] > 0.0 {
            Color::srgb(0.9, 0.1, 0.1).with_alpha(0.4 + 0.6 * edge_state.damage[i])
        } else if edge_state.last_flow[i] > 0.0 {
            Color::srgb(1.0, 0.55, 0.0)
        } else {
            match e.kind {
                EdgeKind::Rail { .. } => Color::srgb(0.1, 0.1, 0.1),
                EdgeKind::Road => Color::srgb(0.55, 0.35, 0.15),
                EdgeKind::River => Color::srgb(0.2, 0.3, 0.8),
            }
        };
        gizmos.line_2d(v(g.node(e.a).pos), v(g.node(e.b).pos), color);
    }
    for n in g.nodes.iter().filter(|_| network.0) {
        gizmos.circle_2d(v(n.pos), 3.0 * px, Color::BLACK);
    }
    // Division lines (frontage across the facing) with a tick toward the enemy side, and depth.
    for (p, side, d, b) in &lines {
        let l = sim::layout(b, d.posture);
        let (a, e) = sim::line_ends(p.0, d.facing, l.frontage_m);
        let c = if side.0 == PLAYER_SIDE { ui::FRIENDLY } else { ui::ENEMY };
        let f = Vec2::new(d.facing.0, d.facing.1);
        gizmos.line_2d(v(a), v(e), c);
        gizmos.line_2d(v(p.0), v(p.0) + f * ring * 1.6, c);
        gizmos.line_2d(v(p.0), v(p.0) - f * l.depth_m, c.with_alpha(0.25));
    }
    // Units in combat this step.
    for (a, b) in &engagements.0 {
        if let (Ok(pa), Ok(pb)) = (positions.get(*a), positions.get(*b)) {
            gizmos.line_2d(v(pa.0), v(pb.0), Color::srgb(1.0, 0.1, 0.1));
        }
    }
    // Pockets: magenta ring. Retreat: white arrow along the retreat direction.
    for (p, f) in &fronts {
        if f.encircled {
            gizmos.circle_2d(v(p.0), ring * 1.25, Color::srgb(1.0, 0.0, 1.0));
        }
        if let Some((dx, dy)) = f.retreat {
            gizmos.arrow_2d(v(p.0), v(p.0) + Vec2::new(dx, dy) * ring * 2.0, Color::WHITE);
        }
    }

    let Some(sel) = selected.0 else { return };
    let Some((_, pos, order)) = units.iter().find(|(id, _, _)| **id == sel) else { return };
    gizmos.circle_2d(v(pos.0), ring, Color::srgb(1.0, 0.9, 0.0));
    if let Some(order) = order {
        let cell_m = map.0.terrain.cell_m;
        let mut prev = v(pos.0);
        for &(cx, cy) in &order.path[order.next..] {
            let p = Vec2::new((cx as f32 + 0.5) * cell_m, (cy as f32 + 0.5) * cell_m);
            gizmos.line_2d(prev, p, Color::srgb(1.0, 0.9, 0.0));
            prev = p;
        }
    }
}

/// Space: pause/resume. 1-4: 1x / 2x / 10x / 60x.
fn speed_controls(
    keys: Res<ButtonInput<KeyCode>>,
    mut speed: ResMut<Speed>,
    mut fixed: ResMut<Time<Fixed>>,
    mut virt: ResMut<Time<Virtual>>,
) {
    if keys.just_pressed(KeyCode::Space) {
        if virt.is_paused() {
            virt.unpause();
        } else {
            virt.pause();
        }
    }
    let digits = [KeyCode::Digit1, KeyCode::Digit2, KeyCode::Digit3, KeyCode::Digit4];
    if let Some(i) = digits.iter().position(|k| keys.just_pressed(*k)) {
        speed.0 = i;
        fixed.set_timestep_hz(SPEEDS[i]);
    }
}
