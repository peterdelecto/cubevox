# Sourced by flash.sh and bootloader/flash.sh. dfu_unexpected_errors LOG prints the lines of a
# dfu-util log that name an error the flash scripts must not ignore, or nothing when there are none.

# dfu-util ends a transfer that resets or leaves with a get_status error, so those lines are skipped.
# dfu-util 0.11 prints "No error condition is present" as its normal status, so that line is skipped.
dfu_unexpected_errors() {
  awk 'tolower($0) ~ /error/ && $0 !~ /get_status/ && $0 !~ /No error condition/' "$1"
}
