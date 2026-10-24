// Firmware version string. build.sh and the host test build pass CUBEVOX_FW_VERSION as "v" plus
// VERSION.txt; a plain compile without it reads "dev".

#pragma once

#include <stddef.h>

#ifndef CUBEVOX_FW_VERSION
#define CUBEVOX_FW_VERSION "dev"
#endif

constexpr const char* kFirmwareVersion = CUBEVOX_FW_VERSION;

// Copies the tag part of a `git describe` string (up to the first '-') into out, clipped to size - 1.
// "v2.00.04-3-gabc123-dirty" gives "v2.00.04".
inline void versionTag(const char* version, char* out, size_t size) {
  size_t n = 0;
  while (n + 1 < size && version[n] != '\0' && version[n] != '-') {
    out[n] = version[n];
    ++n;
  }
  if (size > 0) out[n] = '\0';
}
