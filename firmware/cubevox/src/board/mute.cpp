#include "board/mute.h"

#include <Arduino.h>

#include "board/pins.h"
#include "core/timing.h"

namespace {

MuteSequence gSequence;
MuteOutputs gDriven;  // pin levels last written

bool switchReleased() { return digitalRead(pins::kMuteSw) == HIGH; }

}  // namespace

void muteInit() {
  digitalWrite(pins::kMuteN, LOW);
  pinMode(pins::kMuteN, OUTPUT);
  digitalWrite(pins::kXsmt, LOW);
  pinMode(pins::kXsmt, OUTPUT);
  pinMode(pins::kMuteSw, INPUT);  // R120 pull-up on the board
  gSequence.reset(switchReleased(), millis());
  gDriven = MuteOutputs();
}

void muteClocksRunning(uint32_t nowMs) { gSequence.clocksRunning(nowMs); }

void mutePoll(uint32_t nowMs) {
  const MuteOutputs next = gSequence.update(nowMs, switchReleased());
  // Each sequence step changes one pin, so the write order here cannot reorder the mute.
  if (next.muteNHigh != gDriven.muteNHigh) digitalWrite(pins::kMuteN, next.muteNHigh ? HIGH : LOW);
  if (next.xsmtHigh != gDriven.xsmtHigh) digitalWrite(pins::kXsmt, next.xsmtHigh ? HIGH : LOW);
  gDriven = next;
}

void muteLatch() { gSequence.latch(); }

bool muteActive() { return gSequence.active(); }

bool muteFullyMuted() { return gSequence.fullyMuted(); }

void muteHold(bool on) { gSequence.hold(on); }

void muteForReset() {
  digitalWrite(pins::kXsmt, LOW);
  delay(kXsmtLeadMs);
  digitalWrite(pins::kMuteN, LOW);
  delay(kRelaySettleMs);
}
