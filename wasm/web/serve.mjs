// Local development server for gk_web (M5): serves build-wasm/wasm/ with the COOP/COEP headers
// SharedArrayBuffer (pthreads) needs. Local only: this build contains preloaded game objects and
// must never be published (CLAUDE.MD hard rules).
//   node wasm/web/serve.mjs [port]   then open http://localhost:8090/gk_web.html
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../build-wasm/wasm");
const port = Number(process.argv[2] || 8090);
const types = {
  ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm",
  ".data": "application/octet-stream",
};

http.createServer((req, res) => {
  const file = path.join(root, decodeURIComponent(new URL(req.url, "http://x").pathname));
  if (!file.startsWith(root) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
    res.writeHead(404);
    res.end("not found");
    return;
  }
  res.writeHead(200, {
    "Content-Type": types[path.extname(file)] || "application/octet-stream",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cache-Control": "no-store",
  });
  fs.createReadStream(file).pipe(res);
}).listen(port, "127.0.0.1", () => console.log(`gk_web on http://localhost:${port}/gk_web.html`));
