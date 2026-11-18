// Generates the APP-6 (D) symbol SVGs the game uses into game/assets/symbols/svg/.
// Needs the milsymbol clone at ../milsymbol (repo root). Run from game/: node tools/symbols/gen.mjs
import { ms, app6d } from "../../../milsymbol/index.mjs";
import { mkdirSync, writeFileSync } from "node:fs";

ms.addIcons(app6d);
// SIDC (20 digits): version 10, context 0, identity (3 friend / 6 hostile), symbol set 10 land unit,
// status 0, HQ 0, echelon (21 division / 00 none), entity (6), modifiers 0000.
const identity = { friend: "3", hostile: "6" };
const kinds = {
  infantry: ["21", "121100"],
  armor: ["21", "120500"],
  artillery: ["21", "130300"],
  partisan: ["00", "121100"], // shortcut: no guerrilla icon chosen yet; echelon-less infantry
  depot: ["00", "163400"], // sustainment / supply
};
const out = new URL("../../assets/symbols/svg/", import.meta.url);
mkdirSync(out, { recursive: true });
for (const [side, id] of Object.entries(identity)) {
  for (const [kind, [echelon, entity]] of Object.entries(kinds)) {
    const sidc = `100${id}1000${echelon}${entity}0000`;
    const sym = new ms.Symbol(sidc, { size: 60, standard: "APP6" });
    if (!sym.isValid()) throw new Error(`invalid SIDC ${sidc}`);
    writeFileSync(new URL(`${side}_${kind}.svg`, out), sym.asSVG());
  }
}
console.log("symbols written to", out.pathname);
