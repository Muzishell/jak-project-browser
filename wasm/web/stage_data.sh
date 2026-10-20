#!/bin/bash
# Stage the game files gk_web preloads for local development (CLAUDE.MD, M5): the wasm iso files of
# the first playable levels and their .fr3 files, under build-wasm/web-data/jak-project/.
# The staged files are game data: local only, never committed or published (CLAUDE.MD hard rules).
# Usage (from the repo root): wasm/web/stage_data.sh [level DGO names, default: VI1 TRA BEA]
# Also stages the sound banks, music, animation streams (.STR, ~100 MB) and English voice lines
# (VAGWAD.ENG, ~170 MB; the IOP dies on a missing VAGWAD) until files load lazily (M7).
set -e
cd "$(dirname "$0")/../.."
levels=${*:-VI1 TRA BEA}
iso_in=out/jak1-wasm/iso
fr3_in=out/jak1/fr3
dst=build-wasm/web-data/jak-project
rm -rf build-wasm/web-data
mkdir -p $dst/out/jak1-wasm/iso $dst/out/jak1/fr3

# boot files: the CGOs, text, and the small overlord tables
for f in $iso_in/*.CGO $iso_in/*.TXT $iso_in/*.AYB; do
  [ -e "$f" ] && cp "$f" $dst/out/jak1-wasm/iso/
done
# sound: banks, music, animation streams, voice lines
cp $iso_in/*.SBK $iso_in/*.MUS $iso_in/*.STR $iso_in/VAGWAD.ENG $dst/out/jak1-wasm/iso/
# levels
declare -A fr3_name=([VI1]=village1 [TRA]=training [BEA]=beach [VI2]=village2 [INT]=intro
                     [TIT]=title [JUN]=jungle [MIS]=misty [FIC]=firecanyon)
cp $fr3_in/GAME.fr3 $dst/out/jak1/fr3/
for l in $levels; do
  cp $iso_in/$l.DGO $dst/out/jak1-wasm/iso/
  [ -e $iso_in/$l.VIS ] && cp $iso_in/$l.VIS $dst/out/jak1-wasm/iso/
  cp $fr3_in/${fr3_name[$l]}.fr3 $dst/out/jak1/fr3/
done
# test objects for -wasm-test (e.g. goto-training.o from test/goalc/source_templates/kernel)
mkdir -p $dst/out/jak1-wasm/obj
for f in goto-training load-engine; do
  [ -e out/jak1-wasm/obj/$f.o ] && cp out/jak1-wasm/obj/$f.o $dst/out/jak1-wasm/obj/
done
du -sh build-wasm/web-data
