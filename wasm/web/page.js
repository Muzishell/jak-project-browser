// The page around gk_web (CLAUDE.MD M7): runtime arguments, the game log, a loading screen, the
// OPFS cache of the game files (data_sw.js), and the saves: kept in OPFS by the game itself
// (main_web.cpp mounts the OPFS root at /user), synced with the Worker's /saves/ (decision 11),
// exported and imported as a .tar.
//
// URL options: ?args=... runtime arguments (default: boot the game normally, title screen;
// "-boot -fakeiso -debug" for the debug boot into Sandover); ?log=1 shows the log; ?cache=0 turns
// the file cache off; ?stats=1 / ?pacing=clock frame timing options (wasm/runtime_wasm.cpp).

const params = new URLSearchParams(location.search);
const $ = (id) => document.getElementById(id);

// ---- log -------------------------------------------------------------------------------------
// the whole log (window.gkLog); the page shows its end
const lines = (window.gkLog = []);
// WebGL messages are kept apart, the game's log scrolls them away (inspect window.webglLog)
window.webglLog = [];
// A pthread's print waits until this thread has handled it (Emscripten proxies stdout writes
// synchronously), so printing must be cheap: the page is redrawn at most every 100 ms.
let redraw = null;
function drawLog() {
  redraw = null;
  const log = $("log");
  if (!log.hidden) {
    log.textContent = lines.slice(-400).join("\n");
    log.scrollTop = log.scrollHeight;
  }
}
function show(text) {
  if (/WebGL/.test(text) && window.webglLog.length < 200) window.webglLog.push(text);
  lines.push(text);
  if (lines.length > 100000) lines.splice(0, 50000);
  loadingFromLog(text);
  redraw ??= setTimeout(drawLog, 100);
}

// ---- loading screen ----------------------------------------------------------------------------
// Covers the canvas until the game shows something: a level's first display command (title
// screen or the debug boot's level) or the end of the debug boot's blackout; 120 s at most.
let loading = true;
function loadingStatus(text) {
  if (loading) $("overlay-status").textContent = text;
}
function loadingDone() {
  if (!loading) return;
  loading = false;
  $("overlay").hidden = true;
  $("canvas").focus();
}
function loadingFromLog(text) {
  if (!loading) return;
  let m;
  if (/Setting level \S+ display command to display|coming out of blackout/.test(text)) {
    loadingDone();
  } else if ((m = /\[link and exec\] (\S+)/.exec(text))) {
    loadingStatus(`Loading game code: ${m[1]}`);
  } else if ((m = /FS Open (\S+)\s+(DGO|CGO)/.exec(text))) {
    loadingStatus(`Loading ${m[1]}.${m[2]}`);
  }
}
setTimeout(loadingDone, 120000);

// ---- runtime ---------------------------------------------------------------------------------
var Module = {
  arguments: (params.get("args") || "-boot -fakeiso").split(" ").filter((a) => a),
  canvas: $("canvas"),
  // the game's output stays on the page: the console keeps the browser's own messages (WebGL)
  print: show,
  printErr: show,
  // the game starts once the cache's service worker controls the page (so its threads' fetches go
  // through it) and the saves are pulled and imported (the game must not hold them open yet)
  preRun: [() => {
    addRunDependency("page");
    Promise.allSettled([startDataCache(), prepareSaves()]).finally(() => {
      loadingStatus("Starting the game");
      removeRunDependency("page");
    });
    // ?stats=1: a frame timing summary in the log every 10 s; ?pacing=clock: frames on a 60 Hz
    // clock instead of the display's refreshes, to compare (wasm/runtime_wasm.cpp, FramePacer)
    if (typeof ENV == "object") {
      if (params.get("stats") === "1") ENV.GK_FRAME_STATS = "1";
      if (params.get("pacing") === "clock") ENV.GK_FRAME_PACING = "clock";
    }
  }],
};

$("log").hidden = params.get("log") !== "1";
$("logbtn").onclick = () => {
  $("log").hidden = !$("log").hidden;
  drawLog();
};

