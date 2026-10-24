#!/usr/bin/env bash
# Compile the cubevox firmware for the H743 with arduino-cli. Never flashes.
# The image links at the application start (0x08020000), behind the bootloader's sector. A stamp
# step then fills the length and CRC of its header and writes cubevox.uf2 next to the .bin.
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
# The compiler the core installs. bootloader/build.sh pins the same version, so one toolchain serves both.
XPACK_VERSION="14.2.1-1.1"
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
gcc="$ARDUINO_DATA/packages/STMicroelectronics/tools/xpack-arm-none-eabi-gcc/$XPACK_VERSION/bin/arm-none-eabi-gcc"
if [ ! -x "$gcc" ]; then
  echo "[ERROR] xpack-arm-none-eabi-gcc $XPACK_VERSION missing at $(dirname "$gcc")"
  echo "        arduino-cli core install $CORE@$CORE_VERSION installs it"
  exit 1
fi
echo "[build] $("$gcc" --version | head -1)"

# The release name is v<VERSION.txt>. The image header, hello, the OLED and the boot report show
# this one string, and the update drive reads it back from the header. The git state only goes in
# the log.
VERSION_TXT="$(head -n 1 "$REPO/VERSION.txt")"
if ! [[ "$VERSION_TXT" =~ ^[0-9]+\.[0-9][0-9]\.[0-9][0-9]$ ]]; then
  echo "[ERROR] VERSION.txt is '$VERSION_TXT'; it must look like 2.00.10"
  exit 1
fi
VERSION="v$VERSION_TXT"
GIT_STATE="$(git -C "$REPO" describe --tags --always --dirty 2>/dev/null || echo no-git)"
echo "[build] core $CORE $CORE_VERSION, firmware $VERSION (git $GIT_STATE)"

# The addresses the image and the bootloader agree on come from the header both read.
BOOT_BLOCK_H="$FW/cubevox/src/board/boot_block.h"
cv_define() {  # integer value of a #define in the contract header, without its u suffix
  awk -v name="$1" '$1 == "#define" && $2 == name { sub(/[uUlL]+$/, "", $3); print $3; exit }' "$BOOT_BLOCK_H"
}
APP_START="$(cv_define CV_APP_START)"
APP_END="$(cv_define CV_APP_END)"
if [ -z "$APP_START" ] || [ -z "$APP_END" ]; then
  echo "[ERROR] CV_APP_START or CV_APP_END is missing from $BOOT_BLOCK_H"
  exit 1
fi
FLASH_BASE=0x08000000
FLASH_OFFSET="$(printf '0x%X' $((APP_START - FLASH_BASE)))"
MAX_BIN=$((APP_END - APP_START))

# The Python tools carry copies of firmware constants; a drift fails the build here.
python3 -I "$REPO/tools/bench_ops_check.py"

# Engine headers are included as "engine/x.h" from the repo root; sketch sources as "core/x.h",
# "board/x.h", "ui/x.h" from cubevox/src; the font editor's export as "cubevox_fonts.h".
INC="-I$REPO -I$FW/cubevox/src -I$REPO/tools/font-editor/fonts"
# The pot mux common on PC0 is the only analogRead; take the longest sample window (spec item 10.8).
# The box is self-powered: the USB configuration descriptor says so (spec 2026-10-08 step 0).
# The core's CDC receive queue must hold every byte that arrives during one loop() pass. When it
# fills, the core arms a zero-length receive and the next packet wraps the queue, which drops or
# replays up to a queue's worth of bytes. 32 packets (2 KB) hold two 768-byte requests.
# arduino-cli hands the quotes of CUBEVOX_FW_VERSION to the compiler as they stand, so one level
# of escaping makes the macro a string literal. A second level put quote characters in the string.
DEFS="-DADC_SAMPLINGTIME=ADC_SAMPLETIME_810CYCLES_5 -DUSBD_SELF_POWERED=1 -DCDC_RECEIVE_QUEUE_BUFFER_PACKET_NUMBER=32 -DCUBEVOX_FW_VERSION=\"$VERSION\""
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
# RAM_D2 (SAI rings), DTCM, the boot block, the fault record and the image header for the H743V.
LDX="-Wl,--script=$FW/cubevox.ld"

