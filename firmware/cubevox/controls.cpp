#include "controls.h"

#include <Arduino.h>
#include <math.h>

#include "pins.h"
#include "registry.h"

namespace {

constexpr int kAdcBits = 12;
constexpr float kAdcFull = 4095.0f;
constexpr float kSmoothing = 0.3f;      // one-pole step per scan visit
constexpr float kMoveThreshold = 0.015f;  // knob travel that counts as touched
constexpr float kEndMargin = 0.01f;     // dead band at each end of pot travel
constexpr bool kToggleOnWhenLow = true;  // throw-to-GND position engages the effect

constexpr uint32_t kMuxSelectPins[4] = {pins::kMuxS0, pins::kMuxS1, pins::kMuxS2, pins::kMuxS3};

struct PotState {
  float smoothed = 0.0f;
  float reported = 0.0f;  // value at the last "moved" report
  int step = 0;           // stepped knobs only
  bool seeded = false;
};

PotState gPots[kPotCount];
DebouncedInput gToggles[kSlotCount];
LastMoved gLastMoved;

uint8_t gScanChannel = 0;
bool gSelected = false;
uint32_t gSelectedAtUs = 0;
int gSeededCount = 0;
bool gChanged = false;

// Reverse lookup from pot index to the slot knob that owns it.
struct PotOwner {
  int8_t slot;
  bool knobB;
};

PotOwner ownerOf(int pot) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (kSlots[s].muxChannelA == pot) return {static_cast<int8_t>(s), false};
    if (kSlots[s].muxChannelB == pot) return {static_cast<int8_t>(s), true};
  }
  return {-1, false};
}

void selectChannel(uint8_t channel) {
  for (int bit = 0; bit < 4; ++bit) digitalWrite(kMuxSelectPins[bit], (channel >> bit) & 1);
}

float readNorm() {
  const float raw = static_cast<float>(analogRead(pins::kPotMuxOut)) / kAdcFull;
  const float n = (raw - kEndMargin) / (1.0f - 2.0f * kEndMargin);
  return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
}

void noteMoved(int pot, uint32_t nowMs) {
  const PotOwner owner = ownerOf(pot);
  if (owner.slot < 0) return;
  gLastMoved = {owner.slot, owner.knobB, nowMs};
  gChanged = true;
}

void updatePot(int pot, float norm, uint32_t nowMs) {
  PotState& p = gPots[pot];
  if (!p.seeded) {
    p.smoothed = norm;
    p.reported = norm;
    p.seeded = true;
    const PotOwner owner = ownerOf(pot);
    const int steps = owner.slot < 0 ? 0 : knobSteps(slotCard(owner.slot), owner.knobB ? 1 : 0);
    if (steps > 0) p.step = steppedPosition(norm, steps, 0, 0.0f);
    if (++gSeededCount == kPotCount) gChanged = true;
    return;
  }
  p.smoothed += kSmoothing * (norm - p.smoothed);

  const PotOwner owner = ownerOf(pot);
  const int steps = owner.slot < 0 ? 0 : knobSteps(slotCard(owner.slot), owner.knobB ? 1 : 0);
  if (steps > 0) {
    const int next = steppedPosition(p.smoothed, steps, p.step);
    if (next == p.step) return;
    p.step = next;
    noteMoved(pot, nowMs);
  } else if (fabsf(p.smoothed - p.reported) > kMoveThreshold) {
    p.reported = p.smoothed;
    noteMoved(pot, nowMs);
  }
}

void scanMux(uint32_t nowMs) {
  if (nowMs < kMuxSelectHoldMs) return;  // selects stay low until 3V3A is up
  if (!gSelected) {
    selectChannel(gScanChannel);
    gSelectedAtUs = micros();
    gSelected = true;
    return;
  }
  if (micros() - gSelectedAtUs < kMuxSettleUs) return;
  updatePot(gScanChannel, readNorm(), nowMs);
  gScanChannel = (gScanChannel + 1) % kPotCount;
  gSelected = false;
}

bool readToggleOn(int slot) {
  const bool low = digitalRead(kSlots[slot].togglePin) == LOW;
  return low == kToggleOnWhenLow;
}

void scanToggles(uint32_t nowMs) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (gToggles[s].update(readToggleOn(s), nowMs, kToggleDebounceMs)) gChanged = true;
  }
}

}  // namespace

bool DebouncedInput::update(bool raw, uint32_t nowMs, uint32_t holdMs) {
  if (raw != candidate_) {
    candidate_ = raw;
    candidateSinceMs_ = nowMs;
    return false;
  }
  if (candidate_ == stable_ || nowMs - candidateSinceMs_ < holdMs) return false;
  stable_ = candidate_;
  return true;
}

int steppedPosition(float norm, int positions, int current, float hysteresis) {
  const float band = 1.0f / static_cast<float>(positions);
  int pos = static_cast<int>(norm * static_cast<float>(positions));
  if (pos >= positions) pos = positions - 1;
  if (pos < 0) pos = 0;
  if (pos == current) return current;

  const float lo = static_cast<float>(current) * band - hysteresis * band;
  const float hi = static_cast<float>(current + 1) * band + hysteresis * band;
  return (norm >= lo && norm <= hi) ? current : pos;
}

void controlsInit() {
  analogReadResolution(kAdcBits);
  for (uint32_t pin : kMuxSelectPins) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
    const PinName pn = digitalPinToPinName(pin);
    LL_GPIO_SetPinSpeed(get_GPIO_Port(STM_PORT(pn)), STM_LL_GPIO_PIN(pn), LL_GPIO_SPEED_FREQ_LOW);
  }
  pinMode(pins::kPotMuxOut, INPUT_ANALOG);
  for (int s = 0; s < kSlotCount; ++s) {
    pinMode(kSlots[s].togglePin, INPUT);  // pull-up is on the board
    gToggles[s].reset(readToggleOn(s));
  }
}

void controlsPoll(uint32_t nowMs) {
  scanMux(nowMs);
  scanToggles(nowMs);
}

bool controlsTakeChanged() {
  const bool changed = gChanged;
  gChanged = false;
  return changed;
}

float controlsKnobValue(int slot, bool knobB) {
  const int pot = knobB ? kSlots[slot].muxChannelB : kSlots[slot].muxChannelA;
  const bool stepped = knobSteps(slotCard(slot), knobB ? 1 : 0) > 0;
  return stepped ? static_cast<float>(gPots[pot].step) : gPots[pot].smoothed;
}

bool controlsToggleOn(int slot) { return gToggles[slot].level(); }

LastMoved controlsLastMoved() { return gLastMoved; }

void controlsApply(ChainParams& out) {
  for (int s = 0; s < kSlotCount; ++s) {
    const Card card = slotCard(s);
    applyPlacement(card, out);
    applyKnob(card, 0, controlsKnobValue(s, false), out);
    applyKnob(card, 1, controlsKnobValue(s, true), out);
    applyToggle(card, gToggles[s].level(), out);
  }
}
