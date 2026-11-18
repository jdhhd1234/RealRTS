//! C2 workstation layout (ui_improve.md, stage 1): top status bar, left ORBAT / Rear / Layers tabs,
//! right context panel with orders, bottom Alerts / Event Log. Status comes first, numbers second.
//! Each panel is rebuilt only when its content changes.

use crate::editor::{Editor, Tool, side_label};
use crate::files::{FileStatus, Loader};
use crate::symbols::main_arm;
use crate::{OverlayMode, PLAYER_SIDE, Raised, SPEEDS, Selected, ShowNetwork, Speed};
use bevy::ecs::hierarchy::ChildSpawnerCommands;
use bevy::prelude::*;
use bevy::ui::RelativeCursorPosition;
use mapdata::RegionId;
use sim::{
    AMMO, AMMO_PER_STEP_T, Arm, Battalions, CommandQueue, Depot, EconomyState, EdgeState, EventLog, FOOD, FUEL, Front,
    IDLE_USE_T_PER_DAY, MOVING_FUEL_T_PER_DAY, Map, MoveOrder, Nations, Partisan, PartisanState, Position, RegionStatus,
    RiskGrade, Side, SimCommand, SimClock, StockTarget, Stocks, SupplyBases, SupplyPriority, Territory, UnitId, UnitName,
};

pub const TOP_H: f32 = 30.0;
pub const LEFT_W: f32 = 270.0;
pub const RIGHT_W: f32 = 300.0;
pub const BOTTOM_H: f32 = 150.0;

// Calm staff-room palette: dark slate panels, desaturated sides, amber warnings.
const PANEL_BG: Color = Color::srgba(0.07, 0.09, 0.11, 0.92);
const PANEL_BORDER: Color = Color::srgb(0.22, 0.27, 0.32);
const BUTTON_BG: Color = Color::srgb(0.13, 0.16, 0.19);
const BUTTON_HOVER: Color = Color::srgb(0.19, 0.23, 0.27);
const BUTTON_ACTIVE: Color = Color::srgb(0.16, 0.28, 0.36);
pub const FRIENDLY: Color = Color::srgb(0.40, 0.60, 0.80);
pub const ENEMY: Color = Color::srgb(0.78, 0.38, 0.35);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tone {
    Normal,
    Dim,
    Info,
    Warn,
    Alert,
    Good,
    Friendly,
    Enemy,
}

impl Tone {
    fn color(self) -> Color {
        match self {
            Tone::Normal => Color::srgb(0.85, 0.87, 0.88),
            Tone::Dim => Color::srgb(0.55, 0.60, 0.64),
            Tone::Info => Color::srgb(0.45, 0.65, 0.90),
            Tone::Warn => Color::srgb(0.95, 0.70, 0.25),
            Tone::Alert => Color::srgb(0.92, 0.32, 0.28),
            Tone::Good => Color::srgb(0.45, 0.80, 0.50),
            Tone::Friendly => FRIENDLY,
            Tone::Enemy => ENEMY,
        }
    }
}

