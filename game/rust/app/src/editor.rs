//! Scenario editor (sandbox): place / drag / delete divisions, toggle depots and supply bases on
//! nodes, set region owners. Edits change `Editor::baked` (the scenario source) and the sim is
//! reloaded from it, so the map always shows the exact start state. Play runs from that state;
//! going back to the editor restores it. Terrain and the network come from QGIS and are not edited.

use crate::symbols::{SYMBOL_PX, world_per_px};
use crate::ui::{ENEMY, FRIENDLY, UiBlock, cursor_over_ui};
use crate::{PLAYER_SIDE, Selected, cursor_world};
use bevy::prelude::*;
use bevy::ui::RelativeCursorPosition;
use bevy::window::PrimaryWindow;
use mapdata::{Baked, NodeId, Pos, ScenarioData, UnitSpec};
use sim::{Map, UnitId};

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Tool {
    #[default]
    Select,
    Division,
    Depot,
    Base,
    /// Legal owner and controller of the region under the cursor.
    Region,
    /// Military controller only (occupied territory).
    Occupy,
    Delete,
}

impl Tool {
    pub const ALL: [(Tool, &str); 7] = [
        (Tool::Select, "Select / drag unit"),
        (Tool::Division, "Place division"),
        (Tool::Depot, "Depot on node (toggle)"),
        (Tool::Base, "Supply base on node (toggle)"),
        (Tool::Region, "Region owner"),
        (Tool::Occupy, "Occupy region"),
        (Tool::Delete, "Delete"),
    ];
}

#[derive(Resource)]
pub struct Editor {
    pub on: bool,
    pub tool: Tool,
    /// Side used by placing tools.
    pub side: u8,
    /// Scenario source: what a load or the editor set up. Play starts from here.
    pub baked: Baked,
    reload: bool,
    drag: Option<usize>,
}

impl Editor {
    pub fn new(baked: Baked) -> Self {
        Self { on: false, tool: Tool::Select, side: PLAYER_SIDE, baked, reload: false, drag: None }
    }

    /// A newly loaded map replaces the scenario source (the sim was already reset by the loader).
    pub fn set_source(&mut self, baked: Baked) {
        self.baked = baked;
        self.drag = None;
    }

    /// Editor: restart at the scenario start and freeze. Play: run from there.
    pub fn toggle(&mut self, virt: &mut Time<Virtual>, selected: &mut Selected) {
        self.on = !self.on;
        self.drag = None;
        if self.on {
            self.reload = true;
            selected.0 = None;
            virt.pause();
        } else {
            virt.unpause();
        }
    }

    fn scenario(&mut self) -> &mut ScenarioData {
        self.reload = true;
        &mut self.baked.scenario
    }

    /// Unit index of the selection; divisions get ids index + 1 on load.
    pub fn selected_index(&self, selected: &Selected) -> Option<usize> {
        selected.0.map(|id| id.0 as usize - 1).filter(|&i| self.on && i < self.baked.scenario.units.len())
    }

    pub fn delete_unit(&mut self, i: usize, selected: &mut Selected) {
        self.scenario().units.remove(i);
        selected.0 = None;
    }

    pub fn flip_side(&mut self, i: usize) {
        let u = &mut self.scenario().units[i];
        u.side = if u.side == PLAYER_SIDE { 1 } else { PLAYER_SIDE };
    }
}

pub fn editing(ed: Res<Editor>) -> bool {
    ed.on
}

pub fn playing(ed: Res<Editor>) -> bool {
    !ed.on
}

pub fn side_label(side: u8) -> &'static str {
    if side == PLAYER_SIDE { "Blue" } else { "Red" }
}

/// Exclusive: rebuild the sim from the edited scenario (tick 0, paused).
pub fn apply_edits(world: &mut World) {
    let mut ed = world.resource_mut::<Editor>();
    if !std::mem::take(&mut ed.reload) {
        return;
    }
    let baked = ed.baked.clone();
    sim::load_scenario(world, baked);
}

fn nearest_unit(s: &ScenarioData, p: Vec2, reach: f32) -> Option<usize> {
    s.units
        .iter()
        .enumerate()
        .map(|(i, u)| (i, Vec2::new(u.pos.x, u.pos.y).distance(p)))
        .filter(|(_, d)| *d < reach)
        .min_by(|a, b| a.1.total_cmp(&b.1))
        .map(|(i, _)| i)
}

fn nearest_node(m: &mapdata::MapData, p: Vec2, reach: f32) -> Option<NodeId> {
    m.graph
        .nodes
        .iter()
        .enumerate()
        .map(|(i, n)| (NodeId(i as u32), Vec2::new(n.pos.x, n.pos.y).distance(p)))
        .filter(|(_, d)| *d < reach)
        .min_by(|a, b| a.1.total_cmp(&b.1))
        .map(|(n, _)| n)
}

/// Removes every entry at `node`; adds `(side, node)` if there was none. One per node.
fn toggle_at(list: &mut Vec<(u8, NodeId)>, side: u8, node: NodeId) {
    let before = list.len();
    list.retain(|(_, n)| *n != node);
    if list.len() == before {
        list.push((side, node));
    }
}

