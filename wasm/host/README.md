# jak-web: private hosting (CLAUDE.MD decision 10, M7)

One Cloudflare Worker, `jak-web`, behind Cloudflare Access:

- static assets: `gk_web.{html,js,wasm,data}`, `page.js` and `data_sw.js` (runtime, page, shaders,
  no game data), copied to `build-wasm/host-site` by `build_site.sh`;
- `/data/*`: the game files from the private R2 bucket `jak-data` (HEAD + Range, which the WasmFS
  fetch backend in `wasm/web/main_web.cpp` needs);
- `/saves/*`: each signed-in user's saves and settings in the private bucket `jak-saves`, under
  their Access email (list, GET, PUT with `X-Mtime`; an older write gets 409). The page
  (`wasm/web/page.js`) pulls newer files before the game starts and pushes changed ones every 10 s;
- COOP/COEP on every response, and an Access token check on every request (`src/access.js`), as a
  second barrier behind Access itself. Empty `ACCESS_TEAM_DOMAIN` / `ACCESS_AUD` = everything is
  refused.

The allowed emails live only in the Access policy, never in this folder.

## Local test (nothing leaves the machine)

```bash
bash wasm/web/stage_data.sh                 # game files -> build-wasm/web-data/out
bash wasm/host/build_site.sh                # runtime -> build-wasm/host-site
bash wasm/host/upload_data.sh --local       # local R2 in build-wasm/wrangler-state (~10 min)
echo DEV_SKIP_ACCESS=1 > wasm/host/.dev.vars    # local only, git-ignored, never deployed
npx wrangler@4.151.3 dev -c wasm/host/wrangler.jsonc --persist-to build-wasm/wrangler-state
```

Then http://localhost:8787/gk_web.html (title screen; `?args=-boot+-fakeiso+-debug` for the debug
boot into Sandover). Saves go to a local `jak-saves` under the user `local-dev`. Without `.dev.vars` every
request gets 403 (the token check fails closed). `node wasm/host/test/access.test.mjs` tests the
token check.

## Deploy, in this order (each step needs the owner's go-ahead)

Nothing must be reachable before Access protects the Worker.

1. `npx wrangler@4.151.3 r2 bucket create jak-data`: private by default; never enable public
   access or an `r2.dev` URL on it.
2. `bash wasm/host/upload_data.sh --remote`: ~330 files, ~0.77 GB (English voices only). Nothing
   can read them yet.
3. `npx wrangler@4.151.3 deploy -c wasm/host/wrangler.jsonc`: creates the Worker with
   `workers_dev: false` and `preview_urls: false`, so it has no address.
4. Dashboard: Workers & Pages → jak-web → Access → Protect this Worker behind Access → All traffic,
   with the owner's email policy. Set a long session duration (an expired session fails file reads
   mid-game).
5. Put the team domain (`<team>.cloudflareaccess.com`) and the application's AUD tag in
   `wrangler.jsonc` `vars`, set `workers_dev: true`, deploy again.
6. Check: `curl -sI https://jak-web.<account>.workers.dev/` and `.../data/manifest.txt` must answer
   with the Access login redirect, not content; then sign in and boot Geyser Rock in the browser.

Saves (added after the first deploy): `npx wrangler@4.151.3 r2 bucket create jak-saves` (private,
same rules as jak-data) before deploying a Worker that binds it.

Updating later: `build_site.sh` + `wrangler deploy` for the runtime; `stage_data.sh` +
`upload_data.sh --remote` for game files (browsers keep `/data/` files for a day:
`Cache-Control: private, max-age=86400`).
