#!/usr/bin/env bash
# Installs the bootloader, then the firmware. build.sh never flashes.
#
#   ./flash.sh                 # the drive path: no buttons, enclosure closed
#   ./flash.sh FILE.uf2        # the drive path with a given bootloader file
#   ./flash.sh --rom           # the ST ROM DFU path, shop only; the cubevox board's build
#   ./flash.sh --rom FILE.bin  # the ROM DFU path with a given bootloader image
#   ./flash.sh --help          # print this text
#
# The drive path takes a box that already runs a bootloader. With a cubevox serial port present it
# sends enter_dfu, waits for /Volumes/CUBEVOX, copies <out dir>/cubevox-bootloader-v<version>.uf2
# onto it and ejects. The bootloader takes the file as an application and starts it; the updater
# writes the new bootloader into sector 0, erases itself and restarts the box, which comes back as
# the drive. flash.sh waits for it, requires INFO_UF2.TXT to name the new bootloader and
# "Installed: none" and DIAG.TXT's app summary to read "bootloader v<version> written", then
# installs the firmware with ../flash.sh and exits with its exit code. When a check fails it still
# installs the firmware, which the updater erased, then prints the error and exits 1. The
# firmware must be built first (../build.sh), and flash.sh checks it before it starts. While the
# box shows "Updating the bootloader." the power must stay on, about 2 s: a cut there leaves no
# bootloader, and only --rom recovers the box.
#
# --rom writes the bootloader into bank 1 sector 0 through the ST ROM DFU (0483:df11). With the
# enclosure open, hold BOOT0 down for the whole run, tap NRST, and let go of BOOT0 when flash.sh
# says so. A bootloader that has run write protects its own sector, and the ROM DFU then skips the
# erase and the write of that sector without an error. flash.sh reads the option bytes first and
# clears that protection when it is set; the ROM resets the chip after an option byte write, and
# BOOT0, still held, brings it back into the ROM DFU. After the write flash.sh reads the sector back
# and compares it with the file, then starts the new bootloader, which protects its sector again.
#
# Waits up to WAIT seconds (default 300) for the ROM DFU device, or for the drive when no serial
# port is present. With several cu.usbmodem* ports, name one in CUBEVOX_PORT.

set -euo pipefail
BL="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$BL/../.." && pwd)"
source "$BL/../cv_define.sh"
source "$BL/../dfu_util.sh"

usage() {  # the header comment above, without its marks
  awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
}
ROM=0
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  --rom) ROM=1; shift ;;
  -*) echo "[ERROR] unknown option '$1'; flash.sh takes --rom, an optional bootloader file, or --help"; exit 1 ;;
esac
if [ "$#" -gt 1 ]; then
  echo "[ERROR] flash.sh takes one bootloader file at most"
  exit 1
fi

WAIT="${WAIT:-300}"
OUTDIR="$("$BL/build.sh" --outdir)"

#--------------------------------------------------------------------+
# ROM DFU path
#--------------------------------------------------------------------+

rom_main() {
  BIN="${1:-$OUTDIR/cubevox-bootloader.bin}"
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

  wait_dfu
  unprotect_sector0
  dfu_download "$work/write.log" -a 0 -s "$FLASH_START" -D "$BIN"
  dfu_upload "$work/readback.log" "$work/readback.bin" "$size" -a 0 -s "$FLASH_START:$size:leave"
  if ! cmp -s "$BIN" "$work/readback.bin"; then
    echo "[ERROR] sector 0 does not read back as $BIN; the write did not take (BOOT0 let go too early?)"
    exit 1
  fi
  echo "[flash] bootloader written and read back ($size bytes); let go of BOOT0, the box starts into it"
}

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

#--------------------------------------------------------------------+
# Drive path
#--------------------------------------------------------------------+

VOLUME=CUBEVOX          # the drive label (boards/cubevox/board.h)
MOUNT_WAIT=15           # the drive mounts within 5 s of enter_dfu
LEAVE_WAIT=15           # the drive leaves within a few seconds of the eject
RETURN_WAIT=60          # the restart, the updater's few seconds and the second restart
PORT_TOOL=(python3 -I "$BL/../cubevox_port.py")
STAMP_TOOL=(python3 -I "$REPO/tools/stamp_image.py")

# A line of a drive text file, CR LF and padding removed. Empty when the file lacks it.
drive_line() {
  tr -d '\r' < "$1" | grep -m 1 -- "^$2" | sed 's/ *$//' || true
}

# The firmware ../flash.sh will install: built, stamped, and named as VERSION.txt says. Checked
# before the bootloader moves, so the box is never left without an application for a missing build.
firmware_check() {
  local image checked expected
  image="$("$BL/../build.sh" --outdir)/cubevox.uf2"
  if [ ! -f "$image" ]; then
    echo "[ERROR] $image missing; build the firmware first (../build.sh)"
    exit 1
  fi
  checked="$("${STAMP_TOOL[@]}" check "$image")"
  expected="v$(head -n 1 "$REPO/VERSION.txt")"
  if [ "$(awk '{ print $2 }' <<<"$checked")" != "$expected" ] &&
     [ "$(awk '{ print $2 }' <<<"$checked")" != "$expected-bench" ]; then
    echo "[ERROR] $image is '$checked' but VERSION.txt says $expected; build the firmware again first"
    exit 1
  fi
  echo "[flash] firmware ready: $checked"
}

