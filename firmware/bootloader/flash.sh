#!/usr/bin/env bash
# Writes the bootloader into bank 1 sector 0 through the ST ROM DFU (0483:df11). Shop only: with the
# enclosure open, hold BOOT0, tap NRST, release BOOT0. build.sh never flashes.
#
#   ./flash.sh               # the cubevox board's build; run ./build.sh first
#   ./flash.sh FILE.bin      # a given bootloader image
#
# Waits up to WAIT seconds (default 300) for the ROM DFU device.

set -euo pipefail
BL="$(cd "$(dirname "$0")" && pwd)"
BIN="${1:-$("$BL/build.sh" --outdir)/cubevox-bootloader.bin}"
WAIT="${WAIT:-300}"
# Bank 1 sector 0. A larger file would reach the application's first sector.
LIMIT=131072

if ! command -v dfu-util >/dev/null; then
  echo "[ERROR] dfu-util not installed (brew install dfu-util)"
  exit 1
fi
if [ ! -f "$BIN" ]; then
  echo "[ERROR] $BIN missing; run ./build.sh first"
  exit 1
fi
size=$(($(wc -c < "$BIN")))
if [ "$size" -eq 0 ] || [ "$size" -gt "$LIMIT" ]; then
  echo "[ERROR] $BIN is $size bytes; a bootloader is 1..$LIMIT bytes (bank 1 sector 0)"
  exit 1
fi

in_dfu() {
  local listing
  listing="$(dfu-util -l)" || { echo "[ERROR] dfu-util -l failed"; exit 1; }
  [[ "$listing" == *0483:df11* ]]
}

for ((i = 0; i < WAIT; i++)); do
  if in_dfu; then break; fi
  if [ "$i" -eq 0 ]; then echo "[flash] waiting for the ST ROM DFU (hold BOOT0, tap NRST, release BOOT0)"; fi
  sleep 1
done
if ! in_dfu; then
  echo "[ERROR] no DFU device 0483:df11 after ${WAIT}s"
  exit 1
fi

# The leave request resets the chip before it answers, so dfu-util reports a get_status error
# and may exit non-zero. The write counts as good when the download finished and no other error shows.
dfu_log="$(mktemp)"
trap 'rm -f "$dfu_log"' EXIT
dfu_rc=0
dfu-util -a 0 -s 0x08000000:leave -D "$BIN" >"$dfu_log" 2>&1 || dfu_rc=$?
grep -E "Erase|downloaded|Error" "$dfu_log" | grep -v get_status || true  # no match is a normal outcome
unexpected="$(awk 'tolower($0) ~ /error/ && $0 !~ /get_status/' "$dfu_log")"
if ! grep -q "File downloaded successfully" "$dfu_log" || [ -n "$unexpected" ]; then
  echo "[ERROR] dfu-util did not finish the download (exit $dfu_rc); output follows"
  cat "$dfu_log"
  exit 1
fi
echo "[flash] bootloader written ($size bytes); the box restarts into it"