/// What a button does.
#[derive(Component, Clone, Copy, PartialEq, Debug)]
pub enum Action {
    LeftTab(LeftTab),
    BottomTab(BottomTab),
    Select(UnitId),
    Deselect,
    ToggleHold,
    TogglePriority,
    Layer(OverlayMode),
    ToggleNetwork,
    Build,
    Sweep(RegionId, bool),
    Requisition(RegionId),
    Pause,
    Speed(usize),
    ToggleFileMenu,
    OpenMap,
    LoadTestMap,
    Quit,
    /// Editor on (restarts at the scenario start) / Play.
    ToggleEditor,
    Tool(Tool),
    EditSide(u8),
    EditFlip,
    EditDelete,
    SaveScenario,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum LeftTab {
    #[default]
    Orbat,
    Rear,
    Layers,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum BottomTab {
    #[default]
    Alerts,
    Events,
}

#[derive(Resource, Default)]
pub struct UiState {
    left: LeftTab,
    bottom: BottomTab,
    file_menu: bool,
}

/// One UI element. Panels compare these lists to decide whether to rebuild.
#[derive(Clone, PartialEq, Debug)]
enum El {
    Title(String),
    Text(String, Tone),
    Kv(String, String, Tone),
    /// (label, action, active). Rendered as one wrapping row.
    Buttons(Vec<(String, Action, bool)>),
    /// Full-width button.
    Wide(String, Action, bool),
}

#[derive(Component, Clone, Copy, PartialEq, Eq)]
pub enum Slot {
    Top,
    Left,
    Right,
    Bottom,
    /// File drop-down under the top bar; hidden while empty.
    Menu,
}

/// Panels that swallow mouse input so clicks on them never reach the map.
#[derive(Component)]
pub struct UiBlock;

pub fn cursor_over_ui(panels: &Query<&RelativeCursorPosition, With<UiBlock>>) -> bool {
    panels.iter().any(|p| p.cursor_over())
}

pub struct UiPlugin;

impl Plugin for UiPlugin {
    fn build(&self, app: &mut App) {
        app.init_resource::<UiState>()
            .add_systems(Startup, spawn_panels)
            .add_systems(Update, (ui_actions, button_hover, build_panels).chain());
    }
}

fn spawn_panels(mut commands: Commands) {
    let panel = |slot: Slot, node: Node| {
        (
            Node {
                position_type: PositionType::Absolute,
                border: UiRect::all(px(1)),
                padding: UiRect::all(px(8)),
                overflow: Overflow::clip(),
                ..node
            },
            BackgroundColor(PANEL_BG),
            BorderColor::all(PANEL_BORDER),
            RelativeCursorPosition::default(),
            UiBlock,
            slot,
        )
    };
    let column = |row_gap: f32| Node { flex_direction: FlexDirection::Column, row_gap: px(row_gap), ..default() };
    commands.spawn(panel(
        Slot::Top,
        Node {
            left: px(0),
            right: px(0),
            top: px(0),
            height: px(TOP_H),
            padding: UiRect::axes(px(10), px(4)),
            flex_direction: FlexDirection::Row,
            align_items: AlignItems::Center,
            column_gap: px(18),
            ..default()
        },
    ));
    commands.spawn(panel(
        Slot::Left,
        Node { left: px(0), top: px(TOP_H), bottom: px(BOTTOM_H), width: px(LEFT_W), ..column(3.0) },
    ));
    commands.spawn(panel(
        Slot::Right,
        Node { right: px(0), top: px(TOP_H), bottom: px(BOTTOM_H), width: px(RIGHT_W), ..column(3.0) },
    ));
    commands.spawn(panel(Slot::Bottom, Node { left: px(0), right: px(0), bottom: px(0), height: px(BOTTOM_H), ..column(2.0) }));
    commands.spawn((
        panel(Slot::Menu, Node { left: px(4), top: px(TOP_H), width: px(420), display: Display::None, ..column(4.0) }),
        GlobalZIndex(10),
    ));
}

#[allow(clippy::too_many_arguments)]
fn ui_actions(
    pressed: Query<(&Interaction, &Action), Changed<Interaction>>,
    mut ui: ResMut<UiState>,
    mut selected: ResMut<Selected>,
    mut overlay: ResMut<OverlayMode>,
    mut network: ResMut<ShowNetwork>,
    mut queue: ResMut<CommandQueue>,
    units: Query<(&UnitId, &SupplyPriority, &Front)>,
    econ: Res<EconomyState>,
    mut raised: ResMut<Raised>,
    (mut speed, mut fixed, mut virt): (ResMut<Speed>, ResMut<Time<Fixed>>, ResMut<Time<Virtual>>),
    (loader, mut status, mut exit): (Res<Loader>, ResMut<FileStatus>, MessageWriter<AppExit>),
    mut ed: ResMut<Editor>,
) {
    for (interaction, action) in &pressed {
        if *interaction != Interaction::Pressed {
            continue;
        }
        let sel = selected.0.and_then(|s| units.iter().find(|(id, ..)| **id == s));
        if !matches!(action, Action::ToggleFileMenu) {
            ui.file_menu = false; // any other click closes the menu
        }
        match *action {
            Action::ToggleFileMenu => ui.file_menu = !ui.file_menu,
            Action::OpenMap => loader.open_dialog(&mut status),
            Action::LoadTestMap => loader.load_test_map(),
            Action::Quit => {
                exit.write(AppExit::Success);
            }
            Action::ToggleEditor => ed.toggle(&mut virt, &mut selected),
            Action::Tool(t) => ed.tool = t,
            Action::EditSide(side) => ed.side = side,
            Action::EditFlip => {
                if let Some(i) = ed.selected_index(&selected) {
                    ed.flip_side(i);
                }
            }
            Action::EditDelete => {
                if let Some(i) = ed.selected_index(&selected) {
                    ed.delete_unit(i, &mut selected);
                }
            }
            Action::SaveScenario => loader.save_dialog(&ed.baked, &mut status),
            Action::LeftTab(t) => ui.left = t,
            Action::BottomTab(t) => ui.bottom = t,
            Action::Select(id) => selected.0 = Some(id),
            Action::Deselect => selected.0 = None,
            Action::ToggleHold => {
                if let Some((id, _, f)) = sel {
                    queue.0.push(SimCommand::SetHold { unit: *id, hold: !f.hold });
                }
            }
            Action::TogglePriority => {
                if let Some((id, p, _)) = sel {
                    queue.0.push(SimCommand::SetSupplyPriority { unit: *id, priority: if p.0 == 0 { 5 } else { 0 } });
                }
            }
            Action::Layer(m) => *overlay = m,
            Action::ToggleNetwork => network.0 = !network.0,
            Action::Build => queue.0.push(raised.order()),
            Action::Sweep(region, harsh) => queue.0.push(SimCommand::AntiPartisan { region, harsh }),
            Action::Requisition(region) => {
                let on = !econ.requisition[region.0 as usize];
                queue.0.push(SimCommand::SetRequisition { region, on });
            }
            Action::Pause => {
                if virt.is_paused() {
                    virt.unpause();
                } else {
                    virt.pause();
                }
            }
            Action::Speed(i) => {
                speed.0 = i;
                fixed.set_timestep_hz(SPEEDS[i]);
                virt.unpause();
            }
        }
    }
}

/// Active state is baked into the button's base color at build time (see `ActiveButton`).
#[derive(Component)]
struct ActiveButton(bool);

fn button_hover(mut q: Query<(&Interaction, &ActiveButton, &mut BackgroundColor), Changed<Interaction>>) {
    for (i, active, mut bg) in &mut q {
        bg.0 = match (i, active.0) {
            (Interaction::Hovered | Interaction::Pressed, _) => BUTTON_HOVER,
            (_, true) => BUTTON_ACTIVE,
            _ => BUTTON_BG,
        };
    }
}

/// Derived unit status for the "state first" display.
struct Status {
    supply_frac: f32,
    forecast_h: f32,
    combat_ready: bool,
    mobility_limited: bool,
}

fn status(stocks: &Stocks, target: &StockTarget, bats: &Battalions, front: &Front, moving: bool) -> Status {
    let frac = |g: usize| if target.0[g] > 0.0 { stocks.0[g] / target.0[g] } else { 1.0 };
    let mut use_per_day = IDLE_USE_T_PER_DAY;
    if moving {
        use_per_day[FUEL] = MOVING_FUEL_T_PER_DAY;
    }
    if front.ratio.is_some() {
        use_per_day[AMMO] = AMMO_PER_STEP_T * 288.0;
    }
    let forecast_h = (0..3).map(|g| stocks.0[g] / use_per_day[g] * 24.0).fold(f32::INFINITY, f32::min);
    Status {
        supply_frac: (0..3).map(frac).fold(1.0, f32::min),
        forecast_h,
        combat_ready: front.org >= 0.5 && bats.total() >= 5.0 && frac(AMMO) >= 0.3,
        mobility_limited: frac(FUEL) < 0.2,
    }
}

fn supply_grade(frac: f32) -> (&'static str, Tone) {
    match frac {
        f if f >= 0.6 => ("GREEN", Tone::Good),
        f if f >= 0.3 => ("AMBER", Tone::Warn),
        _ => ("RED", Tone::Alert),
    }
}

fn posture(front: &Front, moving: bool) -> &'static str {
    if front.encircled {
        "Encircled"
    } else if front.retreat.is_some() {
        "Retreat"
    } else if front.hold {
        "Hold"
    } else if moving && front.ratio.is_some() {
        "Attack"
    } else if moving {
        "Move"
    } else if front.ratio.is_some() {
        "Defend (engaged)"
    } else {
        "Defend"
    }
}

fn dominant_arm(b: &Battalions) -> &'static str {
    match main_arm(b) {
        Arm::Armor => "Armor",
        Arm::Artillery => "Artillery",
        Arm::Infantry => "Infantry",
    }
}

