#!/usr/bin/env node
// Regression test for the shader translator: fixtures/shaders/rules.* exercise every rewrite rule
// (version-first, sampler1D, noperspective, int->float incl. hex, int->uint incl. array index,
// uint switch labels). Must translate cleanly, pass glslang ES 3.00, and link in Chromium WebGL2.
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const out = fs.mkdtempSync(path.join(os.tmpdir(), "selftest-"));
const node = process.execPath;
const step = (name, argv) => {
  process.stdout.write(`- ${name} ... `);
  try { execFileSync(node, argv, { stdio: "pipe" }); console.log("ok"); }
  catch (e) { console.log("FAIL"); console.log(String(e.stdout) + String(e.stderr)); process.exit(1); }
};
step("translate + glslang", [path.join(here, "gles-shaders.mjs"), "--src", path.join(here, "fixtures/shaders"), "--out", out, "--patches", path.join(here, "fixtures/no-patches.json")]);
const v = fs.readFileSync(path.join(out, "rules.vert"), "utf8"), f = fs.readFileSync(path.join(out, "rules.frag"), "utf8");
const expect = (cond, what) => { if (!cond) { console.log(`FAIL expectation: ${what}`); process.exit(1); } };
expect(v.startsWith("#version 300 es"), "#version on line 1");
expect(/jak_tex1d\(time_of_day_index\)/.test(v), "texelFetch rewritten through jak_tex1d");
expect(!/noperspective/.test(v + f), "noperspective dropped");
expect(/32768\.0/.test(v) && /4096\.0/.test(v), "hex literals converted to float");
expect(/hvdf\[flags - 1u\]/.test(v), "uint literal inside array index");
expect(/case 0u:/.test(f) && /case 1u:/.test(f), "uint switch labels");
console.log("- expectations ... ok");
if (!process.argv.includes("--no-browser")) step("WebGL2 compile+link", [path.join(here, "webgl-shader-smoke.mjs"), "--dir", out]);
fs.rmSync(out, { recursive: true, force: true });
console.log("selftest passed");
