#!/usr/bin/env bash
# Compile the cubevox firmware for the H743 with arduino-cli. Never flashes.
# Core, FQBN and flags follow Teensy-H7-Port/test/build_fxbox_h7.sh.
#
#   ./build.sh              # release image into /tmp/cubevox-fw-release
#   BENCH=1 ./build.sh      # bench image (-DCUBEVOX_BENCH: drills and panel test) into /tmp/cubevox-fw-bench
#   TAIL=5 ./build.sh       # show only the last 5 lines of compiler output
#   BOARD=weact ./build.sh  # bench image for the WeAct H743 (no VBUS divider: USB always on)
#   OPT=-O3 EXTRA_FLAGS=-ffast-math ./build.sh   # experiments; the release build is -O2
#   ./build.sh --variant    # print release or bench for the current environment and exit
#   ./build.sh --outdir     # print the output dir for the current environment and exit
#
# VARIANT=release|bench names the variant outright; BENCH=1 and BOARD=weact imply bench. This
# script is the one place that rule lives; flash.sh and bringup.py ask it.
# Each variant has its own output dir and a lock, so two builds of one variant queue up.

set -euo pipefail
FW="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$FW/.." && pwd)"

implied=release
if [ "${BENCH:-}" = 1 ] || [ "${BOARD:-}" = "weact" ]; then implied=bench; fi
VARIANT="${VARIANT:-$implied}"
case "$VARIANT" in
  release|bench) ;;
  *) echo "[ERROR] VARIANT is '$VARIANT'; use release or bench"; exit 1 ;;
esac
if [ "$implied" = bench ] && [ "$VARIANT" != bench ]; then
  echo "[ERROR] VARIANT=$VARIANT conflicts with BENCH=1 / BOARD=weact, which need the bench image"
  exit 1
fi
OUT="/tmp/cubevox-fw-$VARIANT"

case "${1:-}" in
  "") ;;
  --variant) echo "$VARIANT"; exit 0 ;;
  --outdir) echo "$OUT"; exit 0 ;;
  *) echo "[ERROR] unknown argument '$1'; use --variant or --outdir"; exit 1 ;;
esac

# A second build of the same variant waits for the first; the lock is released on every exit path.
# The lock dir holds the owner's PID. A lock whose owner is dead is reaped under a second mkdir
# mutex, so two waiters cannot both remove it and the second then remove a lock the first re-created.
LOCK="$OUT.lock"
LOCK_WAIT_S="${LOCK_WAIT_S:-600}"  # env override exists so the wait can be tested
held=0
reaping=0

release_locks() {
  if [ "$reaping" -eq 1 ]; then rmdir "$LOCK.reap"; reaping=0; fi
  if [ "$held" -eq 1 ]; then rm -f "$LOCK/pid"; rmdir "$LOCK"; held=0; fi
}
trap release_locks EXIT
trap 'exit 130' INT TERM

# Sets reaped=1 when it removed a stale lock. Called as a plain command so set -e applies inside.
reaped=0
reap_stale_lock() {
  reaped=0
  mkdir "$LOCK.reap" 2>/dev/null || return 0  # another waiter is reaping
  reaping=1
  local owner
  owner="$(cat "$LOCK/pid" 2>/dev/null || true)"  # absent while the owner is still writing it
  if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
    echo "[build] removing stale lock $LOCK (pid $owner is gone)"
    rm -f "$LOCK/pid"
    rmdir "$LOCK"
    reaped=1
  fi
  rmdir "$LOCK.reap"
  reaping=0
}

waited=0
while true; do
  if mkerr="$(mkdir "$LOCK" 2>&1)"; then break; fi
  if [ ! -d "$LOCK" ]; then
    echo "[ERROR] cannot create lock $LOCK: $mkerr"
    exit 1
  fi
  reap_stale_lock
  if [ "$reaped" -eq 1 ]; then continue; fi
  owner="$(cat "$LOCK/pid" 2>/dev/null || true)"
  if [ "$waited" -ge "$LOCK_WAIT_S" ]; then
    echo "[ERROR] a $VARIANT build (pid ${owner:-unknown}) held $LOCK for $LOCK_WAIT_S s"
    exit 1
  fi
  if [ "$waited" -eq 0 ]; then
    echo "[build] waiting for the running $VARIANT build (pid ${owner:-unknown}, lock $LOCK)"
  fi
  sleep 2
  waited=$((waited + 2))
done
held=1
echo $$ > "$LOCK/pid"

CORE="STMicroelectronics:stm32"
CORE_VERSION="${CORE_VERSION:-3.0.0}"  # env override exists only so the check itself can be tested
FQBN="$CORE:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen"

if ! command -v arduino-cli >/dev/null; then
  echo "[ERROR] arduino-cli not installed (brew install arduino-cli)"
  exit 1
fi

# The core is pinned: a different version changes the HAL, the USB stack and the linker defaults.
# A failing arduino-cli stops the script with its own message instead of reading as "not installed".
core_list="$(arduino-cli core list)"
installed="$(awk -v c="$CORE" '$1 == c {print $2}' <<<"$core_list")"
if [ "$installed" != "$CORE_VERSION" ]; then
  echo "[ERROR] core $CORE is '${installed:-not installed}', build needs $CORE_VERSION"
  echo "        arduino-cli core install $CORE@$CORE_VERSION"
  exit 1
fi

