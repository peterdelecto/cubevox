#!/usr/bin/env bash
# Flash <out dir>/cubevox.ino.bin over DFU and print the boot report.
#
# The image sits at 0x08020000, so the bootloader must already be in sector 0
# (firmware/bootloader/flash.sh).
#
# The variant and out dir come from build.sh (VARIANT=release|bench; BENCH=1 and BOARD=weact
# imply bench). If a cubevox CDC port is present the box is asked to enter DFU itself (the
# enter_dfu command). Otherwise hold BOOT0, tap NRST, release BOOT0; the script waits up to WAIT
# seconds (default 300). With several cu.usbmodem* ports, name one in CUBEVOX_PORT.
#
#   ./flash.sh                    # release image; build first with ./build.sh
#   VARIANT=bench ./flash.sh      # bench image; build first with BENCH=1 ./build.sh
#   BOARD=weact ./flash.sh        # bench image for the WeAct board

set -euo pipefail
FW="$(cd "$(dirname "$0")" && pwd)"
VARIANT="$("$FW/build.sh" --variant)"
BIN="$("$FW/build.sh" --outdir)/cubevox.ino.bin"
WAIT="${WAIT:-300}"
PORT_TOOL=(python3 -I "$FW/cubevox_port.py")

if ! command -v dfu-util >/dev/null; then
  echo "[ERROR] dfu-util not installed (brew install dfu-util)"
  exit 1
fi
if [ ! -f "$BIN" ]; then
  hint="./build.sh"
  if [ "$VARIANT" = bench ]; then hint="BENCH=1 ./build.sh"; fi
  echo "[ERROR] $BIN missing; run $hint first"
  exit 1
fi
echo "[flash] $VARIANT image"

in_dfu() {
  local listing
  listing="$(dfu-util -l)" || { echo "[ERROR] dfu-util -l failed"; exit 1; }
  [[ "$listing" == *0483:df11* ]]
}

# Fails with the candidates listed when several ports match and none was named.
box_port="$("${PORT_TOOL[@]}" --allow-none)"

if ! in_dfu && [ -n "$box_port" ]; then
  echo "[flash] asking the box on $box_port to enter DFU"
  if ! "${PORT_TOOL[@]}" --port "$box_port" --request enter_dfu >/dev/null; then
    echo "[flash] the box did not accept enter_dfu; enter DFU by hand with BOOT0"
  fi
fi

for ((i = 0; i < WAIT; i++)); do
  if in_dfu; then break; fi
  if [ "$i" -eq 0 ]; then echo "[flash] waiting for the ST bootloader (hold BOOT0, tap NRST, release BOOT0)"; fi
  sleep 1
done
if ! in_dfu; then
  echo "[ERROR] no DFU device after ${WAIT}s"
  exit 1
fi

# The leave request resets the chip before it answers, so dfu-util reports a get_status error
# and may exit non-zero. The flash counts as good when the download finished and no other error shows.
dfu_log="$(mktemp)"
trap 'rm -f "$dfu_log"' EXIT
dfu_rc=0
dfu-util -a 0 -s 0x08020000:leave -D "$BIN" >"$dfu_log" 2>&1 || dfu_rc=$?
grep -E "Erase|downloaded|Error" "$dfu_log" | grep -v get_status || true  # no match is a normal outcome
unexpected="$(awk 'tolower($0) ~ /error/ && $0 !~ /get_status/' "$dfu_log")"
if ! grep -q "File downloaded successfully" "$dfu_log" || [ -n "$unexpected" ]; then
  echo "[ERROR] dfu-util did not finish the download (exit $dfu_rc); output follows"
  cat "$dfu_log"
  exit 1
fi

rm -f "$dfu_log"  # exec below skips the EXIT trap

# The port name can change across the reset, so ask again.
box_port="$("${PORT_TOOL[@]}" --wait 30)"
sleep 2
exec "$FW/term.py" -t 3 -s '{"id":2,"cmd":"bench","op":"report"}' "$box_port"
