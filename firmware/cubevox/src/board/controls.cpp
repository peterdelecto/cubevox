#include "board/controls.h"

#include <Arduino.h>

#include "board/pins.h"
#include "board/slots.h"
#include "core/registry.h"

namespace {

constexpr int kAdcBits = 12;
constexpr float kAdcFull = 4095.0f;
constexpr float kEndMargin = 0.01f;     // dead band at each end of pot travel
constexpr bool kToggleOnWhenLow = true;  // throw-to-GND position engages the effect

constexpr uint32_t kMuxSelectPins[4] = {pins::kMuxS0, pins::kMuxS1, pins::kMuxS2, pins::kMuxS3};

KnobFilter gKnobs[kPotCount];
DebouncedInput gToggles[kSlotCount];
LastMoved gLastMoved;
Layout gLayout = defaultLayout();

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

int stepsFor(int pot) {
  const PotOwner owner = ownerOf(pot);
  return owner.slot < 0 ? 0 : knobSteps(gLayout.card[owner.slot], owner.knobB ? 1 : 0);
}

void updatePot(int pot, float norm, uint32_t nowMs) {
  switch (gKnobs[pot].update(norm, stepsFor(pot))) {
    case KnobEvent::Seeded:
      if (++gSeededCount == kPotCount) gChanged = true;
      break;
    case KnobEvent::Reseeded:
      gChanged = true;
      break;
    case KnobEvent::Moved:
      noteMoved(pot, nowMs);
      break;
    case KnobEvent::None:
      break;
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

void controlsPoll(uint32_t nowMs, const Layout& layout) {
  gLayout = layout;
  scanMux(nowMs);
  scanToggles(nowMs);
}

bool controlsTakeChanged() {
  const bool changed = gChanged;
  gChanged = false;
  return changed;
}

PanelReading controlsReading() {
  PanelReading r;
  for (int s = 0; s < kSlotCount; ++s) {
    const int pots[kKnobsPerCard] = {kSlots[s].muxChannelA, kSlots[s].muxChannelB};
    for (int k = 0; k < kKnobsPerCard; ++k) {
      r.knob[s][k] = gKnobs[pots[k]].value();
    }
    r.toggle[s] = gToggles[s].level();
  }
  r.lastMoved = gLastMoved;
  return r;
}