fn side_name(s: u8) -> &'static str {
    if s == PLAYER_SIDE { "Blue" } else { "Red" }
}

type UnitRow<'a> = (
    &'a UnitId,
    &'a UnitName,
    &'a Side,
    &'a Position,
    &'a Stocks,
    &'a StockTarget,
    &'a SupplyPriority,
    &'a Battalions,
    &'a Front,
    Has<MoveOrder>,
);

#[allow(clippy::too_many_arguments)]
fn build_panels(
    mut commands: Commands,
    panels: Query<(Entity, &Slot)>,
    mut last: Local<Vec<(Slot, Vec<El>)>>,
    ui: Res<UiState>,
    selected: Res<Selected>,
    units: Query<UnitRow>,
    partisans: Query<(&UnitName, &Side), With<Partisan>>,
    depots: Query<(Entity, &Depot, &Stocks)>,
    (map, territory, overlay, network): (Res<Map>, Res<Territory>, Res<OverlayMode>, Res<ShowNetwork>),
    (bases, nations, econ, pstate, log, edges): (
        Res<SupplyBases>,
        Res<Nations>,
        Res<EconomyState>,
        Res<PartisanState>,
        Res<EventLog>,
        Res<EdgeState>,
    ),
    (clock, speed, virt, file): (Res<SimClock>, Res<Speed>, Res<Time<Virtual>>, Res<FileStatus>),
    mut nodes: Query<(&Slot, &mut Node)>,
    ed: Res<Editor>,
    deps: Query<(&UnitId, &sim::Deployment)>,
) {
    let m = &map.0;
    let node_name = |n: mapdata::NodeId| m.graph.node(n).name.clone().unwrap_or_default();
    let pct = |have: f32, want: f32| if want > 0.0 { (have / want * 100.0).round() as i32 } else { 100 };
    let mut own: Vec<_> = units.iter().filter(|u| u.2.0 == PLAYER_SIDE).collect();
    own.sort_by_key(|u| u.0.0);
    let regions = sim::region_status(m, &territory);

    // ---- Alerts (derived from current state, most urgent first) ----
    let mut alerts: Vec<(Tone, String)> = Vec::new();
    for (_, name, _, pos, st, tg, _, b, f, moving) in &own {
        let s = status(st, tg, b, f, *moving);
        if f.encircled {
            alerts.push((Tone::Alert, format!("[ALERT] {} encircled - no supply", name.0)));
        }
        if f.retreat.is_some() {
            alerts.push((Tone::Warn, format!("[WARN] {} retreating, org {:.0}%", name.0, f.org * 100.0)));
        }
        if s.forecast_h < 12.0 {
            alerts.push((Tone::Warn, format!("[WARN] {} supplies exhausted in ~{:.0}h", name.0, s.forecast_h)));
        }
        if !f.encircled && sim::nearest_depot(&m.graph, depots.iter().map(|(e, d, _)| (e, d)), PLAYER_SIDE, pos.0).is_none() {
            alerts.push((Tone::Warn, format!("[WARN] {} out of truck range", name.0)));
        }
    }
    for (i, e) in m.graph.edges.iter().enumerate() {
        if edges.damage[i] >= 0.3 {
            let line = format!("{} - {}", node_name(e.a), node_name(e.b));
            alerts.push((Tone::Warn, format!("[WARN] Line {line} damaged {:.0}%", edges.damage[i] * 100.0)));
        }
    }
    for (r, st) in regions.iter().enumerate() {
        if let RegionStatus::Occupied { by } = st
            && *by == PLAYER_SIDE
            && matches!(sim::risk_grade(pstate.pool[r]), RiskGrade::Medium | RiskGrade::High)
        {
            let grade = sim::risk_grade(pstate.pool[r]);
            alerts.push((Tone::Warn, format!("[WARN] {} partisan risk {grade:?}", m.regions[r].name)));
        }
    }
    alerts.sort_by_key(|(t, _)| *t != Tone::Alert);
    let urgent = alerts.iter().filter(|(t, _)| *t == Tone::Alert).count();

    // ---- Top bar ----
    let (have, want) = own.iter().fold((0.0, 0.0), |(h, w), u| (h + u.4.0.iter().sum::<f32>(), w + u.5.0.iter().sum::<f32>()));
    let supply_pct = pct(have, want);
    let mut speed_buttons = vec![("II".to_string(), Action::Pause, virt.is_paused())];
    for (i, s) in SPEEDS.iter().enumerate() {
        speed_buttons.push((format!("{s}x"), Action::Speed(i), !virt.is_paused() && speed.0 == i));
    }
    let short: String = file.text.chars().take(60).collect();
    let short = if short.len() < file.text.len() { format!("{short}...") } else { short };
    let file_tone = if file.error { Tone::Alert } else { Tone::Dim };
    let mut top = vec![El::Buttons(vec![
        ("File".into(), Action::ToggleFileMenu, ui.file_menu),
        (if ed.on { "> Play" } else { "Editor" }.into(), Action::ToggleEditor, ed.on),
    ])];
    if ed.on {
        top.push(El::Text("SCENARIO EDITOR - start state, paused".into(), Tone::Warn));
    } else {
        top.push(El::Text(format!("D+{} {:02}:{:02}", clock.day(), clock.hour(), clock.minute()), Tone::Normal));
        top.push(El::Buttons(speed_buttons));
    }
    top.extend([
        El::Text("BLUE FORCES".into(), Tone::Friendly),
        El::Text(format!("Supply {supply_pct}%"), supply_grade(supply_pct as f32 / 100.0).1),
        El::Text(
            format!("SITREP: {} alerts", alerts.len()),
            if urgent > 0 { Tone::Alert } else if alerts.is_empty() { Tone::Dim } else { Tone::Warn },
        ),
        El::Text(format!("Layer: {:?}{}", *overlay, if network.0 { " + Network" } else { "" }), Tone::Dim),
        El::Text(short, file_tone),
    ]);

    // ---- File menu ----
    let mut menu = Vec::new();
    if ui.file_menu {
        let open = if file.busy { "Opening... (see file dialog)" } else { "Open map (.gpkg / .geojson / .shp / .ron)..." };
        menu.push(El::Wide(open.into(), Action::OpenMap, false));
        menu.push(El::Wide("Load built-in test map".into(), Action::LoadTestMap, false));
        menu.push(El::Wide("Save scenario (.ron)...".into(), Action::SaveScenario, false));
        menu.push(El::Wide("Quit".into(), Action::Quit, false));
        menu.push(El::Text(file.text.clone(), file_tone));
        menu.push(El::Title("QGIS LAYERS (projected CRS, meters)".into()));
        menu.push(El::Text("GeoJSON/Shapefile: one file per layer, named terrain.shp, rail.geojson, ...; any one opens the folder".into(), Tone::Dim));
        for line in [
            "terrain  polygons  kind: plains|forest|marsh|mountain|urban|water",
            "regions  polygons  name, side, control, fuel/ammo/food/equipment",
            "nodes    points    name, kind: junction|station|port, base_side, depot_side",
            "rail / roads / rivers  lines  capacity (t/day), rail: gauge",
            "units    points    name, side",
            "Loading restarts the game on the new map, paused.",
        ] {
            menu.push(El::Text(line.into(), Tone::Dim));
        }
    }

    // ---- Left: tabs ----
    let mut left = vec![El::Buttons(vec![
        ("ORBAT".into(), Action::LeftTab(LeftTab::Orbat), ui.left == LeftTab::Orbat),
        ("Rear".into(), Action::LeftTab(LeftTab::Rear), ui.left == LeftTab::Rear),
        ("Layers".into(), Action::LeftTab(LeftTab::Layers), ui.left == LeftTab::Layers),
    ])];
    if ed.on {
        left = editor_palette(&ed);
    }
    match ui.left {
        _ if ed.on => {}
        LeftTab::Orbat => {
            left.push(El::Title("BLUE FORCES".into()));
            for (id, name, _, _, st, tg, _, b, f, moving) in &own {
                let s = status(st, tg, b, f, *moving);
                left.push(El::Wide(format!("{}  [{}]", name.0, posture(f, *moving)), Action::Select(**id), selected.0 == Some(**id)));
                let (grade, tone) = supply_grade(s.supply_frac);
                let tone = if f.encircled { Tone::Alert } else { tone };
                left.push(El::Text(format!("   Str {:.1}  Org {:.0}%  Sup {grade}", b.total(), f.org * 100.0), tone));
            }
            left.push(El::Title("REAR COMMAND".into()));
            let mut ds: Vec<_> = depots.iter().filter(|(_, d, _)| d.side == PLAYER_SIDE).collect();
            ds.sort_by_key(|(_, d, _)| d.node);
            for (_, d, st) in ds {
                let fill = pct(st.0.iter().sum(), sim::DEPOT_TARGET.iter().sum());
                left.push(El::Kv(format!("Depot {}", node_name(d.node)), format!("{fill}%"), supply_grade(fill as f32 / 100.0).1));
            }
            for b in bases.0.iter().filter(|b| b.side == PLAYER_SIDE) {
                left.push(El::Kv(format!("Base {}", node_name(b.node)), format!("{:.0} t", b.stock.iter().sum::<f32>()), Tone::Normal));
            }
            left.push(El::Kv("Equipment".into(), format!("{:.0} t", nations.equipment[PLAYER_SIDE as usize]), Tone::Normal));
            for b in econ.builds.iter().filter(|b| b.side == PLAYER_SIDE) {
                let days = b.ready_tick.saturating_sub(clock.tick).div_ceil(sim::TICKS_PER_DAY);
                left.push(El::Kv(format!("Raising {}", b.name), format!("~{days} d"), Tone::Info));
            }
            left.push(El::Wide("Raise new division (B)".into(), Action::Build, false));
            left.push(El::Title("CONTACTS".into()));
            let mut enemies: Vec<_> = units.iter().filter(|u| u.2.0 != PLAYER_SIDE).collect();
            enemies.sort_by_key(|u| u.0.0);
            for (_, name, _, _, _, _, _, _, f, _) in enemies {
                let state = if f.ratio.is_some() { "engaged" } else { "" };
                left.push(El::Kv(name.0.clone(), state.into(), Tone::Enemy));
            }
            for (name, side) in &partisans {
                left.push(El::Text(name.0.clone(), if side.0 == PLAYER_SIDE { Tone::Friendly } else { Tone::Enemy }));
            }
        }
        LeftTab::Rear => {
            left.push(El::Title("REAR AREA SECURITY".into()));
            for (r, st) in regions.iter().enumerate() {
                let name = m.regions[r].name.clone();
                let legal = side_name(territory.legal[r]);
                match st {
                    RegionStatus::Home => left.push(El::Kv(name, format!("{legal}, home"), Tone::Dim)),
                    RegionStatus::Contested => left.push(El::Kv(name, format!("{legal}, contested"), Tone::Warn)),
                    RegionStatus::Occupied { by } => {
                        let grade = sim::risk_grade(pstate.pool[r]);
                        let tone = match grade {
                            RiskGrade::High => Tone::Alert,
                            RiskGrade::Medium => Tone::Warn,
                            _ => Tone::Normal,
                        };
                        left.push(El::Kv(name, format!("held by {}, risk {grade:?}", side_name(*by)), tone));
                        if *by == PLAYER_SIDE {
                            let rid = RegionId(r as u16);
                            left.push(El::Buttons(vec![
                                ("Sweep".into(), Action::Sweep(rid, false), false),
                                ("Reprisal".into(), Action::Sweep(rid, true), false),
                                ("Requisition".into(), Action::Requisition(rid), econ.requisition[r]),
                            ]));
                        }
                    }
                }
            }
            left.push(El::Text("Keys at cursor: K sweep, H reprisal, Q requisition".into(), Tone::Dim));
        }
        LeftTab::Layers => {
            left.push(El::Title("TERRITORY".into()));
            for (label, mode) in [
                ("Military control", OverlayMode::Military),
                ("Legal territory", OverlayMode::Legal),
                ("Regions", OverlayMode::Regions),
                ("Partisan risk", OverlayMode::Risk),
                ("Off", OverlayMode::Off),
            ] {
                left.push(El::Wide(label.into(), Action::Layer(mode), *overlay == mode));
            }
            left.push(El::Title("NETWORK".into()));
            left.push(El::Wide(format!("Supply network: {}", if network.0 { "ON" } else { "OFF" }), Action::ToggleNetwork, network.0));
            left.push(El::Title("LEGEND".into()));
            for (t, tone) in [
                ("Orange line: carrying supply", Tone::Warn),
                ("Red line: damaged", Tone::Alert),
                ("Red link: units in combat", Tone::Alert),
                ("Magenta ring: encircled", Tone::Normal),
                ("White arrow: retreating", Tone::Normal),
                ("Symbols: NATO APP-6 (blue friend, red hostile)", Tone::Normal),
                ("T cycles territory layer", Tone::Dim),
            ] {
                left.push(El::Text(t.into(), tone));
            }
        }
    }

    // ---- Right: context panel ----
    let mut right = Vec::new();
    if ed.on {
        right = editor_detail(&ed, &selected, m);
    }
    match selected.0.and_then(|s| units.iter().find(|u| *u.0 == s)) {
        _ if ed.on => {}
        Some((_, name, side, pos, st, tg, prio, b, f, moving)) => {
            let s = status(st, tg, b, f, moving);
            right.push(El::Title("UNIT DETAIL".into()));
            right.push(El::Text(name.0.clone(), if side.0 == PLAYER_SIDE { Tone::Friendly } else { Tone::Enemy }));
            right.push(El::Kv("Side".into(), side_name(side.0).into(), Tone::Normal));
            right.push(El::Kv("Type".into(), dominant_arm(b).into(), Tone::Normal));
            right.push(El::Kv("Posture".into(), posture(f, moving).into(), Tone::Normal));
            if let Some((_, d)) = deps.iter().find(|(id, _)| Some(**id) == selected.0) {
                let l = sim::layout(b, d.posture);
                right.push(El::Title("DEPLOYMENT".into()));
                right.push(El::Kv("Formation".into(), format!("{:?}", d.posture), Tone::Normal));
                right.push(El::Kv(
                    "Frontage / depth".into(),
                    format!("{:.0} / {:.0} km", l.frontage_m / 1000.0, l.depth_m / 1000.0),
                    Tone::Normal,
                ));
                right.push(El::Text(
                    format!("line {:.1}  reserve {:.1}  support {:.1} bn", l.strength[0], l.strength[1], l.strength[2]),
                    Tone::Dim,
                ));
            }
            right.push(El::Title("STATUS".into()));
            let yes_no = |v: bool, good_if: bool| (if v { "YES" } else { "NO" }.to_string(), if v == good_if { Tone::Good } else { Tone::Warn });
            let (t, tone) = yes_no(s.combat_ready, true);
            right.push(El::Kv("Combat ready".into(), t, tone));
            let (grade, tone) = supply_grade(s.supply_frac);
            right.push(El::Kv("Supply".into(), grade.into(), tone));
            let forecast = if s.forecast_h > 999.0 { "> 40 d".to_string() } else { format!("{:.0} h", s.forecast_h) };
            right.push(El::Kv("Sustainment".into(), forecast, if s.forecast_h < 12.0 { Tone::Warn } else { Tone::Normal }));
            let (t, tone) = yes_no(s.mobility_limited, false);
            right.push(El::Kv("Mobility limited".into(), t, tone));
            let link = sim::nearest_depot(&m.graph, depots.iter().map(|(e, d, _)| (e, d)), side.0, pos.0);
            let (iso, tone) = match (f.encircled, link) {
                (true, _) => ("HIGH (pocket)".to_string(), Tone::Alert),
                (_, None) => ("MEDIUM (no depot in range)".to_string(), Tone::Warn),
                (_, Some((dist, _))) => (format!("LOW ({:.0} km to depot)", dist / 1000.0), Tone::Good),
            };
            right.push(El::Kv("Isolation risk".into(), iso, tone));
            let contact = match f.ratio {
                None => ("None".to_string(), Tone::Dim),
                Some(r) if r >= sim::ADVANCE_RATIO => (format!("Winning, R {r:.2}"), Tone::Good),
                Some(r) if r <= sim::RETREAT_RATIO => (format!("Losing, R {r:.2}"), Tone::Alert),
                Some(r) => (format!("Stalemate, R {r:.2}"), Tone::Warn),
            };
            right.push(El::Kv("Contact".into(), contact.0, contact.1));
            right.push(El::Title("DETAIL".into()));
            right.push(El::Kv("Strength".into(), format!("{:.1} / {} bn", b.total(), b.0.len()), Tone::Normal));
            right.push(El::Kv("Organization".into(), format!("{:.0}%", f.org * 100.0), Tone::Normal));
            for (label, g) in [("Fuel", FUEL), ("Ammo", AMMO), ("Food", FOOD)] {
                right.push(El::Kv(label.into(), format!("{:.0} / {:.0} t", st.0[g], tg.0[g]), Tone::Normal));
            }
            let arm = |a: Arm| b.0.iter().filter(|x| x.arm == a).map(|x| x.strength).sum::<f32>();
            right.push(El::Text(
                format!("inf {:.1}  armor {:.1}  arty {:.1}", arm(Arm::Infantry), arm(Arm::Armor), arm(Arm::Artillery)),
                Tone::Dim,
            ));
            if side.0 == PLAYER_SIDE {
                right.push(El::Title("ORDERS".into()));
                right.push(El::Text("RMB on map: Move / Attack (engages on contact)".into(), Tone::Dim));
                right.push(El::Wide(format!("Hold - no retreat (G): {}", if f.hold { "ON" } else { "OFF" }), Action::ToggleHold, f.hold));
                right.push(El::Wide(
                    format!("Resupply priority (P): {}", if prio.0 > 0 { "HIGH" } else { "NORMAL" }),
                    Action::TogglePriority,
                    prio.0 > 0,
                ));
                right.push(El::Wide("Deselect".into(), Action::Deselect, false));
            }
        }
        None => {
            right.push(El::Title("THEATER SUMMARY".into()));
            right.push(El::Kv("Divisions".into(), own.len().to_string(), Tone::Normal));
            right.push(El::Kv("Supply level".into(), format!("{supply_pct}%"), supply_grade(supply_pct as f32 / 100.0).1));
            let pockets = own.iter().filter(|u| u.8.encircled).count();
            right.push(El::Kv("Units encircled".into(), pockets.to_string(), if pockets > 0 { Tone::Alert } else { Tone::Good }));
            let engaged = own.iter().filter(|u| u.8.ratio.is_some()).count();
            right.push(El::Kv("Units engaged".into(), engaged.to_string(), Tone::Normal));
            right.push(El::Text("Select a unit: LMB on map or ORBAT list".into(), Tone::Dim));
        }
    }

    // ---- Bottom: Alerts / Event Log ----
    let mut bottom = vec![El::Buttons(vec![
        (format!("Alerts ({})", alerts.len()), Action::BottomTab(BottomTab::Alerts), ui.bottom == BottomTab::Alerts),
        ("Event Log".into(), Action::BottomTab(BottomTab::Events), ui.bottom == BottomTab::Events),
    ])];
    match ui.bottom {
        BottomTab::Alerts if alerts.is_empty() => bottom.push(El::Text("No alerts.".into(), Tone::Dim)),
        BottomTab::Alerts => bottom.extend(alerts.into_iter().take(7).map(|(t, s)| El::Text(s, t))),
        BottomTab::Events => {
            for (tick, text) in log.0.iter().rev().take(7) {
                let c = SimClock { tick: *tick };
                let warn = ["Partisan", "Sabotage", "Cannot", "reprisals"].iter().any(|k| text.contains(k));
                let (tag, tone) = if warn { ("WARN", Tone::Warn) } else { ("INFO", Tone::Info) };
                bottom.push(El::Text(format!("D+{} {:02}:{:02} [{tag}] {text}", c.day(), c.hour(), c.minute()), tone));
            }
        }
    }

    // ---- Rebuild changed panels ----
    let panels_list = [(Slot::Top, top), (Slot::Left, left), (Slot::Right, right), (Slot::Bottom, bottom), (Slot::Menu, menu)];
    for (slot, els) in panels_list {
        if last.iter().any(|(s, e)| *s == slot && *e == els) {
            continue;
        }
        if slot == Slot::Menu
            && let Some((_, mut node)) = nodes.iter_mut().find(|(s, _)| **s == Slot::Menu)
        {
            node.display = if els.is_empty() { Display::None } else { Display::Flex };
        }
        if let Some((entity, _)) = panels.iter().find(|(_, s)| **s == slot) {
            commands.entity(entity).despawn_related::<Children>().with_children(|p| render(p, &els));
        }
        last.retain(|(s, _)| *s != slot);
        last.push((slot, els));
    }
}

