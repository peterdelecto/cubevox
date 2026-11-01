// The host's millis(): milliseconds since the first call, from a steady clock.

#pragma once

#include <chrono>
#include <cstdint>

namespace host {

inline uint32_t millis() {
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

}  // namespace host
