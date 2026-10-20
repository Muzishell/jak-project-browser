# Handoff - 2026-10-20

## Where we are
Milestone: M6 done (804ab36e1), next is M7 Browser shell + hosting. Sandover is playable in Chrome
through gk_web: Jak and Daxter drawn (merc index-buffer fix, 867b7985d), keyboard and Gamepad API
input (162373965), 989snd audio through Web Audio, 60 fps with page focus (M6 perf slice 1,
3e9ed26c0). Geyser Rock renders (M5, 521db0f0b). Last reported: native goalc-test 1669/1669, wasm
tests 76/76, kernel-test.mjs 4/4 (from the M6 commit message, not re-run in this session).

## Uncommitted / in-flight
The Claude Code harness was just installed, uncommitted, for the owner to review:
`.claude/settings.json` (hooks run WSL's emsdk node through `wsl.exe`), `.claude/hooks/guard.mjs`,
`.claude/hooks/session-start.mjs`, `.claude/skills/*` (4 skills), `wasm/tools/` (shader checkers,
capture, img-diff; deps not installed), `wasm/docs/*.md`, this file, and a "Harness" block + one
Log line in CLAUDE.MD. Revert = delete those paths and the CLAUDE.MD additions.

## Next 3 steps
1. M7: replace `--preload-file` of `build-wasm/web-data` with fetch + OPFS loading per level
   (decision 9): `game/overlord/common/fake_iso.cpp` and the `.fr3` loader
   (`game/graphics/opengl_renderer/loader/Loader.cpp`).
2. M7: loading screen in `wasm/web/shell.html`; saves on WasmFS OPFS (decision 11, `kmemcard.cpp`).
3. M7: Cloudflare Pages + Worker (`/data/*`, `/saves/*`) + private R2 behind Access (decision 10).

## Known broken / open questions
- M4 is still unticked in CLAUDE.MD although ENGINE and GAME load and tick headless (Log
  2026-10-18/19): question for the owner whether to tick it.
- Left from M6: renderer cost when the page is unfocused (~47 ms frames), the IOP thread spinning
  while idle (`IOP::wait_run_iop`), a check with a physical gamepad and by ear.
- MSAA off in the web build; multi-draw is a loop (no `WEBGL_multi_draw`).
- `gk_web.data` is ~370 MB (all sound staged) until M7 loads lazily.
- Harness: `wasm/tools` deps (`npm ci`, `npx playwright install chromium`) need the owner's OK;
  `gl-audit.mjs` does not run on this branch; `capture.mjs` has no frame hook (`window.__jak.frame`
  is not set), so it waits for `--timeout`.

## Numbers
Tests: goalc-test 1669/1669, wasm tests 76/76, kernel-test.mjs 4/4 (M6 commit). Perf (Chrome,
focused): GAME.CGO links in 0.6 s, Geyser Rock reached in ~20-30 s, 16.7 ms/frame; unfocused ~47 ms.
