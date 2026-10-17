@AGENTS.md

# Jak 1 in the browser: OpenGOAL → WebAssembly

This is a personal fork of `open-goal/jak-project`. Goal: run Jak and Daxter (Jak 1) in a browser by
adding a WebAssembly target to OpenGOAL. First playable scope: Geyser Rock + Sandover Village.

The native x86-64 build must keep working at all times. It is the reference implementation: when the
wasm build misbehaves, compare against native on the same input.

## Hard rules

- Never commit, print, upload, or paste game data or anything derived from it: `iso_data/`, `out/`,
  `decompiler_out/`, extracted assets, compiled game objects, generated .wasm built from game code.
  These paths are git-ignored. Keep it that way.
- Never create issues or PRs on the upstream repo (see AGENTS.md).
- Commit messages end with `(AI-assisted)`, as AGENTS.md requires. Do not add a `Co-Authored-By` trailer:
  upstream's AI_POLICY.md does not merge AI co-authored commits, and the owner writes/curates commits himself.
- Work on the `wasm` branch. `master` mirrors upstream; never commit to it. Rebase `wasm` on `upstream/master`
  when asked, not on your own initiative.
- Every wasm-specific change sits behind the instruction-set switch or an `__EMSCRIPTEN__` guard.
  Do not change x86 or ARM64 behaviour.

## Environment

- Windows host, all work happens in WSL2 (Ubuntu). The repo lives in the WSL filesystem (`~/jak-project`),
  not under `/mnt/c` (I/O there is far too slow for this build).
- Toolchain: git, CMake, Ninja, clang, Python 3, Node, Task (taskfile.dev), emsdk (version pinned in
  `wasm/EMSDK_VERSION` once created), wabt (`wasm2wat` to inspect output).
- Two build dirs: `build/` (native reference, via the `task` commands in AGENTS.md) and `build-wasm/`
  (Emscripten). Never mix them.

## Architecture decisions (already made, don't relitigate without asking)

1. **Backend at the IR level, not the emitter.** `goalc/emitter/IGen*` emits register machine code
   (x86/ARM64 dispatched by `InstructionSet`); wasm doesn't fit there. Add `InstructionSet::WASM` and a
   `do_codegen_wasm` next to `do_codegen_x86` / `do_codegen_arm64` on each IR class in
   `goalc/compiler/IR.h`. Allocated registers → wasm locals; 128-bit and vector-float → `v128`.
2. **Emit modules with Binaryen's C API.** `IR_GotoLabel` / `IR_ConditionalBranch` are arbitrary gotos;
   use Binaryen's Relooper for structured control flow and `wasm-opt` for optimization.
3. **Calls go through a shared funcref Table.** A GOAL function value stays an address in EE memory; the
   function's table index is stored at that address, and a call is `call_indirect` on the loaded index.
   The kernel's runtime machine-code stubs (`game/kernel/common/codegen.h`) become table entries for C++
   functions. `game/mips2c/` functions are plain C++ and compile directly.
4. **One .wasm per GOAL object file, built ahead of time.** `game/kernel/common/klink.cpp` instantiates
   each module against the shared `Memory` and `Table` instead of copying machine code into EE memory.
   No GOAL compilation in the browser. The listener/REPL (`DMP` thread, deci2) is not ported.
5. **Suspend/resume via Asyncify first.** `thread-suspend` / `thread-resume` in
   `goal_src/jak1/kernel/gkernel.gc` save pc+sp and copy the process stack into the process heap. Wasm
   can't capture a pc. Prototype with Binaryen's Asyncify pass restricted to functions that may suspend;
   only move to compiler-generated resumable functions if profiling demands it.
6. **EE memory:** 128 MB (`common/goal_constants.h`) as a fixed region of linear memory; the base offset
   becomes a constant. The 512 KB `mprotect` null guard is lost: add bounds checks in debug builds.
7. **Runtime threads:** EE, IOP, EE-Worker via Emscripten pthreads, `PROXY_TO_PTHREAD` + OffscreenCanvas.
   Requires COOP/COEP headers. IOP's `libco` → Emscripten fibers or explicit state. `cubeb` → SDL2 audio
   or an AudioWorklet.
8. **Renderer: WebGL2.** Shaders are `#version 410 core` with no geometry/compute → translate to GLSL ES 3.00.
   Restart index `UINT32_MAX` already matches WebGL2. To fix: `GL_TEXTURE_1D` → 2D height 1,
   `GL_UNSIGNED_INT_8_8_8_8_REV` → `GL_UNSIGNED_BYTE`, `glMultiDrawElements` → `WEBGL_multi_draw`,
   drop `glPolygonMode` / debug callback. WebGPU only if profiling requires it.
9. **Files:** `game/overlord/common/fake_iso.cpp` (reads `out/jak1/iso`) and the `.fr3` loader
   (`game/graphics/opengl_renderer/loader/Loader.cpp`) go through Emscripten FS backed by fetch + OPFS,
   loaded lazily per level.

## Scope for the first playable

`KERNEL.CGO`, `ENGINE.CGO`, `GAME.CGO`, `TRA.DGO` (Geyser Rock), `VI1.DGO` (Sandover) + their `.fr3` and
audio. Optional: `INT.DGO` (intro), `TIT.DGO` (title). The game streams neighbouring levels at borders:
either stub those load triggers (`BEA`, `JUN`, `MIS`) or include them.

## Milestones (update the status here as they land)

- [x] M1 Native build from source on `wasm` branch; `goalc-test` passes.
- [ ] M2 `do_codegen_wasm` validated on compiler tests (`test/goalc/`, `test/goalc/test_CodeTester.cpp`
      pattern) executed in Node. No game data needed.
- [ ] M3 Headless runtime under Emscripten (GL and audio stubbed) boots until `KERNEL.CGO` is linked
      and the kernel loop runs.
- [ ] M4 Asyncify suspend/resume: `ENGINE`/`GAME` load and processes tick headless.
- [ ] M5 WebGL2 renderer: Geyser Rock on screen in Chrome.
- [ ] M6 Sandover, audio, Gamepad API, performance.
- [ ] M7 Browser shell: loading screen, OPFS cache, client-side extraction from the user's own ISO.

## How to work

- Before changing a subsystem, read its native code path end to end and state the plan briefly.
- Keep changes small and testable; each one builds both targets (or says why native isn't affected).
- When a wasm run diverges, reproduce on native with the same input and diff logs before guessing.
- Run `task format` before proposing a commit.
- Note new decisions and gotchas at the bottom of this file under "Log", one line each.

## Log

- 2026-10-17: Checkout dir must be named `jak-project` — `try_get_project_path_from_path` (common/util/FileUtil.cpp) does `rfind("jak-project")`, so `jak-project-browser/` makes goalc-test abort at startup.
- 2026-10-17: M1 done — `Release-linux-clang` builds to `build/Release/bin`; run `build/Release/bin/goalc-test --gtest_filter=-*MANUAL_TEST*` (root `test.sh` expects `build/goalc-test`). 1669/1669 pass.
