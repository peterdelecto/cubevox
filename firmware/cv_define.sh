# Sourced by the firmware and bootloader scripts. cv_define NAME prints the integer value of a
# #define in the contract header board/boot_block.h without its u suffix, or nothing when the
# macro is absent. The header is the one place the addresses and sizes live.

CV_BOOT_BLOCK_H="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/cubevox/src/board/boot_block.h"

cv_define() {
  awk -v name="$1" '$1 == "#define" && $2 == name { sub(/[uUlL]+$/, "", $3); print $3; exit }' "$CV_BOOT_BLOCK_H"
}
