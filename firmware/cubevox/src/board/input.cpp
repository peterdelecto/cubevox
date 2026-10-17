#include "board/input.h"

#include <Arduino.h>

#include "board/pins.h"
#include "core/debounce.h"
#include "core/timing.h"
#include "ui/menu.h"

namespace {

constexpr int kCountsPerDetent = 2;  // EC11E: 15 pulses and 30 detents per turn

// Encoder decode. The ISR only updates these.
volatile int gCounts = 0;
volatile uint8_t gQuadState = 0;

DebouncedInput gClick;

void encoderIsr() {
  static const int8_t kStep[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  const uint8_t ab = static_cast<uint8_t>((digitalRead(pins::kEncMenuA) << 1) | digitalRead(pins::kEncMenuB));
  gQuadState = static_cast<uint8_t>(((gQuadState << 2) | ab) & 0x0F);
  gCounts = gCounts + kStep[gQuadState];
}

int takeDetents() {
  noInterrupts();
  const int counts = gCounts;
  const int detents = counts / kCountsPerDetent;
  gCounts = counts - detents * kCountsPerDetent;
  interrupts();
  return detents;
}

}  // namespace

void inputInit() {
  pinMode(pins::kEncMenuA, INPUT_PULLUP);
  pinMode(pins::kEncMenuB, INPUT_PULLUP);
  pinMode(pins::kEncMenuSw, INPUT_PULLUP);
  gClick.reset(digitalRead(pins::kEncMenuSw) == HIGH);
  gQuadState = static_cast<uint8_t>((digitalRead(pins::kEncMenuA) << 1) | digitalRead(pins::kEncMenuB));
  attachInterrupt(digitalPinToInterrupt(pins::kEncMenuA), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(pins::kEncMenuB), encoderIsr, CHANGE);
}

UiInput inputPoll(uint32_t nowMs) {
  UiInput in;
  in.detents = takeDetents();
  in.click = gClick.update(digitalRead(pins::kEncMenuSw) == HIGH, nowMs, kSwitchDebounceMs) && !gClick.level();
  return in;
}
