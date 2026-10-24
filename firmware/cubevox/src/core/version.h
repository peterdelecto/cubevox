// Firmware version string. build.sh and the host test build pass CUBEVOX_FW_VERSION as "v" plus
// VERSION.txt; a plain compile without it reads "dev".

#pragma once

#include <stddef.h>

#ifndef CUBEVOX_FW_VERSION
#define CUBEVOX_FW_VERSION "dev"
#endif

constexpr const char* kFirmwareVersion = CUBEVOX_FW_VERSION;

// Copies `version` up to its first '-' into `out`, clipped to size - 1. kFirmwareVersion never holds
// a '-', because build.sh admits N.NN.NN only and the -bench suffix lives in the image header alone,
// so it can be used as it is.
inline void versionTag(const char* version, char* out, size_t size) {
  size_t n = 0;
  while (n + 1 < size && version[n] != '\0' && version[n] != '-') {
    out[n] = version[n];
    ++n;
  }
  if (size > 0) out[n] = '\0';
}
