#!/usr/bin/env node
// WebGL2 API audit for the OpenGOAL renderer.
//
//   node wasm/tools/gl-audit.mjs [--paths game/graphics] [--out build-wasm/gl-audit.md] [--json f] [-j N]
//
// Two passes over every .cpp under --paths, both with JAK_GL_AUDIT defined so the code sees exactly
// what an Emscripten build sees (gl_target.h switches to <GLES3/gl3.h>):
//   1. compile:  clang++ -fsyntax-only against the Khronos GLES 3.0 headers vendored in khr/.
//                Every desktop-only function/enum shows up as "undeclared identifier".
//   2. runtime:  clang++ -E, then scan the preprocessed code (repo files only, inactive #if branches
//                gone) for calls that compile against GLES3 but fail in WebGL2 at runtime.
// Run from the repo root. Needs wasm/patches/0001-route-gl-includes-through-gl_target.patch applied.
// Works without emsdk; once the wasm build compiles, em++ is the final word.

import { execFile } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";

const run = promisify(execFile);
const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const PATHS = opt("--paths", "game/graphics").split(",");
const OUT = opt("--out", "build-wasm/gl-audit.md");
const JSON_OUT = opt("--json", null);
const JOBS = +opt("-j", Math.max(1, os.cpus().length));
const CXX = process.env.CXX_AUDIT || "clang++";

if (!fs.existsSync("game/graphics/pipelines/gl_target.h")) {
  // Not installed on the wasm branch on purpose: desktop GL calls are adapted at runtime by
  // wasm/web/webgl_compat.cpp instead (see wasm/docs/RENDERER.md).
  console.error("game/graphics/pipelines/gl_target.h missing: gl-audit.mjs does not run on the wasm branch (see wasm/docs/RENDERER.md).");
  process.exit(2);
}
if (!fs.readFileSync("game/graphics/pipelines/opengl.h", "utf8").includes("gl_target.h")) {
  console.error("opengl.h still includes glad directly: git apply wasm/patches/0001-route-gl-includes-through-gl_target.patch");
  process.exit(2);
}

const INC = ["-I" + path.join(here, "khr"), "-I.", "-Ithird-party", "-Ithird-party/fmt/include",
  "-Ithird-party/SDL/include", "-Ithird-party/glad/include", "-Ithird-party/zstd/lib"];
const FLAGS = ["-std=c++20", "-DJAK_GL_AUDIT", "-w", ...INC];

// What to do about each desktop-only identifier. Keep in sync with wasm/docs/RENDERER.md.
const HINTS = {
  GL_TEXTURE_1D: "1D texture -> GL_TEXTURE_2D, height 1 (shaders already use jak_tex1d). glTexImage1D/glTexSubImage1D -> 2D variants with height 1, yoffset 0.",
  glTexImage1D: "-> glTexImage2D(GL_TEXTURE_2D, lvl, fmt, w, 1, 0, ...)",
  glTexSubImage1D: "-> glTexSubImage2D(GL_TEXTURE_2D, lvl, x, 0, w, 1, ...)",
  GL_UNSIGNED_INT_8_8_8_8: "byte-swapped vs _REV. Upstream uses it only to allocate (nullptr data) -> GL_UNSIGNED_BYTE. If real data is ever passed, swizzle on upload.",
  GL_UNSIGNED_INT_8_8_8_8_REV: "gl_target.h maps it to GL_UNSIGNED_BYTE (same bytes on little-endian). If you see this, the file is not including gl_target.h.",
  glPrimitiveRestartIndex: "WebGL2 always restarts on the max index (0xFFFFFFFF for UINT) -> drop the call under JAK_WEBGL2.",
  GL_PRIMITIVE_RESTART: "always on in WebGL2; glEnable(GL_PRIMITIVE_RESTART[_FIXED_INDEX]) is INVALID_ENUM -> skip under JAK_WEBGL2.",
  glMultiDrawElements: "step 1: force render_state.no_multidraw = true (existing single-draw path). step 2 (perf): WEBGL_multi_draw via emscripten glMultiDrawElementsWEBGL (byte offsets as GLsizei[], not void**).",
  GL_LINE: "glPolygonMode wireframe: debug only -> skip under JAK_WEBGL2 (WEBGL_polygon_mode exists but is not universal).",
  GL_FILL: "see GL_LINE",
  glPolygonMode: "see GL_LINE",
  glFramebufferTexture: "-> glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, tex, level) (the 4th arg of glFramebufferTexture is the mip level).",
  glClearDepth: "gl_target.h provides glClearDepth -> glClearDepthf.",
  glColorMaski: "only buffer 0 is used -> glColorMask(r,g,b,a). (OES_draw_buffers_indexed if more are ever needed.)",
  GL_TEXTURE_WIDTH: "glGetTexLevelParameteriv is GLES 3.1: debug/ImGui only -> track sizes yourself or compile out.",
  GL_TEXTURE_HEIGHT: "see GL_TEXTURE_WIDTH",
  glGetTexLevelParameteriv: "see GL_TEXTURE_WIDTH",
  glGetTexImage: "not in ES: debug texture dump -> compile out, or attach to an FBO and glReadPixels(RGBA, UNSIGNED_BYTE).",
  GL_TEXTURE_2D_MULTISAMPLE: "no multisample textures in WebGL2 -> MSAA via glRenderbufferStorageMultisample + glBlitFramebuffer resolve, or force msaa=1 first.",
  glTexImage2DMultisample: "see GL_TEXTURE_2D_MULTISAMPLE",
  GL_TEXTURE_MAX_ANISOTROPY: "EXT_texture_filter_anisotropic: constants in gl_target.h; enable only if the extension is present.",
  GL_MAX_TEXTURE_MAX_ANISOTROPY: "see GL_TEXTURE_MAX_ANISOTROPY",
  GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER: "desktop-only status enum in an error-printing switch -> #if !JAK_WEBGL2 around the case.",
  GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER: "see GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER",
  GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS: "see GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER",
  GL_DEBUG_OUTPUT: "no KHR_debug in WebGL -> compile out; use -sGL_ASSERTIONS / -sGL_DEBUG and the browser console.",
  GL_DEBUG_SOURCE_API: "see GL_DEBUG_OUTPUT",
  GL_DEBUG_TYPE_OTHER: "see GL_DEBUG_OUTPUT",
  glDebugMessageCallback: "see GL_DEBUG_OUTPUT",
  glDebugMessageControl: "see GL_DEBUG_OUTPUT",
  opengl_error_callback: "its definition is compiled out with the debug path; compile the reference out too.",
  gladLoadGL: "no loader on the web: under JAK_WEBGL2 create the context with SDL_GL_* ES 3.0 attributes and skip glad.",
  gladLoadGLLoader: "see gladLoadGL",
  GLADloadproc: "see gladLoadGL",
  GLVersion: "glad global -> under JAK_WEBGL2 report glGetString(GL_VERSION) instead.",
  GL_CLAMP_TO_BORDER: "not in ES 3.0 -> GL_CLAMP_TO_EDGE (+ border color in shader if it matters).",
  glMapBuffer: "not in ES -> glBufferSubData.",
  GL_BGRA: "not in WebGL2 -> swizzle to RGBA on upload.",
  glDrawBuffer: "-> glDrawBuffers(1, &att).",
  GLAPIENTRY: "desktop GL calling-convention macro (only on the debug callback) -> compiled out with the debug path.",
};