// ---- OPFS cache of the game files ----------------------------------------------------------
// data_sw.js answers /data/ from it; this page fills it in the background, one file at a time,
// streamed to disk.
const cacheEl = $("cache");
async function startDataCache() {
  if (!("serviceWorker" in navigator) || !navigator.storage?.getDirectory) {
    cacheEl.textContent = "cache: not supported by this browser";
    return;
  }
  if (params.get("cache") === "0") {
    for (const r of await navigator.serviceWorker.getRegistrations()) await r.unregister();
    cacheEl.textContent = "cache: off";
    return;
  }
  try {
    loadingStatus("Starting the file cache");
    await navigator.serviceWorker.register("data_sw.js");
    await navigator.serviceWorker.ready;
    if (!navigator.serviceWorker.controller) {
      // first visit: the worker claims the page when it activates (never wait for ever)
      await Promise.race([
        new Promise((r) => navigator.serviceWorker.addEventListener("controllerchange", r, { once: true })),
        new Promise((r) => setTimeout(r, 5000)),
      ]);
    }
    if (!navigator.serviceWorker.controller) throw new Error("service worker not in control");
    // ask the browser not to evict the cache (and the saves) under storage pressure
    navigator.storage.persist?.();
    // after the first level has had the network to itself
    setTimeout(() => fillDataCache().catch((e) => (cacheEl.textContent = `cache: ${e.message}`)), 20000);
  } catch (e) {
    // e.g. the desktop app's browser pane on a local http server; files come from the network
    cacheEl.textContent = "cache: off (no service worker)";
    cacheEl.title = e.message;
  }
}

async function fillDataCache() {
  const res = await fetch("data/manifest.txt", { cache: "no-store" });
  if (!res.ok) throw new Error(`manifest: HTTP ${res.status}`);
  const entries = (await res.text()).split("\n").map((l) => l.trim().split(" "))
    .filter(([path, , md5]) => path && md5).map(([path, size, md5]) => ({ path, size: Number(size), md5 }));
  const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle("data-cache", { create: true });
  const have = new Map();
  for await (const [name, h] of dir.entries()) {
    if (h.kind === "file") have.set(name, (await h.getFile()).size);
  }
  const total = entries.reduce((n, e) => n + e.size, 0);
  let done = entries.filter((e) => have.get(e.md5) === e.size);
  let bytes = done.reduce((n, e) => n + e.size, 0);
  const status = () => (cacheEl.textContent =
    `cache: ${done.length}/${entries.length} files, ${(bytes / 2 ** 20).toFixed(0)}/${(total / 2 ** 20).toFixed(0)} MB`);
  status();
  for (const e of entries) {
    if (have.get(e.md5) === e.size) continue;
    // no-store: the HTTP cache would keep a second copy
    const r = await fetch(`data/${e.path}`, { cache: "no-store" });
    if (!r.ok) throw new Error(`${e.path}: HTTP ${r.status}`);
    const part = await dir.getFileHandle(`${e.md5}.part`, { create: true });
    await r.body.pipeTo(await part.createWritable());
    if ((await part.getFile()).size !== e.size) throw new Error(`${e.path}: wrong size`);
    await part.move(e.md5);
    have.set(e.md5, e.size);
    done.push(e);
    bytes += e.size;
    status();
  }
  // versions no longer in the manifest, and interrupted downloads
  const keep = new Set(entries.map((e) => e.md5).concat("manifest.txt"));
  for (const name of have.keys()) {
    if (!keep.has(name)) await dir.removeEntry(name);
  }
  cacheEl.textContent += " (complete)";
}

// ---- saves -----------------------------------------------------------------------------------
// The game writes OPFS files under OpenGOAL/jak1/saves (memory card) and .../settings. Sync with
// /saves/ is last write wins on timestamps: OPFS can't set a file's lastModified, so
// save-sync.json remembers, per file, the version time it holds ("mtime") and the lastModified it
// had then ("stamp"); a different lastModified means the game wrote it since.
const SAVE_DIRS = ["OpenGOAL/jak1/saves", "OpenGOAL/jak1/settings"];
const SAVE_PATH = /^OpenGOAL\/jak1\/(saves|settings)\/[A-Za-z0-9._-]{1,64}$/;
const savesEl = $("saves");
let savesRemote = false; // /saves/ answered (the deployed Worker; not the local dev server)
let saveIndex = {};

