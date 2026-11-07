# Sourced by the firmware and bootloader scripts and by board.mk. cv_define NAME [FILE] prints the
# integer value of a #define in FILE without its integer suffix, or nothing when the macro is
# absent. FILE defaults to the contract header board/boot_block.h, the one place the addresses and
# sizes live.

CV_BOOT_BLOCK_H="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/cubevox/src/board/boot_block.h"

cv_define() {
  awk -v name="$1" '$1 == "#define" && $2 == name { sub(/[uUlL]+$/, "", $3); print $3; exit }' "${2:-$CV_BOOT_BLOCK_H}"
}
