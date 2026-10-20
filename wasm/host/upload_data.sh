#!/bin/bash
# Upload the game files staged by wasm/web/stage_data.sh to the R2 bucket jak-data, under the same
# paths gk_web reads from /data/ (manifest.txt + jak1-wasm/..., jak1/fr3/...).
#   upload_data.sh --local    local R2 for `wrangler dev` (build-wasm/wrangler-state)
#   upload_data.sh --remote   the real private bucket. Game data: only ever to jak-data, never to
#                             anything public (CLAUDE.MD hard rules).
# English voice lines only: the other VAGWAD.* (167 MB each) are left out. manifest.txt is
# rewritten to list exactly what was uploaded, and goes up last.
set -e
case "$1" in
  --local) target=(--local --persist-to build-wasm/wrangler-state) ;;
  --remote) target=(--remote) ;;
  *) echo "usage: $0 --local | --remote"; exit 1 ;;
esac
cd "$(dirname "$0")/../.."
src=build-wasm/web-data/out
bucket=jak-data
wrangler=(npx --yes wrangler@4.151.3)
[ -f $src/manifest.txt ] || { echo "run wasm/web/stage_data.sh first"; exit 1; }

list=build-wasm/host-manifest.txt
grep -vE '^jak1-wasm/iso/VAGWAD\.(FRE|GER|ITA|JAP|SPA)$' $src/manifest.txt > $list
echo "$(wc -l < $list) files, $(cd $src && du -chL $(cat ../../host-manifest.txt) | tail -1 | cut -f1)"

n=0
while read -r f; do
  n=$((n + 1))
  "${wrangler[@]}" r2 object put "$bucket/$f" --file "$src/$f" \
    --content-type application/octet-stream "${target[@]}" > /dev/null
  echo "[$n] $f"
done < $list
"${wrangler[@]}" r2 object put "$bucket/manifest.txt" --file $list \
  --content-type text/plain "${target[@]}" > /dev/null
echo "manifest.txt ($n files) uploaded"
