---
name: native-vs-wasm-diff
description: Use when the wasm build (gk_web or gk_headless) behaves or renders differently from native - wrong pixels, missing geometry, crash, stall, divergent logs. Reproduces on native with the same input, diffs logs, DMA data and frames, and narrows to one renderer or one function before changing code.
---

# Native vs wasm: find the divergence before fixing it

Native is the reference implementation (CLAUDE.MD). Don't guess from the wasm side alone.

## 1. Same input on both sides

- Same game objects (native `out/jak1`, wasm `out/jak1-wasm` built from the same tree), same boot
  args, same level, same frame count.
- Boot args: native `build/Release/bin/game/gk --no-display -- -boot -fakeiso -debug` vs wasm
  headless `node build-wasm/wasm/gk_headless.js -boot -fakeiso -debug`. For Geyser Rock add
  `-wasm-test out/jak1-wasm/obj/goto-training.o wasm-goto-training <frames>` on the wasm side.
- Headless first: Node is deterministic and fast (precise waits), the browser adds timing noise.

## 2. Diff

- **Logs:** same log level, strip timestamps, `diff -u`. The first differing line matters, not the
  last one. In the browser the log is `window.gkLog` on the page.
- **Data:** for renderer bugs, dump what the renderer receives (DMA packets, texture uploads) on
  both sides and diff those before looking at pixels. That is how the stale-vf27 sky bug was found
  (zero GIF tags on wasm only).
- **Pixels:** native screenshot (`game/graphics/screenshot.cpp`) vs a browser capture
  (`node wasm/tools/capture.mjs --url "http://127.0.0.1:8090/gk_web.html?args=..." --timeout N --out build-wasm/captures/wasm`;
  no frame sync: the build doesn't set `window.__jak.frame`, so match by scene, not frame number),
  then `node wasm/tools/img-diff.mjs native.png build-wasm/captures/wasm.png --threshold 0.05` and
  Read the diff PNG. Whole frame wrong → global state (FBO, blit, viewport, `SCISSOR_ADJUST`); one
  region → one bucket renderer. Captures of game scenes are game-derived: keep them local.

## 3. Narrow to one renderer

The ImGui debug menu can disable bucket renderers one by one. Bisect: which renderer, when
disabled on both, makes the frames match? Then compare that renderer's GL state at draw time
(Spector.js in Chrome vs RenderDoc on native), and turn on `kLocateErrors` in
`wasm/web/webgl_compat.cpp`.

## 4. Usual suspects, by symptom

| Symptom | Check |
|---|---|
| sky wrong / sky GIF tags zero | mips2c context cleared between calls (sky reads stale vf27): `wasm/mips2c_call.cpp` must reuse an uncleared context per depth |
| merc models (Jak, Daxter, crates) invisible, no GL error | index buffer bound to the wrong target (console-only WebGL warning); `LoaderStages.cpp` `upload_indices` |
| `INVALID_OPERATION` at draw in one renderer | `int` vertex input fed unsigned data (declare `uint`) |
| `FRAMEBUFFER_UNSUPPORTED` | one texture level at two color attachments (`FramebufferTexturePair`) |
| wrong colors on background only | time-of-day 1D→2D palette, `8_8_8_8` byte swap in `webgl_compat.cpp` |
| geometry missing in one renderer | multi-draw loop, primitive restart index, index type |
| `.fr3` / memcard parse garbage | wasm32 4-byte `size_t` in serialized lengths (u64 under Emscripten) |
| trap on the render thread calling GOAL | only the EE thread's table has GOAL (e.g. `vif_interrupt_callback`, skipped) |
| everything frozen at the first frame | worker never yields, so the OffscreenCanvas never presents; or timed waits rounding up to 15.6 ms |
| EE stalls on sound RPC | IOP thread died on a missing file (VAGWAD.ENG) |

## 5. Fix, then prove it

Fix on the wasm side under `__EMSCRIPTEN__` (or a dual-valid shader change), re-run the same
input and diff, and put the before/after evidence in the commit message body. Add the
symptom → cause pair to the table above and a line to the CLAUDE.MD Log if it was new.
