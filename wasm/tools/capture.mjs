#!/usr/bin/env node
// Open the wasm build in Chromium, wait for a frame, save a screenshot + every console line.
//
//   node wasm/tools/capture.mjs --url http://127.0.0.1:8080/ [--out build-wasm/captures/run1]
//        [--frame 300] [--timeout 120] [--headed] [--size 640x480]
//
// Frame sync: if the page sets `window.__jak = { frame: <int> }` (see RENDERER.md, "capture hook"),
// we wait until frame >= --frame and screenshot the canvas. Otherwise we wait --timeout seconds.
// Writes <out>.png, <out>.console.txt, <out>.json (GL info, errors, crossOriginIsolated, timings).

import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";

const args = process.argv.slice(2);
const opt = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const URL_ = opt("--url", "http://127.0.0.1:8080/");
const OUT = opt("--out", "build-wasm/captures/latest");
const FRAME = +opt("--frame", 300);
const TIMEOUT = +opt("--timeout", 120) * 1000;
const HEADED = args.includes("--headed");
const [W, H] = opt("--size", "640x480").split("x").map(Number);

async function loadPlaywright() {
  try { return await import("playwright"); } catch {}
  const req = createRequire(import.meta.url);
  for (const p of [process.env.PLAYWRIGHT_PATH, ...(process.env.NODE_PATH || "").split(path.delimiter)].filter(Boolean)) {
    try { return req(path.join(p, "playwright")); } catch {}
  }
  console.error("playwright not found: npm ci in wasm/tools, then npx playwright install chromium");
  process.exit(3);
}

const { chromium } = await loadPlaywright();
fs.mkdirSync(path.dirname(OUT), { recursive: true });
const browser = await chromium.launch({
  headless: !HEADED,
  args: HEADED ? ["--ignore-gpu-blocklist"] : ["--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"],
});
const page = await browser.newPage({ viewport: { width: W, height: H } });
const log = [];
const t0 = Date.now();
const stamp = () => ((Date.now() - t0) / 1000).toFixed(2).padStart(7);
page.on("console", (m) => log.push(`${stamp()} [${m.type()}] ${m.text()}`));
page.on("pageerror", (e) => log.push(`${stamp()} [pageerror] ${e.stack || e.message}`));
page.on("requestfailed", (r) => log.push(`${stamp()} [requestfailed] ${r.url()} ${r.failure()?.errorText}`));
page.on("crash", () => log.push(`${stamp()} [crash] renderer process crashed`));

let reached = false, frame = null;
try {
  await page.goto(URL_, { waitUntil: "load", timeout: TIMEOUT });
  const deadline = Date.now() + TIMEOUT;
  while (Date.now() < deadline) {
    frame = await page.evaluate(() => (window.__jak && Number.isFinite(window.__jak.frame) ? window.__jak.frame : null)).catch(() => null);
    if (frame !== null && frame >= FRAME) { reached = true; break; }
    await page.waitForTimeout(250);
  }
} catch (e) {
  log.push(`${stamp()} [capture] ${e.message}`);
}

const info = await page.evaluate(() => {
  const c = document.querySelector("canvas");
  let gl = null;
  try { gl = c && (c.getContext("webgl2") || null); } catch {}
  return {
    crossOriginIsolated: self.crossOriginIsolated,
    canvas: c ? { w: c.width, h: c.height } : null,
    glError: gl ? gl.getError() : null,
    jak: window.__jak || null,
  };
}).catch((e) => ({ error: e.message }));

const canvas = await page.$("canvas");
if (canvas) await canvas.screenshot({ path: OUT + ".png" }).catch((e) => log.push(`[capture] canvas screenshot failed: ${e.message}`));
else await page.screenshot({ path: OUT + ".png" });
await browser.close();

fs.writeFileSync(OUT + ".console.txt", log.join("\n") + "\n");
const summary = { url: URL_, targetFrame: FRAME, reached, lastFrame: frame, seconds: (Date.now() - t0) / 1000, ...info,
  errors: log.filter((l) => /\[(error|pageerror|crash|requestfailed)\]/.test(l)).length };
fs.writeFileSync(OUT + ".json", JSON.stringify(summary, null, 2));
console.log(JSON.stringify(summary));
if (!info.crossOriginIsolated) console.log("WARNING: page is not crossOriginIsolated: pthreads will not start (serve with wasm/tools/serve.mjs)");
process.exit(reached || frame === null ? 0 : 1);
