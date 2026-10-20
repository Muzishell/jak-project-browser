#!/usr/bin/env node
// Translate OpenGOAL's desktop shaders (#version 410 core) to GLSL ES 3.00 for WebGL2.
//
//   node wasm/tools/gles-shaders.mjs [--src DIR] [--out DIR] [--report FILE] [--check-only]
//
// Defaults: --src game/graphics/opengl_renderer/shaders  --out build-wasm/shaders_es
// Never edits the source shaders: native keeps using them unchanged.
//
// Passes, in order:
//   1. header:   #version 410 core -> #version 300 es + default precisions
//   2. sampler1D -> sampler2D, texelFetch(s, i, lod) -> texelFetch(s, jak_tex1d(i), lod)
//                (C++ side must upload those textures as WIDTH x 1 2D textures; see RENDERER.md)
//   3. noperspective dropped (only sky.* uses it, with w == 1.0, so it is exact there; any other
//      shader using it is reported as a visual-diff risk)
//   4. implicit int->float: GLSL 4.10 converts `x * 2` silently, ES 3.00 rejects it. Fixed by
//      compiling with glslang and turning one int literal at a time on an erroring line into a float
//      literal, keeping the change only when the error count drops. Anything left is reported.
//
// Placeholders HEIGHT_SCALE / SCISSOR_HEIGHT / SCISSOR_ADJUST are kept in the output (Shader.cpp
// substitutes them at load time); they are substituted only for validation.

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : def;
};
const SRC = opt("--src", "game/graphics/opengl_renderer/shaders");
const OUT = opt("--out", "build-wasm/shaders_es");
const REPORT = opt("--report", path.join(OUT, "TRANSLATION_REPORT.md"));
const CHECK_ONLY = args.includes("--check-only");

function findGlslang() {
  const cands = [
    process.env.GLSLANG,
    path.join(here, "node_modules/glslang-validator-prebuilt-predownloaded/bin",
      process.platform === "win32" ? "glslangValidator.exe"
        : process.platform === "darwin" ? "glslangValidator.darwin" : "glslangValidator.linux"),
  ].filter(Boolean);
  for (const c of cands) if (fs.existsSync(c)) { try { fs.chmodSync(c, 0o755); } catch {} return c; }
  for (const name of ["glslangValidator", "glslang"]) {
    try { execFileSync(name, ["--version"], { stdio: "ignore" }); return name; } catch {}
  }
  console.error("glslang not found. Run `npm ci` in wasm/tools, or `sudo apt install glslang-tools`, or set GLSLANG=.");
  process.exit(3);
}
const GLSLANG = findGlslang();
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "gles-"));

const substitute = (s) => s
  .replace(/HEIGHT_SCALE/g, "1.0")
  .replace(/SCISSOR_HEIGHT/g, "448.0")
  .replace(/SCISSOR_ADJUST/g, "(512.0 / 448.0)");

// Returns array of {line, msg}; line numbers are 1-based in the given text.
function compile(text, stage) {
  const f = path.join(tmp, `s.${stage}`);
  fs.writeFileSync(f, substitute(text));
  try {
    execFileSync(GLSLANG, [f], { stdio: "pipe" });
    return [];
  } catch (e) {
    const out = String(e.stdout || "") + String(e.stderr || "");
    const errs = [];
    for (const l of out.split(/\r?\n/)) {
      const m = l.match(/^ERROR: [^:]*:(\d+): (.*)$/);
      if (m) errs.push({ line: +m[1], msg: m[2].trim() });
      else if (/^ERROR: /.test(l) && !/compilation errors/.test(l)) errs.push({ line: 0, msg: l });
    }
    return errs.length ? errs : [{ line: 0, msg: out.trim() || "unknown failure" }];
  }
}

