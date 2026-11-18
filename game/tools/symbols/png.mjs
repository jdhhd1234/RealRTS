// Rasterizes the generated symbol SVGs to PNG for the C++ app (Raylib loads PNG, no SVG at runtime).
// Needs @resvg/resvg-js (npm i @resvg/resvg-js@2.6.2). Run from game/: node tools/symbols/png.mjs
import { Resvg } from "@resvg/resvg-js";
import { mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";

// Larger than shown (34 px) so zoomed-in or HiDPI views stay sharp.
const RASTER_PX = 96;
const src = new URL("../../assets/symbols/svg/", import.meta.url);
const out = new URL("../../assets/symbols/", import.meta.url);
mkdirSync(out, { recursive: true });
for (const f of readdirSync(src).filter((f) => f.endsWith(".svg"))) {
  const png = new Resvg(readFileSync(new URL(f, src)), { fitTo: { mode: "height", value: RASTER_PX } }).render().asPng();
  writeFileSync(new URL(f.replace(".svg", ".png"), out), png);
}
console.log("PNGs written to", out.pathname);
