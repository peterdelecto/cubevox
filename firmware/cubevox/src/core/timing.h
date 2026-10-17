// Time constants shared by the switch, mute and relay logic, in ms.

#pragma once

#include <stdint.h>

// Every mechanical switch (mute, menu click, slot toggles) must hold a level this long to count.
constexpr uint32_t kSwitchDebounceMs = 20;

// Covers the G6K-2F-Y operate time. Both relays (K102 mute, K103 input jack) wait this long.
constexpr uint32_t kRelaySettleMs = 20;

// XSMT goes low this long before MUTE_N so the DAC is silent when the relay opens. It exceeds the
// 3.4 ms the DAC needs at 48 kHz.
constexpr uint32_t kXsmtLeadMs = 5;

// The output stays muted this long after the SAI clocks start, so the DAC settles first.
constexpr uint32_t kBootUnmuteDelayMs = 500;

// A periodic job in a polled loop. due() is true once per period; the difference is unsigned, so it
// survives the millisecond counter wrapping.
class Every {
 public:
  explicit constexpr Every(uint32_t periodMs) : periodMs_(periodMs) {}

  bool due(uint32_t nowMs) {
    if (nowMs - lastMs_ < periodMs_) return false;
    lastMs_ = nowMs;
    return true;
  }

 private:
  uint32_t periodMs_;
  uint32_t lastMs_ = 0;
};
