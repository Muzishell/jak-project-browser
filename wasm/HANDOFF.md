# Handoff - 2026-10-20

## Where we are
Milestone: M6 done (804ab36e1), next is M7 Browser shell + hosting. Sandover is playable in Chrome
through gk_web: Jak and Daxter drawn (merc index-buffer fix, 867b7985d), keyboard and Gamepad API
input (162373965), 989snd audio through Web Audio, 60 fps with page focus (M6 perf slice 1,
3e9ed26c0). Geyser Rock renders (M5, 521db0f0b). Last reported: native goalc-test 1669/1669, wasm
tests 76/76, kernel-test.mjs 4/4 (from the M6 commit message, not re-run in this session).

M7 slice 1: game files are no longer preloaded. gk_web uses WasmFS with a fetch backend at
`/jak-project/out` (`wasm/web/main_web.cpp`), reading `/data/manifest.txt` and fetching each file
on first read; `stage_data.sh` + `serve.mjs` provide `/data/` locally. gk_web.data is 0.7 MB.

M7 slice 2 (local only): `wasm/host/` holds the Worker `jak-web` (static runtime + `/data/*` from
R2 `jak-data`, COOP/COEP, Access JWT check) and its scripts; tested with `wrangler dev` and a local
R2 copy. The owner has enabled R2 and Zero Trust (with the email policy) and logged Wrangler in.
Nothing is deployed or uploaded.

## Uncommitted / in-flight
Nothing.

## Next 3 steps
1. M7 deploy, owner's go-ahead per step: `wasm/host/README.md` "Deploy" (bucket, upload, deploy
   without address, Access on the Worker, team domain + AUD in `wrangler.jsonc`, enable workers.dev,
   check it's locked).
2. M7: OPFS cache for the fetched files (decision 9): every page load refetches today (the Worker
   lets the browser cache `/data/` for a day).
3. M7: loading screen in `wasm/web/shell.html`; saves on WasmFS OPFS + `/saves/*` sync in the
   Worker (decision 11, `kmemcard.cpp`, `wasmfs_create_opfs_backend` at `/user`).

## Known broken / open questions
- M4 is still unticked in CLAUDE.MD although ENGINE and GAME load and tick headless (Log
  2026-10-18/19): question for the owner whether to tick it.
- Left from M6: renderer cost when the page is unfocused (~47 ms frames), the IOP thread spinning
  while idle (`IOP::wait_run_iop`), a check with a physical gamepad and by ear.
- MSAA off in the web build; multi-draw is a loop (no `WEBGL_multi_draw`).
- Harness: `wasm/tools` deps (`npm ci`, `npx playwright install chromium`) need the owner's OK;
  `gl-audit.mjs` does not run on this branch; `capture.mjs` has no frame hook (`window.__jak.frame`
  is not set), so it waits for `--timeout`.

## Numbers
Tests: goalc-test 1669/1669, wasm tests 76/76, kernel-test.mjs 4/4 (M6 commit). Perf (Chrome,
focused): GAME.CGO links in 0.6 s, Geyser Rock reached in ~20-30 s, 16.7 ms/frame; unfocused ~47 ms.
