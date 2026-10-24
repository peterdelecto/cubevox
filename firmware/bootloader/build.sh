#!/usr/bin/env bash
# Builds the cubevox TinyUF2 bootloader: adafruit/tinyuf2 at a pinned commit, patches/, and the
# boards in boards/. Never flashes; flash.sh writes it through the ROM DFU (shop only).
#
#   ./build.sh               # cubevox board into /tmp/cubevox-boot-cubevox
#   BOARD=weact ./build.sh   # WeAct H743 test board into /tmp/cubevox-boot-weact
#   ./build.sh --outdir      # print the output dir for the current BOARD and exit
#   ./build.sh --proofs      # host build of screens.c; numbered PNGs and diag-sample.txt in build/proofs/boot/
#   ./build.sh --selftest    # host run of the image check against tools/stamp_image.py, the screen rules and the drive texts
#
# The checkout is build/tinyuf2 (gitignored). Every build resets it to the pin, links
# boards/cubevox and boards/weact into ports/stm32h7/boards/ as symlinks and applies patches/ in
# order, so local edits there do not survive a build.

set -euo pipefail
BL="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$BL/../.." && pwd)"

TINYUF2_URL="https://github.com/adafruit/tinyuf2.git"
TINYUF2_COMMIT="29d9ce231bada513a7200c087f10a91c832aec42"
# The compiler the Arduino core installs for the firmware.
XPACK_VERSION="14.2.1-1.1"
# Bank 1 sector 0, the bootloader's whole space.
BIN_LIMIT=131072

BOARD="${BOARD:-cubevox}"
case "$BOARD" in
  cubevox|weact) ;;
  *) echo "[ERROR] BOARD is '$BOARD'; use cubevox or weact"; exit 1 ;;
esac
OUT="/tmp/cubevox-boot-$BOARD"
TU="$BL/build/tinyuf2"
HOST="$BL/build/host"

MODE=board
case "${1:-}" in
  "") ;;
  --outdir) echo "$OUT"; exit 0 ;;
  --proofs) MODE=proofs ;;
  --selftest) MODE=selftest ;;
  *) echo "[ERROR] unknown argument '$1'; use --outdir, --proofs or --selftest"; exit 1 ;;
esac

# One build at a time: every board shares the checkout and the host build. The lock dir holds the
# owner's PID; a lock whose owner is gone is reaped under a second mkdir, so two waiters cannot
# both remove it.
mkdir -p "$BL/build"
if ! git -C "$REPO" check-ignore -q "$BL/build"; then
  echo "[ERROR] $BL/build is not gitignored; the checkout must stay out of the repository"
  exit 1
fi
LOCK="$BL/build/.lock"
held=0
release_lock() {
  if [ "$held" -eq 1 ]; then rm -f "$LOCK/pid"; rmdir "$LOCK"; held=0; fi
}
trap release_lock EXIT
trap 'exit 130' INT TERM