async function opfsDir(path, create) {
  let dir = await navigator.storage.getDirectory();
  for (const part of path.split("/").filter(Boolean)) {
    dir = await dir.getDirectoryHandle(part, { create });
  }
  return dir;
}

// "a/b/name" -> ["a/b", "name"]; "name" -> ["", "name"] (the OPFS root)
function splitPath(path) {
  const slash = path.lastIndexOf("/");
  return slash < 0 ? ["", path] : [path.slice(0, slash), path.slice(slash + 1)];
}

async function writeOpfs(path, data) {
  const [dirPath, name] = splitPath(path);
  const dir = await opfsDir(dirPath, true);
  const handle = await dir.getFileHandle(name, { create: true });
  const w = await handle.createWritable();
  await w.write(data);
  await w.close();
  return (await handle.getFile()).lastModified;
}

async function readOpfs(path) {
  try {
    const [dirPath, name] = splitPath(path);
    const dir = await opfsDir(dirPath, false);
    return await (await dir.getFileHandle(name)).getFile();
  } catch {
    return null;
  }
}

// path -> File for every save/settings file; a file the game holds open is skipped this round
async function localSaves() {
  const out = new Map();
  for (const d of SAVE_DIRS) {
    let dir;
    try {
      dir = await opfsDir(d, false);
    } catch {
      continue;
    }
    for await (const [name, h] of dir.entries()) {
      if (h.kind !== "file") continue;
      try {
        out.set(`${d}/${name}`, await h.getFile());
      } catch {}
    }
  }
  return out;
}

async function loadIndex() {
  const f = await readOpfs("save-sync.json");
  try {
    saveIndex = f ? JSON.parse(await f.text()) : {};
  } catch {
    saveIndex = {};
  }
}
const saveIndexFile = () => writeOpfs("save-sync.json", JSON.stringify(saveIndex));
const versionOf = (path, file) =>
  saveIndex[path] && saveIndex[path].stamp === file.lastModified ? saveIndex[path].mtime : file.lastModified;

// Before the game starts: newer saves from the cloud, then a pending import.
async function prepareSaves() {
  if (!navigator.storage?.getDirectory) {
    savesEl.textContent = "saves: not kept (no OPFS in this browser)";
    return;
  }
  loadingStatus("Syncing saves");
  await loadIndex();
  try {
    const res = await fetch("saves/", { cache: "no-store" });
    if (res.ok) {
      savesRemote = true;
      const local = await localSaves();
      let pulled = 0;
      for (const r of await res.json()) {
        if (!SAVE_PATH.test(r.path)) continue;
        const file = local.get(r.path);
        if (file && versionOf(r.path, file) >= r.mtime) continue;
        const body = await fetch(`saves/${r.path}`, { cache: "no-store" });
        if (!body.ok) continue;
        const stamp = await writeOpfs(r.path, await body.arrayBuffer());
        saveIndex[r.path] = { mtime: r.mtime, stamp };
        pulled++;
      }
      await saveIndexFile();
      savesEl.textContent = pulled ? `saves: ${pulled} file(s) from the cloud` : "saves: synced";
    } else {
      savesEl.textContent = "saves: on this device only";
    }
  } catch (e) {
    savesEl.textContent = `saves: offline (${e.message}), on this device only`;
  }
  await applyPendingImport();
  if (savesRemote) setInterval(() => pushSaves().catch(() => {}), 10000);
}

// Files the game wrote since the last sync go up.
let pushing = false;
async function pushSaves() {
  if (!savesRemote || pushing) return;
  pushing = true;
  try {
    for (const [path, file] of await localSaves()) {
      if (saveIndex[path]?.stamp === file.lastModified) continue;
      const res = await fetch(`saves/${path}`, {
        method: "PUT",
        headers: { "X-Mtime": String(file.lastModified) },
        body: await file.arrayBuffer(),
      });
      if (res.ok) {
        saveIndex[path] = { mtime: file.lastModified, stamp: file.lastModified };
        savesEl.textContent = `saves: synced ${new Date().toLocaleTimeString()}`;
      } else if (res.status === 409) {
        // another device saved later: leave it, this device pulls it on its next start
        saveIndex[path] = { mtime: file.lastModified, stamp: file.lastModified };
        savesEl.textContent = "saves: newer save on another device (reload to get it)";
      } else {
        savesEl.textContent = `saves: sync failed (HTTP ${res.status})`;
        break;
      }
    }
    await saveIndexFile();
  } finally {
    pushing = false;
  }
}
document.addEventListener("visibilitychange", () => document.hidden && pushSaves().catch(() => {}));

