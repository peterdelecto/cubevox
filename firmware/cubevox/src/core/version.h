// Firmware version string. build.sh and the host test build pass CUBEVOX_FW_VERSION as "v" plus
// VERSION.txt; a plain compile without it reads "dev".

#pragma once

#ifndef CUBEVOX_FW_VERSION
#define CUBEVOX_FW_VERSION "dev"
#endif

constexpr const char* kFirmwareVersion = CUBEVOX_FW_VERSION;
