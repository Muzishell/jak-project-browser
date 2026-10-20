---
name: wasm-build-and-run
description: Use to configure, build, stage data for, serve or launch the Emscripten (WebAssembly) targets of jak-project (gk_web in Chrome, gk_headless in Node), or when a wasm build/link/startup fails. Covers emsdk, build-wasm/, the wasm game objects, stage_data.sh, wasm/web/serve.mjs on port 8090 and the boot arguments.
---

# Building and running the wasm targets

Environment: WSL2 Ubuntu, repo at `~/code/jak-project` (on Windows: `\\wsl.localhost\ubuntu-24.04\home\muzi\code\jak-project`;
run Linux commands as `wsl.exe -e bash -c "..."` or from a script file). emsdk lives in
`~/tools/emsdk`, version pinned in `wasm/EMSDK_VERSION` (6.0.12); its node is
`~/tools/emsdk/node/24.19.0_64bit/bin/node`. Build dirs, never mixed: `build/Release` (native
reference), `build/WasmBackend` (native goalc with the wasm backend), `build-wasm/` (Emscripten).

## 0. Check the current state first

`git log --oneline -15`, `wasm/HANDOFF.md`, the CLAUDE.MD Log tail, `ls build-wasm/wasm`. Trust the
repo over this file if they disagree, and fix this file.

## 1. Toolchain

```bash
source ~/tools/emsdk/emsdk_env.sh
emcc --version | head -1          # must match wasm/EMSDK_VERSION
```

`wasm/tools` deps (`npm ci`, `npx playwright install chromium`) download packages and a browser:
**ask the owner before running them.**

## 2. Game objects for wasm (needs the extracted ISO; output is game data, local only)

```bash
build/WasmBackend/bin/goalc/goalc --game jak1 --instruction-set wasm --cmd '(make-group "iso")'
```

Writes `out/jak1-wasm/` (never over native `out/jak1/`). The `.fr3` files come from native
`out/jak1/fr3`. Test objects such as `goto-training.o` come from
`test/goalc/source_templates/kernel/` (see CLAUDE.MD Log, 2026-10-19).

## 3. Configure + build

```bash
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
bash wasm/web/stage_data.sh                # -> build-wasm/web-data/out (hard links to out/ + manifest.txt)
cmake --build build-wasm --target gk_web   # and/or gk_headless
```

- Output: `build-wasm/wasm/gk_web.{html,js,wasm,data}`, `build-wasm/wasm/gk_headless.{js,wasm}`.
- `gk_web.data` (~0.7 MB) holds only the shaders and the SDL controller DB; shader/shell/pre-js
  changes relink by themselves (LINK_DEPENDS). Game files are not in the build: gk_web uses WasmFS
  and mounts a fetch backend at `/jak-project/out` from `/data/manifest.txt` (`main_web.cpp`), so
  each file is fetched from `/data/` on first read, in 2 MB ranges. Restaging needs no relink.
- A third-party lib that won't configure: guard it out in `wasm/CMakeLists.txt` and say what it
  removes. x86 intrinsics: Emscripten's SSE emulation (`-msse4.2`) covers mips2c/VU code.

## 4. Run

Browser (local only: the build contains game data, never publish or upload it):

```bash
node wasm/web/serve.mjs            # COOP/COEP, serves build-wasm/wasm on 127.0.0.1:8090
```

- Sandover (debug boot starts in village1): http://localhost:8090/gk_web.html?args=-boot+-fakeiso+-debug
- Geyser Rock: `?args=-boot+-fakeiso+-debug+-wasm-test+/jak-project/out/jak1-wasm/obj/goto-training.o+wasm-goto-training+100000000`
- Kernel only, no game data: `?args=-fakeiso+-debug+-nosound` (the shell's default).
- Audio starts on the first click/key (Web Audio resume). Keep the page focused: unfocused Chrome
  throttles the GPU work (~47 ms frames).
- The game log is on the page (`window.gkLog`; WebGL lines in `window.webglLog`), not the console.
- Run the server in the background (`run_in_background`) and stop it when done.
- Scripted capture: `node wasm/tools/capture.mjs --url "http://127.0.0.1:8090/gk_web.html?args=-boot+-fakeiso+-debug" --timeout 120 --out build-wasm/captures/run1`.
  The build does not set `window.__jak.frame`, so `--frame` never triggers: it screenshots after `--timeout`.

Headless (Node, NODERAWFS, real files):

```bash
node build-wasm/wasm/gk_headless.js -fakeiso -debug -nosound                 # kernel loop
node build-wasm/wasm/gk_headless.js -boot -fakeiso -debug                     # GAME + village1
node build-wasm/wasm/gk_headless.js -boot -fakeiso -debug -wasm-test out/jak1-wasm/obj/goto-training.o wasm-goto-training 600
```

Native reference: `build/Release/bin/game/gk --no-display -- -fakeiso -debug -nosound` (native gk
needs `--no-display --` before the game args).

Tests: `build/Release/bin/goalc-test --gtest_filter=-*MANUAL_TEST*` (1669), wasm suites with
`OPENGOAL_NODE` set to emsdk's node, `node test/goalc/wasm/kernel-test.mjs` (4 cases).

## 5. Typical failures

| Symptom | Likely cause |
|---|---|
| `SharedArrayBuffer is not defined`, pthreads never start | page not cross-origin isolated → use `wasm/web/serve.mjs`, not `python -m http.server` |
| stuck at the first frame, frames ~1 s | a timed wait in a worker rounding up to the Windows 15.6 ms tick, or rAF without focus (CLAUDE.MD Log, M6 perf) |
| "STALL: waiting for IOP on RPC port #0" forever | a missing sound file killed the IOP thread (VAGWAD.ENG): restage with `stage_data.sh` |
| "GAME.fr3 cannot be opened" | data not staged / not relinked after staging |
| `table index is out of bounds` | GOAL call through an unset funcref Table slot (decision 3), or GOAL called from a non-EE thread (only the EE thread's table has GOAL) |
| `memory access out of bounds` near low addresses | null deref that native's 512 KB mprotect guard used to catch (decision 6) |
| "recompile with -fPIC" at link | an undefined data symbol |
| indirect call trap | function pointer called through a mismatched signature (IOP thread entries return u32) |
