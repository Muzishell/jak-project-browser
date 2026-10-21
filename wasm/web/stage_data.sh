#!/bin/bash
# Stage the game files gk_web fetches from /data/ for local development (CLAUDE.MD, M7 slice 1):
# the wasm iso files, the .fr3 files and the -wasm-test objects, hard-linked from out/ into
# build-wasm/web-data/out/, plus the manifest.txt main_web.cpp mounts them from (path, size,
# md5 per line). Nothing is
# downloaded until the game reads a file, so every level is staged.
# The staged files are game data: local only, never committed or published (CLAUDE.MD hard rules).
# Usage (from the repo root): wasm/web/stage_data.sh, then node wasm/web/serve.mjs
set -e
cd "$(dirname "$0")/../.."
dst=build-wasm/web-data/out
rm -rf build-wasm/web-data
mkdir -p $dst/jak1-wasm/iso $dst/jak1/fr3 $dst/jak1-wasm/obj

# hard links: same bytes as out/, no copy (falls back to a copy across filesystems)
link() { cp -l "$@" 2>/dev/null || cp "$@"; }
link out/jak1-wasm/iso/* $dst/jak1-wasm/iso/
link out/jak1/fr3/*.fr3 $dst/jak1/fr3/
# test objects for -wasm-test (e.g. goto-training.o from test/goalc/source_templates/kernel)
for f in goto-training goto-intro load-engine; do
  [ -e out/jak1-wasm/obj/$f.o ] && link out/jak1-wasm/obj/$f.o $dst/jak1-wasm/obj/
done

# one line per file: path, size, md5 (the page's OPFS cache keys files by md5, data_sw.js)
(cd $dst && find . -type f ! -name manifest.txt | sed 's|^\./||' | sort | while read -r f; do
  echo "$f $(stat -Lc%s "$f") $(md5sum < "$f" | cut -c1-32)"
done > manifest.txt)
echo "$(wc -l < $dst/manifest.txt) files, $(du -shL $dst | cut -f1) under $dst"
