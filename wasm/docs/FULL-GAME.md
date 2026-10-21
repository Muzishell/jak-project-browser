# Full game in the browser: strategy (proposed M8)

M1–M7 delivered the first playable (title, intro, Geyser Rock, Sandover) on the hosted site. This
is the plan to make the whole of Jak 1 playable, smoothly, in Chrome and Firefox. Nothing here is
started; each phase ends with something the owner can check.

## What changes with the whole game

| Area | First playable | Whole game |
|---|---|---|
| GOAL code | GAME.CGO + VI1/TRA/BEA paths | every level's code, enemies, bosses, zoomer, all mips2c functions |
| Rendering | tfrag/tie/shrub, merc, sky, ocean near Sandover | every renderer at its worst: Citadel and Snowy density, Spider Cave lighting, Lava Tube, Precursor Basin speed |
| Streaming | one cutscene chain, a few level borders | ~160 STR cutscene files, level-to-level streaming at every border |
| Sound | Sandover/Geyser banks and music | every sound bank, music track, voice stream |
| Storage | ~200 MB really used | ~900 MB cached (English + French) |

The data is already hosted (every level's DGO/fr3 is in `jak-data`); what's missing is proof
that each part runs correctly and fast enough.

## Principles

- **Find problems by sweeping, not by playing.** A run through the game takes ~15 hours; a
  scripted sweep of every continue point takes minutes and runs after every fix.
- **Native is the reference.** Each wasm divergence is compared with native on the same input
  (the `native-vs-wasm-diff` skill): DMA chains, logs, screenshots.
- **Measure first, optimize the measured hot spot.** `?stats=1`, the Firefox Profiler (GOAL
  names are in the modules now) and the Chrome profiler.
- **No regression on what works:** the first-playable checks stay in the sweep.

## Phase A: instruments (small, first)

1. **Level sweep, headless.** A test object (like `goto-training.gc`) that walks every continue
   point of `*game-info*` in story order: start there, wait until the level is active, let it run
   N seconds, next. Headless (`gk_headless`) catches crashes, asserts, unimplemented mips2c
   functions and GOAL errors in minutes. Output: one line per continue point, pass/fail.
2. **Level sweep, browser.** The same object in `gk_web`, driven by `wasm/tools/capture.mjs`
   (headless Chromium now works, see the Log): a screenshot and the `[frames]` stats per continue
   point, plus WebGL errors. Add the frame hook capture.mjs expects (`window.__jak.frame`).
3. **Native references.** The same sweep in native `gk` with screenshots at the same resolution,
   then `wasm/tools/img-diff.mjs` per continue point. A large diff in one region points at one
   renderer.
4. **Cutscene sweep.** Cutscenes start from their triggers; a second test object that plays each
   scene's spool (`ambient`/`sequence` entities by name) back to back, checking every STR chunk
   links (the class of bug fixed in `wasmfs_fetch_fix.js`).

## Phase B: correctness, level by level

Story order (Sandover → Forbidden Jungle → Sentinel Beach → Misty Island → Fire Canyon → Rock
Village → Precursor Basin → Lost Precursor City → Boggy Swamp → Mountain Pass → Volcanic Crater →
Spider Cave → Snowy Mountain → Lava Tube → Gol and Maia's Citadel), fixing what the sweeps find.
Expected kinds of failures, from what M2–M7 met:

- wasm codegen gaps in GOAL code paths not yet run (128-bit/vector ops, rare IR forms): fix in
  `goalc/compiler/wasm`, add a compiler test each time.
- mips2c functions only used by later levels: the adapters exist; stale-register reliance (the
  sky's vf27 case) shows as wrong DMA data, found by DMA diff.
- WebGL2 traps in renderers not yet exercised (ocean variants, shadow, sprite distort, eyes, glow,
  collide mesh): `webgl_compat.cpp` and `RENDERER.md` list the traps met so far.
- Streaming edge cases: level borders loading two DGOs while streaming music and voices.

Done when the headless and browser sweeps pass and the image diffs are within threshold for every
continue point.

## Phase C: performance

Target: 60 fps (or the display's refresh) on the owner's PC in Chrome and Firefox at "Sharp"
quality, in the worst level; no frame over 50 ms except loading.

Measured candidates, in expected order of payoff:

1. **Optimize GOAL modules.** They are emitted unoptimized, then instrumented by Asyncify
   everywhere (any function that calls may suspend). Running Binaryen's optimizer (`-O2`) on
   each module before Asyncify should speed GOAL code up severalfold; Asyncify's own overhead
   then remains. Cost: longer game compiles; check: compiler tests + sweeps.
2. **Narrow Asyncify.** Only functions that can reach `suspend` need instrumenting. Indirect
   calls make this hard to prove; a runtime-guided list (functions seen on a suspending stack in
   the sweeps) plus a safe fallback is the practical route.
3. **`WEBGL_multi_draw`** for tfrag/tie/shrub (see the Log's tradeoffs): fewer calls into the
   browser per frame, keeps the loop as fallback.
4. **Texture animation and uploads** (`TextureAnimator`, `TextureUploadHandler`) in levels with
   animated textures (Lava Tube, Citadel): measure first.
5. **Fewer prints and RPC round trips** from pthreads to the page (each is synchronous): already
   done for the log; check what remains under profiles.

Each change is measured with `?stats=1` in the worst levels before and after.

## Phase D: owner playthrough

A full playthrough on the hosted site, in French, with the Menu's "Copy log" for any problem.
Saves already sync, so it can be spread over machines and days.

## Risks and limits

- **Storage:** a full cache is ~900 MB; a browser with little free disk may refuse part of it (the
  game still runs from the network, slower first loads).
- **Login expiry:** long sessions should be set in Access, or the page should detect an expired
  login and ask for a reload (planned).
- **Machine variety:** the targets are the owner's PCs; slower GPUs may need "Balanced"/"Fast".
- **Effort:** Phase A is days; Phase B depends on what the sweeps find (likely the bulk); Phase C
  item 1 is the largest single win and should come early if Phase A shows GOAL time dominating.
