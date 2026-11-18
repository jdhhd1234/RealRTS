//! File menu: open a QGIS map (.gpkg / GeoJSON / Shapefile folder, baked on the fly by `mapbake`)
//! or a baked map (.ron), go back to the built-in test map, or save the edited scenario as .ron. The dialog and baking run on a worker thread; the result is
//! applied at the start of a frame by an exclusive system.

use crate::editor::Editor;
use crate::{MapVersion, Selected};
use bevy::prelude::*;
use mapdata::Baked;
use std::path::Path;
use std::sync::Mutex;
use std::sync::mpsc::{Receiver, Sender, channel};

type Loaded = Result<(Baked, String), String>;
/// Worker result: a map to load, or (None, status text) after a save. `Err("")` = cancelled.
type Msg = Result<(Option<Baked>, String), String>;

/// Shown in the top bar and the File menu.
#[derive(Resource)]
pub struct FileStatus {
    pub text: String,
    pub error: bool,
    pub busy: bool,
}

impl Default for FileStatus {
    fn default() -> Self {
        Self { text: "Map: built-in test map".into(), error: false, busy: false }
    }
}

#[derive(Resource)]
pub struct Loader {
    tx: Sender<Msg>,
    rx: Mutex<Receiver<Msg>>,
}

impl Default for Loader {
    fn default() -> Self {
        let (tx, rx) = channel();
        Self { tx, rx: Mutex::new(rx) }
    }
}

impl Loader {
    /// Native file dialog on a worker thread, then bake or parse there too.
    pub fn open_dialog(&self, status: &mut FileStatus) {
        if status.busy {
            return;
        }
        status.busy = true;
        status.text = "Opening...".into();
        let tx = self.tx.clone();
        std::thread::spawn(move || {
            let picked = rfd::FileDialog::new()
                .set_title("Open map")
                .add_filter("QGIS map (GeoPackage, GeoJSON, Shapefile) or baked map", &["gpkg", "geojson", "json", "shp", "ron"])
                .pick_file();
            let result = match picked {
                None => Err(String::new()), // cancelled
                Some(path) => read_map(&path).map(|(b, label)| (Some(b), label)),
            };
            let _ = tx.send(result);
        });
    }

    pub fn load_test_map(&self) {
        let _ = self.tx.send(Ok((Some(mapdata::test_baked()), "built-in test map".into())));
    }

    /// Save dialog on a worker thread; writes map + scenario as a baked .ron that File > Open reads back.
    pub fn save_dialog(&self, baked: &Baked, status: &mut FileStatus) {
        if status.busy {
            return;
        }
        status.busy = true;
        status.text = "Saving...".into();
        let (tx, text, name) = (self.tx.clone(), baked.to_ron(), format!("{}.ron", baked.map.name));
        std::thread::spawn(move || {
            let picked = rfd::FileDialog::new().set_title("Save scenario").set_file_name(name).add_filter("Baked map", &["ron"]).save_file();
            let result = match picked {
                None => Err(String::new()),
                Some(path) => std::fs::write(&path, text)
                    .map(|_| (None, format!("Saved: {}", path.display())))
                    .map_err(|e| format!("Save failed: {e}")),
            };
            let _ = tx.send(result);
        });
    }
}

/// .ron = baked map; .geojson/.shp = the whole folder of layer files; anything else = GeoPackage.
pub fn read_map(path: &Path) -> Loaded {
    let label = path.file_name().map_or_else(|| path.display().to_string(), |n| n.to_string_lossy().into_owned());
    let baked = if path.extension().is_some_and(|e| e.eq_ignore_ascii_case("ron")) {
        let text = std::fs::read_to_string(path).map_err(|e| format!("{label}: {e}"))?;
        Baked::from_ron(&text).map_err(|e| format!("{label}: {e:?}"))?
    } else {
        mapbake::bake_path(path).map_err(|e| format!("{label}: {e}"))?
    };
    // Folder maps are named after the folder (set by mapbake).
    let lower = label.to_ascii_lowercase();
    let label = if lower.ends_with(".gpkg") || lower.ends_with(".ron") { label } else { format!("{}/ folder", baked.map.name) };
    Ok((baked, label))
}

/// Swaps in a finished load: the whole sim restarts on the new map, paused.
pub fn apply_loaded(world: &mut World) {
    let Some(result) = world.resource::<Loader>().rx.lock().ok().and_then(|rx| rx.try_recv().ok()) else { return };
    let mut status = FileStatus { busy: false, ..default() };
    match result {
        Ok((None, text)) => status.text = text,
        Ok((Some(baked), label)) => {
            let t = &baked.map.terrain;
            status.text = format!(
                "Map: {label} ({} x {} km, {} units)",
                (t.width as f32 * t.cell_m / 1000.0).round(),
                (t.height as f32 * t.cell_m / 1000.0).round(),
                baked.scenario.units.len()
            );
            world.resource_mut::<Editor>().set_source(baked.clone());
            world.resource_mut::<MapVersion>().0 += 1;
            sim::load_scenario(world, baked);
            world.resource_mut::<Selected>().0 = None;
            world.resource_mut::<Time<Virtual>>().pause();
        }
        Err(e) if e.is_empty() => status.text = world.resource::<FileStatus>().text.clone(),
        Err(e) => {
            status.text = if e.starts_with("Save failed") { e } else { format!("Load failed: {e}") };
            status.error = true;
        }
    }
    world.insert_resource(status);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ron_maps_load_and_bad_files_report_errors() {
        let dir = std::env::temp_dir();
        let good = dir.join(format!("realrts_test_{}.ron", std::process::id()));
        std::fs::write(&good, mapdata::test_baked().to_ron()).unwrap();
        let (b, label) = read_map(&good).unwrap();
        assert_eq!(b.scenario.units.len(), 3);
        assert!(label.ends_with(".ron"));
        std::fs::write(&good, "not a map").unwrap();
        assert!(read_map(&good).is_err());
        std::fs::remove_file(&good).unwrap();
        assert!(read_map(&dir.join("does_not_exist.gpkg")).is_err());
    }
}
