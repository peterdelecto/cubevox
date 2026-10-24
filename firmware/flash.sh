#!/usr/bin/env bash
# Usage: flash.sh [--rom | --help]
#
# Installs the application on a box whose sector 0 holds the bootloader (bootloader/flash.sh puts
# it there), then prints the boot report.
#
# By default the script uses the update drive, the path Adam takes by hand. With a cubevox serial
# port present it sends enter_dfu, waits for /Volumes/CUBEVOX, copies <out dir>/cubevox.uf2 onto
# it, waits for the drive to go and the box's port to return, and prints the boot report. With no
# serial port it waits up to WAIT seconds (default 300) for the drive. A failed start or a MENU
# held at power-on brings the drive up, and so does a bootloader that has no application yet.
#
# --rom goes through the ST ROM DFU with dfu-util, for a box whose application is unreachable. It
# writes <out dir>/cubevox.ino.bin at the application start (0x08020000, read from boot_block.h),
# above the bootloader's sector. Hold BOOT0, tap NRST, release BOOT0; the script waits up to WAIT
# seconds. Written at 0x08000000, that file would destroy the bootloader.
#
# The variant and out dir come from build.sh (VARIANT=release|bench; BENCH=1 implies bench). With
# several cu.usbmodem* ports, name one in CUBEVOX_PORT.
#
#   ./flash.sh                    # release image; build first with ./build.sh
#   VARIANT=bench ./flash.sh      # bench image; build first with BENCH=1 ./build.sh
#   ./flash.sh --rom              # the same image through the ROM DFU
#   ./flash.sh --help             # the short usage

set -euo pipefail
FW="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$FW/.." && pwd)"
VARIANT="$("$FW/build.sh" --variant)"
OUT="$("$FW/build.sh" --outdir)"
WAIT="${WAIT:-300}"
MOUNT_WAIT=15  # the drive mounts within 5 s of enter_dfu
LEAVE_WAIT=15  # the drive leaves within a few seconds of the last block
PORT_TOOL=(python3 -I "$FW/cubevox_port.py")
STAMP_TOOL=(python3 -I "$REPO/tools/stamp_image.py")

# The drive label the bootloader serves (bootloader/boards/cubevox/board.h).
VOLUME=CUBEVOX

usage() {
  cat <<'USAGE'
usage: ./flash.sh [--rom | --help]
  (no flag)  update-drive path: send enter_dfu over the serial port if one is present, wait for
             /Volumes/CUBEVOX, copy <out dir>/cubevox.uf2 onto it,
             wait for the drive to go and the box's port to return, print the boot report
  --rom      ST ROM DFU path: hold BOOT0, tap NRST, release; dfu-util writes <out dir>/cubevox.ino.bin
             at the application start from boot_block.h (0x08020000), above the bootloader's sector
  VARIANT=release|bench and BENCH=1 pick the image as build.sh does; WAIT=<s> bounds the waits
  (default 300); CUBEVOX_PORT names the port when several cu.usbmodem* exist
USAGE
}

ROM=0
case "${1:-}" in
  "") ;;
  --rom) ROM=1 ;;
  -h|--help) usage; exit 0 ;;
  *) echo "[ERROR] unknown argument '$1'; use --rom or --help"; exit 1 ;;
esac

IMAGE="$OUT/cubevox.uf2"
if [ "$ROM" -eq 1 ]; then IMAGE="$OUT/cubevox.ino.bin"; fi
if [ ! -f "$IMAGE" ]; then
  hint="./build.sh"
  if [ "$VARIANT" = bench ]; then hint="BENCH=1 ./build.sh"; fi
  echo "[ERROR] $IMAGE missing; run $hint first"
  exit 1
fi
# The tool refuses an unstamped image, a bad CRC and a .uf2 outside the application region.
checked="$("${STAMP_TOOL[@]}" check "$IMAGE")"
echo "[flash] $VARIANT image, $checked"

copy_log=""
trap 'if [ -n "$copy_log" ]; then rm -f "$copy_log"; fi' EXIT

in_dfu() {
  local listing
  listing="$(dfu-util -l)" || { echo "[ERROR] dfu-util -l failed"; exit 1; }
  [[ "$listing" == *0483:df11* ]]
}