fn editor_palette(ed: &Editor) -> Vec<El> {
    let s = &ed.baked.scenario;
    let mut v = vec![El::Title("SCENARIO EDITOR".into()), El::Title("SIDE".into())];
    v.push(El::Buttons(vec![
        ("Blue".into(), Action::EditSide(PLAYER_SIDE), ed.side == PLAYER_SIDE),
        ("Red".into(), Action::EditSide(1), ed.side == 1),
    ]));
    v.push(El::Title("TOOLS".into()));
    v.extend(Tool::ALL.iter().map(|(t, label)| El::Wide((*label).into(), Action::Tool(*t), ed.tool == *t)));
    v.push(El::Title("SCENARIO".into()));
    for side in [PLAYER_SIDE, 1] {
        let units = s.units.iter().filter(|u| u.side == side).count();
        let depots = s.depots.iter().filter(|d| d.0 == side).count();
        let bases = s.bases.iter().filter(|b| b.0 == side).count();
        let tone = if side == PLAYER_SIDE { Tone::Friendly } else { Tone::Enemy };
        v.push(El::Kv(side_label(side).into(), format!("{units} div, {depots} depots, {bases} bases"), tone));
    }
    v.push(El::Wide("Save scenario (.ron)...".into(), Action::SaveScenario, false));
    v.push(El::Text("LMB uses the tool. Del deletes the selected unit. Play runs from here; Editor returns here.".into(), Tone::Dim));
    v
}

