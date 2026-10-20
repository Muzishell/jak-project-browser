---
name: session-handoff
description: Use at the end of a work session, before stopping for the night, or when asked to "wrap up", "hand off" or "save progress". Writes wasm/HANDOFF.md and updates the CLAUDE.MD milestones and Log so the next session starts where this one stopped.
---

# End-of-session handoff

The next session starts cold. The SessionStart hook prints `wasm/HANDOFF.md`, so that file is the
bridge. Write it for someone who has CLAUDE.MD (uppercase file name; the filesystem is
case-sensitive) and nothing else.

## Steps

1. **Collect facts, don't recall them.** `git status --short`, `git log --oneline master..HEAD | head -30`,
   `git diff --stat`, last build/test results (re-run the cheap ones if unsure: native
   `goalc-test`, `node test/goalc/wasm/kernel-test.mjs`, and for shader edits the two checkers in
   `wasm/tools` if their deps are installed).
2. **Overwrite `wasm/HANDOFF.md`** with this shape (keep it under ~60 lines):

   ```markdown
   # Handoff - <YYYY-MM-DD HH:MM>
   ## Where we are
   Milestone: M<n> <name>. One paragraph: what works now that didn't before this session.
   ## Uncommitted / in-flight
   Files touched and why; anything half-done and how to finish or revert it.
   ## Next 3 steps
   1. ... (concrete: file, function, command)
   ## Known broken / open questions
   - ... (and whether it's a question for the owner)
   ## Numbers
   Tests: goalc-test x/1669, wasm tests y/76, kernel-test.mjs z/4. Perf: ms/frame focused/unfocused.
   ```
3. **CLAUDE.MD:** tick milestones that landed (only if their stated check passed), and append one
   line per new decision or gotcha under "Log" (`- YYYY-MM-DD: ...`). Don't rewrite other sections.
4. **Renderer knowledge** that isn't a one-liner goes into `wasm/docs/RENDERER.md` instead.
5. **Commits follow CLAUDE.MD:** commit each successful, tested slice on `wasm` (never `master`);
   `task format` first (restore the untouched upstream files clang-format 18 rewrites); the message
   ends with `(AI-assisted)`; no `Co-Authored-By` trailer; never push, never create PRs or issues.
   If the owner said not to commit in this session, leave the work uncommitted and say so.
6. Tell the owner in two or three sentences what changed and what's next. No game data in the message.