const HEADER_V = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
const HEADER_F = "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n";
const TEX1D_HELPER =
  "// jak_tex1d: 1D textures are uploaded as JAK_TEX1D_W x N 2D textures on WebGL2.\n" +
  "#ifndef JAK_TEX1D_W\n#define JAK_TEX1D_W 8192\n#endif\n" +
  "ivec2 jak_tex1d(int i) { return ivec2(i % JAK_TEX1D_W, i / JAK_TEX1D_W); }\n";

// Literals that may be converted: plain decimal ints not part of identifiers/floats/hex/uint,
// and not directly inside [] (array index) or after a `case`.
function literalCandidates(line) {
  const res = [];
  const re = /(?<![\w.])(0[xX][0-9a-fA-F]+|\d+)(?![\w.])/g;
  let m;
  while ((m = re.exec(line))) {
    const before = line.slice(0, m.index);
    const depth = (before.match(/\[/g) || []).length - (before.match(/\]/g) || []).length;
    if (/\bcase\s*$/.test(before)) continue;
    if (/^\s*#/.test(line)) continue;
    if (/location\s*=\s*$/.test(before) || /binding\s*=\s*$/.test(before)) continue;
    res.push({ index: m.index, text: m[1], inIndex: depth > 0 });
  }
  return res;
}

const PATCHES_FILE = opt("--patches", path.join(here, "shader-patches.json"));
const PATCHES = fs.existsSync(PATCHES_FILE) ? JSON.parse(fs.readFileSync(PATCHES_FILE, "utf8")) : {};

function translate(name, src) {
  const patchNotes = [];
  for (const [from, to, why] of PATCHES[name] || []) {
    if (!src.includes(from)) throw new Error(`${name}: patch no longer applies (upstream changed?): ${from}`);
    src = src.split(from).join(to);
    patchNotes.push(`patch: ${why || from}`);
  }
  const stage = name.endsWith(".vert") ? "vert" : "frag";
  const notes = patchNotes;
  let lines = src.split(/\r?\n/);

  // 1. header
  const vi = lines.findIndex((l) => /^\s*#version/.test(l));
  if (vi < 0) throw new Error(`${name}: no #version`);
  if (!/#version\s+410\s+core/.test(lines[vi])) notes.push(`unexpected version line: \`${lines[vi].trim()}\``);
  const header = (stage === "frag" ? HEADER_F : HEADER_V).trimEnd().split("\n");
  // ES requires #version on the very first line (no comments before it)
  lines.splice(vi, 1);
  lines.unshift(...header);

  // 2. sampler1D
  let text = lines.join("\n");
  const s1d = [...text.matchAll(/uniform\s+sampler1D\s+(\w+)\s*;/g)].map((m) => m[1]);
  if (s1d.length) {
    text = text.replace(/uniform\s+sampler1D\s+(\w+)\s*;/g, "uniform highp sampler2D $1;");
    for (const s of s1d) {
      // texelFetch(s, <expr>, <lod>) with balanced parens in <expr>
      let out = "", i = 0;
      const key = new RegExp(`texelFetch\\(\\s*${s}\\s*,`, "g");
      let m;
      while ((m = key.exec(text))) {
        out += text.slice(i, m.index) + `texelFetch(${s}, jak_tex1d(`;
        let j = m.index + m[0].length, depth = 0;
        for (; j < text.length; j++) {
          const c = text[j];
          if (c === "(") depth++;
          else if (c === ")") depth--;
          else if (c === "," && depth === 0) break;
        }
        out += text.slice(m.index + m[0].length, j).trim() + ")";
        i = j;
        key.lastIndex = j;
      }
      text = out + text.slice(i);
      const other = new RegExp(`\\b(texture|textureLod|textureSize|textureProj)\\s*\\(\\s*${s}\\b`);
      if (other.test(text)) notes.push(`**manual**: ${s} is a sampler1D used with a non-texelFetch call`);
    }
    // helper after the precision lines
    const tl = text.split("\n");
    tl.splice(header.length, 0, ...TEX1D_HELPER.trimEnd().split("\n"));
    text = tl.join("\n");
    notes.push(`sampler1D -> sampler2D: ${s1d.join(", ")} (C++ must upload as Wx1 2D, see jak_tex1d)`);
  }

  // 3. noperspective
  if (/\bnoperspective\b/.test(text)) {
    text = text.replace(/\bnoperspective\s+/g, "");
    notes.push(name.startsWith("sky.")
      ? "noperspective dropped (exact: sky.vert writes w = 1.0)"
      : "**visual-diff risk**: noperspective dropped; emulate with (v*w, w) varyings if output differs");
  }

  // 4. implicit int -> float / uint, error driven. glslang stops at the first hard error, so
  //    progress means: the first error moves to a later line, or that line has fewer errors.
  lines = text.split("\n");
  const score = (es) => {
    const ls = es.filter((e) => e.line > 0).map((e) => e.line);
    if (!es.length) return [Infinity, 0];
    const first = ls.length ? Math.min(...ls) : 0;
    return [first, es.filter((e) => e.line === first).length];
  };
  const better = (a, b) => a[0] > b[0] || (a[0] === b[0] && a[1] < b[1]);
  const apply = (ls, idx, changes) => {
    // changes: [{c, suffix}] applied right to left so indices stay valid
    const t = ls.slice();
    let l = t[idx];
    for (const { c, suffix } of [...changes].sort((x, y) => y.c.index - x.c.index))
      l = l.slice(0, c.index) + (suffix === ".0" ? `${Number(c.text)}.0` : c.text + suffix) +
        l.slice(c.index + c.text.length);
    t[idx] = l;
    return t;
  };
  const sufFor = (c) => (c.inIndex ? ["u"] : [".0", "u"]);
  let errs = compile(lines.join("\n"), stage);
  let fixedF = 0, fixedU = 0;
  for (let guard = 0; errs.length && guard < 1000; guard++) {
    const cur = score(errs);
    if (!(cur[0] > 0 && cur[0] !== Infinity)) break;
    const idx = cur[0] - 1;
    const cands = literalCandidates(lines[idx]);
    // candidate change sets, cheapest first: singles, all-float, all-uint, pairs
    const sets = [];
    for (const c of cands) for (const suffix of sufFor(c)) sets.push([{ c, suffix }]);
    const nonIdx = cands.filter((c) => !c.inIndex);
    if (nonIdx.length > 1) sets.push(nonIdx.map((c) => ({ c, suffix: ".0" })));
    if (cands.length > 1) sets.push(cands.map((c) => ({ c, suffix: "u" })));
    for (let i = 0; i < cands.length; i++)
      for (let j = i + 1; j < cands.length; j++)
        for (const suffix of [".0", "u"])
          if (!(suffix === ".0" && (cands[i].inIndex || cands[j].inIndex)))
            sets.push([{ c: cands[i], suffix }, { c: cands[j], suffix }]);
    let accepted = null;
    for (const set of sets) {
      const trial = apply(lines, idx, set);
      const e2 = compile(trial.join("\n"), stage);
      if (better(score(e2), cur)) { accepted = { set, e2 }; break; }
    }
    if (!accepted) break;
    // minimise: drop any change in the set that is not needed
    let set = accepted.set, e2 = accepted.e2;
    for (const ch of [...set]) {
      if (set.length === 1) break;
      const smaller = set.filter((x) => x !== ch);
      const e3 = compile(apply(lines, idx, smaller).join("\n"), stage);
      const s3 = score(e3), s2 = score(e2);
      if (!better(s2, s3)) { set = smaller; e2 = e3; }
    }
    lines = apply(lines, idx, set);
    errs = e2;
    for (const ch of set) ch.suffix === "u" ? fixedU++ : fixedF++;
  }
  const fixed = fixedF + fixedU;
  // 5. switch on uint: GLSL 4.10 and glslang accept int case labels, ANGLE (Chrome) rejects them.
  //    Probe each switch expression's type by inserting `uint probe = (expr);` (ES has no implicit
  //    int->uint, so it only compiles when expr is uint), then suffix that switch's case labels.
  if (!errs.length) {
    let t = lines.join("\n"), pos = 0, nU = 0, m;
    const sw = /\bswitch\s*\(/g;
    while ((m = sw.exec(t))) {
      let j = m.index + m[0].length, d = 1;
      for (; j < t.length && d; j++) { if (t[j] === "(") d++; else if (t[j] === ")") d--; }
      const expr = t.slice(m.index + m[0].length, j - 1);
      const probe = t.slice(0, m.index) + `uint _jak_probe = (${expr}); ` + t.slice(m.index);
      if (compile(probe, stage).length) continue;
      const open = t.indexOf("{", j);
      let k = open + 1; d = 1;
      for (; k < t.length && d; k++) { if (t[k] === "{") d++; else if (t[k] === "}") d--; }
      const body = t.slice(open, k).replace(/\bcase\s+(0[xX][0-9a-fA-F]+|\d+)\s*:/g, (_, n) => { nU++; return `case ${n}u:`; });
      t = t.slice(0, open) + body + t.slice(k);
      sw.lastIndex = open + body.length;
    }
    if (nU) {
      const e = compile(t, stage);
      if (e.length) notes.push(`**manual**: uint switch fix broke compilation: ${e[0].msg}`);
      else { lines = t.split("\n"); notes.push(`${nU} case label(s) -> uint (switch on uint; ANGLE is strict)`); }
    }
  }
  // Integer-literal division was integer division in 4.10; if both sides became float literals the
  // result changed. Flag for review (should never happen with the current minimise step).
  for (const l of lines) if (/\b\d+\.0\s*[/%]\s*\d+\.0\b/.test(l) && !src.includes(l.trim()))
    notes.push(`**review**: literal/literal division now float: \`${l.trim()}\``);
  if (fixedF) notes.push(`${fixedF} int literal(s) -> float literal (ES has no implicit int->float)`);
  if (fixedU) notes.push(`${fixedU} int literal(s) -> uint literal (ES has no implicit int->uint)`);
  return { stage, text: lines.join("\n"), notes, errors: errs };
}

const files = fs.readdirSync(SRC).filter((f) => /\.(vert|frag)$/.test(f)).sort();
if (!files.length) { console.error(`no shaders in ${SRC}`); process.exit(2); }
if (!CHECK_ONLY) fs.mkdirSync(OUT, { recursive: true });

const rows = [];
let failed = 0;
for (const f of files) {
  const r = translate(f, fs.readFileSync(path.join(SRC, f), "utf8"));
  if (!CHECK_ONLY) fs.writeFileSync(path.join(OUT, f), r.text.endsWith("\n") ? r.text : r.text + "\n");
  if (r.errors.length) failed++;
  rows.push({ f, ...r });
}

const md = [
  "# GLSL ES 3.00 translation report",
  "",
  `Source: \`${SRC}\` -> \`${OUT}\`. ${files.length} files, ${failed} still failing glslang (ES 3.00).`,
  "",
  "| shader | status | notes |",
  "|---|---|---|",
  ...rows.map((r) => `| ${r.f} | ${r.errors.length ? "FAIL" : "ok"} | ${[...r.notes,
    ...r.errors.slice(0, 3).map((e) => `L${e.line}: ${e.msg.replace(/\|/g, "\\|")}`)].join("<br>")} |`),
  "",
].join("\n");
if (!CHECK_ONLY) fs.writeFileSync(REPORT, md);
for (const r of rows) {
  if (r.errors.length) {
    console.log(`FAIL ${r.f}`);
    for (const e of r.errors) console.log(`   L${e.line}: ${e.msg}`);
  }
}
console.log(`${files.length - failed}/${files.length} shaders valid GLSL ES 3.00${CHECK_ONLY ? "" : ` -> ${OUT} (report: ${REPORT})`}`);
fs.rmSync(tmp, { recursive: true, force: true });
process.exit(failed ? 1 : 0);
