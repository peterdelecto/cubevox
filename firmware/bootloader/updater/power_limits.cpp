// The updater's one C++ file besides the fonts. core/power_fail.h is the application's C++ header
// for the 5VA levels, so this unit hands them to the C code. No value is copied.

#include "power_limits.h"

#include "../../cubevox/src/core/power_fail.h"

// The rail must read at or above the level that releases the application's power-fail mute.
extern "C" const uint32_t cvRailMinCounts = railCounts(kPowerClearMv, CV_RAIL_BITS);

extern "C" uint32_t cvRailMv(uint32_t counts) {
  return railMv(counts, CV_RAIL_BITS);
}
