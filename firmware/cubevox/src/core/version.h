// Firmware version string. build.sh passes CUBEVOX_FW_VERSION from `git describe`;
// a plain compile without it reads "dev".

#pragma once

#ifndef CUBEVOX_FW_VERSION
#define CUBEVOX_FW_VERSION "dev"
#endif

constexpr const char* kFirmwareVersion = CUBEVOX_FW_VERSION;
