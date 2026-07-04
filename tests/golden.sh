#!/usr/bin/env bash
# Golden-image test: render a scene headless and compare against checked-in
# goldens.
#
# usage: golden.sh <drift> <imgcmp> <scene.sceneproject> <golden-dir>
#
# The golden dir contains frame_NNNN.png files (which frames to render is
# derived from their names) and optionally:
#   size     - render resolution as WxH (default 320x180)
#   mouse    - fixed pointer position as X,Y (passed as --mouse)
#   presents - expected "presented N of M frames" count (efficiency
#              contract: a static scene must present exactly once)
#
# Renders on the lavapipe software rasterizer by default so results are
# reproducible across machines; set DRIFT_ADAPTER to override (imgcmp's
# tolerances absorb real-GPU LSB differences). Regenerate goldens with:
#   DRIFT_ADAPTER=llvmpipe drift <scene> --frames <list> --size 320x180 --out <golden-dir>
set -eu

drift=$1
imgcmp=$2
scene=$3
golden=$4

: "${DRIFT_ADAPTER:=llvmpipe}"
export DRIFT_ADAPTER

size=320x180
[ -f "$golden/size" ] && size=$(cat "$golden/size")
mouse_args=()
[ -f "$golden/mouse" ] && mouse_args=(--mouse "$(cat "$golden/mouse")")

frames=""
for f in "$golden"/frame_*.png; do
    [ -e "$f" ] || { echo "golden.sh: no goldens in $golden" >&2; exit 2; }
    n=${f##*/frame_}
    n=${n%.png}
    n=$((10#$n))
    frames="$frames${frames:+,}$n"
done

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

log=$("$drift" "$scene" --frames "$frames" --size "$size" --out "$out" "${mouse_args[@]}")
echo "$log"

rc=0
if [ -f "$golden/presents" ]; then
    want="presented $(cat "$golden/presents") frames"
    case "$log" in
        *"$want"*) ;;
        *) echo "golden.sh: FAIL: expected '$want' in output" >&2; rc=1 ;;
    esac
fi
for f in "$golden"/frame_*.png; do
    "$imgcmp" "$out/$(basename "$f")" "$f" || rc=1
done
exit $rc
