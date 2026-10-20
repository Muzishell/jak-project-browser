#!/usr/bin/env node
// PreToolUse hook (Bash|PowerShell): enforces the CLAUDE.MD hard rules mechanically.
// Exit 2 = block; the stderr text is shown to Claude. Exit 0 = allow.
//
// Runs under WSL's node (see .claude/settings.json): Claude Code runs on Windows, the repo and
// git live in WSL. The payload's `cwd` is then a Windows UNC path (\\wsl.localhost\...), which
// doesn't exist for this process; it is only used when it exists, else process.cwd() (the repo,
// via `wsl.exe --cd`).
//
// Blocks:
//  - git commit on master; commit messages without "(AI-assisted)" or with a Co-Authored-By trailer
//  - git add of game data (iso_data/, out/, decompiler_out/, *.iso, *.CGO, *.DGO, *.fr3 ...) or `git add -f`
//  - gh pr create / gh issue create (AGENTS.md) and any git push (the owner pushes)
//  - sending game data off the machine (curl/scp/rsync/gh with a game-data path)
//
// Only the command line is inspected: a commit made inside a script file (`bash x.sh`,
// `wsl.exe -e bash /mnt/c/.../x.sh`) or with `git commit -F file` is invisible to this hook.

import { execFileSync } from "node:child_process";
import fs from "node:fs";

let input = "";
process.stdin.on("data", (d) => (input += d));
process.stdin.on("end", () => {
  let cmd = "", cwd = process.cwd();
  try {
    // a BOM appears when the payload is piped through Windows PowerShell
    const j = JSON.parse(input.replace(/^﻿/, "") || "{}");
    cmd = j?.tool_input?.command || "";
    if (j?.cwd && fs.existsSync(j.cwd)) cwd = j.cwd;
  } catch {
    process.exit(0); // never block on a malformed payload
  }
  const problems = check(cmd, cwd);
  if (problems.length) {
    process.stderr.write(`Blocked by .claude/hooks/guard.mjs (CLAUDE.MD hard rules):\n- ${problems.join("\n- ")}\n`);
    process.exit(2);
  }
  process.exit(0);
});

const GAME_DATA = /(^|[\s"'=/])(iso_data|decompiler_out|out\/jak[123x]?|out\/build|out\/iso)(\/|\s|$|["'])|\.(iso|ISO|CGO|cgo|DGO|dgo|fr3|STR|VAG|vag|o)\b(?![\w.])/;

function branch(cwd) {
  try { return execFileSync("git", ["rev-parse", "--abbrev-ref", "HEAD"], { cwd, stdio: ["ignore", "pipe", "ignore"] }).toString().trim(); }
  catch { return ""; }
}

// Remove what wraps the real command: `wsl.exe [--cd d] [-e]`, `bash -c "`, `$env:X=1`, and the
// quotes left at either end of a part after splitting `bash -c "cd x && git commit ..."`.
function unwrap(p) {
  let prev;
  do {
    prev = p;
    p = p.trim()
      .replace(/^wsl(\.exe)?((\s+(--cd|-d|--distribution|-u|--user)\s+\S+)|(\s+(-e|--exec|--)))*\s+/, "")
      .replace(/^(\/usr)?(\/bin\/)?(bash|sh)(\s+-\w+)*\s+-c\s+/, "")
      .replace(/^["'`]+/, "")
      .replace(/["'`]+$/, "")
      .replace(/^(\$env:\w+=\S*)$/, "");
  } while (p !== prev);
  return p;
}

function check(cmd, cwd) {
  const out = [];
  // split on shell separators so `cd x && git commit ...` is still seen
  const parts = cmd.split(/&&|\|\||;|\n/).map(unwrap).filter(Boolean);
  for (const p of parts) {
    if (/^git\b.*\bcommit\b/.test(p)) {
      const b = branch(cwd);
      if (b === "master" || b === "main") out.push(`git commit on '${b}': work on the 'wasm' branch (master mirrors upstream).`);
      if (/Co-Authored-By/i.test(cmd)) out.push("no Co-Authored-By trailer (upstream AI_POLICY.md does not merge AI co-authored commits).");
      const hasMsg = /\s-(m|-message)[\s=]/.test(p) || /\s-[a-zA-Z]*m\b/.test(p);
      if (hasMsg && !/\(AI-assisted\)/.test(cmd)) out.push("commit message must end with (AI-assisted) (AGENTS.md).");
    }
    if (/^git\b.*\badd\b/.test(p)) {
      if (/\s(-f|--force)\b/.test(p)) out.push("git add -f: ignored paths are ignored on purpose (game data, build outputs).");
      if (GAME_DATA.test(p.replace(/^git\s+(-C\s+\S+\s+)?add/, ""))) out.push("git add of a game-data path. Game data and anything derived from it never enters git.");
    }
    if (/^gh\b.*\b(pr|issue)\s+create\b/.test(p)) out.push("never create PRs or issues (AGENTS.md). Write the text to a file for the owner instead.");
    if (/^git\b.*\bpush\b/.test(p)) out.push("never push: the owner pushes (and never to the upstream remote).");
    if (/^(curl|wget|scp|rsync|gh|gsutil|aws|rclone)\b/.test(p) && GAME_DATA.test(p) && !/127\.0\.0\.1|localhost/.test(p))
      out.push("this looks like it sends game data off the machine. Game data stays local.");
  }
  return [...new Set(out)];
}