rom_flash() {
  if ! command -v dfu-util >/dev/null; then
    echo "[ERROR] dfu-util not installed (brew install dfu-util)"
    exit 1
  fi
  local start i dfu_log dfu_rc=0 unexpected
  start="$(awk '$1 == "#define" && $2 == "CV_APP_START" { sub(/[uUlL]+$/, "", $3); print $3; exit }' \
    "$FW/cubevox/src/board/boot_block.h")"
  if [ -z "$start" ]; then
    echo "[ERROR] CV_APP_START is missing from boot_block.h"
    exit 1
  fi
  for ((i = 0; i < WAIT; i++)); do
    if in_dfu; then break; fi
    if [ "$i" -eq 0 ]; then echo "[flash] waiting for the ST ROM DFU (hold BOOT0, tap NRST, release BOOT0)"; fi
    sleep 1
  done
  if ! in_dfu; then
    echo "[ERROR] no DFU device after ${WAIT}s"
    exit 1
  fi

  # The leave request resets the chip before it answers, so dfu-util reports a get_status error
  # and may exit non-zero. The flash counts as good when the download finished and no other error shows.
  dfu_log="$(mktemp)"
  dfu-util -a 0 -s "$start:leave" -D "$IMAGE" >"$dfu_log" 2>&1 || dfu_rc=$?
  grep -E "Erase|downloaded|Error" "$dfu_log" | grep -v get_status || true  # no match is a normal outcome
  unexpected="$(awk 'tolower($0) ~ /error/ && $0 !~ /get_status/' "$dfu_log")"
  if ! grep -q "File downloaded successfully" "$dfu_log" || [ -n "$unexpected" ]; then
    echo "[ERROR] dfu-util did not finish the download (exit $dfu_rc); output follows"
    cat "$dfu_log"
    rm -f "$dfu_log"
    exit 1
  fi
  rm -f "$dfu_log"
}

drive_flash() {
  local box_port drive_wait="$WAIT" volume copy_rc=0
  # Fails with the candidates listed when several ports match and none was named.
  box_port="$("${PORT_TOOL[@]}" --allow-none)"
  if [ -n "$box_port" ]; then
    echo "[flash] asking the box on $box_port to enter update mode"
    if "${PORT_TOOL[@]}" --port "$box_port" --request enter_dfu >/dev/null; then
      drive_wait="$MOUNT_WAIT"
    else
      echo "[flash] the box did not accept enter_dfu; switch it on again while holding MENU"
    fi
  fi

  echo "[flash] waiting for /Volumes/$VOLUME"
  if ! volume="$("${PORT_TOOL[@]}" --wait-volume "$VOLUME" --wait "$drive_wait")"; then
    echo "        hold MENU while switching the box on, or put the bootloader in with bootloader/flash.sh;"
    echo "        a box without the bootloader enters the ST ROM DFU instead, so use --rom"
    exit 1
  fi

  # The box resets after the last block, so the OS may report an error for a copy that worked. The
  # drive leaving and the port returning decide.
  copy_log="$(mktemp)"
  echo "[flash] copying $(basename "$IMAGE") to $volume"
  cp -X "$IMAGE" "$volume/" >"$copy_log" 2>&1 || copy_rc=$?
  if [ "$copy_rc" -ne 0 ]; then
    echo "[flash] cp reported '$(tr '\n' ' ' < "$copy_log")'"
  fi
  if ! "${PORT_TOOL[@]}" --wait-volume "$VOLUME" --gone --wait "$LEAVE_WAIT" >/dev/null; then
    echo "[ERROR] the drive is still mounted ${LEAVE_WAIT}s after the copy; cp said '$(tr '\n' ' ' < "$copy_log")'"
    exit 1
  fi
  echo "[flash] the drive left; waiting for the box to restart"
}

if [ "$ROM" -eq 1 ]; then
  rom_flash
else
  drive_flash
fi

# The port name can change across the reset, so ask again.
if ! box_port="$("${PORT_TOOL[@]}" --wait 30)"; then
  if [ -n "$copy_log" ]; then echo "[ERROR] the box did not come back after the copy; cp said '$(tr '\n' ' ' < "$copy_log")'"; fi
  exit 1
fi
if [ -n "$copy_log" ]; then rm -f "$copy_log"; fi  # exec below skips the EXIT trap
sleep 2
exec "$FW/term.py" -t 3 -s '{"id":2,"cmd":"bench","op":"report"}' "$box_port"