// Valid against GLES3 headers, broken in WebGL2 at runtime. Scanned on preprocessed code.
const RUNTIME = [
  [/glEnable\(\s*GL_PRIMITIVE_RESTART_FIXED_INDEX/, "INVALID_ENUM in WebGL2 (restart is always on)"],
  [/glTexImage2D\([^;]*?,\s*GL_RED\s*,[^;]*,\s*GL_RED\s*,/, "unsized GL_RED internal format is invalid in ES 3.0 -> GL_R8"],
  [/glTexImage2D\([^;]*?,\s*GL_RG\s*,/, "unsized GL_RG internal format is invalid in ES 3.0 -> GL_RG8"],
  [/glTexImage2D\([^;]*?,\s*GL_DEPTH_COMPONENT\s*,[^;]*GL_FLOAT/, "DEPTH_COMPONENT + FLOAT needs sized GL_DEPTH_COMPONENT32F"],
  [/glMapBufferRange\s*\(/, "Emscripten does not implement glMapBufferRange for WebGL -> glBufferSubData"],
  [/glGetBufferSubData\s*\(/, "synchronous readback stalls the GPU in browsers; avoid in the frame loop"],
  [/glReadPixels\s*\(/, "readback: only RGBA/UNSIGNED_BYTE (or the IMPLEMENTATION_COLOR_READ pair) is guaranteed; stalls if in the frame loop"],
  [/glTexParameter[if]v?\(\s*GL_TEXTURE_2D\s*,\s*GL_TEXTURE_LOD_BIAS/, "GL_TEXTURE_LOD_BIAS does not exist in ES -> bias in shader"],
  [/glGetTexLevelParameteriv\s*\(/, "glGetTexLevelParameteriv is GLES 3.1, not WebGL2 -> track texture sizes yourself (debug/ImGui only)"],
  [/GL_TIME_ELAPSED|GL_TIMESTAMP|glQueryCounter/, "timer queries need EXT_disjoint_timer_query_webgl2 (often disabled) -> CPU timers"],
];

function walk(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? walk(path.join(dir, e.name)) : e.name.endsWith(".cpp") ? [path.join(dir, e.name)] : []);
}
const files = PATHS.flatMap((p) => (fs.statSync(p).isDirectory() ? walk(p) : [p])).sort();

async function compileOne(f) {
  try { await run(CXX, [...FLAGS, "-fsyntax-only", "-ferror-limit=0", f], { maxBuffer: 64 << 20 }); return ""; }
  catch (e) { return String(e.stderr || e.message); }
}
async function preprocessOne(f) {
  try { const { stdout } = await run(CXX, [...FLAGS, "-E", f], { maxBuffer: 256 << 20 }); return stdout; }
  catch (e) { return String(e.stdout || ""); }
}

async function pool(items, fn) {
  const out = new Array(items.length);
  let i = 0;
  await Promise.all(Array.from({ length: JOBS }, async () => {
    while (i < items.length) { const k = i++; out[k] = await fn(items[k]); }
  }));
  return out;
}

const compileOut = await pool(files, compileOne);
const byIdent = new Map(); // ident -> Set(loc)
const other = new Set();
for (const out of compileOut) {
  for (const line of out.split("\n")) {
    const m = line.match(/^(.+?):(\d+):\d+: error: (.*)$/);
    if (!m) continue;
    const loc = `${path.relative(".", m[1])}:${m[2]}`;
    const u = m[3].match(/undeclared identifier '(\w+)'/) || m[3].match(/no member named '(\w+)'/);
    if (u) { if (!byIdent.has(u[1])) byIdent.set(u[1], new Set()); byIdent.get(u[1]).add(loc); }
    else if (/incomplete type 'void'|expected ';' after top level declarator/.test(m[3]) && /opengl_error_callback|GLAPIENTRY/.test(fs.readFileSync(m[1], "utf8").split("\n")[+m[2] - 1] || ""))
      { if (!byIdent.has("GLAPIENTRY")) byIdent.set("GLAPIENTRY", new Set()); byIdent.get("GLAPIENTRY").add(loc); }
    else other.add(`${loc}: ${m[3]}`);
  }
}

// Runtime pass: -E tells us which source lines survive the preprocessor with JAK_GL_AUDIT (so code
// already fixed under `#if !JAK_WEBGL2` is skipped); the patterns are matched on the raw source,
// because -E expands GL_* macros to numbers.
const ppOut = await pool(files, preprocessOne);
const live = new Map(); // file -> Set(line)
for (const pp of ppOut) {
  let cur = null, ln = 0;
  for (const line of pp.split("\n")) {
    const mk = line.match(/^# (\d+) "([^"]+)"/);
    if (mk) { ln = +mk[1]; cur = path.relative(".", mk[2]); continue; }
    if (cur && /^(game|common)\//.test(cur.split(path.sep).join("/")) && line.trim()) {
      if (!live.has(cur)) live.set(cur, new Set());
      live.get(cur).add(ln);
    }
    ln++;
  }
}
const runtime = new Map(); // msg -> Set(loc)
for (const [f, lines] of live) {
  if (!fs.existsSync(f)) continue;
  const src = fs.readFileSync(f, "utf8").split("\n");
  let buf = "", start = 0;
  for (let i = 0; i < src.length; i++) {
    const t = src[i].replace(/\/\/.*$/, "").trim();
    if (!buf) start = i + 1;
    buf += " " + t;
    if (/[;{}]\s*$/.test(t) || !t) {
      if (lines.has(start)) for (const [re, msg] of RUNTIME) if (re.test(buf)) {
        if (!runtime.has(msg)) runtime.set(msg, new Set());
        runtime.get(msg).add(`${f}:${start}`);
      }
      buf = "";
    }
  }
}

const idents = [...byIdent.entries()].sort((a, b) => b[1].size - a[1].size);
const total = idents.reduce((n, [, s]) => n + s.size, 0);
const rtTotal = [...runtime.values()].reduce((n, s) => n + s.size, 0);
const md = [
  "# WebGL2 GL audit",
  "",
  `${files.length} files under ${PATHS.join(", ")}. **${total}** compile-time sites (${idents.length} identifiers), **${rtTotal}** runtime-risk sites, ${other.size} other errors.`,
  "",
  "## Compile-time (not in GLES 3.0)",
  "",
  ...idents.flatMap(([id, locs]) => [`### \`${id}\` (${locs.size})`, "", HINTS[id] ? `Fix: ${HINTS[id]}` : "Fix: _no hint yet, add one to gl-audit.mjs HINTS_", "", ...[...locs].sort().map((l) => `- ${l}`), ""]),
  "## Runtime risks (compile fine, fail or stall in WebGL2)",
  "",
  ...[...runtime.entries()].flatMap(([msg, locs]) => [`### ${msg} (${locs.size})`, "", ...[...locs].sort().map((l) => `- ${l}`), ""]),
  ...(other.size ? ["## Other compile errors", "", ...[...other].sort().map((l) => `- ${l}`), ""] : []),
].join("\n");

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, md);
if (JSON_OUT) fs.writeFileSync(JSON_OUT, JSON.stringify({
  compile: Object.fromEntries(idents.map(([k, v]) => [k, [...v].sort()])),
  runtime: Object.fromEntries([...runtime].map(([k, v]) => [k, [...v].sort()])),
  other: [...other].sort(),
}, null, 2));
for (const [id, locs] of idents) console.log(`${String(locs.size).padStart(4)}  ${id}`);
if (rtTotal) for (const [m, s] of runtime) console.log(`${String(s.size).padStart(4)}  [runtime] ${m.split(" -> ")[0]}`);
if (other.size) console.log(`${other.size} other compile errors (see report)`);
console.log(`${total} compile-time + ${rtTotal} runtime sites -> ${OUT}`);
process.exit(total || other.size ? 1 : 0);
