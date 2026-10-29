#!/usr/bin/env bash
# Writes the bootloader into bank 1 sector 0 through the ST ROM DFU (0483:df11). Shop only: with the
# enclosure open, hold BOOT0 down for the whole run, tap NRST, and let go of BOOT0 when flash.sh says
# so. build.sh never flashes.
#
#   ./flash.sh               # the cubevox board's build; run ./build.sh first
#   ./flash.sh FILE.bin      # a given bootloader image
#   ./flash.sh --help        # print this text
#
# A bootloader that has run write protects its own sector, and the ROM DFU then skips the erase and
# the write of that sector without an error. flash.sh reads the option bytes first and clears that
# protection when it is set; the ROM resets the chip after an option byte write, and BOOT0, still
# held, brings it back into the ROM DFU. After the write flash.sh reads the sector back and compares
# it with the file, then starts the new bootloader, which protects its sector again.
#
# Waits up to WAIT seconds (default 300) for the ROM DFU device.

set -euo pipefail
BL="$(cd "$(dirname "$0")" && pwd)"
source "$BL/../cv_define.sh"
source "$BL/../dfu_util.sh"

usage() {  # the header comment above, without its marks
  awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  -*) echo "[ERROR] unknown option '$1'; flash.sh takes an optional bootloader .bin, or --help"; exit 1 ;;
esac

BIN="${1:-$("$BL/build.sh" --outdir)/cubevox-bootloader.bin}"
WAIT="${WAIT:-300}"
FLASH_START=0x08000000
OPTIONS=(python3 -I "$BL/tools/option_bytes.py")
if ! command -v dfu-util >/dev/null; then
  echo "[ERROR] dfu-util not installed (brew install dfu-util)"
  exit 1
fi
if [ ! -f "$BIN" ]; then
  echo "[ERROR] $BIN missing; run ./build.sh first"
  exit 1
fi
# Bank 1 sector 0. A larger file would reach the application's first sector.
limit=$(($(cv_define CV_FLASH_SECTOR_SIZE)))
size=$(($(wc -c < "$BIN")))
if [ "$size" -eq 0 ] || [ "$size" -gt "$limit" ]; then
  echo "[ERROR] $BIN is $size bytes; a bootloader is 1..$limit bytes (bank 1 sector 0)"
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

listing() {
  dfu-util -l 2>/dev/null || { echo "[ERROR] dfu-util -l failed"; exit 1; }
}

wait_dfu() {
  for ((i = 0; i < WAIT; i++)); do
    if [[ "$(listing)" == *0483:df11* ]]; then return 0; fi
    if [ "$i" -eq 0 ]; then echo "[flash] waiting for the ST ROM DFU (BOOT0 held, tap NRST)"; fi
    sleep 1
  done
  echo "[ERROR] no DFU device 0483:df11 after ${WAIT}s"
  exit 1
}

# dfu-util may exit non-zero on a transfer that resets or leaves, so its exit code alone says
# little. Any error line dfu_unexpected_errors reports fails the run.
dfu_failed() {
  echo "[ERROR] dfu-util ${*:3} did not finish (exit $2); output follows"
  cat "$1"
  exit 1
}

# A download counts as good when dfu-util says the file went down.
dfu_download() {
  local log="$1" rc=0
  shift
  dfu-util "$@" > "$log" 2>&1 || rc=$?
  if ! grep -q "File downloaded successfully" "$log" || [ -n "$(dfu_unexpected_errors "$log")" ]; then
    dfu_failed "$log" "$rc" "$@"
  fi
}

# An upload counts as good when the file holds every byte asked for.
dfu_upload() {
  local log="$1" file="$2" bytes="$3" rc=0
  shift 3
  dfu-util "$@" -U "$file" > "$log" 2>&1 || rc=$?
  if [ ! -f "$file" ] || [ "$(($(wc -c < "$file")))" -ne "$bytes" ] || [ -n "$(dfu_unexpected_errors "$log")" ]; then
    dfu_failed "$log" "$rc" "$@" -U "$file"
  fi
}

# The option byte block's address and size from the ROM's "@Option Bytes /ADDR/01*SIZE e" string.
option_area() {
  listing | sed -n 's/.*@Option Bytes *\/\(0x[0-9A-Fa-f]*\)\/01\*\([0-9]*\) .*/\1 \2/p' | head -n 1
}

# Clears the write protection of bank 1 sector 0 when it is set. The ROM resets the chip after the
# write; with BOOT0 still held it comes back into the ROM DFU.
unprotect_sector0() {
  local area address bytes
  area="$(option_area)"
  read -r address bytes <<< "$area"
  if [ -z "${address:-}" ] || [ -z "${bytes:-}" ]; then
    echo "[ERROR] the ROM DFU lists no option byte area; dfu-util -l follows"
    listing
    exit 1
  fi
  dfu_upload "$work/ob-read.log" "$work/ob.bin" "$bytes" -a 1 -s "$address:$bytes"
  if [ "$("${OPTIONS[@]}" status "$work/ob.bin")" = writable ]; then
    echo "[flash] sector 0 is writable"
    return
  fi
  "${OPTIONS[@]}" unprotect "$work/ob.bin" "$work/ob-writable.bin"
  echo "[flash] sector 0 is write protected; clearing it (the chip restarts, keep BOOT0 held)"
  dfu_download "$work/ob-write.log" -a 1 -s "$address:will-reset" -D "$work/ob-writable.bin"
  sleep 2
  wait_dfu
  dfu_upload "$work/ob-check.log" "$work/ob-after.bin" "$bytes" -a 1 -s "$address:$bytes"
  if [ "$("${OPTIONS[@]}" status "$work/ob-after.bin")" != writable ]; then
    echo "[ERROR] sector 0 is still write protected after the option byte write"
    exit 1
  fi
  echo "[flash] sector 0 is writable"
}

wait_dfu
unprotect_sector0
dfu_download "$work/write.log" -a 0 -s "$FLASH_START" -D "$BIN"
dfu_upload "$work/readback.log" "$work/readback.bin" "$size" -a 0 -s "$FLASH_START:$size:leave"
if ! cmp -s "$BIN" "$work/readback.bin"; then
  echo "[ERROR] sector 0 does not read back as $BIN; the write did not take (BOOT0 let go too early?)"
  exit 1
fi
echo "[flash] bootloader written and read back ($size bytes); let go of BOOT0, the box starts into it"
