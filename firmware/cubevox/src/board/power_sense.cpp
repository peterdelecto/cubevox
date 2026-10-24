#include "board/power_sense.h"

#include <atomic>

#include "board/adc_setup.h"
#include "board/mute.h"
#include "board/pins.h"
#include "board/serial_link.h"
#include "core/power_fail.h"

namespace {

// The rails may be on their way down, so this runs ahead of every other interrupt. SysTick shares
// the level; both are a few instructions long.
constexpr uint32_t kIrqPriority = 0;

// The 100 nF at the pin supplies each sample's charge, so the window only sets the conversion rate
// and with it the load on the 5 k divider: about 40 us per reading at the 10 MHz ADC clock.
constexpr uint32_t kSampleTime = ADC_SAMPLETIME_387CYCLES_5;

// Longest wait for the first reading at init.
constexpr uint32_t kFirstReadingMs = 2;

ADC_TypeDef* adc() { return ADC1; }

ADC_HandleTypeDef gAdc;
const char* gError = "not set up";  // the ADC1 step that failed; null once it runs
std::atomic<bool> gFired{false};    // set by the watchdog interrupt, taken by powerSensePoll
PowerFailWatch gWatch;
uint32_t gTrips = 0;
uint32_t gRailMv = 0;  // the last reading

uint32_t readRailMv() { return railMv(HAL_ADC_GetValue(&gAdc), kAdcSetupBits); }

bool fail(const char* step) {
  gError = step;
  return false;
}

// The watchdog window runs from the trip level to full scale, so a reading under the trip level
// falls outside it and raises the interrupt.
bool initWatchdog() {
  ADC_AnalogWDGConfTypeDef wd = {};
  wd.WatchdogNumber = ADC_ANALOGWATCHDOG_1;
  wd.WatchdogMode = ADC_ANALOGWATCHDOG_SINGLE_REG;
  wd.Channel = pins::kVaSenseAdcChannel;
  wd.ITMode = ENABLE;
  wd.HighThreshold = kAdcSetupFullScale;
  wd.LowThreshold = railCounts(kPowerTripMv, kAdcSetupBits);
  return HAL_ADC_AnalogWDGConfig(&gAdc, &wd) == HAL_OK || fail("watchdog setup failed");
}

// Starts the conversions and waits for the first, so the first poll reads the rail and not zero.
bool startConversions() {
  HAL_NVIC_SetPriority(ADC_IRQn, kIrqPriority, 0);
  HAL_NVIC_EnableIRQ(ADC_IRQn);
  if (HAL_ADC_Start(&gAdc) != HAL_OK) return fail("would not start");
  if (HAL_ADC_PollForConversion(&gAdc, kFirstReadingMs) != HAL_OK) return fail("gave no reading");
  gRailMv = readRailMv();
  return true;
}

// A stale flag from the trip would fire at once, so it is cleared first.
void rearm() {
  adc()->ISR = ADC_ISR_AWD1;
  adc()->IER |= ADC_IER_AWD1IE;
}

void onTrip() {
  muteTrip(MuteReason::PowerFail);  // the interrupt did this already unless the poll saw the rail first
  ++gTrips;
  serialOut().print("[WARN] power: 5VA at ");
  serialOut().print(gRailMv);
  serialOut().print(" mV, under ");
  serialOut().print(kPowerTripMv);
  serialOut().println(" mV; output muted");
}

// The trip clears before the interrupt re-arms, so a new fall in between trips again at the next
// conversion instead of being cleared.
void onRelease() {
  muteTripClear(MuteReason::PowerFail);
  rearm();
  serialOut().print("[power] 5VA back at ");
  serialOut().print(gRailMv);
  serialOut().print(" mV for ");
  serialOut().print(kPowerHoldMs);
  serialOut().println(" ms; mute released");
}

}  // namespace

extern "C" void ADC_IRQHandler(void) {
  ADC_TypeDef* const a = adc();
  if ((a->ISR & ADC_ISR_AWD1) == 0) return;
  muteTrip(MuteReason::PowerFail);
  // Every conversion under the line sets the flag again, so the interrupt stays off until
  // powerSensePoll re-arms it.
  a->IER &= ~ADC_IER_AWD1IE;
  a->ISR = ADC_ISR_AWD1;
  gFired.store(true);
}

bool powerSenseInit() {
  pinMode(pins::kVaSense, INPUT_ANALOG);
  const char* setupError = adcSetup(gAdc, adc(), pins::kVaSenseAdcChannel, kSampleTime, true);
  if (setupError != nullptr) return fail(setupError);
  if (!initWatchdog() || !startConversions()) return false;
  gError = nullptr;
  return true;
}

void powerSensePoll(uint32_t nowMs) {
  if (gError != nullptr) return;
  const bool fired = gFired.exchange(false);
  gRailMv = readRailMv();
  const bool wasTripped = gWatch.tripped();
  const bool tripped = gWatch.update(nowMs, gRailMv, fired);
  if (tripped && !wasTripped) onTrip();
  if (!tripped && wasTripped) onRelease();
}

uint32_t powerSenseTrips() { return gTrips; }

void powerSenseReport(Print& out) {
  if (gError != nullptr) {
    out.print("[ERROR] power sense: ADC1 ");
    out.print(gError);
    out.println(", no power-fail mute");
    return;
  }
  out.print("[boot] power sense: 5VA ");
  out.print(readRailMv());
  out.print(" mV, mute under ");
  out.print(kPowerTripMv);
  out.print(" mV, trips ");
  out.println(gTrips);
}
