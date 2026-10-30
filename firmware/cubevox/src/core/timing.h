// Time constants shared by the switch, mute and relay logic, in ms.

#pragma once

#include <stdint.h>

// Every mechanical switch (mute, menu click, slot toggles) must hold a level this long to count.
constexpr uint32_t kSwitchDebounceMs = 20;

// Covers the G6K-2F-Y operate time. Both relays (K102 mute, K103 input jack) wait this long.
constexpr uint32_t kRelaySettleMs = 20;

// XSMT goes low this long before MUTE_N goes low so the DAC is silent when the relay opens, and goes
// high this long before MUTE_N goes high so its unmute step is over before the relay closes. It
// exceeds the 3.4 ms the DAC needs at 48 kHz.
constexpr uint32_t kXsmtLeadMs = 5;

// The output relay closes no earlier than this long after the app started, counted from setup(). The
// output buffer's bias reaches the jack side of the 10 uF coupling capacitor at power-on and decays
// through 100 kOhm to ground, a 1 s time constant that takes about 5.5 s to fall under 10 mV.
// Closing the relay sooner puts the remainder on the tip. The bootloader's time is not counted, so
// the capacitor has had longer. With the board's next revision (10 kOhm to ground) 1000 ms is enough.
constexpr uint32_t kRelayCloseAfterPowerOnMs = 6000;

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
