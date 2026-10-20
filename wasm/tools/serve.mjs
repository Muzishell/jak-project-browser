#!/usr/bin/env node
// Static server for the wasm build with the headers the runtime needs.
//
//   node wasm/tools/serve.mjs [--root build-wasm/web] [--port 8080] [--data out/jak1]
//
// - COOP/COEP so SharedArrayBuffer (Emscripten pthreads, PROXY_TO_PTHREAD) is available.
// - application/wasm MIME so streaming compilation works.
// - HTTP Range support (the .fr3 / ISO files are fetched lazily per level).
// - --data mounts a local game-data folder at /data/ for local testing only. It is served from
//   localhost to your own browser and never copied into the build or the repo.

import fs from "node:fs";
import http from "node:http";
import path from "node:path";

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const ROOT = path.resolve(opt("--root", "build-wasm/web"));
const DATA = opt("--data", null) && path.resolve(opt("--data"));
const PORT = +opt("--port", 8080);
const HOST = opt("--host", "127.0.0.1");

const TYPES = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".mjs": "text/javascript",
  ".wasm": "application/wasm", ".json": "application/json", ".data": "application/octet-stream",
  ".png": "image/png", ".css": "text/css", ".map": "application/json",
};

function resolve(urlPath) {
  const p = decodeURIComponent(urlPath.split("?")[0]);
  if (DATA && p.startsWith("/data/")) return path.join(DATA, path.normalize(p.slice(6)).replace(/^(\.\.[/\\])+/, ""));
  const f = path.join(ROOT, path.normalize(p).replace(/^(\.\.[/\\])+/, ""));
  return f.endsWith(path.sep) || p.endsWith("/") ? path.join(f, "index.html") : f;
}

http.createServer((req, res) => {
  const f = resolve(req.url);
  const base = {
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cross-Origin-Resource-Policy": "same-origin",
    "Cache-Control": "no-store",
    "Accept-Ranges": "bytes",
  };
  fs.stat(f, (err, st) => {
    if (err || !st.isFile()) { res.writeHead(404, base); res.end("not found"); console.log(`404 ${req.url}`); return; }
    const type = TYPES[path.extname(f)] || "application/octet-stream";
    const range = req.headers.range && req.headers.range.match(/bytes=(\d*)-(\d*)/);
    if (range) {
      const start = range[1] ? +range[1] : st.size - +range[2];
      const end = range[1] && range[2] ? +range[2] : st.size - 1;
      res.writeHead(206, { ...base, "Content-Type": type, "Content-Length": end - start + 1, "Content-Range": `bytes ${start}-${end}/${st.size}` });
      fs.createReadStream(f, { start, end }).pipe(res);
    } else {
      res.writeHead(200, { ...base, "Content-Type": type, "Content-Length": st.size });
      if (req.method === "HEAD") return res.end();
      fs.createReadStream(f).pipe(res);
    }
  });
}).listen(PORT, HOST, () => {
  console.log(`serving ${ROOT} on http://${HOST}:${PORT}/ (COOP/COEP on)${DATA ? `, /data -> ${DATA}` : ""}`);
});
