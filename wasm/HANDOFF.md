# Handoff - 2026-10-21

## Where we are
Milestones M1–M7 are done (CLAUDE.MD). The first playable (title, intro, Geyser Rock, Sandover)
runs in Chrome and Firefox from https://jak-web.adrienmuzika.workers.dev and
https://jak.anomandoris.com, both behind Cloudflare Access (owner's email policy).

- Hosting (`wasm/host/`): one Worker `jak-web` = static runtime (`gk_web.*`, `page.js`,
  `data_sw.js`) + `/data/*` from the private R2 bucket `jak-data` (game files, HEAD + Range) +
  `/saves/*` from the private bucket `jak-saves` (per Access email). COOP/COEP and an Access JWT
  check on every request. Deploy: `build_site.sh` + `wrangler deploy`; game files:
  `stage_data.sh` + `upload_data.sh --remote` (only new/changed files; voices `VOICE_LANGS`,
  default ENG FRE). Check after each deploy: `lockcheck`-style curl of both hostnames (90/90 got
  the Access login, last on version bf75cc83).
- Page (`wasm/web/page.js`, `shell.html`): the game view fills the window and renders at its size
  (`gk_set_view_size`, Quality setting), Menu panel (saves sync/export/import, fullscreen,
  game-file cache progress, log drawer with copy), loading screen, title-screen boot by default,
  default language from the browser (`?lang=fr`).
- Files: WasmFS fetch backend at `/jak-project/out` (`main_web.cpp`) with Emscripten's backend JS
  replaced by `wasmfs_fetch_fix.js` (whole-file chunk bug that crashed the intro); OPFS cache
  through the service worker; saves/settings on the OPFS backend at `/user`.
- Frames: refresh-paced, immediate main-loop ticks only when needed (`update_loop_timing`), which
  fixed Firefox's 200–500 ms frames (collector pauses from back-to-back postTask). `?stats=1`
  prints frame and render timing every 10 s.
- GOAL wasm modules keep their name section (profiles and stack traces show GOAL names).

## Uncommitted / in-flight
Nothing.

## Next 3 steps
1. Full game (beyond the first playable): see `wasm/docs/FULL-GAME.md` for the plan.
2. Graphics quality: MSAA (renderbuffer + blit resolve) and `WEBGL_multi_draw`; tradeoffs in
   the CLAUDE.MD Log (2026-10-21).
3. Access session expiry mid-game: detect a redirected `/data/` fetch and ask for a reload.

## Known broken / open questions
- The IOP thread spins (one core) by the owner's choice: sleeping it made frames worse (CPU
  clocks); the sleep version is in the CLAUDE.MD Log, not in the code.
- Frames are slower (~47 ms) while the page is unfocused.
- Only English and French voices are hosted; other levels than the first playable are uploaded
  but not played in the browser yet.
- The desktop app's browser pane: no service worker on local http, CSS transitions and
  screenshots freeze while the app window is hidden, ~650 MB storage quota.

## Numbers
Tests: see the last test run in the CLAUDE.MD Log. Perf (Chrome and Firefox, focused, intro and
Geyser Rock): 550+ frames per 10 s on a 55 Hz display, render 3–6 ms per frame.
