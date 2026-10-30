#include "board/mute.h"

#include <Arduino.h>

#include <atomic>

#include "board/pins.h"
#include "core/debounce.h"
#include "core/mute_sequence.h"
#include "core/timing.h"

namespace {

MuteSequence gSequence;
uint8_t gHolds = 0;              // MuteReason bits held from loop()
std::atomic<uint8_t> gTrips{0};  // MuteReason bits tripped from interrupts, cleared from loop()

// The switch as the BYPASS role sees it. The sequence debounces its own copy of the same pin.
DebouncedInput gBypassSwitch;
volatile bool gBypassOn = false;  // written by mutePoll, read by the audio interrupt
// Written by mutePoll and muteTrip, read by the audio interrupt. It starts true so the first block
// is already at zero.
volatile bool gOutputHeld = true;

bool switchReleased() { return digitalRead(pins::kMuteSw) == HIGH; }

uint8_t bitOf(MuteReason reason) { return static_cast<uint8_t>(reason); }

// By register: no HAL state, so an interrupt can use it while loop() is mid-call.
void setPin(uint32_t pin, bool high) {
  pins::portOf(pin)->BSRR = high ? pins::maskOf(pin) : pins::maskOf(pin) << 16;
}

// A trip can land between the sequence step and this write, so the write runs with interrupts off
// and a trip set by then keeps both pins low. Each sequence step changes one pin, so writing both
// cannot reorder the mute.
void drive(MuteOutputs next) {
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (gTrips.load() != 0) next = MuteOutputs();
  setPin(pins::kXsmt, next.xsmtHigh);
  setPin(pins::kMuteN, next.muteNHigh);
  __set_PRIMASK(primask);
}

}  // namespace

void muteInit() {
  digitalWrite(pins::kMuteN, LOW);
  pinMode(pins::kMuteN, OUTPUT);
  digitalWrite(pins::kXsmt, LOW);
  pinMode(pins::kXsmt, OUTPUT);
  pinMode(pins::kMuteSw, INPUT);  // R120 pull-up on the board
  gSequence.reset(switchReleased(), millis());
  gBypassSwitch.reset(switchReleased());
  gHolds = 0;
  gTrips.store(0);
  gOutputHeld = true;
}

void muteClocksRunning() { gSequence.clocksRunning(); }

bool muteOutputHeld() { return gOutputHeld; }

void mutePoll(uint32_t nowMs, TopSwitch role) {
  const bool released = switchReleased();
  gBypassSwitch.update(released, nowMs, kSwitchDebounceMs);
  gBypassOn = role == TopSwitch::Bypass && !gBypassSwitch.level();

  gSequence.hold(gHolds != 0);
  gSequence.trip(gTrips.load() != 0);
  // BYPASS leaves the sequence a switch that never mutes; MUTE hands it the pin as it always was.
  drive(gSequence.update(nowMs, role == TopSwitch::Mute ? released : true));
  gOutputHeld = gSequence.outputHeld() || gTrips.load() != 0;  // a trip landing mid-pass still holds
}

void muteLatch() { gSequence.latch(); }

bool muteBypassOn() { return gBypassOn; }

bool muteActive() { return gSequence.active(); }

bool muteFullyMuted() { return gSequence.fullyMuted(); }

void muteHold(bool on) { muteHoldFor(MuteReason::Panel, on); }

void muteHoldFor(MuteReason reason, bool on) {
  gHolds = static_cast<uint8_t>(on ? gHolds | bitOf(reason) : gHolds & ~bitOf(reason));
}

void muteTrip(MuteReason reason) {
  setPin(pins::kXsmt, false);
  setPin(pins::kMuteN, false);
  gTrips.fetch_or(bitOf(reason));
  gOutputHeld = true;
}

void muteTripClear(MuteReason reason) { gTrips.fetch_and(static_cast<uint8_t>(~bitOf(reason))); }

// The polled sequence cannot finish before a reset, so this blocks through the same order and
// delays: XSMT low, then MUTE_N low once the DAC is silent.
void muteForReset() {
  setPin(pins::kXsmt, false);
  delay(kXsmtLeadMs);
  setPin(pins::kMuteN, false);
  delay(kRelaySettleMs);
}
