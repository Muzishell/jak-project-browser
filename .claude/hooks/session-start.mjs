#!/usr/bin/env node
// SessionStart hook: prints a short status block that Claude receives as context.
// Must stay fast (< 1-2 s) and never fail the session: every step is best effort.
//
// Runs under WSL's node (see .claude/settings.json) with the repo as cwd. CLAUDE_PROJECT_DIR, when
// it is set at all, is the Windows UNC path, so it is only used if it exists for this process.

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const envRoot = process.env.CLAUDE_PROJECT_DIR;
const root = envRoot && fs.existsSync(envRoot) ? envRoot : process.cwd();
const sh = (cmd, args) => {
  try { return execFileSync(cmd, args, { cwd: root, stdio: ["ignore", "pipe", "ignore"], timeout: 3000 }).toString().replace(/\s+$/, ""); }
  catch { return ""; }
};
const lines = ["## Session status (from .claude/hooks/session-start.mjs)"];

const branch = sh("git", ["rev-parse", "--abbrev-ref", "HEAD"]);
lines.push(`- branch: ${branch || "?"}${branch && branch !== "wasm" ? "  WARNING: expected 'wasm' (master mirrors upstream; never commit there)" : ""}`);
const ahead = sh("git", ["rev-list", "--count", "master..HEAD"]);
if (ahead) lines.push(`- commits on top of master: ${ahead}`);
const log = sh("git", ["log", "--oneline", "-5"]);
if (log) lines.push("- last commits:\n" + log.split("\n").map((l) => "    " + l).join("\n"));
const status = sh("git", ["status", "--short"]);
if (status) {
  const s = status.split("\n");
  lines.push(`- uncommitted: ${s.length} path(s)` + "\n" + s.slice(0, 12).map((l) => "    " + l).join("\n") + (s.length > 12 ? "\n    ..." : ""));
}

// the instructions file is CLAUDE.MD on this branch (case-sensitive filesystem)
for (const name of ["CLAUDE.MD", "CLAUDE.md"]) {
  const f = path.join(root, name);
  if (!fs.existsSync(f)) continue;
  try {
    const md = fs.readFileSync(f, "utf8");
    // "- [x] M5 WebGL2 renderer: ..." in the Milestones section
    const ms = md.split("\n").filter((l) => /^- \[[ x]\] M\d/.test(l));
    const open = ms.filter((l) => l.startsWith("- [ ]")).map((l) => l.slice(6).replace(/^(M\d+ [^:.]*).*/, "$1"));
    if (ms.length) lines.push(`- milestones (${name}) done: ${ms.length - open.length}/${ms.length}; open: ${open.length ? open.join(" | ") : "none"}`);
  } catch {}
  break;
}

lines.push(`- emsdk in env: ${process.env.EMSDK ? "yes" : "no (source ~/tools/emsdk/emsdk_env.sh before wasm builds)"}`);
lines.push(`- wasm/tools deps: ${fs.existsSync(path.join(root, "wasm/tools/node_modules")) ? "installed" : "missing (cd wasm/tools && npm ci: ask the owner first, it downloads)"}`);
lines.push("- skills: webgl2-renderer-port, wasm-build-and-run, native-vs-wasm-diff, session-handoff (run it before stopping)");

const handoff = path.join(root, "wasm/HANDOFF.md");
if (fs.existsSync(handoff)) {
  const t = fs.readFileSync(handoff, "utf8");
  lines.push("", "## wasm/HANDOFF.md (left by the previous session)", "", t.length > 5000 ? t.slice(0, 5000) + "\n...(truncated)" : t);
} else {
  lines.push("- no wasm/HANDOFF.md yet: ask what was done last, or read `git log` and the CLAUDE.MD Log.");
}
process.stdout.write(lines.join("\n") + "\n");
