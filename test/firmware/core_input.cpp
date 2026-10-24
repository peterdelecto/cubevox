// Checks the input logic on the host. Covers debouncing, knob filtering, the periodic timer,
// the publish gate, the input jack switch, the version tag and the mute sequence.

#include "support.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/debounce.h"
#include "core/jack_switch.h"
#include "core/knob_filter.h"
#include "core/loop_policy.h"
#include "core/mute_sequence.h"
#include "core/panel.h"
#include "core/settings.h"
#include "core/slots.h"
#include "core/timing.h"
#include "core/version.h"

namespace {

void checkDebounce() {
  DebouncedInput in;
  in.reset(false);
  check(!in.update(true, 100, 20), "debounce: edge not reported at once");
  check(!in.update(true, 110, 20) && !in.level(), "debounce: no change before hold time");
  check(in.update(true, 120, 20) && in.level(), "debounce: changes after hold time");
  check(!in.update(true, 200, 20), "debounce: reported once");
  in.update(false, 300, 20);
  in.update(true, 310, 20);
  check(!in.update(true, 325, 20) && in.level(), "debounce: glitch shorter than hold ignored");
}

void checkSteppedPosition() {
  for (int p = 0; p < 4; ++p) {
    const float centre = (static_cast<float>(p) + 0.5f) / 4.0f;
    check(steppedPosition(centre, 4, 0) == p, "stepped: band centre");
  }
  check(steppedPosition(0.26f, 4, 0) == 0, "stepped: hysteresis holds just past the edge");
  check(steppedPosition(0.40f, 4, 0) == 1, "stepped: moves once clear of the hysteresis");
  check(steppedPosition(0.24f, 4, 1) == 1, "stepped: hysteresis holds just below the edge");
  check(steppedPosition(-0.5f, 4, 2) == 0, "stepped: clamps at 0");
  check(steppedPosition(1.0f, 4, 0) == 3 && steppedPosition(2.0f, 4, 0) == 3, "stepped: clamps at 1");
  check(steppedPosition(NAN, 4, 2) == 0, "stepped: NaN reads as 0");
  check(steppedPosition(0.5f, 0, 0) == 0 && steppedPosition(0.5f, -3, 1) == 0, "stepped: no positions gives 0");
}

void settle(KnobFilter& f, float norm, int steps) {
  for (int i = 0; i < 60; ++i) f.update(norm, steps);
}

void checkKnobFilterContinuous() {
  KnobFilter f;
  check(f.update(0.5f, 0) == KnobEvent::Seeded, "filter: first sample seeds");
  check(std::fabs(f.value() - 0.5f) < 1e-6f, "filter: seed takes the sample");
  check(f.update(0.505f, 0) == KnobEvent::None, "filter: jitter under threshold is not a move");
  bool moved = false;
  for (int i = 0; i < 20; ++i) {
    const KnobEvent e = f.update(0.8f, 0);
    moved = moved || e == KnobEvent::Moved;
  }
  check(moved, "filter: real move reported");
  check(f.value() > 0.7f, "filter: value follows the knob");
}

void checkKnobFilterStepped() {
  KnobFilter s;
  s.update(0.1f, 5);
  check(s.value() == 0.0f, "filter: stepped seeds at the step under the knob");
  bool stepMoved = false;
  for (int i = 0; i < 30; ++i) stepMoved = stepMoved || s.update(0.3f, 5) == KnobEvent::Moved;
  check(stepMoved && s.value() == 1.0f, "filter: stepped knob moves one step");
  check(s.update(0.3f, 5) == KnobEvent::None, "filter: stepped knob rests");
  settle(s, 0.95f, 5);
  check(s.update(0.95f, 10) == KnobEvent::Reseeded, "filter: step count change re-seeds");
  check(s.value() == 9.0f, "filter: re-seed lands on the position under the knob");
  check(s.update(0.95f, 0) == KnobEvent::Reseeded && s.value() < 1.0f, "filter: back to continuous re-seeds");
}

// A re-seed restarts the move reference, so a knob parked where it re-seeded does not report a move.
void checkKnobFilterReseedReference() {
  KnobFilter f;
  f.update(0.2f, 0);
  settle(f, 0.2f, 0);
  f.update(0.9f, 5);  // the card changed to a stepped knob
  settle(f, 0.9f, 5);
  check(f.update(0.9f, 0) == KnobEvent::Reseeded, "filter: back to continuous re-seeds again");
  bool spurious = false;
  for (int i = 0; i < 20; ++i) spurious = spurious || f.update(0.9f, 0) == KnobEvent::Moved;
  check(!spurious, "filter: a parked knob reports no move after a re-seed");
}

void checkKnobPosition() {
  check(knobPosition(0.0f) == 0.0f && knobPosition(kKnobEndMargin) == 0.0f, "knob position: low dead band reads 0");
  check(knobPosition(1.0f) == 1.0f && knobPosition(1.0f - kKnobEndMargin) > 0.999f, "knob position: high dead band reads 1");
  check(std::fabs(knobPosition(0.5f) - 0.5f) < 1e-6f, "knob position: mid travel is unchanged");
  check(knobPosition(-0.2f) == 0.0f && knobPosition(1.7f) == 1.0f, "knob position: out of range clamps");
  check(knobPosition(NAN) == 0.0f, "knob position: NaN reads 0");
}

void checkKnobRestep() {
  KnobFilter f;
  check(f.restep(5) == KnobEvent::None, "restep: an unseeded filter does nothing");
  settle(f, 0.95f, 5);
  check(f.restep(5) == KnobEvent::None, "restep: an unchanged count does nothing");
  check(f.restep(10) == KnobEvent::Reseeded && f.value() == 9.0f, "restep: a new count re-derives the position");
}

// Feeds every pot once, a slot-owned pot first.
void seedBank(KnobBank& bank, float position) {
  for (int pot = 0; pot < kPotCount; ++pot) bank.update(pot, position, LastMoved{static_cast<int8_t>(pot / 2), false});
}

void checkKnobBankSeeding() {
  KnobBank bank;
  check(!bank.seeded() && !bank.takeChanged(), "bank: starts unseeded and unchanged");
  for (int pot = 0; pot < kPotCount - 1; ++pot) bank.update(pot, 0.5f, LastMoved{});
  check(!bank.seeded() && !bank.takeChanged(), "bank: not seeded until the last pot is read");
  bank.update(kPotCount - 1, 0.5f, LastMoved{});
  check(bank.seeded() && bank.takeChanged() && !bank.takeChanged(), "bank: the last pot seeds and reports once");
  bank.update(-1, 0.5f, LastMoved{});
  bank.update(kPotCount, 0.5f, LastMoved{});
  check(!bank.takeChanged(), "bank: a pot outside the panel is ignored");
}

void checkKnobBankMoves() {
  KnobBank bank;
  seedBank(bank, 0.2f);
  bank.takeChanged();
  for (int i = 0; i < 30; ++i) bank.update(5, 0.9f, LastMoved{-1, false});
  check(!bank.takeChanged() && bank.lastMoved().slot == -1, "bank: a pot no slot owns moves without a report");
  for (int i = 0; i < 30; ++i) bank.update(6, 0.9f, LastMoved{3, true});
  check(bank.takeChanged(), "bank: a slot pot move is reported");
  check(bank.lastMoved().slot == 3 && bank.lastMoved().knobB, "bank: the last moved knob is the owner");
  check(bank.value(6) > 0.8f, "bank: the value follows the pot");
}

void checkKnobBankSteps() {
  KnobBank bank;
  std::array<int, kPotCount> steps{};
  steps[2] = 5;
  bank.setSteps(steps);
  seedBank(bank, 0.9f);
  bank.takeChanged();
  check(bank.value(2) == 4.0f, "bank: a stepped pot seeds at its step");
  steps[2] = 10;
  bank.setSteps(steps);
  check(bank.takeChanged() && bank.value(2) == 9.0f, "bank: a new step count re-seeds every seeded pot at once");
  bank.setSteps(steps);
  check(!bank.takeChanged(), "bank: an unchanged count reports nothing");
}

// A touch is a move past the dead band on a slot's pot, taken once. It feeds the OLED readout.
void checkKnobBankTouch() {
  KnobBank bank;
  LastMoved touch;
  seedBank(bank, 0.2f);
  check(!bank.takeTouch(touch), "touch: seeding is no touch");
  for (int i = 0; i < 30; ++i) bank.update(5, 0.9f, LastMoved{-1, false});
  check(!bank.takeTouch(touch), "touch: a pot no slot owns is no touch");
  for (int i = 0; i < 30; ++i) bank.update(6, 0.9f, LastMoved{3, true});
  check(bank.takeTouch(touch) && touch.slot == 3 && touch.knobB, "touch: a slot pot move reports its owner");
  check(!bank.takeTouch(touch), "touch: one move reports once");
  for (int i = 0; i < 30; ++i) bank.update(6, 0.2f, LastMoved{3, true});
  for (int i = 0; i < 30; ++i) bank.update(2, 0.9f, LastMoved{1, false});
  check(bank.takeTouch(touch) && touch.slot == 1 && !touch.knobB, "touch: two moves between calls report the last");

  KnobBank stepped;
  std::array<int, kPotCount> steps{};
  steps[2] = 5;
  stepped.setSteps(steps);
  seedBank(stepped, 0.9f);
  steps[2] = 10;
  stepped.setSteps(steps);
  check(!stepped.takeTouch(touch), "touch: a re-seed after a card change is no touch");
  for (int i = 0; i < 30; ++i) stepped.update(2, 0.1f, LastMoved{1, false});
  check(stepped.takeTouch(touch) && touch.slot == 1, "touch: a stepped knob crossing to another step is a touch");
}

void checkKnobFilter() {
  checkKnobFilterContinuous();
  checkKnobFilterStepped();
  checkKnobFilterReseedReference();
  checkKnobPosition();
  checkKnobRestep();
  checkKnobBankSeeding();
  checkKnobBankMoves();
  checkKnobBankSteps();
  checkKnobBankTouch();
}

void checkEvery() {
  Every e(100);
  check(!e.due(50), "every: not due inside the first period");
  check(e.due(100) && !e.due(150), "every: due once per period");
  check(e.due(200), "every: due again after a period");
  Every wrap(100);
  wrap.due(0xFFFFFF00u);
  check(!wrap.due(0xFFFFFF50u) && wrap.due(0x00000020u), "every: survives the millisecond counter wrapping");
}

void checkPublishGate() {
  PublishGate gate;
  check(!gate.take(true, false), "publish gate: nothing changed, nothing to publish");
  check(gate.take(true, true), "publish gate: a change publishes");
  check(!gate.take(false, true) && gate.take(true, false), "publish gate: a change before the panel is read waits for it");
  gate.markStale();
  check(!gate.take(false, false) && gate.take(true, false) && !gate.take(true, false),
        "publish gate: a stale request waits for the panel and fires once");
}

void checkCanvasHandoff() {
  CanvasHandoff h;
  check(!h.update(false), "handoff: the menu owning the canvas is not a release");
  check(!h.update(true) && !h.update(true), "handoff: holding the canvas is not a release");
  check(h.update(false) && !h.update(false), "handoff: the first pass after the release reports it once");
}

void checkJackSwitch() {
  JackSwitch j(InputJack::Mic);
  j.update(0, InputJack::Mic, false);
  check(!j.holdMute() && j.relay() == InputJack::Mic, "jack: no change, no hold");

  j.update(10, InputJack::Line, false);
  check(j.holdMute() && j.relay() == InputJack::Mic, "jack: request holds the mute, relay waits");
  j.update(20, InputJack::Line, false);
  check(j.relay() == InputJack::Mic, "jack: relay waits until fully muted");
  j.update(30, InputJack::Line, true);
  check(j.relay() == InputJack::Line && j.holdMute(), "jack: relay switches when fully muted");
  j.update(30 + kRelaySettleMs - 1, InputJack::Line, true);
  check(j.holdMute(), "jack: hold stays until the relay settles");
  j.update(30 + kRelaySettleMs, InputJack::Line, true);
  check(!j.holdMute() && j.relay() == InputJack::Line, "jack: hold released after the settle time");

  j.update(100, InputJack::Mic, true);
  j.update(110, InputJack::Line, true);
  check(j.relay() == InputJack::Mic, "jack: relay moved on the first request");
  j.update(111, InputJack::Line, true);
  j.update(111 + kRelaySettleMs, InputJack::Line, true);
  j.update(112 + kRelaySettleMs, InputJack::Line, true);
  check(j.holdMute() && j.relay() == InputJack::Line, "jack: a second change mid-sequence ends on the latest request");
  j.update(112 + 2 * kRelaySettleMs, InputJack::Line, true);
  check(!j.holdMute() && j.relay() == InputJack::Line, "jack: latest request settles and releases");
}

void checkJackRequestWithdrawn() {
  JackSwitch j(InputJack::Mic);
  j.update(0, InputJack::Line, false);
  check(j.holdMute(), "jack: a request holds the mute");
  j.update(5, InputJack::Mic, false);
  check(!j.holdMute() && j.relay() == InputJack::Mic, "jack: a request withdrawn before the mute needs no dip");
}

void checkVersionTag() {
  char buf[16];
  versionTag("v2.00.04-3-gabc123-dirty", buf, sizeof(buf));
  check(std::strcmp(buf, "v2.00.04") == 0, "version: tag is cut at the first dash");
  versionTag("v2.00.04", buf, sizeof(buf));
  check(std::strcmp(buf, "v2.00.04") == 0, "version: a bare tag passes through");
  versionTag("dev", buf, sizeof(buf));
  check(std::strcmp(buf, "dev") == 0, "version: dev passes through");
  char small[5];
  versionTag("v2.00.04-1-gabc", small, sizeof(small));
  check(std::strcmp(small, "v2.0") == 0, "version: clipped to the buffer");
}

// Steps the sequence once per ms from `from` to `to` inclusive and returns the last outputs.
MuteOutputs runMute(MuteSequence& m, uint32_t from, uint32_t to, bool released) {
  MuteOutputs out;
  for (uint32_t t = from; t <= to; ++t) out = m.update(t, released);
  return out;
}

void checkMuteSequence() {
  MuteSequence m;
  m.reset(true, 0);
  MuteOutputs out = runMute(m, 0, kBootUnmuteDelayMs - 1, true);
  check(!out.xsmtHigh && !out.muteNHigh && m.fullyMuted(), "mute: stays muted before the clocks run");
  m.clocksRunning(100);
  out = runMute(m, 100, 100 + kBootUnmuteDelayMs - 1, true);
  check(!out.xsmtHigh && !out.muteNHigh, "mute: boot delay holds the output muted");

  uint32_t t = 100 + kBootUnmuteDelayMs;
  out = m.update(t, true);
  check(out.muteNHigh && !out.xsmtHigh && !m.fullyMuted(), "mute: release raises MUTE_N first");
  out = runMute(m, t + 1, t + kRelaySettleMs - 1, true);
  check(out.muteNHigh && !out.xsmtHigh, "mute: XSMT waits for the relay to settle");
  out = m.update(t + kRelaySettleMs, true);
  check(out.muteNHigh && out.xsmtHigh && !m.active(), "mute: XSMT rises after the settle");

  t += kRelaySettleMs + 100;
  runMute(m, t, t + kSwitchDebounceMs - 1, false);
  out = m.update(t + kSwitchDebounceMs, false);
  check(!out.xsmtHigh && out.muteNHigh, "mute: XSMT falls first, MUTE_N still high");
  out = runMute(m, t + kSwitchDebounceMs + 1, t + kSwitchDebounceMs + kXsmtLeadMs - 1, false);
  check(!out.xsmtHigh && out.muteNHigh && !m.fullyMuted(), "mute: MUTE_N waits out the XSMT lead");
  out = m.update(t + kSwitchDebounceMs + kXsmtLeadMs, false);
  check(!out.xsmtHigh && !out.muteNHigh && m.fullyMuted(), "mute: MUTE_N falls after the lead");
}

void checkMuteLatchAndHold() {
  MuteSequence m;
  m.reset(true, 0);
  m.clocksRunning(0);
  runMute(m, 0, kBootUnmuteDelayMs + kRelaySettleMs + 1, true);
  check(!m.active(), "mute: unmuted after the boot delay and settle");

  uint32_t t = 1000;
  m.hold(true);
  MuteOutputs out = runMute(m, t, t + kXsmtLeadMs, true);
  check(!out.xsmtHigh && !out.muteNHigh && m.fullyMuted(), "mute: a hold mutes through the sequence");
  t += 100;
  out = runMute(m, t, t + kRelaySettleMs + 1, true);
  check(!out.xsmtHigh && !out.muteNHigh, "mute: the output stays muted while held");
  m.hold(false);
  t += 100;
  out = runMute(m, t, t + kRelaySettleMs + 1, true);
  check(out.xsmtHigh && out.muteNHigh, "mute: releasing the hold runs the release sequence");

  t += 100;
  m.latch();
  out = runMute(m, t, t + kXsmtLeadMs, true);
  check(m.fullyMuted() && !out.xsmtHigh, "mute: a latch mutes");
  out = runMute(m, t + 100, t + 300, true);
  check(!out.xsmtHigh && !out.muteNHigh, "mute: a latch ignores a released switch");
}

void checkMuteMidRelease() {
  MuteSequence mid;
  mid.reset(true, 0);
  mid.clocksRunning(0);
  runMute(mid, 0, kBootUnmuteDelayMs, true);
  mid.hold(true);
  MuteOutputs out = mid.update(kBootUnmuteDelayMs + 1, true);
  check(!out.xsmtHigh && out.muteNHigh, "mute: muted again mid-release keeps XSMT low");
  out = runMute(mid, kBootUnmuteDelayMs + 2, kBootUnmuteDelayMs + kXsmtLeadMs + 2, true);
  check(!out.muteNHigh && mid.fullyMuted(), "mute: a mid-release mute ends fully muted");
}

// reset() returns to the fully muted start: no latch, hold or running clocks survive it.
void checkMuteReset() {
  MuteSequence m;
  m.reset(true, 0);
  m.clocksRunning(0);
  m.latch();
  m.hold(true);
  m.reset(true, 5000);
  const MuteOutputs out = runMute(m, 5000, 5000 + 2 * kBootUnmuteDelayMs, true);
  check(!out.xsmtHigh && !out.muteNHigh && m.fullyMuted(), "mute: reset waits for the clocks again");
  m.clocksRunning(20000);
  runMute(m, 20000, 20000 + kBootUnmuteDelayMs + kRelaySettleMs + 1, true);
  check(!m.active(), "mute: after reset a released switch and running clocks unmute");
}

}  // namespace

void checkInputLogic() {
  checkDebounce();
  checkSteppedPosition();
  checkKnobFilter();
  checkEvery();
  checkPublishGate();
  checkCanvasHandoff();
  checkJackSwitch();
  checkJackRequestWithdrawn();
  checkVersionTag();
  checkMuteSequence();
  checkMuteLatchAndHold();
  checkMuteMidRelease();
  checkMuteReset();
}
