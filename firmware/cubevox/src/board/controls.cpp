#include "board/controls.h"

#include <array>

#include "board/adc_setup.h"
#include "board/pins.h"
#include "board/serial_link.h"
#include "board/slots.h"
#include "core/debounce.h"
#include "core/knob_filter.h"
#include "core/registry.h"
#include "core/timing.h"

namespace {

constexpr float kAdcFull = static_cast<float>(kAdcSetupFullScale);
constexpr bool kToggleOnWhenLow = true;  // throw-to-GND position engages the effect

// The 100 R / 1 nF at the ADC pin and the mux source impedance need at least 0.5 ms after a
// select change before the sample.
constexpr uint32_t kMuxSettleUs = 500;
// U107 holds 3V3A off for up to 1030 ms after power-on; a high select before that pushes
// ~20 mA through U101's input clamp into the dead rail. Selects stay low until this passes.
constexpr uint32_t kMuxSelectHoldMs = 1100;

// The longest sample window (hardware spec item 10): 810.5 cycles and 12.5 to convert, about
// 82 us at the 10 MHz ADC clock. A conversion still running after kConversionTimeoutUs is dropped.
constexpr uint32_t kSampleTime = ADC_SAMPLETIME_810CYCLES_5;
constexpr uint32_t kConversionTimeoutUs = 1000;
constexpr uint32_t kDropReportMs = 1000;  // at most one dropped-sample line this often

constexpr int kMuxSelectBits = 4;
constexpr std::array<uint32_t, kMuxSelectBits> kMuxSelectPins = {pins::kMuxS0, pins::kMuxS1, pins::kMuxS2,
                                                                 pins::kMuxS3};
static_assert(kPotCount <= (1 << kMuxSelectBits), "the mux select pins must address every pot");

ADC_HandleTypeDef gAdc;  // ADC3; ADC1 and ADC2 belong to board/power_sense
const char* gAdcError = "not set up";  // the ADC3 step that failed; null once it runs
uint32_t gDropped = 0;
Every gDropReport{kDropReportMs};

KnobBank gKnobs;
std::array<DebouncedInput, kSlotCount> gToggles;
bool gToggleChanged = false;
#if CUBEVOX_BENCH
std::array<float, kPotCount> gRaw;  // last unfiltered read per mux channel
#endif

// One pot's turn: select its mux channel, start the conversion once the mux has settled, read it.
enum class Scan : uint8_t { Select, Convert, Read };
Scan gScan = Scan::Select;
uint8_t gScanChannel = 0;
uint32_t gStepAtUs = 0;

void selectChannel(uint8_t channel) {
  for (int bit = 0; bit < kMuxSelectBits; ++bit) digitalWrite(kMuxSelectPins[bit], (channel >> bit) & 1);
}

void nextChannel() {
  gScanChannel = (gScanChannel + 1) % kPotCount;
  gScan = Scan::Select;
}

// A failed conversion skips its pot for this round rather than reading as a knob at its stop. The
// stop leaves ADC3 ready for the next start.
void dropSample(uint32_t nowMs, const char* why) {
  HAL_ADC_Stop(&gAdc);
  ++gDropped;
  if (gDropReport.due(nowMs)) {
    serialOut().print("[WARN] pots: ADC3 conversion ");
    serialOut().print(why);
    serialOut().print(" on mux channel ");
    serialOut().print(gScanChannel);
    serialOut().print(", sample dropped, ");
    serialOut().print(gDropped);
    serialOut().println(" since boot");
  }
  nextChannel();
}

void startConversion(uint32_t nowMs) {
  if (micros() - gStepAtUs < kMuxSettleUs) return;
  if (HAL_ADC_Start(&gAdc) != HAL_OK) {
    dropSample(nowMs, "would not start");
    return;
  }
  gStepAtUs = micros();
  gScan = Scan::Read;
}

void readConversion(uint32_t nowMs) {
  if (!__HAL_ADC_GET_FLAG(&gAdc, ADC_FLAG_EOC)) {
    if (micros() - gStepAtUs >= kConversionTimeoutUs) dropSample(nowMs, "timed out");
    return;
  }
  const float position = knobPosition(static_cast<float>(HAL_ADC_GetValue(&gAdc)) / kAdcFull);
#if CUBEVOX_BENCH
  gRaw[gScanChannel] = position;
#endif
  const PotOwner owner = potOwner(gScanChannel);
  gKnobs.update(gScanChannel, position, LastMoved{owner.slot, owner.knobB}, nowMs);
  nextChannel();
}

void scanMux(uint32_t nowMs) {
  if (nowMs < kMuxSelectHoldMs || gAdcError != nullptr) return;  // selects stay low until 3V3A is up
  switch (gScan) {
    case Scan::Select:
      selectChannel(gScanChannel);
      gStepAtUs = micros();
      gScan = Scan::Convert;
      break;
    case Scan::Convert: startConversion(nowMs); break;
    case Scan::Read: readConversion(nowMs); break;
  }
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
  for (uint32_t pin : kMuxSelectPins) {
    digitalWrite(pin, LOW);
    pinMode(pin, OUTPUT);
    LL_GPIO_SetPinSpeed(pins::portOf(pin), pins::maskOf(pin), LL_GPIO_SPEED_FREQ_LOW);
  }
  pinMode(pins::kPotMuxOut, INPUT_ANALOG);
  gAdcError = adcSetup(gAdc, ADC3, pins::kPotMuxAdcChannel, kSampleTime, false);
  for (int s = 0; s < kSlotCount; ++s) {
    pinMode(kSlots[s].togglePin, INPUT);  // pull-up is on the board
    gToggles[s].reset(readToggleOn(s));
  }
}

void controlsReport(Print& out) {
  if (gAdcError != nullptr) {
    out.print("[ERROR] pots: ADC3 ");
    out.print(gAdcError);
    out.println(", the panel stays unread and the output muted");
    return;
  }
  out.print("[boot] pots: ADC3, ");
  out.print(gDropped);
  out.println(" samples dropped");
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