/// Mouse editing on the map. Delete key removes the selected unit.
#[allow(clippy::too_many_arguments)]
pub fn edit_mouse(
    buttons: Res<ButtonInput<MouseButton>>,
    keys: Res<ButtonInput<KeyCode>>,
    window: Single<&Window, With<PrimaryWindow>>,
    camera: Single<(&Camera, &GlobalTransform, &Projection)>,
    panels: Query<&RelativeCursorPosition, With<UiBlock>>,
    map: Res<Map>,
    mut ed: ResMut<Editor>,
    mut selected: ResMut<Selected>,
    mut sprites: Query<(&UnitId, &mut Transform)>,
) {
    if keys.just_pressed(KeyCode::Delete)
        && let Some(i) = ed.selected_index(&selected)
    {
        ed.delete_unit(i, &mut selected);
    }
    let (camera, cam_t, proj) = *camera;
    let Some(p) = cursor_world(&window, camera, cam_t) else { return };
    let reach = SYMBOL_PX * 0.7 * world_per_px(proj);
    let m = &map.0;
    let pos = Pos { x: p.x, y: p.y };

    // Dragging: move the sprite only; the scenario changes on release.
    if let Some(i) = ed.drag {
        if buttons.pressed(MouseButton::Left) {
            if let Some((_, mut t)) = sprites.iter_mut().find(|(id, _)| id.0 as usize == i + 1) {
                t.translation.x = p.x;
                t.translation.y = p.y;
            }
            return;
        }
        ed.drag = None;
        let old = ed.baked.scenario.units[i].pos;
        if m.terrain.cell_at(pos).is_some() && Vec2::new(old.x, old.y).distance(p) > reach * 0.2 {
            ed.scenario().units[i].pos = pos;
        } else {
            ed.reload = true; // snap the sprite back
        }
        return;
    }

    if !buttons.just_pressed(MouseButton::Left) || cursor_over_ui(&panels) {
        return;
    }
    let side = ed.side;
    let region = m.terrain.cell_at(pos).map(|(cx, cy)| m.region_at(cx, cy).0 as usize);
    match ed.tool {
        Tool::Select => {
            let hit = nearest_unit(&ed.baked.scenario, p, reach);
            selected.0 = hit.map(|i| UnitId(i as u32 + 1));
            ed.drag = hit;
        }
        Tool::Division => {
            if region.is_none() {
                return;
            }
            let s = ed.scenario();
            let n = s.units.iter().filter(|u| u.side == side).count() + 1;
            s.units.push(UnitSpec { side, name: format!("{} Division {n}", side_label(side)), pos });
            selected.0 = Some(UnitId(s.units.len() as u32));
        }
        Tool::Depot | Tool::Base => {
            let Some(node) = nearest_node(m, p, reach.max(2.0 * m.terrain.cell_m)) else { return };
            let depot = ed.tool == Tool::Depot;
            let s = ed.scenario();
            toggle_at(if depot { &mut s.depots } else { &mut s.bases }, side, node);
        }
        Tool::Region | Tool::Occupy => {
            let Some(r) = region else { return };
            let legal = ed.tool == Tool::Region;
            let s = ed.scenario();
            s.control[r] = side;
            if legal {
                s.legal[r] = side;
            }
        }
        Tool::Delete => {
            if let Some(i) = nearest_unit(&ed.baked.scenario, p, reach) {
                ed.delete_unit(i, &mut selected);
            } else if let Some(node) = nearest_node(m, p, reach) {
                let s = ed.scenario();
                s.depots.retain(|(_, n)| *n != node);
                s.bases.retain(|(_, n)| *n != node);
            }
        }
    }
}

/// Editor markers: every node, and supply bases (not drawn in play) as side-colored squares.
pub fn draw_editor(mut gizmos: Gizmos, ed: Res<Editor>, map: Res<Map>, camera: Single<&Projection, With<Camera2d>>) {
    let px = world_per_px(&camera);
    let g = &map.0.graph;
    for n in &g.nodes {
        gizmos.circle_2d(Vec2::new(n.pos.x, n.pos.y), 4.0 * px, Color::BLACK);
    }
    for &(side, node) in &ed.baked.scenario.bases {
        let p = g.node(node).pos;
        let c = if side == PLAYER_SIDE { FRIENDLY } else { ENEMY };
        gizmos.rect_2d(Vec2::new(p.x, p.y), Vec2::splat(SYMBOL_PX * 1.1 * px), c);
        gizmos.rect_2d(Vec2::new(p.x, p.y), Vec2::splat(SYMBOL_PX * 1.0 * px), c);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn node_toggle_and_side_flip() {
        let mut list = vec![(0, NodeId(1))];
        toggle_at(&mut list, 1, NodeId(2));
        toggle_at(&mut list, 1, NodeId(1));
        assert_eq!(list, vec![(1, NodeId(2))]);

        let mut ed = Editor::new(mapdata::test_baked());
        ed.on = true;
        let side = ed.baked.scenario.units[0].side;
        ed.flip_side(0);
        assert_ne!(ed.baked.scenario.units[0].side, side);
        assert!(ed.reload);
        let mut sel = Selected(Some(UnitId(1)));
        assert_eq!(ed.selected_index(&sel), Some(0));
        ed.delete_unit(0, &mut sel);
        assert_eq!(ed.baked.scenario.units.len(), 2);
        assert!(sel.0.is_none());
        ed.baked.validate().unwrap();
    }
}
