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
10. **Hosting: private, everything served.** One Cloudflare Worker (`jak-web`, `wasm/host/`) serves
    the shell + runtime as static assets. The extracted
    game files (iso files, `.fr3`, audio, per-object game `.wasm`) live in a private R2 bucket (`jak-data`),
    streamed by the same Worker (`/data/*`) so COOP/COEP stays simple. Cloudflare Access protects the
    Worker (all traffic) with an allow-list of the owner's emails, kept in the Access policy (Zero Trust
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
- [x] M2 `do_codegen_wasm` validated on compiler tests (`test/goalc/`, `test/goalc/test_CodeTester.cpp`
      pattern) executed in Node. No game data needed.
- [x] M3 Headless runtime under Emscripten (GL and audio stubbed) boots until `KERNEL.CGO` is linked
      and the kernel loop runs.
- [x] M4 Asyncify suspend/resume: `ENGINE`/`GAME` load and processes tick headless (ticked by the
      owner 2026-10-21: the game runs in the browser).
      Slice 1 done: kernel suspend/resume, go and throw work (Jak1KernelTest cases pass in wasm).
      Slice 2 done: `ENGINE.CGO` builds for wasm. Next: mips2c functions in the wasm runtime, then
      load ENGINE headless; `GAME.CGO` needs the decompiler's `tpage-dir.txt` (`task extract`).
- [x] M5 WebGL2 renderer: Geyser Rock on screen in Chrome.
- [x] M6 Sandover, audio, Gamepad API, performance: Sandover playable in Chrome with Jak and
      Daxter drawn, keyboard and Gamepad API input, 989snd audio through Web Audio, 60 fps with
      page focus. Left for later: renderer cost when the page is unfocused, an IOP that doesn't
      spin while idle. The owner played with a PS5 controller (works; R2 was the debug fly control
      of the `-debug` boot) and in Firefox (2026-10-21).
- [x] M7 Browser shell + hosting: loading screen, OPFS cache, Pages + Worker + private R2 behind
      Cloudflare Access, save cloud sync and export/import. Owner-verified 2026-10-21: saves from
      gameplay sync and load on a second machine, a second visit loads from the cache, French
      text and voices, the new page in Chrome and Firefox.
      Slice 1 done: game files fetched lazily from `/data/` (WasmFS fetch backend), no preload.
      Slice 2: `wasm/host/` Worker `jak-web` + R2 `jak-data`, deployed 2026-10-20 at
      https://jak-web.adrienmuzika.workers.dev behind Access (all traffic); unauthenticated and
      forged-token requests get the Access login on every path, version previews don't exist.
      Slice 3: OPFS cache of the game files through a service worker (`wasm/web/data_sw.js`),
      verified in Firefox locally, deployed 2026-10-20 (version 10a31d1c).
      Slice 4: saves and settings in OPFS, cloud sync through the Worker's `/saves/*` (private
      bucket `jak-saves`), export/import as .tar, loading screen, title-screen boot by default
      (`wasm/web/page.js`); deployed 2026-10-21 (version e8741fc4), both hostnames still locked
      (90 unauthenticated/forged requests incl. PUT /saves/ all got the Access login).

### M3 plan

1. Emscripten build: `wasm/CMakeLists.txt` (only when `EMSCRIPTEN`), jak1-only headless runtime in
   `build-wasm/`, graphics/sound/input/discord/curl stubbed behind `__EMSCRIPTEN__`. Runs under Node
   (`NODERAWFS`) with `-nokernel` up to the C kernel loop.
2. C <-> GOAL calls: GOAL's funcref table is Emscripten's `__indirect_function_table`, so a C function
   pointer is a table index. Exported C functions get a typed `(i64 x8) -> i64` adapter (macro at the
   registration sites, `(void*)fn` on native). `call_goal` = typed call through the table. 128-bit
   args/returns through a register area in memory.
3. KERNEL.CGO for wasm: `goalc --instruction-set wasm` appends each object's wasm module after its v3
   data (DGO format unchanged). Kernel sources get `INSTRUCTION_SET 'wasm` branches for asm functions.
4. klink instantiates modules against the shared Memory/Table; kernel version check passes and
   `kernel-dispatcher` runs from the C kernel loop.

### M5 plan

1. Browser target `gk_web` (wasm/CMakeLists.txt) with the jak1 OpenGL renderer: SDL3 + WebGL2 on
   an OffscreenCanvas (PROXY_TO_PTHREAD), glad kept for the GL declarations and loaded through
   `SDL_GL_GetProcAddress`. Dev files via `--preload-file` of `out/jak1-wasm/` (local only, never
   published; fetch + OPFS is decision 9 / M7). Done when the kernel boots in Chrome and frames
   are presented (cleared screen).
2. WebGL2 fixes from decision 8 behind `__EMSCRIPTEN__`: shaders rewritten to GLSL ES 3.00 at
   load, 1D textures, `GL_UNSIGNED_INT_8_8_8_8_REV`, multi-draw, polygon mode, debug callback.
3. With the owner's extracted ISO: GAME.CGO + Geyser Rock `.fr3`, then the first level frame.

## How to work

