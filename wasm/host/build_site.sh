#!/bin/bash
# Copy the browser runtime into build-wasm/host-site, the Worker's static assets (wrangler.jsonc).
# Only gk_web.{html,js,wasm,data}: the runtime and the repo's shaders. Game data never goes there;
# it goes to the private R2 bucket (upload_data.sh).
set -e
cd "$(dirname "$0")/../.."
src=build-wasm/wasm
dst=build-wasm/host-site
rm -rf $dst
mkdir -p $dst
for ext in html js wasm data; do
  cp $src/gk_web.$ext $dst/
done
ls -la $dst | awk 'NR > 1 && $9 !~ /^\.+$/ {print $5, $9}'
