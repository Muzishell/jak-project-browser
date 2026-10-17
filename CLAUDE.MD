@AGENTS.md

# Jak 1 in the browser: OpenGOAL → WebAssembly

This is a personal fork of `open-goal/jak-project`. Goal: run Jak and Daxter (Jak 1) in a browser by
adding a WebAssembly target to OpenGOAL. First playable scope: Geyser Rock + Sandover Village.

The native x86-64 build must keep working at all times. It is the reference implementation: when the
wasm build misbehaves, compare against native on the same input.

## Hard rules

- Game data (`iso_data/`, `out/`, `decompiler_out/`, extracted assets, compiled game objects, .wasm
  built from game code) never goes into git or anywhere public. These paths are git-ignored; keep it
  that way. Its only allowed destination is the private R2 bucket behind Cloudflare Access (decision 10).
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
10. **Hosting: private, everything served.** Cloudflare Pages serves the shell + runtime. The extracted
    game files (iso files, `.fr3`, audio, per-object game `.wasm`) live in a private R2 bucket, streamed
    by a same-origin Worker (`/data/*`) so COOP/COEP stays simple. Cloudflare Access protects the
    whole domain with an allow-list of the owner's emails, kept in the Access policy (Zero Trust
    dashboard or Terraform), never hard-coded in the app. OPFS caches downloaded files per machine.
    No client-side ISO extraction: any machine that can log in can play.
11. **Saves: OPFS locally + cloud sync.** `kmemcard.cpp` writes bank files under
    `file_util::get_user_memcard_dir()`; under Emscripten that dir (plus settings) is mounted on the
    WasmFS OPFS backend (sync writes work from the pthread). The Worker exposes `GET/PUT /saves/*`
    backed by R2, keyed by the Access identity (`Cf-Access-Authenticated-User-Email`). Pull on boot, push
    after each save, last-write-wins by timestamp. Save format stays identical to native, and the shell
    also offers export/import of the raw files.

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
- [ ] M7 Browser shell + hosting: loading screen, OPFS cache, Pages + Worker + private R2 behind
      Cloudflare Access, save cloud sync and export/import.

## How to work

- Before changing a subsystem, read its native code path end to end and state the plan briefly.
- Keep changes small and testable; each one builds both targets (or says why native isn't affected).
- When a wasm run diverges, reproduce on native with the same input and diff logs before guessing.
- Run `task format` before proposing a commit.
- Note new decisions and gotchas at the bottom of this file under "Log", one line each.

## Log

- 2026-10-17: Checkout dir must be named `jak-project` — `try_get_project_path_from_path` (common/util/FileUtil.cpp) does `rfind("jak-project")`, so `jak-project-browser/` makes goalc-test abort at startup.
- 2026-10-17: M1 done — `Release-linux-clang` builds to `build/Release/bin`; run `build/Release/bin/goalc-test --gtest_filter=-*MANUAL_TEST*` (root `test.sh` expects `build/goalc-test`). 1669/1669 pass.
- 2026-10-17: M2 slice 1 — `InstructionSet::WASM` reuses the x86 `RegisterInfo` (`gRegInfo`): each x86 register id maps to a wasm local. x86-only RAX/RCX constraints for div/mod/shift are skipped for WASM.
- 2026-10-17: Binaryen `version_133` via FetchContent (release tarball + SHA256) only when `OPENGOAL_WASM_BACKEND=ON`; preset `Release-linux-clang-wasm-backend` → `build/WasmBackend/bin`. Keep `BUILD_LLVM_DWARF` ON (Outlining.cpp needs llvm/Support).
- 2026-10-17: `task format` with Ubuntu's clang-format 18 rewrites ~14 untouched upstream files (`() {};` → `(){};`); restore those before committing. `format-json` needs `npx` (not installed).
- 2026-10-17: M2 slice 2 — module ABI documented in `goalc/compiler/wasm/WasmCodeGenerator.h`. Every GOAL function is `(i64 x8) -> i64` (GPR arg regs in, rax out); 128-bit args/returns not supported yet. Imports: `env.memory/table/st/table_base/ee_base`, `sym.<name>` = symbol offset from st. Functions enter the table via an element segment at `table_base`.
- 2026-10-17: EE base is an imported immutable global `ee_base`, not a compile-time constant (decision 6), so the Emscripten runtime can place EE memory itself. Revisit if profiling shows it matters.
- 2026-10-17: Basic blocks come from `to_rai()` jumps/fallthrough (same CFG the allocator uses) → Relooper. Spill slots are extra locals. IR opts in via `IR_WASM_CODEGEN` in IR.h; anything else throws "wasm backend does not support IR: …".
- 2026-10-17: Dumped modules (`OPENGOAL_WASM_DUMP_DIR`) for 14 arithmetic templates + a while loop return the native expected values under Node 24. defun (`mov-fa`) and static data (`mov-sva`) wait for slice 3 (linking).
- 2026-10-17: Hosting decided (decisions 10–11): private Cloudflare Pages + R2 + Worker behind Access with an email allow-list; saves synced to R2 per Access identity. Replaces client-side ISO extraction. Game data may now go to that private bucket, still never to git or anything public.
- 2026-10-17: M2 slice 3 — the wasm backend emits a pair: a normal v3 object (statics + per-function 4-byte slot after the `function` type tag holding the file-local index) and the module. Linker rule: add `table_base` to the word after every `function` type-pointer link. Code reaches data via imported `seg_main/seg_debug/seg_top` globals; offsets come from ObjectGenerator after `generate_data_v3`.
- 2026-10-17: Top-level function has `idx_in_file == -1` and is last in `FileEnv::functions()`; use the position in that vector as the file index.
- 2026-10-17: Node harness `test/goalc/wasm/run.mjs` = mini no-kernel runtime (klink v3 rules, `_format`/`format` subset, prints result like kboot `%ld\n`). Native suites share one runtime, so `WasmTestRunner` replays earlier passing files of the suite first (e.g. add-int-multiple-2 calls add-five from the previous file). `WASM_RUN_DEBUG=1` traces linking. Set `OPENGOAL_NODE` (emsdk's node) when running goalc-test.
- 2026-10-17: M2 slice 4 — FLOAT values live in f32 locals; FLOAT<->GPR moves are bit reinterprets (sign-extended like x86 movd+movsx). maxss/minss = `select` (f32.max/min differ for NaN/-0); cvttss2si = trunc_sat + range check → 0x80000000 like x86. Float compares are ordered (differ from comiss only on NaN). test_arithmetic.cpp is fully ported and matches native output.
- 2026-10-17: Gotcha: `InstructionARM64` with a single zero word is the null instruction (emits 0 bytes). Function slots are therefore 8 bytes (index + zero word); a 4-byte slot for function 0 let static data overlap it.
- 2026-10-17: Harness `~f` is a port of kernel `cvt_float` (single precision, truncating, default precision 4); toFixed() would round differently.
