// Local development server for gk_web (M5, M7): serves build-wasm/wasm/ with the COOP/COEP headers
// SharedArrayBuffer (pthreads) needs, and /data/ from the game files wasm/web/stage_data.sh staged
// (with HEAD and Range requests, which the WasmFS fetch backend uses). Local only: /data/ is game
// data and must never be published (CLAUDE.MD hard rules).
//   node wasm/web/serve.mjs [port]   then open http://localhost:8090/gk_web.html
// GK_DATA_DELAY_MS=300 delays every /data/ answer, like a round trip to the hosted bucket.
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const root = path.join(repo, "build-wasm/wasm");
const dataRoot = path.join(repo, "build-wasm/web-data/out");
const port = Number(process.argv[2] || 8090);
const dataDelay = Number(process.env.GK_DATA_DELAY_MS || 0);
const types = {
  ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm",
  ".data": "application/octet-stream", ".txt": "text/plain",
};

function resolve(urlPath) {
  const p = path.normalize(decodeURIComponent(urlPath));
  const [base, rel] = p.startsWith("/data/") ? [dataRoot, p.slice(6)] : [root, p];
  const file = path.join(base, rel);
  return file.startsWith(base + path.sep) ? file : null;
}

http.createServer((req, res) => {
  if (dataDelay && req.url.startsWith("/data/")) {
    setTimeout(() => serve(req, res), dataDelay);
  } else {
    serve(req, res);
  }
}).listen(port, "127.0.0.1", () => console.log(`gk_web on http://localhost:${port}/gk_web.html` +
  (dataDelay ? ` (/data/ delayed ${dataDelay} ms)` : "")));

function serve(req, res) {
  const file = resolve(new URL(req.url, "http://x").pathname);
  const st = file && fs.existsSync(file) ? fs.statSync(file) : null;
  if (!st || st.isDirectory()) {
    res.writeHead(404);
    res.end("not found");
    return;
  }
  const headers = {
    "Content-Type": types[path.extname(file)] || "application/octet-stream",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cache-Control": "no-store",
    "Accept-Ranges": "bytes",
  };
  let start = 0, end = st.size - 1, status = 200;
  const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || "");
  if (range && st.size > 0) {
    start = range[1] ? Number(range[1]) : Math.max(0, st.size - Number(range[2]));
    end = range[1] && range[2] ? Math.min(Number(range[2]), st.size - 1) : st.size - 1;
    if (start > end) {
      res.writeHead(416, { ...headers, "Content-Range": `bytes */${st.size}` });
      res.end();
      return;
    }
    status = 206;
    headers["Content-Range"] = `bytes ${start}-${end}/${st.size}`;
  }
  headers["Content-Length"] = st.size ? end - start + 1 : 0;
  res.writeHead(status, headers);
  if (req.method === "HEAD" || st.size === 0) {
    res.end();
    return;
  }
  fs.createReadStream(file, { start, end }).pipe(res);
}