// ---- export / import (.tar of OpenGOAL/jak1/{saves,settings}) --------------------------------
function tarHeader(name, size, mtimeMs) {
  const h = new Uint8Array(512);
  const put = (off, len, s) => h.set(new TextEncoder().encode(s).subarray(0, len), off);
  const oct = (n, len) => n.toString(8).padStart(len - 1, "0") + "\0";
  put(0, 100, name);
  put(100, 8, "0000644\0");
  put(108, 8, "0000000\0");
  put(116, 8, "0000000\0");
  put(124, 12, oct(size, 12));
  put(136, 12, oct(Math.floor(mtimeMs / 1000), 12));
  put(148, 8, "        ");
  put(156, 1, "0");
  put(257, 6, "ustar\0");
  put(263, 2, "00");
  const sum = h.reduce((a, b) => a + b, 0);
  put(148, 8, sum.toString(8).padStart(6, "0") + "\0 ");
  return h;
}

$("export").onclick = async () => {
  const parts = [];
  const files = await localSaves();
  for (const [path, file] of files) {
    parts.push(tarHeader(path, file.size, file.lastModified), new Uint8Array(await file.arrayBuffer()));
    const pad = (512 - (file.size % 512)) % 512;
    if (pad) parts.push(new Uint8Array(pad));
  }
  parts.push(new Uint8Array(1024));
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob(parts, { type: "application/x-tar" }));
  a.download = `jak1-saves-${new Date().toISOString().slice(0, 10)}.tar`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
  savesEl.textContent = `saves: exported ${files.size} file(s)`;
};

function untar(buf) {
  const out = [];
  const bytes = new Uint8Array(buf);
  for (let off = 0; off + 512 <= bytes.length; ) {
    const h = bytes.subarray(off, off + 512);
    if (h.every((b) => b === 0)) break;
    const str = (o, l) => new TextDecoder().decode(h.subarray(o, o + l)).replace(/\0.*$/s, "");
    const name = str(0, 100);
    const size = parseInt(str(124, 12).trim() || "0", 8);
    const type = str(156, 1) || "0";
    if (type === "0") out.push({ name, data: bytes.slice(off + 512, off + 512 + size) });
    off += 512 + Math.ceil(size / 512) * 512;
  }
  return out;
}

// The game holds its files open while running, so an import is stored aside and applied on the
// next start, before the game (prepareSaves).
$("import").onclick = () => $("importfile").click();
$("importfile").onchange = async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  const entries = untar(await file.arrayBuffer()).filter((x) => SAVE_PATH.test(x.name));
  if (!entries.length) {
    savesEl.textContent = "saves: no Jak 1 save files in that archive";
    return;
  }
  if (!confirmReplace(entries.length)) return;
  await writeOpfs("pending-import.tar", await file.arrayBuffer());
  location.reload();
};
function confirmReplace(n) {
  // a native confirm() would freeze the game's threads waiting on this one
  return window.gkImportConfirmed || (savesEl.textContent =
    `saves: import ${n} file(s)? This replaces the current ones. Click Import again to confirm.`,
    (window.gkImportConfirmed = true), false);
}

async function applyPendingImport() {
  const f = await readOpfs("pending-import.tar");
  if (!f) return;
  let n = 0;
  for (const { name, data } of untar(await f.arrayBuffer())) {
    if (!SAVE_PATH.test(name)) continue;
    await writeOpfs(name, data);
    // not in the index: counts as written by this device now, so it is pushed
    delete saveIndex[name];
    n++;
  }
  await (await navigator.storage.getDirectory()).removeEntry("pending-import.tar");
  await saveIndexFile();
  savesEl.textContent = `saves: imported ${n} file(s)`;
}