# The CLI knows where its packages and libraries live, so nothing here hard-codes a platform path.
ARDUINO_DATA="$(arduino-cli config get directories.data)"
ARDUINO_USER="$(arduino-cli config get directories.user)"
gcc=""
for candidate in "$ARDUINO_DATA"/packages/STMicroelectronics/tools/xpack-arm-none-eabi-gcc/*/bin/arm-none-eabi-gcc; do
  if [ -x "$candidate" ]; then gcc="$candidate"; break; fi
done
if [ -n "$gcc" ]; then echo "[build] $("$gcc" --version | head -1)"; fi

# Version string from the git tag; "dev" when the build is not from a checkout.
VERSION="$(git -C "$REPO" describe --tags --always --dirty 2>/dev/null || echo dev)"
echo "[build] core $CORE $CORE_VERSION, firmware $VERSION"

# The Python tools carry copies of firmware constants; a drift fails the build here.
python3 -I "$REPO/tools/bench_ops_check.py"

# Engine headers are included as "engine/x.h" from the repo root; sketch sources as "core/x.h",
# "board/x.h", "ui/x.h" from cubevox/src; the copied platform files by bare name; the font
# editor's export as "cubevox_fonts.h".
INC="-I$REPO -I$FW/cubevox/src -I$FW/platform -I$REPO/tools/font-editor/fonts"
# The pot mux common on PC0 is the only analogRead; take the longest sample window (spec item 10.8).
# The box is self-powered: the USB configuration descriptor says so (spec 2026-10-08 step 0).
# The core's CDC receive queue must hold every byte that arrives during one loop() pass. When it
# fills, the core arms a zero-length receive and the next packet wraps the queue, which drops or
# replays up to a queue's worth of bytes. 32 packets (2 KB) hold two 768-byte requests.
DEFS="-DADC_SAMPLINGTIME=ADC_SAMPLETIME_810CYCLES_5 -DUSBD_SELF_POWERED=1 -DCDC_RECEIVE_QUEUE_BUFFER_PACKET_NUMBER=32 -DCUBEVOX_FW_VERSION=\"\\\"$VERSION\\\"\""
if [ "${BOARD:-}" = "weact" ]; then
  DEFS="$DEFS -DCUBEVOX_VBUS_ALWAYS"
  echo "[build] bench board weact: USB attach not gated on VBUS"
fi
if [ "$VARIANT" = bench ]; then DEFS="$DEFS -DCUBEVOX_BENCH"; fi
OPT="${OPT:--O2}"
DEFS="$DEFS ${EXTRA_FLAGS:-}"
echo "[build] $VARIANT image, $OPT ${EXTRA_FLAGS:-}"

# Bench clip: a WAV baked into flash that the clip op loops in place of the SAI input.
# Only the bench image carries it, so a release image never holds the sample.
# The weact build takes Adam's dry vocal from samples/ (untracked; the repo is public).
CLIP="${CLIP:-}"
if [ "$VARIANT" != bench ]; then CLIP=""; fi
WEACT_CLIP="$REPO/samples/adam_vox_sample2_4s.wav"
if [ "${BOARD:-}" = "weact" ] && [ -z "$CLIP" ] && [ -f "$WEACT_CLIP" ]; then CLIP="$WEACT_CLIP"; fi
INC_CLIP=""
if [ -n "$CLIP" ]; then
  mkdir -p "$OUT-gen"
  python3 -I "$FW/make_clip.py" "$CLIP" "$OUT-gen/bench_clip.h"
  INC_CLIP="-I$OUT-gen"
  DEFS="$DEFS -DCUBEVOX_BENCH_CLIP"
fi
# RAM_D2 (SAI rings) and DTCM sections for the H743V linker script, then the fault record.
LDX="-Wl,--script=$FW/platform/h7_h743v_sections.ld -Wl,--script=$FW/cubevox_noinit.ld"

mkdir -p "$OUT"
if ! arduino-cli compile --fqbn "$FQBN" \
  --build-path "$OUT" --warnings more \
  --build-property "build.flags.optimize=$OPT" \
  --build-property "build.fpu=-mfpu=fpv5-d16" \
  --build-property "compiler.cpp.extra_flags=$INC $INC_CLIP $DEFS -ffp-contract=fast" \
  --build-property "compiler.c.extra_flags=$INC $DEFS -ffp-contract=fast" \
  --build-property "compiler.c.elf.extra_flags=$LDX" \
  "$FW/cubevox" 2>&1 | tee "$OUT.log" | tail -n "${TAIL:-40}"; then
  echo "[ERROR] arduino-cli compile failed; full log $OUT.log"
  exit 1
fi

# Bank 1 is 1 MB. A larger image would reach bank 2, where the layout record lives and DFU would overwrite it.
BIN="$OUT/cubevox.ino.bin"
if [ ! -f "$BIN" ]; then
  echo "[ERROR] $BIN missing after a successful compile"
  exit 1
fi
size="$(wc -c < "$BIN")"
if [ "$size" -eq 0 ] || [ "$size" -gt 1048576 ]; then
  echo "[ERROR] cubevox.ino.bin is $size bytes; it must be 1..1048576 (flash bank 1 only)"
  exit 1
fi

# Warnings gate: our sources again with -Wall -Wextra -Werror, third-party headers as system headers.
python3 -I "$FW/warn_gate.py" "$OUT" --tail "${TAIL:-40}" \
  --system-dir "$ARDUINO_DATA" --system-dir "$ARDUINO_USER/libraries" --system-dir "$FW/platform"
