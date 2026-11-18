//! GeoPackage geometry blobs (GP header + WKB) and scanline rasterization.

/// (x, y) in the layer's CRS units.
pub type P = (f64, f64);

/// Geometry flattened to what the baker needs.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Geom {
    pub points: Vec<P>,
    pub lines: Vec<Vec<P>>,
    /// Each polygon is a list of rings (outer first, then holes).
    pub polygons: Vec<Vec<Vec<P>>>,
}

impl Geom {
    pub fn all_points(&self) -> impl Iterator<Item = P> + '_ {
        self.points
            .iter()
            .copied()
            .chain(self.lines.iter().flatten().copied())
            .chain(self.polygons.iter().flatten().flatten().copied())
    }
}

/// Parse a GeoPackage geometry blob (http://www.geopackage.org/spec/#gpb_format).
pub fn parse_gpb(b: &[u8]) -> Result<Geom, String> {
    if b.len() < 8 || &b[0..2] != b"GP" {
        return Err("not a GeoPackage geometry blob".into());
    }
    let flags = b[3];
    if flags & 0b1_0000 != 0 {
        return Ok(Geom::default()); // empty geometry
    }
    let envelope = match (flags >> 1) & 0b111 {
        0 => 0,
        1 => 32,
        2 | 3 => 48,
        4 => 64,
        e => return Err(format!("bad envelope code {e}")),
    };
    let start = 8 + envelope;
    if b.len() < start {
        return Err("truncated geometry header".into());
    }
    let mut g = Geom::default();
    let mut r = Reader { b: &b[start..], i: 0 };
    read_wkb(&mut r, &mut g, 0)?;
    Ok(g)
}

struct Reader<'a> {
    b: &'a [u8],
    i: usize,
}

impl Reader<'_> {
    fn take<const N: usize>(&mut self) -> Result<[u8; N], String> {
        let s = self.b.get(self.i..self.i + N).ok_or("truncated WKB")?;
        self.i += N;
        Ok(s.try_into().unwrap())
    }
    fn u32(&mut self, le: bool) -> Result<u32, String> {
        let a = self.take::<4>()?;
        Ok(if le { u32::from_le_bytes(a) } else { u32::from_be_bytes(a) })
    }
    fn f64(&mut self, le: bool) -> Result<f64, String> {
        let a = self.take::<8>()?;
        Ok(if le { f64::from_le_bytes(a) } else { f64::from_be_bytes(a) })
    }
}

fn read_wkb(r: &mut Reader, g: &mut Geom, depth: u32) -> Result<(), String> {
    if depth > 8 {
        return Err("geometry nested too deeply".into());
    }
    let le = r.take::<1>()?[0] == 1;
    let raw = r.u32(le)?;
    // ISO (1000/2000/3000 offsets) and EWKB (high flag bits) Z/M variants.
    let (z, m) = (raw & 0x8000_0000 != 0, raw & 0x4000_0000 != 0);
    let iso = raw & 0x0FFF_FFFF;
    let (kind, iz, im) = (iso % 1000, matches!(iso / 1000, 1 | 3), matches!(iso / 1000, 2 | 3));
    let dims = 2 + (z || iz) as usize + (m || im) as usize;
    let point = |r: &mut Reader| -> Result<P, String> {
        let (x, y) = (r.f64(le)?, r.f64(le)?);
        for _ in 2..dims {
            r.f64(le)?;
        }
        Ok((x, y))
    };
    let count = |r: &mut Reader| -> Result<usize, String> {
        let n = r.u32(le)? as usize;
        // Every element needs at least 16 bytes; reject counts the blob cannot hold.
        if n > r.b.len() / 16 + 1 { Err("bad WKB count".into()) } else { Ok(n) }
    };
    let ring = |r: &mut Reader| -> Result<Vec<P>, String> { (0..count(r)?).map(|_| point(r)).collect() };
    match kind {
        1 => {
            let p = point(r)?;
            if p.0.is_finite() {
                g.points.push(p); // NaN point = empty
            }
        }
        2 => {
            let l = ring(r)?;
            g.lines.push(l);
        }
        3 => {
            let rings = (0..count(r)?).map(|_| ring(r)).collect::<Result<_, _>>()?;
            g.polygons.push(rings);
        }
        4..=7 => {
            for _ in 0..count(r)? {
                read_wkb(r, g, depth + 1)?;
            }
        }
        k => return Err(format!("unsupported geometry type {k}")),
    }
    Ok(())
}

