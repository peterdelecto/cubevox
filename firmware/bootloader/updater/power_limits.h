// The 5VA levels the updater checks before it touches sector 0, from the application's
// core/power_fail.h through power_limits.cpp.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The ADC resolution the updater reads VA_SENSE at.
#define CV_RAIL_BITS 12

// The lowest reading that counts as power present, in ADC counts.
extern const uint32_t cvRailMinCounts;

// 5VA in mV for a reading.
uint32_t cvRailMv(uint32_t counts);

#ifdef __cplusplus
}
#endif
