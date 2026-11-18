//! Virtual battalion deployment (architect.md 15). A division is still one entity, but its
//! battalions are treated as placed in roles (line, reserve, support) by its posture. That gives the
//! division a frontage (a short line segment across its facing), a depth, and how much of its
//! firepower is in the line. Combat uses these for contact, local strength and flank exposure.

use crate::combat::{Arm, Battalions, arm_firepower};
use bevy_ecs::prelude::*;
use mapdata::Pos;

// shortcut: fixed tuning; per-arm and per-scenario values from Lua later.
/// Line frontage per line battalion when defending (wide, thin line).
pub const DEFEND_M_PER_BN: f32 = 3_000.0;
/// Line frontage per line battalion when attacking (narrow, concentrated).
pub const ATTACK_M_PER_BN: f32 = 1_500.0;
/// Road column: the whole division on a narrow front.
pub const MARCH_FRONTAGE_M: f32 = 1_000.0;
pub const MIN_FRONTAGE_M: f32 = 1_000.0;
pub const DEFEND_DEPTH_M: f32 = 8_000.0;
pub const ATTACK_DEPTH_M: f32 = 5_000.0;
/// Column length per battalion on the march.
pub const MARCH_M_PER_BN: f32 = 1_500.0;
/// Reserves fire only in local counterattacks.
pub const RESERVE_FIRE: f32 = 0.3;
/// A column caught on the march fights at this fraction.
pub const MARCH_FIRE: f32 = 0.5;
/// Share of losses taken by line / reserve / support battalions (re-spread if a role is empty).
pub const LOSS_SHARE: [f32; 3] = [0.7, 0.15, 0.15];
/// Enemy more than this off the facing (cos < 0.5, i.e. > 60 deg) hits the flank.
pub const FLANK_COS: f32 = 0.5;
/// An engaged line turns at most this much per combat step (5 min): 2.5 deg, 30 deg an hour.
pub const TURN_PER_STEP_RAD: f32 = 0.044;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum Posture {
    /// Wide front, armor held in reserve, deep.
    #[default]
    Defend,
    /// Narrow front, armor in the line.
    Attack,
    /// Road column: narrow and long, weak if hit.
    March,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Role {
    Line,
    Reserve,
    Support,
}

impl Role {
    pub fn index(self) -> usize {
        self as usize
    }
}

/// Facing (unit vector, direction the line looks at) and posture. Set by movement and combat.
#[derive(Component, Debug, Clone, Copy, PartialEq)]
pub struct Deployment {
    pub facing: (f32, f32),
    pub posture: Posture,
}

impl Default for Deployment {
    fn default() -> Self {
        Self { facing: (1.0, 0.0), posture: Posture::Defend }
    }
}

pub fn role(arm: Arm, posture: Posture) -> Role {
    match (arm, posture) {
        (Arm::Artillery, _) => Role::Support,
        (Arm::Armor, Posture::Defend) => Role::Reserve,
        _ => Role::Line,
    }
}

/// What the deployment adds up to.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Layout {
    pub frontage_m: f32,
    pub depth_m: f32,
    /// Battalion-equivalents per role (line, reserve, support).
    pub strength: [f32; 3],
    /// Fire of the line (spread along the front) and of reserve + support (reaches any contact).
    pub line_fire: f32,
    pub depth_fire: f32,
}

pub fn layout(b: &Battalions, posture: Posture) -> Layout {
    let mut strength = [0.0; 3];
    let mut fire = [0.0; 3];
    for bat in &b.0 {
        let r = role(bat.arm, posture).index();
        strength[r] += bat.strength;
        fire[r] += arm_firepower(bat.arm) * bat.strength;
    }
    let line_bn = strength[Role::Line.index()];
    let (frontage_m, depth_m) = match posture {
        Posture::Defend => (line_bn * DEFEND_M_PER_BN, DEFEND_DEPTH_M),
        Posture::Attack => (line_bn * ATTACK_M_PER_BN, ATTACK_DEPTH_M),
        Posture::March => (MARCH_FRONTAGE_M, b.total() * MARCH_M_PER_BN),
    };
    let march = if posture == Posture::March { MARCH_FIRE } else { 1.0 };
    Layout {
        frontage_m: frontage_m.max(MIN_FRONTAGE_M),
        depth_m,
        strength,
        line_fire: fire[0] * march,
        depth_fire: (fire[1] * RESERVE_FIRE + fire[2]) * march,
    }
}

