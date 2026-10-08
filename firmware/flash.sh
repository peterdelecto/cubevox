#!/usr/bin/env bash
# Flash /tmp/cubevox-fw/cubevox.ino.bin over DFU and print the boot report.
#
# If a cubevox CDC port is present the box is asked to enter DFU itself (bench command 'd').
# Otherwise hold BOOT0, tap NRST, release BOOT0; the script waits up to WAIT seconds (default 300).
#
#   ./flash.sh            # build first with ./build.sh or BOARD=weact ./build.sh

set -uo pipefail
FW="$(cd "$(dirname "$0")" && pwd)"
BIN=/tmp/cubevox-fw/cubevox.ino.bin
WAIT="${WAIT:-300}"

command -v dfu-util >/dev/null || { echo "[ERROR] dfu-util not installed (brew install dfu-util)"; exit 1; }
[ -f "$BIN" ] || { echo "[ERROR] $BIN missing; run build.sh first"; exit 1; }

have_port() { ls /dev/ | grep -q '^cu\.usbmodem'; }
in_dfu() { dfu-util -l 2>/dev/null | grep -q "0483:df11"; }

if ! in_dfu && have_port; then
  echo "[flash] asking the box to enter DFU"
  "$FW/term.py" -t 2 -s d >/dev/null 2>&1
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
exec "$FW/term.py" -t 3 -s r
