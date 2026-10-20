#!/usr/bin/env node
// Compare a native screenshot with a browser capture of the same frame.
//
//   node wasm/tools/img-diff.mjs native.png wasm.png [--out diff.png] [--threshold 0.1] [--max-ratio 0.01]
//
// Resizes nothing: capture both at the same game resolution (pc-set-game-resolution / --size).
// Prints mismatched pixel ratio and per-quadrant ratios (helps tell "sky wrong" from "HUD wrong").
// Exit 1 if ratio > --max-ratio.

import fs from "node:fs";
import { createRequire } from "node:module";
import path from "node:path";

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const [A, B] = args.filter((a, i) => !a.startsWith("--") && !(i > 0 && args[i - 1].startsWith("--")));
if (!A || !B) { console.error("usage: img-diff.mjs a.png b.png [--out diff.png]"); process.exit(2); }
const OUT = opt("--out", B.replace(/\.png$/, "") + ".diff.png");
const TH = +opt("--threshold", 0.05);
const MAX = +opt("--max-ratio", 0.01);

const req = createRequire(import.meta.url);
let PNG, pixelmatch;
try { ({ PNG } = req("pngjs")); pixelmatch = (await import("pixelmatch")).default; }
catch { console.error("missing deps: npm ci in wasm/tools"); process.exit(3); }

const a = PNG.sync.read(fs.readFileSync(A));
const b = PNG.sync.read(fs.readFileSync(B));
if (a.width !== b.width || a.height !== b.height) {
  console.error(`size mismatch: ${A} ${a.width}x${a.height} vs ${B} ${b.width}x${b.height}`);
  process.exit(2);
}
const { width: w, height: h } = a;
const diff = new PNG({ width: w, height: h });
const n = pixelmatch(a.data, b.data, diff.data, w, h, { threshold: TH, includeAA: false });
fs.mkdirSync(path.dirname(path.resolve(OUT)), { recursive: true });
fs.writeFileSync(OUT, PNG.sync.write(diff));

const quad = (x0, y0) => {
  let c = 0;
  for (let y = y0; y < y0 + h / 2; y++) for (let x = x0; x < x0 + w / 2; x++) {
    const i = (Math.floor(y) * w + Math.floor(x)) * 4;
    if (diff.data[i] === 255 && diff.data[i + 1] === 0) c++; // pixelmatch paints mismatches red
  }
  return (c / ((w / 2) * (h / 2))).toFixed(4);
};
const ratio = n / (w * h);
console.log(JSON.stringify({ a: A, b: B, size: `${w}x${h}`, mismatched: n, ratio: +ratio.toFixed(5),
  quadrants: { topLeft: quad(0, 0), topRight: quad(w / 2, 0), bottomLeft: quad(0, h / 2), bottomRight: quad(w / 2, h / 2) }, diff: OUT }));
process.exit(ratio > MAX ? 1 : 0);
