#!/usr/bin/env bash
# Flash /tmp/cubevox-fw-<variant>/cubevox.ino.bin over DFU and print the boot report.
#
# VARIANT=release|bench picks the image. It defaults to bench when BOARD=weact and to release
# otherwise. If a cubevox CDC port is present the box is asked to enter DFU itself (the
# enter_dfu command). Otherwise hold BOOT0, tap NRST, release BOOT0; the script waits up to WAIT
# seconds (default 300).
#
#   ./flash.sh                    # release image; build first with ./build.sh
#   VARIANT=bench ./flash.sh      # bench image; build first with BENCH=1 ./build.sh
#   BOARD=weact ./flash.sh        # bench image for the WeAct board

set -uo pipefail
FW="$(cd "$(dirname "$0")" && pwd)"
if [ -z "${VARIANT:-}" ]; then
  [ "${BOARD:-}" = "weact" ] && VARIANT=bench || VARIANT=release
fi
case "$VARIANT" in
  release|bench) ;;
  *) echo "[ERROR] VARIANT is '$VARIANT'; use release or bench"; exit 1 ;;
esac
BIN="/tmp/cubevox-fw-$VARIANT/cubevox.ino.bin"
WAIT="${WAIT:-300}"

command -v dfu-util >/dev/null || { echo "[ERROR] dfu-util not installed (brew install dfu-util)"; exit 1; }
if [ ! -f "$BIN" ]; then
  [ "$VARIANT" = bench ] && hint="BENCH=1 ./build.sh" || hint="./build.sh"
  echo "[ERROR] $BIN missing; run $hint first"
  exit 1
fi
echo "[flash] $VARIANT image"

have_port() { ls /dev/ | grep -q '^cu\.usbmodem'; }
in_dfu() { dfu-util -l 2>/dev/null | grep -q "0483:df11"; }

if ! in_dfu && have_port; then
  echo "[flash] asking the box to enter DFU"
  "$FW/term.py" -t 2 -s '{"id":1,"cmd":"enter_dfu"}' >/dev/null 2>&1
fi

for ((i = 0; i < WAIT; i++)); do
  in_dfu && break
  [ "$i" -eq 0 ] && echo "[flash] waiting for the ST bootloader (hold BOOT0, tap NRST, release BOOT0)"
  sleep 1
done
in_dfu || { echo "[ERROR] no DFU device after ${WAIT}s"; exit 1; }

# The leave request resets the chip before it answers, so dfu-util reports a get_status error.
dfu-util -a 0 -s 0x08000000:leave -D "$BIN" 2>&1 | grep -E "Erase|downloaded|Error" | grep -v get_status
for ((i = 0; i < 30; i++)); do
  have_port && break
  sleep 1
done
have_port || { echo "[ERROR] box did not enumerate after the flash"; exit 1; }
sleep 2
exec "$FW/term.py" -t 3 -s '{"id":2,"cmd":"bench","op":"report"}'
