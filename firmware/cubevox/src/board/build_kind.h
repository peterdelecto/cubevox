// Which image this is. build.sh passes -DCUBEVOX_BENCH for the bench image; the release image
// leaves it out, and the code behind it is not compiled.

#pragma once

#ifndef CUBEVOX_BENCH
#define CUBEVOX_BENCH 0
#endif

constexpr bool kBenchBuild = CUBEVOX_BENCH != 0;