/// Ends of the line: centered on `pos`, across `facing`.
pub fn line_ends(pos: Pos, facing: (f32, f32), frontage_m: f32) -> (Pos, Pos) {
    let (hx, hy) = (-facing.1 * frontage_m / 2.0, facing.0 * frontage_m / 2.0);
    (Pos { x: pos.x - hx, y: pos.y - hy }, Pos { x: pos.x + hx, y: pos.y + hy })
}

pub fn point_segment_dist(p: Pos, a: Pos, b: Pos) -> f32 {
    let (abx, aby) = (b.x - a.x, b.y - a.y);
    let len2 = abx * abx + aby * aby;
    let t = if len2 > 0.0 { (((p.x - a.x) * abx + (p.y - a.y) * aby) / len2).clamp(0.0, 1.0) } else { 0.0 };
    ((a.x + t * abx - p.x).powi(2) + (a.y + t * aby - p.y).powi(2)).sqrt()
}

/// Shortest distance between two segments (2D).
pub fn segment_dist(a: (Pos, Pos), b: (Pos, Pos)) -> f32 {
    let cross = |o: Pos, p: Pos, q: Pos| (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x);
    let (d1, d2) = (cross(a.0, a.1, b.0), cross(a.0, a.1, b.1));
    let (d3, d4) = (cross(b.0, b.1, a.0), cross(b.0, b.1, a.1));
    if d1 * d2 < 0.0 && d3 * d4 < 0.0 {
        return 0.0; // crossing
    }
    point_segment_dist(a.0, b.0, b.1)
        .min(point_segment_dist(a.1, b.0, b.1))
        .min(point_segment_dist(b.0, a.0, a.1))
        .min(point_segment_dist(b.1, a.0, a.1))
}

/// Unit vector, or `None` for a zero vector.
pub fn unit(x: f32, y: f32) -> Option<(f32, f32)> {
    let len = (x * x + y * y).sqrt();
    (len > 1e-3).then(|| (x / len, y / len))
}

/// Rotate `from` toward `to` by at most `max_rad`.
pub fn turn_toward(from: (f32, f32), to: (f32, f32), max_rad: f32) -> (f32, f32) {
    let a = from.1.atan2(from.0);
    let mut d = to.1.atan2(to.0) - a;
    d = (d + std::f32::consts::PI).rem_euclid(std::f32::consts::TAU) - std::f32::consts::PI;
    let a = a + d.clamp(-max_rad, max_rad);
    (a.cos(), a.sin())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn posture_sets_frontage_and_roles() {
        let b = Battalions::standard(); // 6 inf, 2 armor, 2 arty
        let d = layout(&b, Posture::Defend);
        assert_eq!(d.strength, [6.0, 2.0, 2.0]);
        assert_eq!(d.frontage_m, 18_000.0);
        let a = layout(&b, Posture::Attack);
        assert_eq!(a.strength, [8.0, 0.0, 2.0]);
        assert_eq!(a.frontage_m, 12_000.0);
        assert!(a.line_fire > d.line_fire, "armor joins the line");
        let m = layout(&b, Posture::March);
        assert_eq!(m.frontage_m, MARCH_FRONTAGE_M);
        assert_eq!(m.depth_m, 15_000.0);
    }

    #[test]
    fn segment_geometry() {
        let p = |x, y| Pos { x, y };
        // Facing east: the line runs north-south.
        let (a, b) = line_ends(p(0.0, 0.0), (1.0, 0.0), 10.0);
        assert!((a.x).abs() < 1e-4 && (a.y + 5.0).abs() < 1e-4 && (b.y - 5.0).abs() < 1e-4);
        assert_eq!(segment_dist((p(0.0, -5.0), p(0.0, 5.0)), (p(3.0, -1.0), p(3.0, 1.0))), 3.0);
        assert_eq!(segment_dist((p(-1.0, 0.0), p(1.0, 0.0)), (p(0.0, -1.0), p(0.0, 1.0))), 0.0);
        let t = turn_toward((1.0, 0.0), (0.0, 1.0), 0.5);
        assert!((t.1.atan2(t.0) - 0.5).abs() < 1e-4);
    }
}