reap_stale_lock() {
  mkdir "$LOCK.reap" 2>/dev/null || return 0
  local owner
  owner="$(cat "$LOCK/pid" 2>/dev/null || true)"
  if [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; then
    echo "[build] removing stale lock $LOCK (pid $owner is gone)"
    rm -f "$LOCK/pid"
    rmdir "$LOCK"
  fi
  rmdir "$LOCK.reap"
}

waited=0
until mkdir "$LOCK" 2>/dev/null; do
  reap_stale_lock
  if [ "$waited" -ge 600 ]; then
    echo "[ERROR] another bootloader build held $LOCK for 600 s"
    exit 1
  fi
  if [ "$waited" -eq 0 ]; then echo "[build] waiting for the running bootloader build ($LOCK)"; fi
  sleep 2
  waited=$((waited + 2))
done
held=1
echo $$ > "$LOCK/pid"

# Runs a command with its output in a log; on failure prints the tail and stops.
run_logged() {
  local log="$1"
  shift
  if ! "$@" > "$log" 2>&1; then
    tail -n 40 "$log"
    echo "[ERROR] $1 failed; full log $log"
    exit 1
  fi
}

host_tools() {
  mkdir -p "$HOST"
  run_logged "$HOST/configure.log" cmake -S "$BL/tools" -B "$HOST"
  run_logged "$HOST/build.log" cmake --build "$HOST" -j
}

proofs() {
  host_tools
  local dir="$REPO/build/proofs/boot"
  mkdir -p "$dir"
  rm -f "$dir"/*.png
  "$HOST/proof" "$dir"
  echo "[proofs] $(find "$dir" -name '*.png' | wc -l | tr -d ' ') screens in $dir"

  # DIAG.TXT of the sample box, its flash log read by diag.c from a synthetic sector
  mkdir -p "$HOST/drive"
  "$HOST/drivetext" "$HOST/drive" > /dev/null
  cp "$HOST/drive/DIAG.TXT" "$dir/diag-sample.txt"
  echo "[proofs] diag-sample.txt in $dir"
}

expect_check() {
  local got
  got="$("$HOST/imagecheck" check "$1")"
  if [ "$got" != "$2" ]; then
    echo "[ERROR] image check on $(basename "$1") says '$got', expected '$2'"
    exit 1
  fi
  echo "[selftest] $(basename "$1"): $got"
}

# The bootloader's CRC and header check against the stamp tool: a stamped image passes both, a
# flipped byte fails both, an unstamped image and an erased region are refused. Then the screens
# against the approved wording and the owner's row rules, and the drive texts.
selftest() {
  host_tools
  local dir="$BL/build/selftest"
  local stamp=(python3 -I "$REPO/tools/stamp_image.py")
  mkdir -p "$dir"
  "$HOST/imagecheck" fake "$dir/unstamped.bin" 200000
  run_logged "$dir/stamp.log" "${stamp[@]}" stamp "$dir/unstamped.bin" --out "$dir/stamped.bin"
  "$HOST/imagecheck" flip "$dir/stamped.bin" "$dir/flipped.bin" 4096
  : > "$dir/erased.bin"
  run_logged "$dir/check.log" "${stamp[@]}" check "$dir/stamped.bin"
  if "${stamp[@]}" check "$dir/flipped.bin" > "$dir/check-flipped.log" 2>&1; then
    echo "[ERROR] stamp_image.py accepts the flipped image"
    exit 1
  fi
  expect_check "$dir/stamped.bin" "good v2.00.10"
  expect_check "$dir/flipped.bin" "bad v2.00.10"
  expect_check "$dir/unstamped.bin" "bad v2.00.10"
  expect_check "$dir/erased.bin" "none"
  echo "[selftest] PASS: image_check.c agrees with tools/stamp_image.py"
  "$HOST/proof" --check
  drive_texts "$dir"
}

expect_line() {
  if ! grep -qF -- "$2" "$1"; then
    echo "[ERROR] $(basename "$1") lacks '$2'"
    exit 1
  fi
}

expect_diag_size() {
  local size
  size=$(($(wc -c < "$1")))
  if [ "$size" -ne 2048 ] || [ "$(tail -c 2 "$1" | od -An -c | tr -d ' ')" != '\r\n' ]; then
    echo "[ERROR] $(basename "$1") is $size bytes or lacks its closing CR LF; it must be 2048"
    exit 1
  fi
}

# The sequence numbers of a DIAG file's log lines, newest first, as "#9 #8 #7". Empty without lines.
log_order() {
  grep -o '^#[0-9]*' "$1" | paste -sd' ' - || true
}

expect_log_order() {
  local got
  got="$(log_order "$1")"
  if [ "$got" != "$2" ]; then
    echo "[ERROR] $(basename "$1") lists '$got', expected '$2'"
    exit 1
  fi
}

# diag.c's files for a sample box. Every DIAG file keeps its fixed size. DIAG.TXT carries every field
# and a flash log of three valid entries, one torn and blank slots. The others cover an empty log, a
# log in a faulted sector that must not be read, a full one (newest 16 of 40) and a crowded one
# (the longest lines, so fewer than 16 fit).
drive_texts() {
  local dir="$1" diag info name newest crowded first
  "$HOST/drivetext" "$dir" > /dev/null
  diag="$dir/DIAG.TXT"
  info="$dir/INFO_UF2.TXT"
  for name in DIAG DIAG-empty DIAG-full DIAG-crowded DIAG-unreadable; do expect_diag_size "$dir/$name.TXT"; done
  expect_line "$diag" "reason: ImageUnreadable"
  expect_line "$diag" "fault address: 0x8045000"
  expect_line "$diag" "app summary: v2.00.10 up 1234 s software none"
  expect_line "$diag" "fault: bus pc=0x8021A4C lr=0x8020F31 cfsr=0x8200 hfsr=0x40000000 bfar=0xDEADBEE0 mmfar=0xDEADBEE0 bank2-ecc=0x8102460"
  expect_line "$diag" "refused blocks: 12, last family 0x43565741"
  expect_line "$diag" "#9 boot v2.00.10 up=? rst=0x1000000 fails=0 AppRequest fa=0x0 software none sai=0 ovr=0"
  expect_line "$diag" "#8 update v2.00.10 up=1234s rst=0x1000000 fails=0 none fa=0x0 software none sai=2 ovr=0"
  expect_line "$diag" "#7 boot v2.00.09 up=? rst=0x4000000 fails=1 none fa=0x0 watchdog bus sai=0 ovr=3"
  expect_line "$diag" "flash log: 3 valid, 1 torn, 508 blank"
  expect_log_order "$diag" "#9 #8 #7"
  expect_line "$info" "Board-ID: cubevox-h743-v4"
  expect_line "$info" "Serial: 0123456789ABCDEF01234567"
  expect_line "$info" "Installed: v2.00.10"

  expect_line "$dir/DIAG-empty.TXT" "flash log: empty"
  expect_log_order "$dir/DIAG-empty.TXT" ""
  expect_line "$dir/DIAG-unreadable.TXT" "reason: MenuHeld"
  expect_line "$dir/DIAG-unreadable.TXT" "flash log: unreadable at 0x8141230"
  expect_log_order "$dir/DIAG-unreadable.TXT" ""
  newest="$(printf '#%s ' $(seq 139 -1 124))"
  expect_line "$dir/DIAG-full.TXT" "flash log: 40 valid, 0 torn, 472 blank"
  expect_log_order "$dir/DIAG-full.TXT" "${newest% }"
  expect_line "$dir/DIAG-crowded.TXT" "flash log: 40 valid, 0 torn, 472 blank"
  crowded=$(grep -c '^#' "$dir/DIAG-crowded.TXT" || true)
  first="$(log_order "$dir/DIAG-crowded.TXT" | cut -d' ' -f1)"
  if [ "$crowded" -lt 8 ] || [ "$crowded" -gt 16 ] || [ "$first" != '#1039' ]; then
    echo "[ERROR] DIAG-crowded.TXT lists $crowded lines from '$first'; expected 8 to 16 from #1039"
    exit 1
  fi
  echo "[selftest] PASS: DIAG files 2048 bytes; full log lists 16 of 40, crowded log $crowded of 16; INFO_UF2.TXT $(($(wc -c < "$info"))) bytes"
}

checkout() {
  if [ ! -d "$TU/.git" ]; then
    echo "[build] cloning adafruit/tinyuf2 into $TU"
    git clone --quiet "$TINYUF2_URL" "$TU"
  fi
  if [ "$(git -C "$TU" rev-parse HEAD)" != "$TINYUF2_COMMIT" ]; then
    git -C "$TU" fetch --quiet origin "$TINYUF2_COMMIT"
    git -C "$TU" checkout --quiet --force "$TINYUF2_COMMIT"
  fi
  # Drops the last build's patches. Untracked files, the board links and _build, stay.
  git -C "$TU" reset --quiet --hard "$TINYUF2_COMMIT"
  local head
  head="$(git -C "$TU" rev-parse HEAD)"
  if [ "$head" != "$TINYUF2_COMMIT" ]; then
    echo "[ERROR] $TU is at $head, the pin is $TINYUF2_COMMIT"
    exit 1
  fi
  echo "[build] tinyuf2 $(git -C "$TU" describe --always --tags) at the pin"
}

# Only what ports/stm32h7/port.mk uses: TinyUSB (submodule), the H7 HAL and CMSIS device (get_deps).
# lib/CMSIS_5 is part of the tinyuf2 tree.
dependencies() {
  run_logged "$BL/build/submodule.log" git -C "$TU" submodule update --init --depth 1 lib/tinyusb
  run_logged "$BL/build/get-deps.log" python3 -I "$TU/tools/get_deps.py" stm32h7
  local dep
  for dep in lib/mcu/st/stm32h7xx_hal_driver lib/mcu/st/cmsis_device_h7; do
    if [ ! -d "$TU/$dep/.git" ]; then
      echo "[ERROR] $dep missing after get_deps.py; log $BL/build/get-deps.log"
      exit 1
    fi
  done
}

patch_tree() {
  local board patch
  for board in cubevox weact; do ln -sfn "$BL/boards/$board" "$TU/ports/stm32h7/boards/$board"; done
  for patch in "$BL"/patches/*.patch; do
    if ! git -C "$TU" apply --check "$patch"; then
      echo "[ERROR] $(basename "$patch") does not apply to tinyuf2 $TINYUF2_COMMIT"
      exit 1
    fi
    git -C "$TU" apply "$patch"
    echo "[build] applied $(basename "$patch")"
  done
}

toolchain() {
  if ! command -v arduino-cli >/dev/null; then
    echo "[ERROR] arduino-cli not installed; it holds xpack-arm-none-eabi-gcc $XPACK_VERSION"
    exit 1
  fi
  TC="$(arduino-cli config get directories.data)/packages/STMicroelectronics/tools/xpack-arm-none-eabi-gcc/$XPACK_VERSION/bin"
  if [ ! -x "$TC/arm-none-eabi-gcc" ]; then
    echo "[ERROR] xpack-arm-none-eabi-gcc $XPACK_VERSION missing at $TC"
    echo "        arduino-cli core install STMicroelectronics:stm32@3.0.0 installs it"
    exit 1
  fi
  echo "[build] $("$TC/arm-none-eabi-gcc" --version | head -1)"
}

build_board() {
  local port="$TU/ports/stm32h7"
  local make_args=(-C "$port" BOARD="$BOARD" CROSS_COMPILE="$TC/arm-none-eabi-")
  # A cross build takes no host flags from the environment.
  unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS
  run_logged "$OUT.clean.log" make "${make_args[@]}" clean
  run_logged "$OUT.log" make "${make_args[@]}" -j"$(getconf _NPROCESSORS_ONLN)" all
  grep -A4 "Memory region" "$OUT.log" || true  # the linker's own region report, when present

  mkdir -p "$OUT"
  cp "$port/_build/$BOARD/tinyuf2-$BOARD.elf" "$OUT/cubevox-bootloader.elf"
  cp "$port/_build/$BOARD/tinyuf2-$BOARD.bin" "$OUT/cubevox-bootloader.bin"
  local size
  size=$(($(wc -c < "$OUT/cubevox-bootloader.bin")))
  if [ "$size" -eq 0 ] || [ "$size" -gt "$BIN_LIMIT" ]; then
    echo "[ERROR] cubevox-bootloader.bin is $size bytes; it must be 1..$BIN_LIMIT (bank 1 sector 0)"
    exit 1
  fi
  echo "[build] cubevox-bootloader.bin $size bytes of $BIN_LIMIT"
  "$TC/arm-none-eabi-size" "$OUT/cubevox-bootloader.elf"
  echo "[build] $OUT"
}

case "$MODE" in
  proofs) proofs ;;
  selftest) selftest ;;
  board)
    echo "[build] board $BOARD"
    checkout
    dependencies
    patch_tree
    toolchain
    build_board
    ;;
esac