# build.flash_offset sets the FLASH origin and -DVECT_TAB_OFFSET together, so the link address and
# the VTOR the core writes in SystemInit agree. Nothing else sets VTOR.
mkdir -p "$OUT"
if ! arduino-cli compile --fqbn "$FQBN" \
  --build-path "$OUT" --warnings more \
  --build-property "build.flash_offset=$FLASH_OFFSET" \
  --build-property "build.flags.optimize=$OPT" \
  --build-property "build.fpu=-mfpu=fpv5-d16" \
  --build-property "compiler.cpp.extra_flags=$INC $INC_CLIP $DEFS -ffp-contract=fast" \
  --build-property "compiler.c.extra_flags=$INC $DEFS -ffp-contract=fast" \
  --build-property "compiler.c.elf.extra_flags=$LDX" \
  "$FW/cubevox" 2>&1 | tee "$OUT.log" | tail -n "${TAIL:-40}"; then
  echo "[ERROR] arduino-cli compile failed; full log $OUT.log"
  exit 1
fi

# The bootloader owns the first sector of bank 1 and the layout record lives in bank 2, so the
# image gets what lies between. The linker script asserts the image end; this guards the shipped file.
BIN="$OUT/cubevox.ino.bin"
if [ ! -f "$BIN" ]; then
  echo "[ERROR] $BIN missing after a successful compile"
  exit 1
fi
size="$(wc -c < "$BIN")"
if [ "$size" -eq 0 ] || [ "$size" -gt "$MAX_BIN" ]; then
  echo "[ERROR] cubevox.ino.bin is $size bytes; it must be 1..$MAX_BIN (flash bank 1 after the bootloader's sector, short of bank 2)"
  exit 1
fi

# cubevox.ld cannot include the header, so it carries copies of the contract addresses. Compare
# the linked symbols with the header.
NM="$(dirname "$gcc")/arm-none-eabi-nm"
SYMBOLS="$("$NM" "$OUT/cubevox.ino.elf")"
check_symbol_address() {  # symbol, expected address
  local got
  got="$(awk -v s="$1" '$3 == s { print "0x" $1; exit }' <<<"$SYMBOLS")"
  if [ -z "$got" ] || [ $((got)) -ne $(($2)) ]; then
    echo "[ERROR] $1 is at ${got:-nowhere} in the elf; boot_block.h puts it at $(printf '0x%08X' $(($2))), so fix firmware/cubevox.ld"
    exit 1
  fi
}
check_symbol_address gBootBlock "$(cv_define CV_BOOT_BLOCK_ADDR)"
check_symbol_address __cv_noinit_start "$(cv_define CV_FAULT_RECORD_ADDR)"
check_symbol_address kImageHeader "$((APP_START + $(cv_define CV_IMAGE_HEADER_OFFSET)))"

# Fill the header's length and CRC in place, write the update file next to the .bin, and read the
# update file back the way the bootloader will.
FAMILY=cubevox
if [ "${BOARD:-}" = "weact" ]; then FAMILY=weact; fi
STAMP=(python3 -I "$REPO/tools/stamp_image.py")
"${STAMP[@]}" stamp "$BIN"
"${STAMP[@]}" uf2 "$BIN" "$OUT/cubevox.uf2" --family "$FAMILY"
uf2_check="$("${STAMP[@]}" check "$OUT/cubevox.uf2")"
echo "[build] cubevox.uf2 $uf2_check"

# Warnings gate: our sources again with -Wall -Wextra -Werror, third-party headers as system headers.
python3 -I "$FW/warn_gate.py" "$OUT" --tail "${TAIL:-40}" \
  --system-dir "$ARDUINO_DATA" --system-dir "$ARDUINO_USER/libraries"
