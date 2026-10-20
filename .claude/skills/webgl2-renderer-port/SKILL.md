---
name: webgl2-renderer-port
description: Use for any change that touches OpenGOAL's renderer in the WebGL2 (gk_web) build - files under game/graphics/ (opengl_renderer, pipelines, texture), the shared .vert/.frag shaders, Shader.cpp, wasm/web/webgl_compat.cpp, or the wasm/tools shader tooling. Covers the read -> fix under __EMSCRIPTEN__ -> verify loop and the WebGL2 traps already hit.
---

# Working on the renderer for WebGL2

Background, the adapter list and the traps are in `wasm/docs/RENDERER.md`. Read the section for the
area you touch before editing. CLAUDE.MD hard rules apply: web-only changes under `__EMSCRIPTEN__`,
native behaviour unchanged, no game data anywhere public or in git.

## Where things live

- Desktop-only GL calls: adapted at runtime in `wasm/web/webgl_compat.cpp` (installed over glad's
  pointers right after `gladLoadGLLoader` in `game/graphics/pipelines/opengl.cpp`). Prefer adding an
  adapter there over `#ifdef`s at many renderer call sites. A fix that must live in the renderer
  (e.g. `LoaderStages.cpp` `upload_indices`) goes under `#ifdef __EMSCRIPTEN__` with the native
  branch untouched.
- Shaders: one set of `.vert/.frag` files, valid as GLSL 4.10 core **and** GLSL ES 3.00. `Shader.cpp`
  (`to_glsl_es`) only rewrites the header at load (version line, precisions, sampler1D → sampler2D
  with `ivec2(i, 0)`, noperspective dropped). Editing the shared shaders is allowed (owner's decision)
  as long as both dialects stay valid with the same values.

## Loop for every renderer change

1. **Read the native path end to end** for the renderer you touch (e.g. `background/TFragment.cpp`
   + its shader + where `tex_T10` is bound). State the plan in two or three lines.
2. **Edit**: adapter in `webgl_compat.cpp`, or `__EMSCRIPTEN__` branch in the renderer, or a
   dual-valid shader change (explicit `2.0` / `1u` literals, `float(...)` around uint results,
   `uint` vertex inputs for unsigned data, `case 0u:` on uint switches).
3. **Verify, cheapest first:**
   - Shader edits: `node wasm/tools/gles-shaders.mjs --src game/graphics/opengl_renderer/shaders --out build-wasm/shaders_es`
     then `node wasm/tools/webgl-shader-smoke.mjs --dir build-wasm/shaders_es`. These are checkers
     now; on a good tree they report no literal changes and every program links. Both need
     `npm ci` in `wasm/tools` (+ `npx playwright install chromium` for the smoke test): **ask the
     owner before running those installs, they download.** `cd wasm/tools && npm test` checks the
     checker itself.
   - Native still builds and compiles every shader: `task build-release` (or the target), then
     native gk on the same input (`build/Release/bin/game/gk`).
   - gk_web: `wasm-build-and-run` skill, then look in Chrome (`window.webglLog`, DevTools console),
     then `native-vs-wasm-diff` if pixels differ.
   - `gl-audit.mjs` does **not** run on this branch (needs the uninstalled `gl_target.h` + patch 0001).
4. **Record.** New trap or decision → one line in the CLAUDE.MD "Log"; renderer knowledge that
   isn't a one-liner → `wasm/docs/RENDERER.md`.

## Traps already hit (don't rediscover them)

- `GL_UNSIGNED_INT_8_8_8_8` is byte-swapped vs `_REV`; the adapter swaps bytes for it, `_REV` maps
  to `GL_UNSIGNED_BYTE` directly. The 1D palettes are allocated with `8_8_8_8`.
- One texture level attached at two color attachments (`FramebufferTexturePair` mips) →
  `FRAMEBUFFER_UNSUPPORTED`. The adapter never attaches a duplicate.
- `int` vertex inputs fed unsigned data → `INVALID_OPERATION` at draw time (WebGL checks signedness).
- An element array buffer can't be bound to `GL_ARRAY_BUFFER`: no GL error, only a console warning,
  and the draws are degenerate (invisible merc models).
- Primitive restart is always on (0xFFFFFFFF); `glEnable(GL_PRIMITIVE_RESTART)` is an error (dropped).
- `#version` must be on line 1 in ES; uint switch labels need `u` (ANGLE rejects what glslang accepts).
- The OffscreenCanvas presents only when its worker returns to the event loop (frame per main-loop
  tick in `runtime_wasm.cpp`); don't add blocking waits to the frame path.
- `vif_interrupt_callback` is skipped under Emscripten (GOAL lives only in the EE thread's table).
- mips2c contexts must keep stale registers between calls (sky code reads the old vf27).
- wasm32 `size_t` is 4 bytes: serialized lengths are u64 under Emscripten (`Serializer`, zstd header).
- `ub_bones` is exactly 16384 B, the WebGL2 minimum uniform block size. Don't grow it.
- MSAA is off in the web build (no multisample textures).

## When the browser shows nothing / something wrong

Work outward from the smallest thing that draws: clear color → splash → sky → tfrag. Read
`window.webglLog` and the DevTools console first (some WebGL refusals are console-only), then set
`kLocateErrors = true` in `webgl_compat.cpp` to find the call, then Spector.js on one frame.
