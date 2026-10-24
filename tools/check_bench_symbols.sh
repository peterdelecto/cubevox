#!/usr/bin/env bash
# Checks that only the bench image holds bench code.
#
#   tools/check_bench_symbols.sh release FILE.elf   # fails when any bench-only function is in the elf
#   tools/check_bench_symbols.sh bench FILE.elf     # fails unless every bench-only function is in the elf
#
# The bench check is the control for the release check. A function that was renamed would leave
# the release check searching for a name nothing defines, and the bench check fails on that name.
# firmware/build.sh runs this on every image it builds. NM names the symbol tool (default nm).
#
# Every name below is defined only under CUBEVOX_BENCH, with no stub in the release image.

set -euo pipefail

BENCH_ONLY=(benchDrillRun benchPanelRun recordEraseAll audioStopForBench faultForceBusFault)

variant="${1:-}"
elf="${2:-}"
if { [ "$variant" != release ] && [ "$variant" != bench ]; } || [ -z "$elf" ] || [ $# -ne 2 ]; then
  echo "usage: check_bench_symbols.sh release|bench FILE.elf"
  exit 1
fi
if [ ! -f "$elf" ]; then
  echo "[ERROR] $elf is missing"
  exit 1
fi

symbols="$("${NM:-nm}" "$elf")"
if [ -z "$symbols" ]; then
  echo "[ERROR] $elf has no symbols, so no name can be checked"
  exit 1
fi
present=()
absent=()
for name in "${BENCH_ONLY[@]}"; do
  if grep -q -- "$name" <<<"$symbols"; then present+=("$name"); else absent+=("$name"); fi
done

if [ "$variant" = release ] && [ "${#present[@]}" -gt 0 ]; then
  echo "[ERROR] the release image holds bench code: ${present[*]}"
  exit 1
fi
if [ "$variant" = bench ] && [ "${#absent[@]}" -gt 0 ]; then
  echo "[ERROR] the bench image lacks ${absent[*]}; rename them in BENCH_ONLY of tools/check_bench_symbols.sh"
  exit 1
fi
if [ "$variant" = release ]; then
  echo "[symbols] release image holds none of the ${#BENCH_ONLY[@]} bench-only functions"
else
  echo "[symbols] bench image holds all ${#BENCH_ONLY[@]} bench-only functions"
fi