drive_main() {
  local version file checked box_port drive_wait="$WAIT" volume before copy_rc=0 eject_rc=0 log
  version="$(sed -n 's/^#define CV_BOOT_VERSION "\([0-9.]*\)"$/\1/p' "$BL/boards/cubevox/board.h")"
  file="${1:-$OUTDIR/cubevox-bootloader-v$version.uf2}"
  if [ ! -f "$file" ]; then
    echo "[ERROR] $file missing; run ./build.sh first"
    exit 1
  fi
  case "$file" in
    *.uf2) ;;
    *) echo "[ERROR] $file is not a bootloader file (.uf2); a .bin goes in with --rom"; exit 1 ;;
  esac
  # The file's header names the bootloader it carries as boot-<version>.
  checked="$("${STAMP_TOOL[@]}" check "$file")"
  version="$(awk '{ print $2 }' <<<"$checked")"
  if [ "${version#boot-}" = "$version" ]; then
    echo "[ERROR] $file is '$checked', not a bootloader file (its header must read boot-<version>)"
    exit 1
  fi
  version="${version#boot-}"
  echo "[flash] bootloader file, $checked"
  firmware_check

  box_port="$("${PORT_TOOL[@]}" --allow-none)"
  if [ -n "$box_port" ]; then
    echo "[flash] asking the box on $box_port to enter update mode"
    if "${PORT_TOOL[@]}" --port "$box_port" --request enter_dfu >/dev/null; then
      drive_wait="$MOUNT_WAIT"
    else
      echo "[flash] the box did not accept enter_dfu; use MENU > Update firmware, or --rom"
    fi
  fi
  echo "[flash] waiting for /Volumes/$VOLUME"
  if ! volume="$("${PORT_TOOL[@]}" --wait-volume "$VOLUME" --wait "$drive_wait")"; then
    echo "        use MENU > Update firmware on the box; a box without a bootloader needs --rom"
    exit 1
  fi
  before="$(drive_line "$volume/INFO_UF2.TXT" "cubevox bootloader")"
  echo "[flash] the box runs ${before:-an unnamed bootloader}"

  # The bootloader resets when the Mac ejects the drive, so the eject follows the copy. The drive
  # leaving decides, whatever cp and diskutil report.
  log="$(mktemp)"
  trap 'rm -f "$log"' EXIT
  echo "[flash] copying $(basename "$file") to $volume"
  cp -X "$file" "$volume/" >"$log" 2>&1 || copy_rc=$?
  diskutil eject "$volume" >>"$log" 2>&1 || eject_rc=$?
  if [ "$copy_rc" -ne 0 ] || [ "$eject_rc" -ne 0 ]; then
    echo "[flash] cp exited $copy_rc and diskutil eject $eject_rc, saying '$(tr '\n' ' ' < "$log")'"
  fi
  if ! "${PORT_TOOL[@]}" --wait-volume "$VOLUME" --gone --wait "$LEAVE_WAIT" >/dev/null; then
    echo "[ERROR] the drive is still mounted ${LEAVE_WAIT}s after the copy and eject; they said '$(tr '\n' ' ' < "$log")'"
    exit 1
  fi
  echo "[flash] the drive left; the box is updating its bootloader, keep the power on"

  if ! volume="$("${PORT_TOOL[@]}" --wait-volume "$VOLUME" --wait "$RETURN_WAIT")"; then
    echo "[ERROR] the drive did not come back within ${RETURN_WAIT}s; if the screen stays dark the box"
    echo "        has no bootloader and needs --rom"
    exit 1
  fi
  local running installed summary failure="" firmware_rc=0
  running="$(drive_line "$volume/INFO_UF2.TXT" "cubevox bootloader")"
  installed="$(drive_line "$volume/INFO_UF2.TXT" "Installed:")"
  summary="$(drive_line "$volume/DIAG.TXT" "app summary:")"
  echo "[flash] the drive is back: $running, $installed"
  echo "[flash] DIAG.TXT $summary"
  echo "[flash] DIAG.TXT $(drive_line "$volume/DIAG.TXT" "option bytes:")"
  # The version alone cannot tell a write from a kept bootloader of the same version; the updater's
  # summary can.
  if [ "$running" != "cubevox bootloader v$version" ]; then
    failure="the box kept its bootloader; DIAG.TXT's app summary above says why"
  elif [ "$summary" != "app summary: bootloader v$version written" ]; then
    failure="DIAG.TXT's app summary does not read 'bootloader v$version written', so the updater did not write sector 0"
  elif [ "$installed" != "Installed: none" ]; then
    failure="INFO_UF2.TXT does not say 'Installed: none', so the updater did not finish"
  fi
  rm -f "$log"
  if [ -z "$failure" ]; then
    echo "[flash] bootloader v$version installed; installing the firmware"
    exec "$BL/../flash.sh"
  fi
  # The updater erased the firmware on its way out, so the box gets it back before the error.
  echo "[flash] the bootloader update did not take; installing the firmware so the box runs"
  "$BL/../flash.sh" || firmware_rc=$?
  echo "[ERROR] $failure (the firmware install exited $firmware_rc)"
  exit 1
}

if [ "$ROM" -eq 1 ]; then
  rom_main "$@"
else
  drive_main "$@"
fi
