#!/usr/bin/env bash
# Compile the cubevox firmware for the H743 with arduino-cli. Never flashes.
# Core, FQBN and flags follow Teensy-H7-Port/test/build_fxbox_h7.sh.
#
#   ./build.sh              # build into /tmp/cubevox-fw
#   TAIL=5 ./build.sh       # show only the last 5 lines of compiler output
#   BOARD=weact ./build.sh  # bench build for the WeAct H743 (no VBUS divider: USB always on)
#   OPT=-O3 EXTRA_FLAGS=-ffast-math ./build.sh   # bench experiments; the release build is -O2

set -uo pipefail
FW="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$FW/.." && pwd)"
OUT=/tmp/cubevox-fw
CORE="STMicroelectronics:stm32"
CORE_VERSION="${CORE_VERSION:-3.0.0}"  # env override exists only so the check itself can be tested
FQBN="$CORE:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen"

command -v arduino-cli >/dev/null || { echo "[ERROR] arduino-cli not installed (brew install arduino-cli)"; exit 1; }

# The core is pinned: a different version changes the HAL, the USB stack and the linker defaults.
installed="$(arduino-cli core list 2>/dev/null | awk -v c="$CORE" '$1 == c {print $2}')"
if [ "$installed" != "$CORE_VERSION" ]; then
  echo "[ERROR] core $CORE is '${installed:-not installed}', build needs $CORE_VERSION"
  echo "        arduino-cli core install $CORE@$CORE_VERSION"
  exit 1
fi
gcc="$(ls -d "$HOME"/Library/Arduino15/packages/STMicroelectronics/tools/xpack-arm-none-eabi-gcc/*/bin/arm-none-eabi-gcc 2>/dev/null | head -1)"
[ -n "$gcc" ] && echo "[build] $("$gcc" --version | head -1)"

# Version string from the git tag; "dev" when the build is not from a checkout.
VERSION="$(git -C "$REPO" describe --tags --always --dirty 2>/dev/null || echo dev)"
echo "[build] core $CORE $CORE_VERSION, firmware $VERSION"

# Engine headers are included as "engine/x.h" from the repo root; sketch sources as "core/x.h",
# "board/x.h", "ui/x.h" from cubevox/src; the copied platform files by bare name.
INC="-I$REPO -I$FW/cubevox/src -I$FW/platform"
# The pot mux common on PC0 is the only analogRead; take the longest sample window (spec item 10.8).
# The box is self-powered: the USB configuration descriptor says so (spec 2026-10-08 step 0).
DEFS="-DADC_SAMPLINGTIME=ADC_SAMPLETIME_810CYCLES_5 -DUSBD_SELF_POWERED=1 -DCUBEVOX_FW_VERSION=\"\\\"$VERSION\\\"\""
if [ "${BOARD:-}" = "weact" ]; then
  DEFS="$DEFS -DCUBEVOX_VBUS_ALWAYS"
  echo "[build] bench board weact: USB attach not gated on VBUS"
fi
OPT="${OPT:--O2}"
DEFS="$DEFS ${EXTRA_FLAGS:-}"
echo "[build] $OPT ${EXTRA_FLAGS:-}"

# Bench clip: a WAV baked into flash that the 'v' command loops in place of the SAI input.
# The weact build takes Adam's dry vocal from samples/ (untracked; the repo is public).
CLIP="${CLIP:-}"
[ "${BOARD:-}" = "weact" ] && [ -z "$CLIP" ] && [ -f "$REPO/samples/adam_vox_sample2_4s.wav" ] &&
  CLIP="$REPO/samples/adam_vox_sample2_4s.wav"
INC_CLIP=""
if [ -n "$CLIP" ]; then
  mkdir -p "$OUT-gen"
  python3 "$FW/make_clip.py" "$CLIP" "$OUT-gen/bench_clip.h" || exit 1
  INC_CLIP="-I$OUT-gen"
  DEFS="$DEFS -DCUBEVOX_BENCH_CLIP"
fi
# RAM_D2 (SAI rings) and DTCM sections for the H743V linker script.
LDX="-Wl,--script=$FW/platform/h7_h743v_sections.ld"

mkdir -p "$OUT"
arduino-cli compile --fqbn "$FQBN" \
  --build-path "$OUT" --warnings more \
  --build-property "build.flags.optimize=$OPT" \
  --build-property "build.fpu=-mfpu=fpv5-d16" \
  --build-property "compiler.cpp.extra_flags=$INC $INC_CLIP $DEFS -ffp-contract=fast" \
  --build-property "compiler.c.extra_flags=$INC $DEFS -ffp-contract=fast" \
  --build-property "compiler.c.elf.extra_flags=$LDX" \
  "$FW/cubevox" 2>&1 | tee "$OUT.log" | tail -${TAIL:-40}
exit "${PIPESTATUS[0]}"