fn editor_detail(ed: &Editor, selected: &Selected, m: &mapdata::MapData) -> Vec<El> {
    let Some(i) = ed.selected_index(selected) else {
        let hint = match ed.tool {
            Tool::Select => "Click a unit to select it; drag to move.",
            Tool::Division => "Click on the map to place a division of the chosen side.",
            Tool::Depot => "Click near a node: adds a field depot, or removes the one there.",
            Tool::Base => "Click near a node: adds a supply base (national supply enters here), or removes it.",
            Tool::Region => "Click a region: chosen side becomes legal owner and controller.",
            Tool::Occupy => "Click a region: chosen side occupies it (owner unchanged).",
            Tool::Delete => "Click a unit, depot or base to delete it.",
        };
        let warn = [PLAYER_SIDE, 1]
            .into_iter()
            .filter(|side| !ed.baked.scenario.bases.iter().any(|b| b.0 == *side))
            .map(|side| El::Text(format!("{} has no supply base: it gets no supply.", side_label(side)), Tone::Warn));
        return [El::Title("TOOL".into()), El::Text(hint.into(), Tone::Normal)].into_iter().chain(warn).collect();
    };
    let u = &ed.baked.scenario.units[i];
    let region = m.terrain.cell_at(u.pos).map(|(cx, cy)| m.regions[m.region_at(cx, cy).0 as usize].name.clone()).unwrap_or_default();
    vec![
        El::Title("UNIT".into()),
        El::Text(u.name.clone(), if u.side == PLAYER_SIDE { Tone::Friendly } else { Tone::Enemy }),
        El::Kv("Side".into(), side_label(u.side).into(), Tone::Normal),
        El::Kv("Position".into(), format!("{:.0}, {:.0} km", u.pos.x / 1000.0, u.pos.y / 1000.0), Tone::Normal),
        El::Kv("Region".into(), region, Tone::Normal),
        El::Text("Standard division: 6 inf, 2 armor, 2 arty bn, full supply.".into(), Tone::Dim),
        El::Buttons(vec![("Switch side".into(), Action::EditFlip, false), ("Delete (Del)".into(), Action::EditDelete, false)]),
    ]
}