- Before changing a subsystem, read its native code path end to end and state the plan briefly.
- Keep changes small and testable; each one builds both targets (or says why native isn't affected).
- When a wasm run diverges, reproduce on native with the same input and diff logs before guessing.
- Run `task format` before proposing a commit.
- Note new decisions and gotchas at the bottom of this file under "Log", one line each.

## Harness (wasm/, .claude/)

- Skills (`.claude/skills/`): `webgl2-renderer-port`, `wasm-build-and-run`, `native-vs-wasm-diff`,
  `session-handoff` (writes `wasm/HANDOFF.md`, which the SessionStart hook prints).
- Hooks (`.claude/settings.json`) run WSL's emsdk node through `wsl.exe` (no node/git on Windows):
  `session-start.mjs` prints branch, recent commits, uncommitted paths, milestones, HANDOFF.md;
  `guard.mjs` (PreToolUse on Bash|PowerShell) blocks commits on master, commit messages without
  `(AI-assisted)` or with `Co-Authored-By`, `git add -f` and `git add` of game-data paths,
  `gh pr/issue create`, any `git push`, and curl/scp/rsync/... of game-data paths. It reads only
  the command line: commits inside a script file are not seen.
- Tools (`wasm/tools/`, dev only, deps via `npm ci` there, ask first): `gles-shaders.mjs` +
  `webgl-shader-smoke.mjs` check that the shared shaders stay valid GLSL ES 3.00 and link in
  Chromium; `capture.mjs` / `img-diff.mjs` for browser screenshots vs native; `gl-audit.mjs` does
  not run on this branch. Notes in `wasm/docs/RENDERER.md` (the `*-baseline.md` files are stale).

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
- 2026-10-17: M2 slice 5 — 128-bit/VF IR on v128 locals (INT_128 and VECTOR_FLOAT share a local per xmm reg). Operand order follows the x86 VEX forms (vvvv = first source): PEXT*/PCPYLD are swapped (PS2 vs x86), PCPYUD/PCGT*/PSUBW/PACKUSWB/PADDB are not. maxps/minps = bitselect on gt/lt; shift immediates ≥ lane width zero (or sign-fill) like x86, wasm would wrap the count.
- 2026-10-17: Moves between xmm classes use separate locals: FLOAT→vector = replace lane 0 of the vector local (movss keeps upper lanes), GPR→vector = movq (upper 64 zeroed). Code relying on upper lanes after scalar FLOAT ops on the same xmm would differ; none found so far.
- 2026-10-17: Stack frames: shadow stack via imported mutable `sp`, x86 frame layout; only address-taken (`forced_on_stack`) spills go to memory. 128-bit function args/returns are still unsupported (needs an ABI change to the `(i64 x8) -> i64` signature).
- 2026-10-17: `test_wasm_vs_native.cpp` runs templates in `source_templates/wasm-vs-native/` on the native no-kernel runtime and in Node and requires identical output — use it for anything without a native expected string.
- 2026-10-17: M2 done (scope: the no-kernel suites). Ported to Node with native expected output: arithmetic (all), control-statements (all but align16-2), collections (all), variables (10 of 15), plus wasm-vs-native. Left for M3: tests that need the C kernel's type system (deftype/object-new/method-set!, built-in type objects), the with_game/kernel suites (need KERNEL.CGO), and asm functions.
- 2026-10-17: (128-bit calls and special registers done in M3, see below) Still unsupported in wasm, needed by M3/M4: asm functions and asm-only IR (.push/.pop/.ret/.jr, IR_GetSymbolValueAsm), 128-bit function args/returns, and the special registers (pp r13, st r14, off r15) as rlet values — the kernel's gkernel.gc relies on all of these.
- 2026-10-17: Harness layout mirrors native jak1 where tests observe it: 128 MB EE, s7 at 0x14fd24, stack top 0x7fffff0, symbols at offsets ≡4 mod 8 with a `symbol` type tag, types laid out like kscheme `Type` with print_object as method 2, pair new = new_pair.
- 2026-10-18: M3 slice 1 — Emscripten 6.0.12 (`wasm/EMSDK_VERSION`). `emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release`; the root CMakeLists hands off to `wasm/CMakeLists.txt` and returns. Run with `node build-wasm/wasm/gk_headless.js -fakeiso -debug -nokernel -nosound`: reaches `KernelCheckAndDispatch`.
- 2026-10-18: Deviation from decision 7 (flagged for the owner): libco is replaced by `wasm/libco_pthread.cpp`, one pthread per IOP coroutine handed a baton (binary semaphore) on `co_switch`. Emscripten fibers need Asyncify for the whole IOP, this needs nothing; revisit if thread count or switch cost matters.
- 2026-10-18: `wasm/runtime_wasm.cpp` is a jak1-only copy of `game/runtime.cpp` without the DMP thread or graphics (pulling runtime.cpp in drags all games and the renderer). Keep it in sync when runtime.cpp's boot changes.
- 2026-10-18: Headless gk uses the real hid sources (display/input managers) with Emscripten's SDL3 port (`--use-port=sdl3`); stubs only for gfx/display/screenshot/mips2c/Deci2Server/xdbg, the discord-rpc library and `sceGsSync*` (`wasm/stubs.cpp`).
- 2026-10-18: Emscripten gotchas: `long` is 32 bits, so `__linux__` paths stay off (`OS_POSIX` also on `__EMSCRIPTEN__`); the root CMake forces `BUILD_SHARED_LIBS ON` (wasm/ sets it OFF, or libfmt.so is dlopen'd); fmt needs `-include stdlib.h`; "recompile with -fPIC" at link = an undefined data symbol; an indirect call through a mismatched signature traps (IOP thread entries return u32).
- 2026-10-18: C functions exported to GOAL are wrapped at the registration site: `GOAL_C_FUNC(f)` / `GOAL_C_FUNC_PP(f)` / `GOAL_C_STACK_FUNC(f)` (game/kernel/common/goal_c_function.h) are `(void*)f` natively and a typed `(i64 x8) -> i64` adapter (`wasm/goal_call.h`) under Emscripten. GOAL's table is Emscripten's `__indirect_function_table`, so the adapter's address is the table index stored in the function object.
- 2026-10-18: The GOAL shadow-stack pointer `sp` is a JS `WebAssembly.Global` (`Module.goalStackPointer()`, wasm/goal_modules.js), shared by every GOAL module and read/written by C++ through EM_JS; `call_goal_on_stack` sets it.
- 2026-10-18: M3 KERNEL.CGO — `build/WasmBackend/bin/goalc/goalc --game jak1 --instruction-set wasm --cmd '(make-group "kernel")'`. The compiler sets `*wasm-target*` for game.gp, which then writes to `out/jak1-wasm/` (never over native `out/jak1/`); the wasm runtime's fake ISO reads `out/jak1-wasm/iso`. Each .o is the v3 object, the module size (u32), then the module (WasmObject.h `pack_wasm_object`).
- 2026-10-18: klink under `__EMSCRIPTEN__`: begin compiles the module found after the last segment's data and grows the table (its table_base); the `function` type link adds table_base to the slot; after the last link pass the module is instantiated (imports in wasm/goal_modules.js, `sym` imports interned through the exported `wasm_goal_symbol_offset`).
- 2026-10-18: Special registers in wasm: pp (r13) is the mutable i64 global `pp` shared by all modules (C++ reads it via EM_JS for `GOAL_C_FUNC_PP`; call_goal sets pp = st and restores it, like the x86 stubs); reading r14/r15 gives st / ee_base (no writes); rsp is the `sp` global as a GOAL address, so kernel code can switch stacks.
- 2026-10-18: 128-bit args/returns (option 3, owner's choice): imported `xmm_area` = linear address of 9×16 bytes (xmm0, xmm1-8). The caller stores 128-bit args in their xmm slot before `call_indirect`, the callee loads them on entry (its IR-0 constraints), `IR_Return` stores a 128-bit result in slot 0 and the caller loads it. Covered by wasm-vs-native `i128-calls.gc`. C functions with 128-bit args are still unsupported.
- 2026-10-18: Kernel asm functions got `INSTRUCTION_SET 'wasm` branches; their `asm-func` declares moved into the native branches (a declare inside a first-form `#cond` is accepted). reset-and-call and catch-frame new are real wasm functions; thread-suspend/resume, return-from-thread(-dead), set-to-run-bootstrap, throw-dispatch, the enter-state code jump, abandon-thread and deactivate of the running process call the C `wasm-unimplemented` (abort) until M4 (done in M4 slice 1, `wasm-unimplemented` is gone).
- 2026-10-18: Modules import a shared memory with a 4 GB maximum (atomics feature): pthreads make Emscripten's memory shared, and a non-shared import fails to link. The Node harness memory is shared too. Each pthread has its own table, so GOAL functions exist only in the EE thread's.
- 2026-10-18: M3 done — `node build-wasm/wasm/gk_headless.js -fakeiso -debug -nosound` links the 8 KERNEL.CGO objects ("Got correct kernel version 2.0"), prints what native `gk --no-display -- -fakeiso -debug -nosound` prints, and the C kernel loop calls kernel-dispatcher (~700/s) without traps. Native gk needs `--no-display --` before the game args.
- 2026-10-18: M4 slice 1 — goalc runs Binaryen's `asyncify` pass on every wasm module with `asyncify-import-globals` (`env.__asyncify_state` / `env.__asyncify_data`, shared by all modules so one unwind crosses them) and removes its control exports; the runtime sets the globals itself (wasm/goal_thread.cpp).
- 2026-10-18: Asyncify isn't restricted yet: every GOAL call is a call_indirect, so every function that calls anything is instrumented. Restricting it (removelist, or ignore-indirect + addlist) needs to know which call targets can suspend; do it when profiling asks.
- 2026-10-18: Suspend: wasm thread-suspend copies the GOAL stack to the backup stack like x86, then `wasm-thread-suspend` starts the unwind; `wasm-thread-run` (from thread-resume) stops it and keeps the bytes under an id stored in the thread's `freg 0` (unused by wasm; moves with a relocated process; zeroed by cpu-thread new, freed by deactivate). Resume rewinds through set-to-run-bootstrap, which is always the thread's outermost frame.
- 2026-10-18: Leaving a thread (return-from-thread(-dead), abandon-thread, deactivate of the running process), the enter-state code jump and throw are C++ exceptions (the runtime is already `-fwasm-exceptions`), caught by `wasm-thread-run`, `wasm-call-thread` (reset-and-call) and `wasm-catch` (catch-frame new). Not Asyncify: an unwind can't stop at a frame in the middle of the stack.
- 2026-10-18: wasm set-to-run-bootstrap takes no arguments: it reads func and args from the main thread's rregs (no a4/a5 register hack), and enter-state writes the code and its args there before restarting the thread.
- 2026-10-18: A suspend can't cross a C++ frame (call_goal, a catch frame): checked, it aborts with a message. A temporary thread (trans/post) that suspends is abandoned, as on x86, which saves it to a backup stack nobody restores.
- 2026-10-18: The `suspend` macro's KERNEL_DEBUG stack check subtracted `off` from rsp; in wasm rsp is already a GOAL address (`#if` in gkernel-h.gc).
- 2026-10-18: Kernel tests in wasm: `-wasm-test <obj> <function> <frames>` (wasm/kernel_test.h) links an object and runs the function as the listener function; `node test/goalc/wasm/kernel-test.mjs` (build steps in the file) runs Basic, RunFunctionInProcess, StateAndXmm and ThrowXmm and they match x86. It compares only format #t (the listener's print channel): format 0 goes to stdout on native too (e.g. "GOT UNKNOWN INIT: dead" when an init deactivates itself).
- 2026-10-18: M4 slice 2 — `build/WasmBackend/bin/goalc/goalc --game jak1 --instruction-set wasm --cmd '(make-cgo "ENGINE")'` builds all 313 targets (`out/jak1-wasm/iso/ENGINE.CGO`). Only two gaps: `.nop.vf`/`.wait.vf` (no-ops) and `#f` into a 128-bit cond result (movq st, wasm-vs-native `engine-misc.gc`). Engine has no asm-funcs; its asm-like code is `def-mips2c` (C++).
- 2026-10-18: Next blockers: `def-mips2c` functions hit the wasm runtime's `LinkedFunctionTable::get` assert (wasm/stubs.cpp) — they need real game/mips2c code as table entries (decision 3) before ENGINE can load; `(make-cgo "GAME")` stops on missing `decompiler_out/jak1/textures/tpage-dir.txt` (needs `task extract`, not a codegen issue).
- 2026-10-18: mips2c in wasm — game/mips2c (jak1 functions + mips2c_table.cpp, other games' tables `#ifdef`'d out) is compiled into the runtime. `LinkedFunctionTable::reg` stores one of 256 fixed adapters (`wasm/mips2c_call.cpp`: `slot<N>` builds the ExecutionContext from the arg regs, pp, st, and reserves the stack size below the GOAL sp, like `_mips2c_call_systemv`); `jalr` calls `wasm_goal::call8`. SSE intrinsics come from Emscripten's emulation (`-msse4.2`).
- 2026-10-18: ENGINE loads headless: `-wasm-test out/jak1-wasm/obj/load-engine.o wasm-load-engine 30` (test/goalc/source_templates/kernel/load-engine.gc) links all 306 ENGINE.CGO objects, runs their top levels and 30 kernel frames without traps. Missing sound banks / subtitles are the absent game data. Not compared with native yet (native needs the listener for this).
- 2026-10-18: The wasm runtime has no home directory: main_headless overrides the user config dir to `out/jak1-wasm/user` (git-ignored); without it pc-settings.gc landed in `./OpenGOAL`.
- 2026-10-18: M5 slice 1 — wasm/CMakeLists.txt now builds `runtime_core` (everything but the renderer and entry point) plus `gk_headless` (Node, `stubs_headless.cpp`) and `gk_web` (wasm/web/gk_web.cmake: the jak1 OpenGL renderer, imgui in GLES3 mode, glad, zstd in C). Run: `node wasm/web/serve.mjs` (COOP/COEP) then http://localhost:8090/gk_web.html?args=-fakeiso+-debug+-nosound. Shaders and `out/jak1-wasm/iso` are `--preload-file`d under /jak-project (local dev only).
- 2026-10-18: In Chrome, gk_web boots the kernel like Node, SDL3 creates the window and a WebGL2 context on the main pthread (OffscreenCanvas). GL: SDL asks for an ES 3.0 context; only `gladLoadGLLoader` (no `gladLoadGL`); glad.c looks up every entry point by name under Emscripten, since WebGL2 has functions glad files under GL 4.x (they were NULL with a parsed "3.0").
- 2026-10-18: wasm32 size_t is 4 bytes: `Serializer` lengths (`Serializer::SizeField`, used by TFrag3Data and the memcard file) and the zstd header in compress.cpp are u64 under Emscripten, so .fr3 files and saves written by 64-bit tools read the same.
- 2026-10-18: Shaders: `Shader.cpp` rewrites them to GLSL ES 3.00 at load under Emscripten (version line first + precisions, sampler1D → sampler2D with `texelFetch(s, ivec2(i, 0), 0)`, `noperspective` dropped).
- 2026-10-18: Shaders fixed at the source (owner's choice): the 59 files that relied on implicit int → float/uint conversions now use the literal of the converted type (`2.0`, `1u`, `float(255u - x)`), valid as GLSL 4.10 and GLSL ES 3.00 with the same values. Checked: all 92 compile and all programs link under WebGL2 (Chrome), and native gk compiles them on Mesa GL 4.5 core. New shaders must avoid implicit conversions.
- 2026-10-18: Desktop-only GL calls are adapted in wasm/web/webgl_compat.cpp, installed over glad's pointers after loading: 1D textures as 2D height 1, `8_8_8_8_REV` → `UNSIGNED_BYTE`, primitive restart always on (WebGL2's fixed 0xFFFFFFFF), multi-draw as a loop, polygon mode / debug output ignored, texture size queries from a size table, glGetTexImage through a framebuffer read. MSAA is off in the web build (no multisample textures in WebGL2).
- 2026-10-18: M5 slice 2 — in Chrome gk_web stops where native gk does without game data: "GAME.fr3 cannot be opened" after the WebGL2 context and the shader library are up. Rendering anything more needs the owner's extracted ISO.
- 2026-10-18: Debugging traps in pthreads: wasm/web/web_pre.js prints the worker-side stack to the console (the page only gets the message).
- 2026-10-18: (resolved 2026-10-19) Blocked on the owner: `iso_data/jak1` was empty.
- 2026-10-19: Extraction: the owner's ISO is a single `.iso` in iso_data/jak1; `build/Release/bin/decompiler/extractor -e -d -c <iso>` unpacks it in place, decompiles and builds the native game (73 s). Gotcha: it deletes the tracked `iso_data/jak1/.gitignore` — `git checkout -- iso_data/jak1/.gitignore` right after.
- 2026-10-19: Wasm game: `goalc --instruction-set wasm --cmd '(make-group "iso")'` builds every game target into out/jak1-wasm (only the custom `test-zone` level fails: no out/jak1-wasm/fr3). The .fr3 files come from native out/jak1/fr3. Headless, `-boot -fakeiso -debug` boots GAME.CGO and enters village1 like native gk --no-display (level loads ~10x slower).
- 2026-10-19: Geyser Rock: debug boot starts in village1 (`*kernel-boot-level*` does nothing); `-wasm-test out/jak1-wasm/obj/goto-training.o wasm-goto-training <frames>` (test/goalc/source_templates/kernel/goto-training.gc) waits for village1, then `(start 'play (get-continue-by-name *game-info* "training-start"))`.
- 2026-10-19: gk_web data: `wasm/web/stage_data.sh` stages the wasm CGOs, text, VI1/TRA/BEA DGOs and their .fr3 (84 MB) into build-wasm/web-data, preloaded at /jak-project. Local only.
- 2026-10-19: mips2c contexts: native `_mips2c_call` leaves the ExecutionContext uninitialized on the x86 stack, and jak1 sky code depends on the stale vf27 it keeps between calls (init-sky-regs / set-tex-offset copy all their vfs to sky_regs_vfs). wasm/mips2c_call.cpp reuses one uncleared context per nesting depth; a fresh zeroed context gave sky GIF tags of 0 (found by diffing native/wasm DMA packets).
- 2026-10-19: Only the EE thread has GOAL in its wasm table: vif_interrupt_callback (render thread calling GOAL's debug vif1 handler, profiler bars only) is skipped under Emscripten.
- 2026-10-19: Browser frame loop: a WebGL context on an OffscreenCanvas presents only when its worker returns to the event loop, so runtime_wasm.cpp (WASM_DISPLAY) renders one frame per `emscripten_set_main_loop` tick (60 Hz timer, not rAF: the EE waits for vsync and a throttled rAF stalls the game; timing superseded by the M6 perf entry below) and main unwinds into it (argv must be static).
- 2026-10-19: WebGL2 rules the desktop renderer broke (all fixed): 1D palette textures allocated as `UNSIGNED_INT_8_8_8_8` (invalid with unsized RGBA; byte-swapped by webgl_compat), one texture level attached at two color attachments (FramebufferTexturePair mips → FRAMEBUFFER_UNSUPPORTED), and integer vertex inputs declared `int` but fed unsigned data (tfrag/tie/shrub/hfrag `time_of_day_index` etc. → INVALID_OPERATION at draw; now `uint` in the shared shaders, same values).
- 2026-10-19: M5 done — Geyser Rock renders in Chrome (gk_web, `?args=-boot+-fakeiso+-debug+-nosound+-wasm-test+/jak-project/out/jak1-wasm/obj/goto-training.o+wasm-goto-training+100000000`, served by wasm/web/serve.mjs). Native gk still compiles every shader (Mesa GL 4.5). Known gaps for M6: ~1.2 s kernel dispatch per frame and minutes to link GAME.CGO (per-object module compile), MSAA off, no audio, GAME load not compared frame-by-frame with native.
- 2026-10-19: M6 perf profiling: headless Node links GAME.CGO in ~1.7 s and runs Geyser Rock at ~2–3 ms of GOAL work per frame, and all 326 GAME modules compile + instantiate in ~0.4 s in Chrome too. So module compilation, Asyncify, pp via EM_JS and codegen were not the browser's cost; the IOP took ~300 ms per GAME object (EE waiting in RpcSync).
- 2026-10-19: Root cause: in a Chrome worker on Windows every timed wait that times out sleeps at least the OS timer tick, ~15.6 ms (`Atomics.wait` 0.1 ms → 15.2 ms, `setTimeout(16)` → 31 ms), and the page's rAF in a worker only runs while the page has focus. Linux/Node waits are precise, which is why headless was fast.
- 2026-10-19: wasm/libco_pthread.cpp's baton is a mutex + condition variable: Emscripten's libc++ has no futex for atomic waits, so `std::binary_semaphore::acquire` polls with `sleep_for(elapsed/2)` and its release wakes nothing — each IOP thread switch cost ≥15.6 ms.
- 2026-10-19: `IOP::wait_run_iop` (Emscripten only) waits on the cv until 20 ms before the wakeup, then spins; `signal_run_iop` sets `run_signaled` to end the spin. IOP delays are mostly the ISO thread's `DelayThread(100)` polls, so the IOP thread now uses a core while idle (native wakes every 100 µs too).
- 2026-10-19: Browser frame loop: setImmediate timing (Emscripten uses `scheduler.postTask` in the worker, ~0.01 ms) and 60 Hz pacing inside the tick, which returns early until the next frame is due; the renderer's FrameLimiter (sleeps) is skipped under Emscripten. SDL's `SDL_GL_SetSwapInterval` resets the main loop timing (rAF for vsync — the old build only rendered while the page had focus), so runtime_wasm.cpp re-asserts it around every frame. Emscripten's setImmediate polyfill doesn't work in a pthread (it posts to the page), postTask does.
- 2026-10-19: Prints from pthreads are proxied synchronously to the page's main thread (`fd_write`), so shell.html's print only buffers lines (full log in `window.gkLog`) and redraws the `<pre>` at most every 100 ms; redrawing per line cost ~5–20 ms per printed line.
- 2026-10-19: Measured in the desktop app's Chrome 152 pane, same boot (`-wasm-test …goto-training.o`). Before (521db0f0b): unfocused, kernel → end of GAME.CGO 88.7 s and the render loop stalled at the first game frame (the ~1.2 s frames); focused, GAME.CGO 49 s, Geyser Rock reached 118 s after page load, 23 ms/frame there. After: GAME.CGO links in 0.6 s, Geyser Rock in ~20–30 s, 16.7 ms/frame (60 fps cap) focused; unfocused Chrome throttles the GPU work (renderer draw ~35–40 ms, ~47 ms frames).
- 2026-10-19: Profiling notes: Chrome has no JS Self-Profiling API in workers, so timings came from `emscripten_get_now` counters (removed); the renderer's `m_profiler.to_string()` gives per-bucket times. The IOP sound-RPC stall ("STALL: waiting for IOP on RPC port #0" forever) was the missing VAGWAD.ENG, see below.
- 2026-10-20: Audio: `Player::InitCubeb` under Emscripten hands its tick to `wasm_audio::start` (wasm/audio_output.h). gk_web (wasm/web/audio_web.cpp) plays an SDL3 audio stream (Emscripten's SDL3 port: Web Audio ScriptProcessor on the page, resampled to the AudioContext rate, resumed on the first click/key); a producer thread renders on the wall clock, 40–150 ms ahead, so the 989snd tick (240 Hz, which the sound code depends on) runs even while the browser holds the output back. gk_headless has a no-op. The synth costs ~10–14% of real time in Chrome.
- 2026-10-20: The IOP thread dies (null FileRecord in `open_wad`) when VAGWAD.ENG is missing, and the EE then stalls forever on the sound RPC; native always has it. stage_data.sh now stages all SBK/MUS/STR and VAGWAD.ENG (gk_web.data ~370 MB, local only) until M7 loads files lazily.
- 2026-10-20: No merc model was drawn in the browser (Jak, Daxter, crates, windmill): MercLoaderStage uploaded its index buffer (created as GL_ELEMENT_ARRAY_BUFFER) by binding it to GL_ARRAY_BUFFER, which WebGL refuses, so the indices stayed zero and every draw was degenerate with no GL error at draw time (Chrome's console showed "element array buffers can not be bound to a different target"). LoaderStages.cpp `upload_indices` goes through GL_ELEMENT_ARRAY_BUFFER with no VAO bound under Emscripten (merc and hfrag).
- 2026-10-20: Gamepad API: SDL3's Emscripten joystick driver reads a pad's vendor/product/XInput flag with EM_JS that runs in the pthread handling the connection, where `navigator.getGamepads` doesn't exist, so a connecting pad killed the render thread. web_pre.js mirrors the pads' ids from the page to the workers over a BroadcastChannel and gives workers a `navigator.getGamepads` returning the mirror; buttons/axes still go through the html5 API, which Emscripten proxies to the page. The SDL controller DB (game/assets/sdl_controller_db.txt) is preloaded.
- 2026-10-20: Input check: keyboard control is on when no pad is connected (`_keyboard_temp_enabled`) and is polled with SDL_GetKeyboardState, so a scripted test must hold keys (dispatch keydown, wait, keyup on `window`); a scripted pad is a page-side `navigator.getGamepads` override plus a `gamepadconnected` event, with its `timestamp` bumped on each change.
- 2026-10-20: Installed the Claude Code harness (skills, hooks, wasm/tools, wasm/docs), adapted to this branch: hooks run emsdk's node through `wsl.exe`, the shader translator/smoke test are checkers of the dual-valid shaders (`shader-patches.json` emptied), and the harness's `gl_target.h` / patch 0001 were not installed (`webgl_compat.cpp` + `__EMSCRIPTEN__` instead), so `gl-audit.mjs` doesn't run.
- 2026-10-20: M7 slice 1 — gk_web links with `-sWASMFS` and no longer preloads game data (gk_web.data 370 MB → 0.7 MB: shaders + SDL controller DB). `main_web.cpp` mounts `wasmfs_create_fetch_backend("data", 2 MB)` at `/jak-project/out` and creates every file listed in `data/manifest.txt` (the fetch backend can't list directories; fake_iso iterates the iso dir). A file is fetched on first read: HEAD, then 2 MB Range chunks if it's over 4 MB, else whole; chunks stay in JS memory. WasmFS makes preloaded directories read-only (0555; mkdir under them fails with ENOENT), so `/jak-project` is chmod'ed first. `stage_data.sh` hard-links all of out/jak1-wasm/iso + out/jak1/fr3 into build-wasm/web-data/out and writes the manifest; `serve.mjs` serves it at /data/ with HEAD + Range. Geyser Rock reached as before. No OPFS cache yet (every page load refetches).
- 2026-10-20: Decision 10 packaging changed with the owner's OK: one Worker with static assets (`jak-web`, `wasm/host/`) instead of Pages + Functions, because Access attaches to a Worker once for all its hostnames, while Pages' switch only covers previews (the production pages.dev needs a manual workaround, and previews are public by default). `run_worker_first`: the Worker adds COOP/COEP and verifies the Access JWT (`src/access.js`: RS256 against the team certs, aud, iss, exp) on every request, failing closed when `ACCESS_TEAM_DOMAIN`/`ACCESS_AUD` are empty. `/data/*` reads R2 `jak-data` with HEAD + Range. R2 holds English voices only (343 files, 730 MB). `wrangler.jsonc` has `workers_dev: false` until Access protects the Worker. Local dev: `DEV_SKIP_ACCESS=1` in git-ignored `.dev.vars`, local R2 in build-wasm/wrangler-state.
- 2026-10-20: M7 slice 3 — OPFS cache of the game files through a service worker, not in C++: the WasmFS fetch backend's HEAD/Range fetches come from pthreads, which the page's service worker controls, so `wasm/web/data_sw.js` answers /data/ from OPFS (Range = `Blob.slice`, nothing loaded whole) and passes anything uncached to the network. The page (shell.html) fills OPFS in the background 20 s after start, one file at a time streamed to `<md5>.part` then moved to `<md5>` (content-addressed: a changed file gets a new name, the next fill deletes old ones), and calls `navigator.storage.persist()`. The game waits in preRun (`addRunDependency`) until the worker controls the page (5 s cap); without one it falls back to the network. Manifest lines are now "path size md5". The desktop app's browser pane can't register one from the local http server ("unknown error when fetching the script") but can on the deployed https site; Firefox 158 works locally, through 127.0.0.1:8090 (its `localhost` tries IPv6 first, which WSL doesn't forward).
- 2026-10-21: Frame pacing (the owner's "freeze for 1-2 frames about every second"): gk_web rendered on a 60 Hz clock (postTask ticks), the page presents on the display's refresh, so the two beat (59.94 Hz, jitter): about once a second a refresh got no new frame or two. Now `FramePacer` (runtime_wasm.cpp) renders once per display refresh, at most 60/s (rAF timestamps, so a refresh noticed late doesn't make the next one look too close), and the EE's chain is polled (`Gfx::chain_ready`, try_lock) instead of `render_game_frame` blocking up to 40 ms (0 ms under Emscripten; 40 ms timeout kept in the pacer). Without refreshes (hidden page, the desktop pane) it falls back to the clock.
- 2026-10-21: With an OffscreenCanvas, a frame drawn in a task after the worker's rAF callback is committed with the next animation frame, and the worker's rAF waits for it: rendering "right after" rAF from the postTask loop ran at ~25 fps. So a worker rAF chain (EM_JS) calls `gk_refresh` (EMSCRIPTEN_KEEPALIVE) and renders inside the callback when the chain is there; only late chains render from the main loop. clang-format turns `=>` in EM_JS bodies into `= >`: use `function`.
- 2026-10-21: Pacing tools: `?stats=1` prints a `[frames]` line every 10 s (frames, how many on refreshes / inside rAF, mean and max gap, gaps > 25 ms, frames without a chain, rAF gaps > 25 ms); `?pacing=clock` restores the old clock pacing for A/B. The desktop pane is GPU-bound (~29 ms/frame) and delivers rAF intermittently: both modes ~35 fps there, refresh mode max gap 44-58 ms vs 59 ms. The 60 Hz check needs a real browser.
- 2026-10-21: M7 slice 4 — saves (decision 11). `main_web.cpp` mounts `wasmfs_create_opfs_backend()` at `/user`; that backend's root is the origin's OPFS root, so the game's settings and memory card are OPFS files `OpenGOAL/jak1/{settings,saves}/...` next to the file cache's `data-cache/`. `wasm/web/page.js` (moved out of shell.html, shipped like data_sw.js) syncs them with the Worker's `/saves/*` (bucket `jak-saves`, keys `<email from the verified Access JWT>/<path>`, customMetadata mtime, PUT older than stored → 409): pull newer files in preRun before the game opens anything, push files whose lastModified changed every 10 s and when the page is hidden. OPFS can't set lastModified, so `save-sync.json` keeps per file the version time it holds and the lastModified it had then. Export/import is a ustar .tar written/read in the page; an import is stored as `pending-import.tar` and applied on the next start (the running game holds its files). The page boots `-boot -fakeiso` (title screen) by default: R2 held in the `-debug` boot is cheat mode's fly control (target.gc, main.gc). Loading screen over the canvas until the log shows a level's first display command or the blackout ending.
- 2026-10-21: Intro crash (c_symlink2 assert "val is 0x49a7f130", 14th streamed chunk of SAISA.STR) was Emscripten's WasmFS fetch backend (libwasmfs_fetch.js, emsdk 6.0.12): a file ≤ 2 × chunk size is downloaded whole and stored as one chunk the file's size, but getFileRange kept computing chunk numbers with the backend's 2 MB, so the first read crossing 2 MB re-fetched "chunk 1" and overwrote chunk 0 (the whole file) with its first 2 MB; that read copied 24 KB and left stale bytes in the overlord's buffer. Deterministic, not timing (same values on the local server without the service worker). `wasm/web/wasmfs_fetch_fix.js` (--js-library; user libraries link after system ones and replace their symbols) is upstream's code with the stored geometry used once a file is in memory and no fetch at/after EOF. Repro: `test/goalc/source_templates/kernel/goto-intro.gc` (debug boot → new game at intro-start), browser only; headless takes a different path (frames run unpaced).
- 2026-10-21: Fetch backend fix deployed (jak-web 7b3514e6), still locked (90/90). Slow intro frames the owner saw (200–750 ms "Kernel dispatch time", printed only above 50 ms) did not reproduce in the desktop app's Chromium pane on the same machine: local server 0–1 per chunk at ~53 ms; with `GK_DATA_DELAY_MS=300` on every /data/ answer (serve.mjs, new) 0 (the IOP waits on the network without stalling the EE's frame); with the cache filling during the intro 2 (69/81 ms). Linking the streamed chunks costs 0.05–0.4 ms. Remaining suspect: the browser (Firefox?). The pane's origin quota stopped a local fill at 655 MB ("exceed its storage quota"); the game kept reading from the network.
- 2026-10-21: Firefox's long frames (200–500 ms about once a second) were its garbage/cycle collectors pausing the render worker, filled by the main loop's setImmediate timing (scheduler.postTask back to back, a task + promise per tick); the EE waited them out in gl_sync_path (Firefox Profiler, threads named after goalc started keeping the wasm name section: `BinaryenSetDebugInfo(true)`, GAME.CGO +1.5%). `update_loop_timing` (runtime_wasm.cpp) ticks immediately only without refreshes or while a refresh's frame waits for a late chain, else on a 10 ms timer. Owner's Firefox 158 during the intro afterwards: 551 frames / 10 s, all on refreshes, 0 gaps > 25 ms (same as the Chromium pane). Still open: the IOP thread spins on steady_clock::now (IOP::wait_run_iop's last 20 ms), one core at 100%, costly in Firefox (performance.now precision reduction).
- 2026-10-21: Loop timing fix deployed (jak-web c40aae61), still locked (90/90). Tried letting the IOP sleep instead of spin when the ISO thread has no load queued (only priority-3 VAG streams or nothing; music/voice streams keep the queue non-empty during play, so "queue empty" never happened): at Geyser Rock the IOP then slept ~100% (spin 0–7 ms per 10 s vs ~9.9 s), but A/B in the Chromium pane showed the browser missing refreshes (28–118 refresh gaps > 25 ms per 10 s vs 0) with render time up from 3–5 ms to 7–11 ms mean. Likely CPU power management: the spinning core kept the clocks up. Not committed; the spin stays until the owner chooses (power vs smoothness).
- 2026-10-21: Page UI: the game view fills the window; page.js passes its size (CSS px × quality) to `gk_set_view_size`, and the render thread applies it with DisplayManager::enqueue_set_window_size. SDL multiplies by the pixel ratio itself in the worker (1280×720 CSS px at 1.25 → 1600×900 drawing buffer, logged as "[view] drawing buffer"), and the game goes widescreen on its own. Saves/display/cache/log controls moved to a side panel behind a fading Menu button; log drawer with copy. In the desktop app's pane CSS transitions freeze while the window is hidden, so measure layout with transitions off.
- 2026-10-21: wasm/tools set up (owner OK): `npm ci`, `npx playwright install chromium`. WSL lacked libnspr4/libnss3/libasound2t64; instead of sudo they are `apt-get download`ed and unpacked with `dpkg-deb -x` into ~/tools/pwlibs/root, so run the browser tools with `LD_LIBRARY_PATH=~/tools/pwlibs/root/usr/lib/x86_64-linux-gnu`. Results: selftest passes (incl. WebGL2), the 92 shared shaders are valid GLSL ES 3.00 with no translator change needed, 46/46 programs link in headless Chromium (SwiftShader, which offers WEBGL_multi_draw).
- 2026-10-21: Graphics tradeoffs (item 9, not done). MSAA: WebGL2 has no multisample textures, only multisample renderbuffers resolved with glBlitFramebuffer, so make_fbo's MSAA becomes renderbuffer + resolve, plus a resolve before every pass that reads the framebuffer (eyes, warp, blit displays); cost scales with pixels (4× samples at 1600×900+), on top of the view now rendering at device pixels, which already reduces aliasing. Options: MSAA as a Quality choice, or a cheap post-process AA. WEBGL_multi_draw: one call for many index ranges instead of the loop in webgl_compat (fewer calls into the browser's GPU process per frame, most useful in dense levels); low risk with the loop kept as fallback where the extension is missing.
- 2026-10-21: Tests after the goalc name-section, French default and log changes: native goalc-test 1669/1669, WasmBackend goalc-test 1745/1745.
- 2026-10-22: Blue eco vents check task-complete? (real-complete) of jungle-eggtop, which needs the switch's power cell collected, not just the switch pressed (owner confirmed working). test/goalc/source_templates/kernel/goto-vents.gc closes the switch stage, warps to jungle-start and prints the blue vents' states.
