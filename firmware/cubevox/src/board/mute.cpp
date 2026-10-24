#include "board/mute.h"

#include <Arduino.h>

#include "board/pins.h"
#include "core/debounce.h"
#include "core/mute_sequence.h"
#include "core/timing.h"

namespace {

MuteSequence gSequence;
MuteOutputs gDriven;  // pin levels last written

// The switch as the BYPASS role sees it. The sequence debounces its own copy of the same pin.
DebouncedInput gBypassSwitch;
volatile bool gBypassOn = false;  // written by mutePoll, read by the audio interrupt

bool switchReleased() { return digitalRead(pins::kMuteSw) == HIGH; }

}  // namespace

void muteInit() {
  digitalWrite(pins::kMuteN, LOW);
  pinMode(pins::kMuteN, OUTPUT);
  digitalWrite(pins::kXsmt, LOW);
  pinMode(pins::kXsmt, OUTPUT);
  pinMode(pins::kMuteSw, INPUT);  // R120 pull-up on the board
  gSequence.reset(switchReleased(), millis());
  gBypassSwitch.reset(switchReleased());
  gDriven = MuteOutputs();
}

void muteClocksRunning(uint32_t nowMs) { gSequence.clocksRunning(nowMs); }

void mutePoll(uint32_t nowMs, TopSwitch role) {
  const bool released = switchReleased();
  gBypassSwitch.update(released, nowMs, kSwitchDebounceMs);
  gBypassOn = role == TopSwitch::Bypass && !gBypassSwitch.level();

  // BYPASS leaves the sequence a switch that never mutes; MUTE hands it the pin as it always was.
  const MuteOutputs next = gSequence.update(nowMs, role == TopSwitch::Mute ? released : true);
  // Each sequence step changes one pin, so the write order here cannot reorder the mute.
  if (next.muteNHigh != gDriven.muteNHigh) digitalWrite(pins::kMuteN, next.muteNHigh ? HIGH : LOW);
  if (next.xsmtHigh != gDriven.xsmtHigh) digitalWrite(pins::kXsmt, next.xsmtHigh ? HIGH : LOW);
  gDriven = next;
}

void muteLatch() { gSequence.latch(); }

bool muteBypassOn() { return gBypassOn; }

bool muteActive() { return gSequence.active(); }

bool muteFullyMuted() { return gSequence.fullyMuted(); }

void muteHold(bool on) { gSequence.hold(on); }

// The polled sequence cannot finish before a reset, so this blocks through the same order and
// delays: XSMT low, then MUTE_N low once the DAC is silent. gDriven follows the pins, so a later
// mutePoll would write the right difference.
void muteForReset() {
  digitalWrite(pins::kXsmt, LOW);
  delay(kXsmtLeadMs);
  digitalWrite(pins::kMuteN, LOW);
  delay(kRelaySettleMs);
  gDriven = MuteOutputs();
}
