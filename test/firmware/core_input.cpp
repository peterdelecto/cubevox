// Checks the input logic on the host. Covers debouncing, knob filtering, the periodic timer,
// the publish gate, the input jack switch, the version tag, the mute sequence and the power-fail
// mute policy.

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
#include "core/power_fail.h"
#include "core/settings.h"
#include "core/slots.h"
#include "core/timing.h"
#include "core/version.h"

namespace {

// One read of a pot per scan round in these tests.
constexpr uint32_t kReadMs = 20;

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

// Feeds `reads` reads of `norm`, one scan round apart, advancing `nowMs`. Returns how many reported
// a move.
int feed(KnobFilter& f, float norm, int steps, uint32_t& nowMs, int reads) {
  int moves = 0;
  for (int i = 0; i < reads; ++i) {
    nowMs += kReadMs;
    if (f.update(norm, steps, nowMs) == KnobEvent::Moved) ++moves;
  }
  return moves;
}

void settle(KnobFilter& f, float norm, int steps, uint32_t& nowMs) { feed(f, norm, steps, nowMs, 60); }

void checkKnobFilterContinuous() {
  KnobFilter f;
  uint32_t t = 0;
  check(f.update(0.5f, 0, t) == KnobEvent::Seeded, "filter: first sample seeds");
  check(std::fabs(f.value() - 0.5f) < 1e-6f, "filter: seed takes the sample");
  check(f.update(0.505f, 0, t += kReadMs) == KnobEvent::None, "filter: jitter under threshold is not a move");
  check(feed(f, 0.8f, 0, t, 20) > 0, "filter: real move reported");
  check(f.value() > 0.7f, "filter: value follows the knob");
}

void checkKnobFilterStepped() {
  KnobFilter s;
  uint32_t t = 0;
  s.update(0.1f, 5, t);
  check(s.value() == 0.0f, "filter: stepped seeds at the step under the knob");
  check(feed(s, 0.3f, 5, t, 30) == 1 && s.value() == 1.0f, "filter: stepped knob moves one step");
  check(s.update(0.3f, 5, t += kReadMs) == KnobEvent::None, "filter: stepped knob rests");
  settle(s, 0.95f, 5, t);
  check(s.update(0.95f, 10, t += kReadMs) == KnobEvent::Reseeded, "filter: step count change re-seeds");
  check(s.value() == 9.0f, "filter: re-seed lands on the position under the knob");
  check(s.update(0.95f, 0, t += kReadMs) == KnobEvent::Reseeded && s.value() < 1.0f,
        "filter: back to continuous re-seeds");
}

// A re-seed restarts the move reference, so a knob parked where it re-seeded does not report a move.
void checkKnobFilterReseedReference() {
  KnobFilter f;
  uint32_t t = 0;
  f.update(0.2f, 0, t);
  settle(f, 0.2f, 0, t);
  f.update(0.9f, 5, t += kReadMs);  // the card changed to a stepped knob
  settle(f, 0.9f, 5, t);
  check(f.update(0.9f, 0, t += kReadMs) == KnobEvent::Reseeded, "filter: back to continuous re-seeds again");
  check(feed(f, 0.9f, 0, t, 20) == 0, "filter: a parked knob reports no move after a re-seed");
}

// Turns a continuous knob from `from` to `to` in `reads` even steps, one read each.
void turn(KnobFilter& f, float from, float to, int reads, uint32_t& nowMs) {
  for (int i = 1; i <= reads; ++i) {
    const float pos = from + (to - from) * static_cast<float>(i) / static_cast<float>(reads);
    f.update(pos, 0, nowMs += kReadMs);
  }
}

// Turned to a stop, the knob reports exactly that end once it rests, though the one-pole alone
// would still be short of it. Distortion blends its fuzz in over DRIVE 0 to 0.05, so a few tenths
// of a percent are audible.
void checkKnobRestsAtTheStop() {
  constexpr int kRestReads = static_cast<int>(kKnobRestMs / kReadMs);
  KnobFilter down;
  uint32_t t = 0;
  down.update(0.5f, 0, t);
  turn(down, 0.5f, 0.0f, 10, t);
  feed(down, 0.0f, 0, t, kRestReads / 2);
  check(down.value() > 0.0f, "rest: before the rest ends the value is still the lagging one-pole");
  check(feed(down, 0.0f, 0, t, kRestReads / 2) > 0 && down.value() == 0.0f,
        "rest: a knob at its lower stop reports exactly 0");

  KnobFilter up;
  t = 0;
  up.update(0.5f, 0, t);
  turn(up, 0.5f, 1.0f, 10, t);
  feed(up, 1.0f, 0, t, kRestReads + 1);
  check(up.value() == 1.0f, "rest: a knob at its upper stop reports exactly 1");
}

// While the knob turns, every read reports, so the audio follows the knob rather than steps of the
// touch threshold.
void checkKnobTracksEveryChange() {
  KnobFilter f;
  uint32_t t = 0;
  f.update(0.2f, 0, t);
  check(feed(f, 0.3f, 0, t, 1) == 1, "track: the touch reports");
  bool everyRead = true;
  float last = f.value();
  for (int i = 1; i <= 8; ++i) {
    const float pos = 0.3f + 0.002f * static_cast<float>(i);
    const bool moved = f.update(pos, 0, t += kReadMs) == KnobEvent::Moved;
    everyRead = everyRead && moved && f.value() > last;
    last = f.value();
  }
  check(everyRead, "track: each read during a touch reports, under the touch threshold too");
}

// Mid-travel with ADC noise, the knob settles on the mean of its resting reads, and the noise after
// that is no touch.
void checkKnobNoisyRest() {
  constexpr std::array<float, 10> kNoise = {0.003f,  -0.002f, 0.001f, -0.003f,  0.002f,
                                            -0.001f, 0.0f,    0.0025f, -0.0025f, 0.0015f};
  KnobFilter f;
  uint32_t t = 0;
  f.update(0.2f, 0, t);
  turn(f, 0.2f, 0.6f, 8, t);
  int movesAfterRest = 0;
  for (int i = 0; i < 120; ++i) {
    const float pos = 0.6f + kNoise[static_cast<size_t>(i) % kNoise.size()];
    const bool moved = f.update(pos, 0, t += kReadMs) == KnobEvent::Moved;
    if (moved && i > 20) ++movesAfterRest;  // 420 ms in, well past the 200 ms rest
  }
  check(std::fabs(f.value() - 0.6f) < 0.002f, "rest: a noisy rest settles on the mean of its reads");
  check(movesAfterRest == 0, "rest: noise after the rest is no touch");
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
  uint32_t t = 0;
  check(f.restep(5) == KnobEvent::None, "restep: an unseeded filter does nothing");
  settle(f, 0.95f, 5, t);
  check(f.restep(5) == KnobEvent::None, "restep: an unchanged count does nothing");
  check(f.restep(10) == KnobEvent::Reseeded && f.value() == 9.0f, "restep: a new count re-derives the position");
}

// Feeds every pot once, a slot-owned pot first.
void seedBank(KnobBank& bank, float position) {
  for (int pot = 0; pot < kPotCount; ++pot) {
    bank.update(pot, position, LastMoved{static_cast<int8_t>(pot / 2), false}, 0);
  }
}

// Feeds `reads` reads of one pot, one scan round apart, advancing `nowMs`.
void feedBank(KnobBank& bank, int pot, float position, LastMoved owner, uint32_t& nowMs, int reads) {
  for (int i = 0; i < reads; ++i) bank.update(pot, position, owner, nowMs += kReadMs);
}

void checkKnobBankSeeding() {
  KnobBank bank;
  check(!bank.seeded() && !bank.takeChanged(), "bank: starts unseeded and unchanged");
  for (int pot = 0; pot < kPotCount - 1; ++pot) bank.update(pot, 0.5f, LastMoved{}, 0);
  check(!bank.seeded() && !bank.takeChanged(), "bank: not seeded until the last pot is read");
  bank.update(kPotCount - 1, 0.5f, LastMoved{}, 0);
  check(bank.seeded() && bank.takeChanged() && !bank.takeChanged(), "bank: the last pot seeds and reports once");
  bank.update(-1, 0.5f, LastMoved{}, 0);
  bank.update(kPotCount, 0.5f, LastMoved{}, 0);
  check(!bank.takeChanged(), "bank: a pot outside the panel is ignored");
}

void checkKnobBankMoves() {
  KnobBank bank;
  uint32_t t = 0;
  seedBank(bank, 0.2f);
  bank.takeChanged();
  feedBank(bank, 5, 0.9f, LastMoved{-1, false}, t, 30);
  check(!bank.takeChanged() && bank.lastMoved().slot == -1, "bank: a pot no slot owns moves without a report");
  feedBank(bank, 6, 0.9f, LastMoved{3, true}, t, 30);
  check(bank.takeChanged(), "bank: a slot pot move is reported");
  check(bank.lastMoved().slot == 3 && bank.lastMoved().knobB, "bank: the last moved knob is the owner");
  check(bank.value(6) > 0.8f, "bank: the value follows the pot");
}

// The value the snapshot and the glass read is the one last reported, so a resting pot whose
// smoothed reading drifts under the touch threshold does not drift in the audio either.
void checkKnobBankReportedValue() {
  KnobBank bank;
  LastMoved touch;
  uint32_t t = 0;
  seedBank(bank, 0.4f);
  feedBank(bank, 6, 0.41f, LastMoved{3, true}, t, 30);
  check(bank.value(6) == 0.4f && !bank.takeTouch(touch), "bank: drift under the threshold leaves the value");
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
  uint32_t t = 0;
  seedBank(bank, 0.2f);
  check(!bank.takeTouch(touch), "touch: seeding is no touch");
  feedBank(bank, 5, 0.9f, LastMoved{-1, false}, t, 30);
  check(!bank.takeTouch(touch), "touch: a pot no slot owns is no touch");
  feedBank(bank, 6, 0.9f, LastMoved{3, true}, t, 30);
  check(bank.takeTouch(touch) && touch.slot == 3 && touch.knobB, "touch: a slot pot move reports its owner");
  check(!bank.takeTouch(touch), "touch: one move reports once");
  feedBank(bank, 6, 0.2f, LastMoved{3, true}, t, 30);
  feedBank(bank, 2, 0.9f, LastMoved{1, false}, t, 30);
  check(bank.takeTouch(touch) && touch.slot == 1 && !touch.knobB, "touch: two moves between calls report the last");

  KnobBank stepped;
  std::array<int, kPotCount> steps{};
  steps[2] = 5;
  stepped.setSteps(steps);
  seedBank(stepped, 0.9f);
  steps[2] = 10;
  stepped.setSteps(steps);
  check(!stepped.takeTouch(touch), "touch: a re-seed after a card change is no touch");
  feedBank(stepped, 2, 0.1f, LastMoved{1, false}, t, 30);
  check(stepped.takeTouch(touch) && touch.slot == 1, "touch: a stepped knob crossing to another step is a touch");
}

void checkKnobFilter() {
  checkKnobFilterContinuous();
  checkKnobFilterStepped();
  checkKnobFilterReseedReference();
  checkKnobRestsAtTheStop();
  checkKnobTracksEveryChange();
  checkKnobNoisyRest();
  checkKnobPosition();
  checkKnobRestep();
  checkKnobBankSeeding();
  checkKnobBankMoves();
  checkKnobBankReportedValue();
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

// An interrupt has already pulled both pins low. The sequence counts as muted at once, holds there
// while the trip lasts, and then releases in the usual order.
void checkMuteTrip() {
  MuteSequence m;
  m.reset(true, 0);
  m.clocksRunning(0);
  runMute(m, 0, kBootUnmuteDelayMs + kRelaySettleMs + 1, true);
  m.trip(true);
  MuteOutputs out = m.update(1000, true);
  check(!out.xsmtHigh && !out.muteNHigh && m.fullyMuted(), "mute trip: both pins low and fully muted at once");
  out = runMute(m, 1001, 1500, true);
  check(!out.xsmtHigh && !out.muteNHigh && m.active(), "mute trip: holds while the trip lasts");
  m.trip(false);
  out = m.update(1501, true);
  check(out.muteNHigh && !out.xsmtHigh, "mute trip: the release raises MUTE_N first");
  out = runMute(m, 1502, 1501 + kRelaySettleMs, true);
  check(out.muteNHigh && out.xsmtHigh && !m.active(), "mute trip: XSMT follows after the settle");

  // Mid-release the trip still wins at once, and a stream restart re-arms the settle delay.
  m.hold(true);
  m.update(2000, true);
  m.hold(false);
  m.trip(true);
  out = m.update(2001, true);
  check(!out.xsmtHigh && !out.muteNHigh && m.fullyMuted(), "mute trip: a trip mid-mute drops both pins");
  m.clocksRunning(2100);
  m.trip(false);
  out = runMute(m, 2100, 2100 + kBootUnmuteDelayMs - 1, true);
  check(!out.muteNHigh && !out.xsmtHigh, "mute trip: a restart waits out the DAC settle again");
}

void checkPowerFailCounts() {
  check(railCounts(kPowerTripMv, 12) == 2947u, "power: 4.75 V on 5VA reads 2947 at 12 bits");
  check(railMv(4095, 12) == kVaSenseDivider * kVrefMv, "power: full scale is twice VREF+");
  check(railMv(2947, 12) == 4749, "power: the trip count reads back just under 4.75 V");
}

// Holds `mv` from `from` to `to` in 10 ms polls. Returns whether the watch still held the mute at
// the last poll.
bool pollRail(PowerFailWatch& w, uint32_t from, uint32_t to, uint32_t mv) {
  bool holding = false;
  for (uint32_t t = from; t <= to; t += 10) holding = w.update(t, mv, false);
  return holding;
}

void checkPowerFailWatch() {
  PowerFailWatch w;
  check(!w.update(0, 5000, false), "power: a good rail does not trip");
  check(w.update(10, 5000, true), "power: the interrupt trips at once");
  check(pollRail(w, 20, 20 + kPowerHoldMs - 10, 5000), "power: the mute holds until the rail has held");
  check(!w.update(20 + kPowerHoldMs, 5000, false), "power: the mute releases after kPowerHoldMs of good rail");

  check(w.update(1000, 5000, true) && pollRail(w, 1010, 2000, 4800),
        "power: a rail between the trip and clear levels never releases");
  check(pollRail(w, 2010, 2010 + kPowerHoldMs - 10, 4900) && !w.update(2010 + kPowerHoldMs, 4900, false),
        "power: above the clear level the hold runs from the first good reading");

  check(w.update(3000, 5000, true) && pollRail(w, 3010, 3290, 5000), "power: tripped again");
  check(w.update(3300, 4700, false), "power: a dip under the trip level in the hold trips again");
  check(pollRail(w, 3310, 3300 + kPowerHoldMs, 5000) && !w.update(3310 + kPowerHoldMs, 5000, false),
        "power: the dip restarts the hold");

  PowerFailWatch polled;
  check(polled.update(0, 4700, false) && polled.tripped(), "power: a low reading trips without the interrupt");
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
  checkMuteTrip();
  checkPowerFailCounts();
  checkPowerFailWatch();
}
