// jak-web: the browser build behind Cloudflare Access (CLAUDE.MD decision 10, M7).
// - static assets (gk_web.html/.js/.wasm/.data, no game data) from build-wasm/host-site
// - /data/<path>: the game files from the private R2 bucket jak-data, with HEAD and Range, which
//   the WasmFS fetch backend in wasm/web/main_web.cpp uses
// - /saves/*: each user's saves and settings in the private bucket jak-saves (serveSaves)
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

// The signed-in user (lowercased email from the verified token), or null if refused.
async function identity(request, env) {
  if (env.DEV_SKIP_ACCESS === "1") {
    return "local-dev";
  }
  try {
    const claims = await verifyAccessJwt(request.headers.get("Cf-Access-Jwt-Assertion"), {
      teamDomain: env.ACCESS_TEAM_DOMAIN,
      aud: env.ACCESS_AUD,
    });
    return String(claims.email || claims.sub || "").toLowerCase() || null;
  } catch (e) {
    console.log(`denied ${new URL(request.url).pathname}: ${e.message}`);
    return null;
  }
}

// Saves (decision 11): the OPFS files OpenGOAL/jak1/{saves,settings}/<name> of each user, in the
// private bucket jak-saves under <email>/, last write wins on the client's timestamp (X-Mtime).
//   GET /saves/          [{ path, size, mtime }]
//   GET /saves/<path>    the file, with X-Mtime
//   PUT /saves/<path>    stores the body, X-Mtime required (ms since epoch)
const SAVE_PATH = /^OpenGOAL\/jak1\/(saves|settings)\/[A-Za-z0-9._-]{1,64}$/;
const SAVE_MAX_BYTES = 4 << 20;

async function serveSaves(request, env, user, rest) {
  const prefix = `${encodeURIComponent(user)}/`;
  const json = (v, status = 200) => respond(JSON.stringify(v), status, { "Content-Type": "application/json", "Cache-Control": "no-store" });
  if (rest === "") {
    if (request.method !== "GET") {
      return respond("method not allowed", 405, { Allow: "GET" });
    }
    const out = [];
    let cursor;
    do {
      const page = await env.SAVES.list({ prefix, cursor, include: ["customMetadata"] });
      for (const o of page.objects) {
        out.push({ path: o.key.slice(prefix.length), size: o.size, mtime: Number(o.customMetadata?.mtime || 0) });
      }
      cursor = page.truncated ? page.cursor : undefined;
    } while (cursor);
    return json(out);
  }
  const path = decodeURIComponent(rest);
  if (!SAVE_PATH.test(path)) {
    return respond("bad path", 400);
  }
  if (request.method === "GET") {
    const obj = await env.SAVES.get(prefix + path);
    if (!obj) {
      return respond("not found", 404, { "Cache-Control": "no-store" });
    }
    return respond(obj.body, 200, {
      "Content-Type": "application/octet-stream",
      "Cache-Control": "no-store",
      "X-Mtime": obj.customMetadata?.mtime || "0",
    });
  }
  if (request.method === "PUT") {
    const mtime = Number(request.headers.get("X-Mtime"));
    if (!Number.isFinite(mtime) || mtime <= 0) {
      return respond("X-Mtime required", 400);
    }
    const body = await request.arrayBuffer();
    if (body.byteLength > SAVE_MAX_BYTES) {
      return respond("too large", 413);
    }
    const prev = await env.SAVES.head(prefix + path);
    if (prev && Number(prev.customMetadata?.mtime || 0) > mtime) {
      // another machine saved later: keep it, the client pulls it on its next boot
      return json({ kept: "newer", mtime: Number(prev.customMetadata.mtime) }, 409);
    }
    await env.SAVES.put(prefix + path, body, { customMetadata: { mtime: String(mtime) } });
    return json({ stored: path, mtime });
  }
  return respond("method not allowed", 405, { Allow: "GET, PUT" });
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
    const user = await identity(request, env);
    if (!user) {
      return respond("forbidden", 403);
    }
    const url = new URL(request.url);
    if (url.pathname === "/saves" || url.pathname.startsWith("/saves/")) {
      return serveSaves(request, env, user, url.pathname.slice("/saves/".length));
    }
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
