# Renderer port: desktop OpenGL 4.x → WebGL2

Working notes for the WebGL2 renderer (architecture decision 8 in CLAUDE.MD). M5 (Geyser Rock in
Chrome) and M6 (Sandover playable, audio, input, 60 fps focused) have landed on the `wasm` branch;
this file describes what the branch actually does and the traps that were hit. The harness that
brought this file was written on upstream `32c3cf7` before that work; the parts kept from it are
marked **(historical)**.

## How it works on this branch

| Area | State | Where |
|---|---|---|
| Guard | Every web-only change is under `__EMSCRIPTEN__` (no `JAK_WEBGL2` / `gl_target.h`). Native compiles what it always did. | — |
| Context | SDL3 (Emscripten port) creates an OpenGL ES 3.0 = WebGL2 context on an OffscreenCanvas owned by the main pthread (`PROXY_TO_PTHREAD`, `-sOFFSCREENCANVAS_SUPPORT=1`). glad stays for the declarations and is loaded only through `gladLoadGLLoader(SDL_GL_GetProcAddress)`; glad.c looks every entry point up by name under Emscripten. | `game/graphics/pipelines/opengl.cpp`, `third-party/glad/src/glad.c` |
| Desktop-only GL calls | Adapted at runtime: `webgl_compat::install()` swaps glad's function pointers right after loading. The renderer sources still call the desktop functions. | `wasm/web/webgl_compat.cpp` |
| Shaders | The shared `.vert/.frag` files are valid GLSL 4.10 core **and** GLSL ES 3.00 at once (owner's decision, commit 533612fe0): explicit float literals (`2.0`), `u` suffixes, `float(...)` around uint results, `uint` vertex inputs for unsigned data. `Shader.cpp` rewrites only the header at load. | `game/graphics/opengl_renderer/shaders/`, `Shader.cpp` (`to_glsl_es`) |
| Shader files in the browser | `--preload-file` of the shader dir at `/jak-project/game/graphics/opengl_renderer/shaders`; a shader change relinks gk_web (LINK_DEPENDS). | `wasm/web/gk_web.cmake` |
| Frame loop | One frame per `emscripten_set_main_loop` tick with 60 Hz pacing inside the tick (setImmediate timing = `scheduler.postTask`); the renderer's FrameLimiter is skipped. | `wasm/runtime_wasm.cpp` (`WASM_DISPLAY`) |
| MSAA | Off in the web build (no multisample textures in WebGL2). | `webgl_compat.cpp` (`tex_image_2d_multisample` logs an error) |

### What `webgl_compat.cpp` adapts

- **1D textures** → 2D textures of height 1 (`GL_TEXTURE_1D` target mapped on bind/parameter/upload;
  `glTexImage1D`/`glTexSubImage1D` → the 2D calls with height 1, yoffset 0).
- **`GL_UNSIGNED_INT_8_8_8_8_REV`** with RGBA → `GL_UNSIGNED_BYTE` (same bytes on little endian).
  **`GL_UNSIGNED_INT_8_8_8_8`** → `GL_UNSIGNED_BYTE` with each pixel **byte-swapped** on upload
  (the time-of-day palettes are allocated with it).
- **Primitive restart**: `glEnable/glDisable(GL_PRIMITIVE_RESTART)` (and `GL_DEBUG_OUTPUT`,
  `GL_TEXTURE_1D`, `GL_PROGRAM_POINT_SIZE`, `GL_MULTISAMPLE`) are dropped; WebGL2 always restarts
  at 0xFFFFFFFF, which is what the renderer sets (`glPrimitiveRestartIndex` warns otherwise).
- **`glMultiDrawElements`** → a loop of `glDrawElements` (`WEBGL_multi_draw` later if profiling asks).
- **`glPolygonMode`, `glDebugMessageCallback/Control`** → no-ops.
- **`glColorMaski(0, …)`** → `glColorMask`; **`glClearDepth`** → `glClearDepthf`.
- **`glFramebufferTexture`** → `glFramebufferTexture2D`, and never attaches one texture level at a
  second color attachment (see traps).
- **`glGetTexLevelParameteriv`** → a table of level-0 sizes recorded at `glTexImage2D`;
  **`glGetTexImage`** → read through a temporary framebuffer.
- **Errors**: `webgl_compat::log_errors()` logs the WebGL errors once per frame (max 40, with a JS
  stack). Set `kLocateErrors = true` to check the error state and framebuffer completeness around
  every draw/clear/upload/blit and find the call that made one (slow: synchronous GL queries).

### What `Shader.cpp` rewrites at load (`to_glsl_es`, `__EMSCRIPTEN__`)

1. `#version 410 core` → `#version 300 es` **on line 1** (comments before the version line move after
   it; `debug_red.*` has one) + default precisions (float, int, sampler2D, usampler2D, isampler2D,
   sampler2DArray, sampler3D).
2. `sampler1D` → `sampler2D`, and `texelFetch(s, i, 0)` → `texelFetch(s, ivec2(i, 0), 0)` for those
   samplers (time-of-day palettes in tfrag3/shrub/tie/etie/hfrag, slime LUT in tex_anim).
3. `noperspective` dropped. Only `sky.*` uses it, and `sky.vert` writes `w = 1.0`, so it is exact.

Everything else must already be valid in both dialects. **New or edited shaders must avoid implicit
conversions** (int→float, int→uint, `case 0:` on a uint switch) and must declare integer vertex
inputs with the signedness of the data the C++ side feeds them.

## The toolbox (`wasm/tools/`, dev only, needs `npm ci` there for glslang/playwright)

| Tool | State on this branch | Typical run (repo root) |
|---|---|---|
| `gles-shaders.mjs` | **Checker.** Applies an ES header/sampler1D/noperspective rewrite like Shader.cpp's (it spells the 1D index `jak_tex1d(i)` instead of `ivec2(i, 0)`), compiles with glslang as ES 3.00 and would turn int literals into float/uint literals. On the current shaders it should be close to a no-op: no literal changes, `shader-patches.json` is empty. Any literal change it reports = a shader that lost dual validity. | `node wasm/tools/gles-shaders.mjs --src game/graphics/opengl_renderer/shaders --out build-wasm/shaders_es` |
| `webgl-shader-smoke.mjs` | **Checker.** Compiles + links every pair in Chromium WebGL2 (SwiftShader headless, `--headed` for the GPU), prints limits/extensions. Run on the checker's output dir. | `node wasm/tools/webgl-shader-smoke.mjs --dir build-wasm/shaders_es` |
| `selftest.mjs` | Regression test of the checker's rules on `fixtures/shaders` (no game data). | `cd wasm/tools && npm test` |
| `gl-audit.mjs` | **Does not run on this branch**: it needs `game/graphics/pipelines/gl_target.h` and patch 0001, which were deliberately not installed (the branch adapts calls at runtime instead). Kept for reference; `gl-audit-baseline.md` is its stale upstream output. | — |
| `capture.mjs` | Loads a page in Chromium, waits for frame N, saves PNG + console + summary JSON. **The build does not set `window.__jak.frame`** (no capture hook yet), so it always waits the full `--timeout` and then screenshots. The game's own log goes to the page (`window.gkLog`, WebGL lines in `window.webglLog`), not the console. | `node wasm/tools/capture.mjs --url "http://127.0.0.1:8090/gk_web.html?args=-boot+-fakeiso+-debug" --timeout 120 --out build-wasm/captures/run1` |
| `img-diff.mjs` | Native screenshot vs wasm capture: mismatch per quadrant + diff PNG. | `node wasm/tools/img-diff.mjs native.png wasm.png --threshold 0.05` |
| `serve.mjs` (tools) | Generic COOP/COEP static server with `--root/--port/--data`. For gk_web use **`node wasm/web/serve.mjs`** (port 8090, serves `build-wasm/wasm/`). | — |

## Traps already hit (don't rediscover them)

- **mips2c contexts keep stale registers.** Native `_mips2c_call` leaves the ExecutionContext
  uninitialized on the x86 stack and jak1 sky code depends on the vf27 left from the previous call
  (`init-sky-regs` / `set-tex-offset`). `wasm/mips2c_call.cpp` reuses one uncleared context per
  nesting depth; a zeroed context gave sky GIF tags of 0. Found by diffing native/wasm DMA packets.
- **OffscreenCanvas presents only when the worker yields.** A WebGL context on an OffscreenCanvas
  shows a frame only when its pthread returns to the event loop, so the runtime renders one frame per
  main-loop tick and `main` unwinds into the loop (argv must be static). SDL's
  `SDL_GL_SetSwapInterval` resets the main-loop timing to rAF (which only runs with page focus), so
  `runtime_wasm.cpp` re-asserts the timing around every frame.
- **`vif_interrupt_callback` is skipped** under Emscripten: it is the render thread calling GOAL's
  debug vif1 handler (profiler bars only), and GOAL functions exist only in the EE thread's table.
- **FRAMEBUFFER_UNSUPPORTED from one level at two attachments.** `FramebufferTexturePair` mips attach
  level i at `COLOR_ATTACHMENTi` and then at `COLOR_ATTACHMENT0`; WebGL makes that framebuffer
  unsupported. The adapter keeps the existing attachment.
- **`int` vertex inputs fed unsigned data → `INVALID_OPERATION` at draw.** WebGL2 checks attribute
  signedness at draw time (tfrag/tie/shrub/hfrag `time_of_day_index` etc.); those inputs are `uint`
  in the shared shaders now (same values on desktop).
- **1D palette textures allocated as `UNSIGNED_INT_8_8_8_8`** are invalid with unsized RGBA in
  WebGL; mapped to UNSIGNED_BYTE with a byte swap.
- **An element buffer can't be bound to another target.** Merc/hfrag uploaded their index buffer via
  `GL_ARRAY_BUFFER`; WebGL refused silently at draw (no GL error, only a console line "element array
  buffers can not be bound to a different target") and every merc model was invisible.
  `LoaderStages.cpp` `upload_indices` uses `GL_ELEMENT_ARRAY_BUFFER` with no VAO bound (867b7985d).
- **wasm32 `size_t` is 4 bytes.** `Serializer` lengths (TFrag3Data, memcards) and the zstd header in
  `compress.cpp` are u64 under Emscripten so `.fr3` files and saves from 64-bit tools read the same.
- **Shader rules ANGLE enforces that desktop GL doesn't:** `#version` on line 1, uint switch labels
  need `u`, no implicit conversions. glslang passing is not enough; Chrome is the final word.
- **Timed waits in a Chrome worker on Windows round up to ~15.6 ms** (OS timer tick). Not a renderer
  bug, but it looked like one (1.2 s frames). See the CLAUDE.MD Log, M6 perf entries.

## Limits that matter **(historical: measured with SwiftShader on upstream)**

| Limit | WebGL2 min | SwiftShader | Who cares |
|---|---|---|---|
| MAX_TEXTURE_SIZE | 2048 | 8192 | time-of-day palettes are 8192×1 (`TIME_OF_DAY_COLOR_COUNT`) |
| MAX_UNIFORM_BLOCK_SIZE | 16384 | 65536 | `ub_bones` in merc2/emerc = 128 × 128 B = **16384, exactly the minimum**. Don't grow it. |
| UNIFORM_BUFFER_OFFSET_ALIGNMENT | – | 256 | Merc2 queries it for `glBindBufferRange`. |
| MAX_COMBINED_TEXTURE_IMAGE_UNITS | 32 | 64 | highest unit used is 29 (DirectRenderer `GL_TEXTURE20 + unit`). |
| MAX_TEXTURE_IMAGE_UNITS (fragment) | 16 | 32 | max samplers in one fragment shader is 10 (`sprite_3d.frag`). |
| MAX_SAMPLES | 4 | 4 | MSAA (off in the web build). |

## Open items

- `WEBGL_multi_draw` instead of the draw loop, if profiling asks.
- MSAA through a multisampled renderbuffer + `glBlitFramebuffer` resolve.
- Renderer cost when the page is unfocused (Chrome throttles GPU work: ~35–40 ms draws).
- A capture hook so `capture.mjs --frame N` means something. Proposed, not implemented: after each
  presented frame, under `__EMSCRIPTEN__`,
  `MAIN_THREAD_ASYNC_EM_ASM({ (globalThis.__jak = globalThis.__jak || {}).frame = $0; }, frame);`
- **(historical, proposed, not decided)** a render replay harness (record the DMA chain + texture
  uploads on native, replay in a small native/wasm executable, diff PNGs). Replay files are game
  data: `out/` only. Ask the owner before building it.

## Debugging in the browser

- Local only: `node wasm/web/serve.mjs` then http://localhost:8090/gk_web.html?args=… (it serves
  the staged game data at `/data/`; never publish that).
- Game log: `window.gkLog` on the page; WebGL messages: `window.webglLog`; the browser's own WebGL
  warnings are in the DevTools console. Traps in pthreads print the worker-side stack
  (`wasm/web/web_pre.js`).
- "No error but nothing drawn": check for console-only WebGL warnings (the merc index buffer case),
  then `kLocateErrors`, then Spector.js on one frame.
- A program that links but draws nothing: uniform locations (`-1` is silently ignored), unit
  bindings (`tex_TN` → unit N in `Shader.cpp`), `ub_bones` binding point 1, attribute signedness.
