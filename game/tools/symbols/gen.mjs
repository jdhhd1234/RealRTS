// Generates the APP-6 (D) symbol SVGs the game uses into game/assets/symbols/svg/.
// Needs the milsymbol clone at ../milsymbol (repo root). Run from game/: node tools/symbols/gen.mjs
import { ms, app6d } from "../../../milsymbol/index.mjs";
import { mkdirSync, writeFileSync } from "node:fs";

ms.addIcons(app6d);
// SIDC (20 digits): version 10, context 0, identity (3 friend / 6 hostile), symbol set (10 land unit /
// 20 land installation), status 0, HQ 0, echelon (21 division / 00 none), entity (6), modifiers 0000.
const identity = { friend: "3", hostile: "6" };
const kinds = {
  infantry: ["10", "21", "121100"],
  armor: ["10", "21", "120500"],
  artillery: ["10", "21", "130300"],
  partisan: ["10", "00", "121100"], // shortcut: no guerrilla icon chosen yet; echelon-less infantry
  depot: ["10", "00", "163400"], // sustainment / supply
  base: ["20", "00", "111200"], // installation: military materiel facility (supply)
  corps: ["10", "22", "110000"], // command and control, echelon XXX (corps)
};
const out = new URL("../../assets/symbols/svg/", import.meta.url);
mkdirSync(out, { recursive: true });
for (const [side, id] of Object.entries(identity)) {
  for (const [kind, [set, echelon, entity]] of Object.entries(kinds)) {
    const sidc = `100${id}${set}00${echelon}${entity}0000`;
    const sym = new ms.Symbol(sidc, { size: 60, standard: "APP6" });
    if (!sym.isValid()) throw new Error(`invalid SIDC ${sidc}`);
    writeFileSync(new URL(`${side}_${kind}.svg`, out), sym.asSVG());
  }
}
console.log("symbols written to", out.pathname);
