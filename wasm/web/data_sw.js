// Service worker of gk_web (CLAUDE.MD decision 9, M7): answers /data/ from an OPFS cache.
//
// The game reads its files through the WasmFS fetch backend (wasm/web/main_web.cpp): a HEAD, then
// Range GETs, from its pthreads. Those fetches come through here. A file whose current version is
// in OPFS is answered from it (Range = Blob.slice, nothing loaded whole); anything else goes to
// the network as before. shell.html fills the cache in the background (fillDataCache).
//
// OPFS layout, in directory "data-cache": one file per md5 from manifest.txt (content-addressed:
// a changed game file gets a new name, the old one is deleted by the next fill), "<md5>.part"
// while downloading, and "manifest.txt", the last manifest seen (path size md5 per line).

const PREFIX = "/data/";
const ISOLATION = {
  "Cross-Origin-Opener-Policy": "same-origin",
  "Cross-Origin-Embedder-Policy": "require-corp",
  "Cross-Origin-Resource-Policy": "same-origin",
};

let manifest = null; // Map path -> { size, md5 }

self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (e) => e.waitUntil(self.clients.claim()));

self.addEventListener("fetch", (e) => {
  const url = new URL(e.request.url);
  if (url.origin !== location.origin || !url.pathname.startsWith(PREFIX)) {
    return;
  }
  // the fetch backend asks for "data//manifest.txt"
  const key = decodeURIComponent(url.pathname.slice(PREFIX.length)).replace(/^\/+/, "");
  e.respondWith(key === "manifest.txt" ? serveManifest(e.request) : serveFile(e.request, key));
});

function parseManifest(text) {
  const m = new Map();
  for (const line of text.split("\n")) {
    const [path, size, md5] = line.trim().split(" ");
    if (path && md5) {
      m.set(path, { size: Number(size), md5 });
    }
  }
  return m;
}

async function cacheDir() {
  return (await navigator.storage.getDirectory()).getDirectoryHandle("data-cache", { create: true });
}

async function readFile(dir, name) {
  try {
    return await (await dir.getFileHandle(name)).getFile();
  } catch {
    return null;
  }
}

// Network first (a new upload must be seen), the saved copy when offline or signed out.
async function serveManifest(request) {
  const dir = await cacheDir();
  try {
    const res = await fetch(request.url, { cache: "no-store" });
    if (res.ok) {
      const text = await res.text();
      manifest = parseManifest(text);
      const w = await (await dir.getFileHandle("manifest.txt", { create: true })).createWritable();
      await w.write(text);
      await w.close();
      return new Response(text, { headers: { ...ISOLATION, "Content-Type": "text/plain" } });
    }
  } catch {}
  const saved = await readFile(dir, "manifest.txt");
  if (!saved) {
    return fetch(request);
  }
  manifest = parseManifest(await saved.text());
  return new Response(saved, { headers: { ...ISOLATION, "Content-Type": "text/plain" } });
}

async function serveFile(request, key) {
  const dir = await cacheDir();
  if (!manifest) {
    // the worker was stopped and restarted mid-game: reload the last manifest
    const saved = await readFile(dir, "manifest.txt");
    manifest = saved ? parseManifest(await saved.text()) : new Map();
  }
  const entry = manifest.get(key);
  const file = entry && (await readFile(dir, entry.md5));
  if (!file || file.size !== entry.size) {
    return fetch(request);
  }
  const headers = {
    ...ISOLATION,
    "Accept-Ranges": "bytes",
    "Content-Type": "application/octet-stream",
  };
  if (request.method === "HEAD") {
    return new Response(null, { headers: { ...headers, "Content-Length": String(file.size) } });
  }
  const range = /^bytes=(\d*)-(\d*)$/.exec(request.headers.get("Range") || "");
  if (range && file.size > 0) {
    const start = range[1] ? Number(range[1]) : Math.max(0, file.size - Number(range[2]));
    const end = range[1] && range[2] ? Math.min(Number(range[2]), file.size - 1) : file.size - 1;
    if (start > end) {
      return new Response(null, { status: 416, headers: { ...headers, "Content-Range": `bytes */${file.size}` } });
    }
    return new Response(file.slice(start, end + 1), {
      status: 206,
      headers: { ...headers, "Content-Range": `bytes ${start}-${end}/${file.size}` },
    });
  }
  return new Response(file, { headers });
}