/// Cells (cx, cy) whose centers lie inside the polygon (even-odd rule, so holes work).
/// Coordinates are in cell units with (0, 0) at the grid's bottom-left corner.
pub fn raster_polygon(rings: &[Vec<P>], w: u32, h: u32, mut f: impl FnMut(u32, u32)) {
    let ys = rings.iter().flatten().map(|p| p.1);
    let (lo, hi) = ys.fold((f64::INFINITY, f64::NEG_INFINITY), |(a, b), y| (a.min(y), b.max(y)));
    if !lo.is_finite() {
        return;
    }
    let first = (lo - 0.5).ceil().max(0.0) as u32;
    let last = ((hi - 0.5).floor()).min(h as f64 - 1.0);
    if last < 0.0 {
        return;
    }
    let mut xs = Vec::new();
    for cy in first..=last as u32 {
        let y = cy as f64 + 0.5;
        xs.clear();
        for ring in rings {
            for (i, &(x0, y0)) in ring.iter().enumerate() {
                let (x1, y1) = ring[(i + 1) % ring.len()];
                if (y0 <= y) != (y1 <= y) {
                    xs.push(x0 + (y - y0) / (y1 - y0) * (x1 - x0));
                }
            }
        }
        xs.sort_by(f64::total_cmp);
        for pair in xs.chunks_exact(2) {
            let a = (pair[0] - 0.5).ceil().max(0.0);
            let b = (pair[1] - 0.5).floor().min(w as f64 - 1.0);
            if b >= a {
                for cx in a as u32..=b as u32 {
                    f(cx, cy);
                }
            }
        }
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// Little-endian WKB writer for tests.
    pub fn wkb_polygon(rings: &[Vec<P>]) -> Vec<u8> {
        let mut v = vec![1u8];
        v.extend(3u32.to_le_bytes());
        v.extend((rings.len() as u32).to_le_bytes());
        for r in rings {
            v.extend((r.len() as u32).to_le_bytes());
            for &(x, y) in r {
                v.extend(x.to_le_bytes());
                v.extend(y.to_le_bytes());
            }
        }
        v
    }

    pub fn wkb_line(pts: &[P]) -> Vec<u8> {
        let mut v = vec![1u8];
        v.extend(2u32.to_le_bytes());
        v.extend((pts.len() as u32).to_le_bytes());
        for &(x, y) in pts {
            v.extend(x.to_le_bytes());
            v.extend(y.to_le_bytes());
        }
        v
    }

    pub fn wkb_point(p: P) -> Vec<u8> {
        let mut v = vec![1u8];
        v.extend(1u32.to_le_bytes());
        v.extend(p.0.to_le_bytes());
        v.extend(p.1.to_le_bytes());
        v
    }

    /// GP header (little-endian, no envelope) around a WKB body.
    pub fn gpb(srs: i32, wkb: Vec<u8>) -> Vec<u8> {
        let mut v = b"GP".to_vec();
        v.push(0);
        v.push(1); // little-endian header, no envelope
        v.extend(srs.to_le_bytes());
        v.extend(wkb);
        v
    }

    fn square(x0: f64, y0: f64, x1: f64, y1: f64) -> Vec<P> {
        vec![(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)]
    }

    #[test]
    fn parses_polygon_with_envelope_skipped() {
        let poly = vec![square(0.0, 0.0, 4.0, 4.0)];
        let mut blob = b"GP".to_vec();
        blob.extend([0, 0b0000_0011]); // little-endian, 32-byte envelope
        blob.extend(3857i32.to_le_bytes());
        blob.extend([0u8; 32]);
        blob.extend(wkb_polygon(&poly));
        assert_eq!(parse_gpb(&blob).unwrap().polygons, vec![poly]);
    }

    #[test]
    fn rejects_garbage() {
        assert!(parse_gpb(b"XX123456").is_err());
        let mut blob = gpb(1, wkb_line(&[(0.0, 0.0), (1.0, 1.0)]));
        blob.truncate(blob.len() - 3);
        assert!(parse_gpb(&blob).is_err());
    }

    #[test]
    fn raster_fills_cell_centers_and_respects_holes() {
        let rings = vec![square(0.0, 0.0, 6.0, 6.0), square(2.0, 2.0, 4.0, 4.0)];
        let mut cells = Vec::new();
        raster_polygon(&rings, 10, 10, |x, y| cells.push((x, y)));
        assert_eq!(cells.len(), 36 - 4);
        assert!(!cells.contains(&(2, 2)) && !cells.contains(&(3, 3)));
        assert!(cells.contains(&(0, 0)) && cells.contains(&(5, 5)) && !cells.contains(&(6, 0)));
    }

    #[test]
    fn raster_clips_to_grid() {
        let mut n = 0;
        raster_polygon(&[square(-5.0, -5.0, 50.0, 50.0)], 3, 2, |_, _| n += 1);
        assert_eq!(n, 6);
    }
}