fn render(p: &mut ChildSpawnerCommands, els: &[El]) {
    let font = |size: f32| TextFont { font_size: FontSize::Px(size), ..default() };
    for el in els {
        match el {
            El::Title(t) => {
                p.spawn((
                    Text::new(t.clone()),
                    font(12.0),
                    TextColor(Color::srgb(0.60, 0.75, 0.85)),
                    Node { margin: UiRect::top(px(8)), ..default() },
                ));
            }
            El::Text(t, tone) => {
                p.spawn((Text::new(t.clone()), font(13.0), TextColor(tone.color())));
            }
            El::Kv(k, v, tone) => {
                p.spawn(Node { justify_content: JustifyContent::SpaceBetween, column_gap: px(8), ..default() }).with_children(|row| {
                    row.spawn((Text::new(k.clone()), font(13.0), TextColor(Tone::Dim.color())));
                    row.spawn((Text::new(v.clone()), font(13.0), TextColor(tone.color())));
                });
            }
            El::Buttons(list) => {
                p.spawn(Node { flex_wrap: FlexWrap::Wrap, column_gap: px(4), row_gap: px(4), ..default() }).with_children(|row| {
                    for (label, action, active) in list {
                        button(row, label, *action, *active, false);
                    }
                });
            }
            El::Wide(label, action, active) => button(p, label, *action, *active, true),
        }
    }
}

fn button(p: &mut ChildSpawnerCommands, label: &str, action: Action, active: bool, wide: bool) {
    p.spawn((
        Button,
        Node {
            padding: UiRect::axes(px(8), px(3)),
            border: UiRect::all(px(1)),
            width: if wide { percent(100) } else { Val::Auto },
            ..default()
        },
        BackgroundColor(if active { BUTTON_ACTIVE } else { BUTTON_BG }),
        BorderColor::all(if active { Color::srgb(0.95, 0.80, 0.30) } else { PANEL_BORDER }),
        ActiveButton(active),
        action,
    ))
    .with_children(|b| {
        b.spawn((Text::new(label.to_string()), TextFont { font_size: FontSize::Px(13.0), ..default() }, TextColor(Tone::Normal.color())));
    });
}
