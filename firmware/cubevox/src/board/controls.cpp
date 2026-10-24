#include "board/controls.h"

#include <Arduino.h>

#include <array>

#include "board/pins.h"
#include "board/slots.h"
#include "core/debounce.h"
#include "core/knob_filter.h"
#include "core/registry.h"
#include "core/timing.h"

namespace {

constexpr int kAdcBits = 12;
constexpr float kAdcFull = 4095.0f;
constexpr bool kToggleOnWhenLow = true;  // throw-to-GND position engages the effect

// The 100 R / 1 nF at the ADC pin and the mux source impedance need at least 0.5 ms after a
// select change before the sample.
constexpr uint32_t kMuxSettleUs = 500;
// U107 holds 3V3A off for up to 1030 ms after power-on; a high select before that pushes
// ~20 mA through U101's input clamp into the dead rail. Selects stay low until this passes.
constexpr uint32_t kMuxSelectHoldMs = 1100;

constexpr int kMuxSelectBits = 4;
constexpr std::array<uint32_t, kMuxSelectBits> kMuxSelectPins = {pins::kMuxS0, pins::kMuxS1, pins::kMuxS2,
                                                                 pins::kMuxS3};
static_assert(kPotCount <= (1 << kMuxSelectBits), "the mux select pins must address every pot");

KnobBank gKnobs;
std::array<DebouncedInput, kSlotCount> gToggles;
bool gToggleChanged = false;
#if CUBEVOX_BENCH
std::array<float, kPotCount> gRaw;  // last unfiltered read per mux channel
#endif

uint8_t gScanChannel = 0;
bool gSelected = false;
uint32_t gSelectedAtUs = 0;

void selectChannel(uint8_t channel) {
  for (int bit = 0; bit < kMuxSelectBits; ++bit) digitalWrite(kMuxSelectPins[bit], (channel >> bit) & 1);
}

// Blocks for one conversion: the sample window set by ADC_SAMPLINGTIME in build.sh (810.5 ADC
// cycles) plus the 12-bit conversion, at the ADC clock the core configures (the AHB clock / 4).
// The mux settle wait is separate and does not block.
float readPosition() {
  return knobPosition(static_cast<float>(analogRead(pins::kPotMuxOut)) / kAdcFull);
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
  const float position = readPosition();
#if CUBEVOX_BENCH
  gRaw[gScanChannel] = position;
#endif
  const PotOwner owner = potOwner(gScanChannel);
  gKnobs.update(gScanChannel, position, LastMoved{owner.slot, owner.knobB});
  gScanChannel = (gScanChannel + 1) % kPotCount;
  gSelected = false;
}

bool readToggleOn(int slot) {
  const bool low = digitalRead(kSlots[slot].togglePin) == LOW;
  return low == kToggleOnWhenLow;
}

void scanToggles(uint32_t nowMs) {
  for (int s = 0; s < kSlotCount; ++s) {
    if (gToggles[s].update(readToggleOn(s), nowMs, kSwitchDebounceMs)) gToggleChanged = true;
  }
}

// Step count of every pot under `layout`; 0 for a continuous knob or a channel no slot uses.
std::array<int, kPotCount> stepCounts(const Layout& layout) {
  std::array<int, kPotCount> steps{};
  for (int pot = 0; pot < kPotCount; ++pot) {
    const PotOwner owner = potOwner(pot);
    if (owner.slot >= 0) steps[pot] = knobSteps(layout.card[owner.slot], owner.knobB ? 1 : 0);
  }
  return steps;
}

}  // namespace

void controlsInit() {
  analogReadResolution(kAdcBits);
  for (uint32_t pin : kMuxSelectPins) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
    LL_GPIO_SetPinSpeed(pins::portOf(pin), pins::maskOf(pin), LL_GPIO_SPEED_FREQ_LOW);
  }
  pinMode(pins::kPotMuxOut, INPUT_ANALOG);
  for (int s = 0; s < kSlotCount; ++s) {
    pinMode(kSlots[s].togglePin, INPUT);  // pull-up is on the board
    gToggles[s].reset(readToggleOn(s));
  }
}

void controlsPoll(uint32_t nowMs, const Layout& layout) {
  gKnobs.setSteps(stepCounts(layout));
  scanMux(nowMs);
  scanToggles(nowMs);
}

bool controlsSeeded() { return gKnobs.seeded(); }

bool controlsTakeChanged() {
  const bool changed = gKnobs.takeChanged() || gToggleChanged;
  gToggleChanged = false;
  return changed;
}

bool controlsTakeTouch(LastMoved& out) { return gKnobs.takeTouch(out); }

#if CUBEVOX_BENCH
float controlsRawPot(int muxChannel) { return gRaw[muxChannel]; }

bool controlsToggleOn(int slot) { return gToggles[slot].level(); }
#endif

PanelReading controlsReading() {
  PanelReading r;
  for (int s = 0; s < kSlotCount; ++s) {
    const std::array<int, kKnobsPerCard> pots = {kSlots[s].muxChannelA, kSlots[s].muxChannelB};
    for (int k = 0; k < kKnobsPerCard; ++k) r.knob[s][k] = gKnobs.value(pots[k]);
    r.toggle[s] = gToggles[s].level();
  }
  r.lastMoved = gKnobs.lastMoved();
  return r;
}
