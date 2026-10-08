#!/usr/bin/env bash
# A/B the engine against a reference commit: render a clip through each stage alone and
# through the full chain with both builds, then compare the outputs.
#
#   tools/engine_ab.sh <ref-commit> [in.wav] [--tol <dB>]
#
# Default input is samples/adam_vox_sample2_4s.wav. Tolerance is the residual (B minus A)
# level relative to A, in dB; default -200 means bit-exact. Exit 1 when any case fails.

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
REF="${1:?ref commit}"
shift
IN="$REPO/samples/adam_vox_sample2_4s.wav"
TOL=-200
while [ $# -gt 0 ]; do
  case "$1" in
    --tol) TOL="$2"; shift 2 ;;
    *) IN="$1"; shift ;;
  esac
done
[ -f "$IN" ] || { echo "[ERROR] input $IN missing"; exit 1; }

WT=/tmp/cubevox-ab-ref
OUT=/tmp/cubevox-ab
rm -rf "$OUT" && mkdir -p "$OUT/a" "$OUT/b"

# Reference build in a worktree (reused across runs when the ref is unchanged).
if [ ! -d "$WT" ] || [ "$(git -C "$WT" rev-parse HEAD)" != "$(git -C "$REPO" rev-parse "$REF")" ]; then
  git -C "$REPO" worktree remove --force "$WT" 2>/dev/null || true
  git -C "$REPO" worktree add --detach "$WT" "$REF" >/dev/null
fi
# The imgui submodule is empty in a fresh worktree; CMake configures the GUI target regardless.
[ -f "$WT/third_party/imgui/imgui.cpp" ] || { rm -rf "$WT/third_party/imgui"; ln -s "$REPO/third_party/imgui" "$WT/third_party/imgui"; }
cmake -S "$WT" -B "$WT/build" >/dev/null
cmake --build "$WT/build" --target cubevox-render -j >/dev/null
cmake --build "$REPO/build" --target cubevox-render -j >/dev/null
A="$WT/build/cubevox-render"
B="$REPO/build/cubevox-render"

# One case per stage at a mid setting, then the whole default layout.
CASES=(
  "ingate|--ingate -40"
  "autotune|--autotune --response 100"
  "octave|--octave -12 --omix 0.5"
  "unison|--on 1 --depth 0.5"
  "slapback|--slap 0.5"
  "distortion|--drive 0.5 --tone 0.5"
  "gate|--gate -40"
  "spring|--reverb springb --decay 0.5 --rmix 0.5"
  "chasm|--reverb chasm --decay 0.5 --rmix 0.5"
  "chain|--ingate -40 --autotune --response 100 --octave -12 --omix 0.5 --on 1 --depth 0.5 --slap 0.5 --drive 0.5 --tone 0.5 --gate -40 --reverb springb --decay 0.5 --rmix 0.5"
)
for c in "${CASES[@]}"; do
  name="${c%%|*}"
  args="${c#*|}"
  # shellcheck disable=SC2086
  "$A" "$IN" "$OUT/a/$name.wav" $args >/dev/null
  # shellcheck disable=SC2086
  "$B" "$IN" "$OUT/b/$name.wav" $args >/dev/null
done

echo "[ab] A = $REF ($(git -C "$REPO" rev-parse --short "$REF")), B = working tree, input $(basename "$IN")"
python3 "$REPO/tools/engine_ab_compare.py" "$OUT/a" "$OUT/b" --tol "$TOL"
