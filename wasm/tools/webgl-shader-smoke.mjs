#!/usr/bin/env node
// Compile + link every translated shader pair in a real WebGL2 context (Chromium via Playwright).
// Catches what glslang alone does not: ANGLE-specific rejections, varying/interface mismatches at
// link time, uniform block limits. Also prints the context limits that matter for this renderer.
//
//   node wasm/tools/webgl-shader-smoke.mjs [--dir build-wasm/shaders_es] [--headed] [--json out.json]
//
// Headless uses SwiftShader (software). --headed uses the real GPU when a display is available
// (WSLg on Windows 11) and is closer to what players get.

import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const DIR = opt("--dir", "build-wasm/shaders_es");
const HEADED = args.includes("--headed");
const JSON_OUT = opt("--json", null);

async function loadPlaywright() {
  try { return await import("playwright"); } catch {}
  const req = createRequire(import.meta.url);
  for (const p of [process.env.PLAYWRIGHT_PATH, ...(process.env.NODE_PATH || "").split(path.delimiter)].filter(Boolean)) {
    try { return req(path.join(p, p.endsWith("playwright") ? "" : "playwright")); } catch {}
  }
  console.error("playwright not found: run `npm ci` in wasm/tools, then `npx playwright install chromium`.");
  process.exit(3);
}

const substitute = (s) => s
  .replace(/HEIGHT_SCALE/g, "1.0")
  .replace(/SCISSOR_HEIGHT/g, "448.0")
  .replace(/SCISSOR_ADJUST/g, "(512.0 / 448.0)");

const names = [...new Set(fs.readdirSync(DIR).filter((f) => /\.(vert|frag)$/.test(f)).map((f) => f.replace(/\.(vert|frag)$/, "")))].sort();
const pairs = names.map((n) => ({
  name: n,
  vert: fs.existsSync(path.join(DIR, n + ".vert")) ? substitute(fs.readFileSync(path.join(DIR, n + ".vert"), "utf8")) : null,
  frag: fs.existsSync(path.join(DIR, n + ".frag")) ? substitute(fs.readFileSync(path.join(DIR, n + ".frag"), "utf8")) : null,
}));

const { chromium } = await loadPlaywright();
const browser = await chromium.launch({
  headless: !HEADED,
  args: HEADED ? ["--ignore-gpu-blocklist"] : ["--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"],
});
const page = await browser.newPage();
await page.setContent("<canvas id=c width=64 height=64></canvas>");
const result = await page.evaluate((pairs) => {
  const gl = document.getElementById("c").getContext("webgl2");
  if (!gl) return { error: "no WebGL2 context" };
  const dbg = gl.getExtension("WEBGL_debug_renderer_info");
  const lim = (k) => gl.getParameter(gl[k]);
  const info = {
    renderer: dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER),
    limits: Object.fromEntries([
      "MAX_TEXTURE_SIZE", "MAX_TEXTURE_IMAGE_UNITS", "MAX_VERTEX_TEXTURE_IMAGE_UNITS",
      "MAX_COMBINED_TEXTURE_IMAGE_UNITS", "MAX_UNIFORM_BLOCK_SIZE", "UNIFORM_BUFFER_OFFSET_ALIGNMENT",
      "MAX_VERTEX_ATTRIBS", "MAX_VARYING_VECTORS", "MAX_SAMPLES", "MAX_DRAW_BUFFERS",
    ].map((k) => [k, lim(k)])),
    extensions: ["WEBGL_multi_draw", "EXT_texture_filter_anisotropic", "OES_draw_buffers_indexed",
      "EXT_color_buffer_float", "WEBGL_polygon_mode", "EXT_disjoint_timer_query_webgl2", "WEBGL_clip_cull_distance"]
      .map((e) => [e, !!gl.getExtension(e)]),
  };
  const compile = (type, src) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, src);
    gl.compileShader(s);
    const ok = gl.getShaderParameter(s, gl.COMPILE_STATUS);
    return { s, ok, log: ok ? "" : gl.getShaderInfoLog(s) };
  };
  const rows = [];
  for (const p of pairs) {
    const r = { name: p.name, ok: false, stage: "", log: "" };
    if (!p.vert || !p.frag) { r.stage = "pair"; r.log = "missing .vert or .frag"; rows.push(r); continue; }
    const v = compile(gl.VERTEX_SHADER, p.vert);
    if (!v.ok) { r.stage = "vert"; r.log = v.log; rows.push(r); continue; }
    const f = compile(gl.FRAGMENT_SHADER, p.frag);
    if (!f.ok) { r.stage = "frag"; r.log = f.log; rows.push(r); continue; }
    const prog = gl.createProgram();
    gl.attachShader(prog, v.s); gl.attachShader(prog, f.s);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) { r.stage = "link"; r.log = gl.getProgramInfoLog(prog); rows.push(r); continue; }
    r.ok = true;
    // sampler uniforms must map to units < MAX_COMBINED_TEXTURE_IMAGE_UNITS (Shader.cpp binds tex_TN -> N)
    const n = gl.getProgramParameter(prog, gl.ACTIVE_UNIFORMS);
    const samplers = [];
    for (let i = 0; i < n; i++) { const u = gl.getActiveUniform(prog, i); if (/^tex_T\d+$/.test(u.name)) samplers.push(+u.name.slice(5)); }
    r.maxUnit = samplers.length ? Math.max(...samplers) : -1;
    r.samplers = samplers.length;
    const nb = gl.getProgramParameter(prog, gl.ACTIVE_UNIFORM_BLOCKS);
    r.blocks = [];
    for (let i = 0; i < nb; i++) r.blocks.push([gl.getActiveUniformBlockName(prog, i), gl.getActiveUniformBlockParameter(prog, i, gl.UNIFORM_BLOCK_DATA_SIZE)]);
    rows.push(r);
  }
  return { info, rows };
}, pairs);
await browser.close();

if (result.error) { console.error(result.error); process.exit(2); }
const { info, rows } = result;
console.log(`renderer: ${info.renderer}`);
console.log("limits:", JSON.stringify(info.limits));
console.log("extensions:", info.extensions.map(([e, ok]) => `${e}=${ok ? "yes" : "no"}`).join(" "));
const bad = rows.filter((r) => !r.ok);
for (const r of bad) console.log(`FAIL ${r.name} [${r.stage}]\n  ${String(r.log).trim().replace(/\n/g, "\n  ")}`);
for (const r of rows.filter((r) => r.ok && r.maxUnit >= info.limits.MAX_COMBINED_TEXTURE_IMAGE_UNITS))
  console.log(`WARN ${r.name}: uses texture unit ${r.maxUnit} >= MAX_COMBINED_TEXTURE_IMAGE_UNITS`);
for (const r of rows.filter((r) => r.ok)) for (const [b, sz] of r.blocks)
  if (sz > info.limits.MAX_UNIFORM_BLOCK_SIZE) console.log(`WARN ${r.name}: block ${b} is ${sz} B > MAX_UNIFORM_BLOCK_SIZE`);
console.log(`${rows.length - bad.length}/${rows.length} programs compile and link in WebGL2`);
if (JSON_OUT) fs.writeFileSync(JSON_OUT, JSON.stringify(result, null, 2));
process.exit(bad.length ? 1 : 0);
