//! Offline baking: `cargo run -p mapbake -- <input.gpkg | layer folder | layer.geojson/.shp> output.ron`

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let [_, input, output] = args.as_slice() else {
        eprintln!("usage: mapbake <input.gpkg | folder of .geojson/.shp layers> <output.ron>");
        std::process::exit(2);
    };
    match mapbake::bake_path(input.as_ref()) {
        Ok(b) => {
            if let Err(e) = std::fs::write(output, b.to_ron()) {
                eprintln!("cannot write {output}: {e}");
                std::process::exit(1);
            }
            let t = &b.map.terrain;
            println!(
                "{output}: {} x {} km, {} regions, {} nodes, {} edges, {} units",
                t.width,
                t.height,
                b.map.regions.len(),
                b.map.graph.nodes.len(),
                b.map.graph.edges.len(),
                b.scenario.units.len()
            );
        }
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    }
}
