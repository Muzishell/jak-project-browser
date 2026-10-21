#!/bin/bash
# Upload the game files staged by wasm/web/stage_data.sh to the R2 bucket jak-data, under the same
# paths gk_web reads from /data/ (manifest.txt + jak1-wasm/..., jak1/fr3/...).
#   upload_data.sh --local    local R2 for `wrangler dev` (build-wasm/wrangler-state)
#   upload_data.sh --remote   the real private bucket. Game data: only ever to jak-data, never to
#                             anything public (CLAUDE.MD hard rules).
#   add --only-manifest to upload just manifest.txt, or --all to upload every file again.
# By default only files that are new or changed (path, size, md5) compared with the manifest
# already in the bucket go up. Voice lines: VOICE_LANGS (default "ENG FRE"); the other VAGWAD.*
# (167 MB each) are left out. manifest.txt lists exactly what the bucket holds, and goes up last.
set -e
case "$1" in
  --local) target=(--local --persist-to build-wasm/wrangler-state) ;;
  --remote) target=(--remote) ;;
  *) echo "usage: $0 --local | --remote [--only-manifest | --all]"; exit 1 ;;
esac
mode=${2:-changed}
cd "$(dirname "$0")/../.."
src=build-wasm/web-data/out
bucket=jak-data
wrangler=(npx --yes wrangler@4.151.3)
[ -f $src/manifest.txt ] || { echo "run wasm/web/stage_data.sh first"; exit 1; }

langs=${VOICE_LANGS:-ENG FRE}
list=build-wasm/host-manifest.txt
awk -v langs=" $langs " '
  match($1, /^jak1-wasm\/iso\/VAGWAD\.[A-Z]+$/) { if (index(langs, " " substr($1, 22) " ") == 0) next }
  { print }' $src/manifest.txt > $list
echo "$(wc -l < $list) files, $(cd $src && du -chL $(cut -d' ' -f1 ../../host-manifest.txt) | tail -1 | cut -f1) (voices: $langs)"

todo=build-wasm/host-upload.txt
case "$mode" in
  --only-manifest) : > $todo ;;
  --all) cp $list $todo ;;
  *)
    # what the bucket's manifest already lists with the same size and md5 stays
    remote=$(mktemp)
    if "${wrangler[@]}" r2 object get "$bucket/manifest.txt" --file "$remote" "${target[@]}" > /dev/null 2>&1; then
      grep -vxF -f "$remote" $list > $todo || true
    else
      cp $list $todo
    fi
    rm -f "$remote"
    ;;
esac
echo "$(wc -l < $todo) file(s) to upload, $(cd $src && cut -d' ' -f1 ../../host-upload.txt | xargs -r du -chL | tail -1 | cut -f1)"

n=0
while read -r f _; do
  n=$((n + 1))
  "${wrangler[@]}" r2 object put "$bucket/$f" --file "$src/$f" \
    --content-type application/octet-stream "${target[@]}" > /dev/null
  echo "[$n] $f"
done < $todo
"${wrangler[@]}" r2 object put "$bucket/manifest.txt" --file $list \
  --content-type text/plain "${target[@]}" > /dev/null
echo "manifest.txt uploaded ($n file(s) before it)"
