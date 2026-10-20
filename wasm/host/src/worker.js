// jak-web: the browser build behind Cloudflare Access (CLAUDE.MD decision 10, M7).
// - static assets (gk_web.html/.js/.wasm/.data, no game data) from build-wasm/host-site
// - /data/<path>: the game files from the private R2 bucket jak-data, with HEAD and Range, which
//   the WasmFS fetch backend in wasm/web/main_web.cpp uses
// - every response gets COOP/COEP (pthreads need SharedArrayBuffer)
// - every request must carry a valid Access token (src/access.js), even though Access runs first.
//   Local `wrangler dev` only: DEV_SKIP_ACCESS=1 in wasm/host/.dev.vars (never deployed).

import { verifyAccessJwt } from "./access.js";

const ISOLATION = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
};

function respond(body, status, headers = {}) {
  return new Response(body, { status, headers: { ...ISOLATION, ...headers } });
}

function isolate(res) {
  const out = new Response(res.body, res);
  for (const [k, v] of Object.entries(ISOLATION)) {
    out.headers.set(k, v);
  }
  return out;
}

async function allowed(request, env) {
  if (env.DEV_SKIP_ACCESS === "1") {
    return true;
  }
  try {
    await verifyAccessJwt(request.headers.get("Cf-Access-Jwt-Assertion"), {
      teamDomain: env.ACCESS_TEAM_DOMAIN,
      aud: env.ACCESS_AUD,
    });
    return true;
  } catch (e) {
    console.log(`denied ${new URL(request.url).pathname}: ${e.message}`);
    return false;
  }
}

// R2 key for /data/<path>. The fetch backend asks for "data//manifest.txt", so leading slashes go.
function dataKey(pathname) {
  const key = decodeURIComponent(pathname.slice("/data/".length)).replace(/^\/+/, "");
  if (!key || key.split("/").some((p) => p === ".." || p === ".")) {
    return null;
  }
  return key;
}

async function serveData(request, env, key) {
  const common = {
    "Accept-Ranges": "bytes",
    "Content-Type": "application/octet-stream",
    // behind Access: only the signed-in browser may cache it; game files rarely change
    "Cache-Control": "private, max-age=86400",
  };
  if (request.method === "HEAD") {
    const obj = await env.DATA.head(key);
    if (!obj) {
      return respond("not found", 404);
    }
    return respond(null, 200, { ...common, "Content-Length": String(obj.size), ETag: obj.httpEtag });
  }
  const ranged = request.headers.has("Range");
  const obj = await env.DATA.get(key, ranged ? { range: request.headers } : {});
  if (!obj) {
    return respond("not found", 404);
  }
  const headers = { ...common, ETag: obj.httpEtag };
  if (ranged && obj.range) {
    const offset = obj.range.offset ?? (obj.range.suffix !== undefined ? obj.size - obj.range.suffix : 0);
    const length = obj.range.length ?? obj.size - offset;
    headers["Content-Range"] = `bytes ${offset}-${offset + length - 1}/${obj.size}`;
    headers["Content-Length"] = String(length);
    return respond(obj.body, 206, headers);
  }
  headers["Content-Length"] = String(obj.size);
  return respond(obj.body, 200, headers);
}

export default {
  async fetch(request, env) {
    if (!(await allowed(request, env))) {
      return respond("forbidden", 403);
    }
    const url = new URL(request.url);
    if (url.pathname.startsWith("/data/")) {
      if (request.method !== "GET" && request.method !== "HEAD") {
        return respond("method not allowed", 405, { Allow: "GET, HEAD" });
      }
      const key = dataKey(url.pathname);
      return key ? serveData(request, env, key) : respond("bad path", 400);
    }
    if (url.pathname === "/") {
      return Response.redirect(new URL("/gk_web.html", url), 302);
    }
    return isolate(await env.ASSETS.fetch(request));
  },
};
