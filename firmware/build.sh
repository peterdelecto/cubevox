#!/usr/bin/env bash
# Compile the cubevox firmware for the H743 with arduino-cli. Never flashes.
# Core, FQBN and flags follow Teensy-H7-Port/test/build_fxbox_h7.sh.
#
#   ./build.sh              # build into /tmp/cubevox-fw

set -uo pipefail
FW="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$FW/.." && pwd)"
OUT=/tmp/cubevox-fw
FQBN="STMicroelectronics:stm32:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen"

command -v arduino-cli >/dev/null || { echo "[ERROR] arduino-cli not installed (brew install arduino-cli)"; exit 1; }

# Engine headers are included as "engine/x.h" from the repo root.
INC="-I$REPO -I$FW/platform"
# The pot mux common on PC0 is the only analogRead; take the longest sample window (spec item 10.8).
# The box is self-powered: the USB configuration descriptor says so (spec 2026-10-08 step 0).
DEFS="-DADC_SAMPLINGTIME=ADC_SAMPLETIME_810CYCLES_5 -DUSBD_SELF_POWERED=1"
# RAM_D2 (SAI rings) and DTCM sections for the H743V linker script.
LDX="-Wl,--script=$FW/platform/h7_h743v_sections.ld"

mkdir -p "$OUT"
arduino-cli compile --fqbn "$FQBN" \
  --build-path "$OUT" --warnings more \
  --build-property "build.flags.optimize=-O2" \
  --build-property "build.fpu=-mfpu=fpv5-d16" \
  --build-property "compiler.cpp.extra_flags=$INC $DEFS -ffp-contract=fast" \
  --build-property "compiler.c.extra_flags=$INC $DEFS -ffp-contract=fast" \
  --build-property "compiler.c.elf.extra_flags=$LDX" \
  "$FW/cubevox" 2>&1 | tee "$OUT.log" | tail -${TAIL:-40}
exit "${PIPESTATUS[0]}"
