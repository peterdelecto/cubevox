// Checks the load rule on the host: the late window's 12-late trip over the last 48 chain blocks, the
// card the rule takes out (the slot with the most cycles in the worst late block), the cards it passes
// over, the 50 ms spacing, each card's wait of 2, 4, 8, 16 and 16 s and the reset after 60 s in, and
// the ends of a hold by a toggle, a layout apply and a stream restart.

#include "support.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "core/late_window.h"
#include "core/layout.h"
#include "core/load_rule.h"

namespace {

using Toggles = std::array<bool, kSlotCount>;

Toggles allOn() {
  Toggles t;
  t.fill(true);
  return t;
}

constexpr uint8_t bit(int slot) { return static_cast<uint8_t>(1u << slot); }

Layout layoutOf(std::initializer_list<Card> cards) {
  Layout layout;
  layout.card.fill(Card::Empty);
  size_t slot = 0;
  for (Card c : cards) layout.card[slot++] = c;
  return layout;
}

// Per-slot cycles with `slot` the heaviest at `cycles` and the rest at 1000.
SlotCycles heavy(int slot, uint32_t cycles = 90000) {
  SlotCycles c;
  c.fill(1000);
  c[static_cast<size_t>(slot)] = cycles;
  return c;
}

// ---- the window ----

void checkWindowTrip() {
  LateWindow w;
  for (int i = 0; i < 100; ++i) w.add(false, 100000, heavy(1));
  for (int i = 0; i < 11; ++i) w.add(true, 700000, heavy(1));
  check(!w.tripPending() && w.lateCount() == 11, "window: 11 late blocks do not trip");
  w.add(true, 700000, heavy(1));
  check(w.tripPending() && w.lateCount() == 12, "window: the 12th late block trips");
  LateTrip trip;
  check(w.take(trip) && trip.lateCount == 12, "window: the take reports the late count");
  check(!w.tripPending() && !w.take(trip) && w.lateCount() == 0, "window: a trip is taken once and the window starts empty");
  for (int i = 0; i < 11; ++i) w.add(true, 700000, heavy(1));
  check(!w.tripPending(), "window: after a take 11 late blocks still do not trip");
}

void checkWindowSpan() {
  LateWindow w;
  for (int i = 0; i < 11; ++i) w.add(true, 700000, heavy(1));
  for (int i = 0; i < kLateWindowBlocks - 11; ++i) w.add(false, 100000, heavy(1));
  check(w.lateCount() == 11, "window: a late block is still counted 48 blocks in");
  for (int i = 0; i < 11; ++i) w.add(false, 100000, heavy(1));
  check(w.lateCount() == 0, "window: a late block leaves after 48 blocks");
  for (int i = 0; i < 12; ++i) {
    w.add(true, 700000, heavy(1));
    for (int k = 0; k < 4; ++k) w.add(false, 100000, heavy(1));
  }
  check(!w.tripPending(), "window: 12 late blocks spread over more than 48 never share a window");
}

void checkWindowWorstBlock() {
  LateWindow w;
  w.add(false, 900000, heavy(6));  // on time, however heavy: never the worst late block
  w.add(true, 650000, heavy(2));
  w.add(true, 800000, heavy(4));
  w.add(true, 700000, heavy(5));
  for (int i = 0; i < 9; ++i) w.add(true, 660000, heavy(3));
  LateTrip trip;
  check(w.take(trip) && trip.worstBlock == heavy(4), "window: the worst block is the late block with the highest total");
}

void checkWindowClear() {
  LateWindow w;
  for (int i = 0; i < 12; ++i) w.add(true, 700000, heavy(1));
  w.clear();
  LateTrip trip;
  check(!w.tripPending() && !w.take(trip) && w.lateCount() == 0, "window: a clear (a stream restart, a layout apply) drops the trip and the counts");
  for (int i = 0; i < 11; ++i) w.add(true, 700000, heavy(1));
  check(!w.tripPending(), "window: the cleared window counts from empty");
}

// ---- the rule ----

LoadStep trip(LoadRule& r, const Layout& layout, const Toggles& t, uint32_t nowMs, const SlotCycles& worst, int late = 20) {
  LoadInput in{layout, t, nowMs};
  in.trip = true;
  in.tripLate = late;
  in.worstBlock = worst;
  in.lateInWindow = 0;
  return r.update(in);
}

LoadStep idle(LoadRule& r, const Layout& layout, const Toggles& t, uint32_t nowMs, int lateInWindow = 0) {
  LoadInput in{layout, t, nowMs};
  in.lateInWindow = lateInWindow;
  return r.update(in);
}

// Default layout: Input gate, Autotune, Octave, Unison, Echo, Distortion, Gate, Spring Reverb.
void checkWhichCard() {
  const Layout layout = defaultLayout();
  const Toggles on = allOn();
  LoadRule r;
  LoadStep s = trip(r, layout, on, 1000, heavy(4), 31);
  check(s.slot == 4 && s.card == Card::Slapback && s.lateCount == 31,
        "rule: the card whose slot took the most cycles goes, even when the cost table ranks it low (Echo over Spring)");
  check(r.heldMask() == bit(4) && r.steps() == 1, "rule: the held mask and the step count follow");

  SlotCycles tie;
  tie.fill(1000);
  tie[2] = tie[6] = 50000;
  LoadRule t;
  check(trip(t, layout, on, 1000, tie).slot == 6, "rule: a tie goes to the later slot");

  LoadRule gate;
  check(trip(gate, layout, on, 1000, heavy(0)).slot != 0 && gate.heldMask() != bit(0),
        "rule: the Input gate is never taken out, however heavy its slot");

  Toggles off = allOn();
  off[4] = false;
  LoadRule o;
  check(trip(o, layout, off, 1000, heavy(4)).slot != 4, "rule: a card with its toggle off is not running, so it is not taken");
  Layout sparse = layout;
  sparse.card[4] = Card::Empty;
  LoadRule e;
  check(trip(e, sparse, on, 1000, heavy(4)).slot != 4, "rule: an empty slot is never taken");
}

void checkAutotunePassedOver() {
  Toggles on = allOn();
  const Layout mob = layoutOf({Card::InputGate, Card::Autotune, Card::Octave, Card::Crowd});
  LoadRule a;
  check(trip(a, mob, on, 1000, heavy(1)).slot == 3, "rule: Autotune is passed over while Mob sits behind it");
  Toggles mobOff = on;
  mobOff[3] = false;
  LoadRule b;
  check(trip(b, mob, mobOff, 1000, heavy(1)).slot == 1, "rule: Autotune goes once the card that read its tracker is off");

  const Layout formant = layoutOf({Card::Autotune, Card::Empty, Card::Formant, Card::Unison});
  LoadRule c;
  check(trip(c, formant, on, 1000, heavy(0)).slot != 0, "rule: Autotune is passed over with Formant directly behind it");
  const Layout apart = layoutOf({Card::Autotune, Card::Octave, Card::Formant});
  LoadRule d;
  check(trip(d, apart, on, 1000, heavy(0)).slot == 0, "rule: a Formant card with a card between runs its own tracker, so Autotune goes");
  const Layout growl = layoutOf({Card::Autotune, Card::Octave, Card::Growl});
  LoadRule g;
  check(trip(g, growl, on, 1000, heavy(0)).slot != 0, "rule: Autotune is passed over with Deepthroat anywhere behind it");
  const Layout follower = layoutOf({Card::Autotune, Card::FormantFollower});
  LoadRule f;
  check(trip(f, follower, on, 1000, heavy(0)).slot != 0, "rule: Autotune is passed over with Apraxia directly behind it");
}

void checkSpacing() {
  const Layout layout = defaultLayout();
  const Toggles on = allOn();
  LoadRule r;
  check(trip(r, layout, on, 5000, heavy(7)).slot == 7, "spacing: the first trip steps at once");
  check(trip(r, layout, on, 5049, heavy(6)).slot == -1 && r.steps() == 1, "spacing: a trip 49 ms later is dropped");
  check(trip(r, layout, on, 5050, heavy(6)).slot == 6 && r.steps() == 2, "spacing: a trip 50 ms after the step steps");
  LoadRule wrap;
  trip(wrap, layout, on, 0xFFFFFFF0u, heavy(7));
  check(trip(wrap, layout, on, 0xFFFFFFF0u + 49, heavy(6)).slot == -1 && trip(wrap, layout, on, 0xFFFFFFF0u + 50, heavy(6)).slot == 6,
        "spacing: the clock may wrap");
}

// Takes slot 7 out at `now` with the window clean, and returns its slot.
int holdSpring(LoadRule& r, const Layout& layout, const Toggles& t, uint32_t now) { return trip(r, layout, t, now, heavy(7)).slot; }

void checkWaits() {
  const Layout layout = defaultLayout();
  const Toggles on = allOn();
  LoadRule r;
  uint32_t now = 10000;
  const uint32_t waits[] = {2000, 4000, 8000, 16000, 16000, 16000};
  for (uint32_t wait : waits) {
    check(holdSpring(r, layout, on, now) == 7 && r.heldMask() == bit(7), "waits: the card is taken out");
    idle(r, layout, on, now + wait - 1);
    const bool heldBefore = r.heldMask() == bit(7);
    idle(r, layout, on, now + wait);
    check(heldBefore && r.heldMask() == 0, "waits: the card stays out for its wait and comes back when it is up");
    now += wait + 1000;  // back in for 1 s, so the next hold doubles the wait
  }

  LoadRule late;
  holdSpring(late, layout, on, 0);
  idle(late, layout, on, 5000, 1);
  check(late.heldMask() == bit(7), "waits: a late block in the window keeps the card out past its wait");
  idle(late, layout, on, 5001, 0);
  check(late.heldMask() == 0, "waits: the card comes back as soon as the window holds no late block");

  LoadRule minute;
  holdSpring(minute, layout, on, 0);
  idle(minute, layout, on, 2000);
  check(holdSpring(minute, layout, on, 3000) == 7, "waits: held a second time");
  idle(minute, layout, on, 3000 + 3999);
  check(minute.heldMask() == bit(7), "waits: the second wait is 4 s");
  idle(minute, layout, on, 3000 + 4000);
  check(minute.heldMask() == 0, "waits: the second hold ends at 4 s");
  const uint32_t back = 3000 + 4000;
  holdSpring(minute, layout, on, back + 60000);
  idle(minute, layout, on, back + 60000 + 1999);
  check(minute.heldMask() == bit(7), "waits: a card in for 60 s starts again, and is held at 2 s");
  idle(minute, layout, on, back + 60000 + 2000);
  check(minute.heldMask() == 0, "waits: a card in for 60 s starts again at 2 s");

  LoadRule near;
  holdSpring(near, layout, on, 0);
  idle(near, layout, on, 2000);
  holdSpring(near, layout, on, 2000 + 59999);
  idle(near, layout, on, 2000 + 59999 + 3999);
  check(near.heldMask() == bit(7), "waits: a card in for 59.999 s does not start again, its wait is 4 s");
}

void checkWaitsAreOwn() {
  const Layout layout = defaultLayout();
  const Toggles on = allOn();
  LoadRule r;
  holdSpring(r, layout, on, 0);
  trip(r, layout, on, 100, heavy(6));
  check(r.heldMask() == (bit(7) | bit(6)), "waits: two cards held");
  idle(r, layout, on, 2000);
  check(r.heldMask() == bit(6), "waits: the first card's wait counts from its own hold");
  idle(r, layout, on, 2100);
  check(r.heldMask() == 0, "waits: the second card comes back 100 ms later");
}

void checkEnds() {
  const Layout layout = defaultLayout();
  Toggles t = allOn();
  LoadRule r;
  holdSpring(r, layout, t, 1000);
  t[7] = false;
  idle(r, layout, t, 1100, 5);
  check(r.heldMask() == 0, "ends: the hold ends when the card's toggle goes off");
  t[7] = true;
  idle(r, layout, t, 1200, 5);
  check(r.heldMask() == 0 && r.steps() == 1, "ends: it does not return held when the toggle comes back on, and is no step");
  holdSpring(r, layout, t, 1300);
  idle(r, layout, t, 1300 + 1999);
  check(r.heldMask() == bit(7), "ends: a toggle off gives the card a fresh 2 s wait");
  idle(r, layout, t, 1300 + 2000);
  check(r.heldMask() == 0, "ends: and it ends at 2 s, not 4");

  LoadRule a;
  const Toggles on = allOn();
  holdSpring(a, layout, on, 1000);
  trip(a, layout, on, 1100, heavy(6));
  LoadInput apply{layout, on, 1200};
  apply.layoutApplied = true;
  apply.lateInWindow = 7;
  const LoadStep s = a.update(apply);
  check(a.heldMask() == 0 && s.layoutCleared && a.steps() == 2, "ends: a layout apply clears every hold, asks for the window to be emptied and keeps the step count");

  LoadRule c;
  holdSpring(c, layout, on, 1000);
  Layout swapped = layout;
  swapped.card[7] = Card::ReverbChasm;
  const LoadStep sw = idle(c, swapped, on, 1100, 3);
  check(c.heldMask() == 0 && sw.layoutCleared, "ends: a card swap seen in the layout clears the holds");
  holdSpring(c, swapped, on, 1200);
  idle(c, swapped, on, 1250, 3);
  check(c.heldMask() == bit(7), "ends: the same layout keeps its holds");

  // A layout apply also resets the waits.
  LoadRule w;
  holdSpring(w, layout, on, 0);
  idle(w, layout, on, 2000);
  holdSpring(w, layout, on, 2100);
  LoadInput again{layout, on, 2200};
  again.layoutApplied = true;
  w.update(again);
  holdSpring(w, layout, on, 2300);
  idle(w, layout, on, 2300 + 1999);
  check(w.heldMask() == bit(7), "ends: after a layout apply the first wait is 2 s again");
  idle(w, layout, on, 2300 + 2000);
  check(w.heldMask() == 0, "ends: and it is up at 2 s");
}

void checkBottom() {
  const Layout layout = defaultLayout();
  const Toggles on = allOn();
  LoadRule r;
  uint32_t now = 1000;
  for (int s = 7; s >= 1; --s) {
    check(trip(r, layout, on, now, heavy(s)).slot == s, "bottom: each card is taken in turn");
    now += 50;
  }
  check(r.heldMask() == static_cast<uint8_t>(0xFF & ~bit(0)) && r.steps() == 7,
        "bottom: every card but the Input gate is out");
  check(trip(r, layout, on, now, heavy(3)).slot == -1 && r.steps() == 7 && r.heldMask() == static_cast<uint8_t>(0xFF & ~bit(0)),
        "bottom: with every card it may take out held, the rule does nothing more");

  // Autotune with a reader that is held: its slot waits behind that reader's hold.
  const Layout mob = layoutOf({Card::InputGate, Card::Autotune, Card::Crowd});
  LoadRule m;
  check(trip(m, mob, on, 1000, heavy(1)).slot == 2, "bottom: Mob goes first, ahead of the Autotune it reads");
  check(trip(m, mob, on, 1050, heavy(1)).slot == 1, "bottom: then Autotune, once no live card reads its tracker");
  check(trip(m, mob, on, 1100, heavy(1)).slot == -1, "bottom: then nothing");
}

void checkStepText() {
  char text[64];
  LoadStep s;
  s.slot = 3;
  s.card = Card::Crowd;
  s.lateCount = 31;
  loadStepText(s, text, sizeof(text));
  check(std::strcmp(text, "overload: Mob out, 31 of 48 late") == 0, "text: a step names the card and the late count");
  s.card = Card::ReverbSpring;
  loadStepText(s, text, sizeof(text));
  check(std::strlen(text) < 64 && std::strncmp(text, "overload: Spring Reverb out, ", 29) == 0, "text: the longest card name fits the entry's field");
  char small[8];
  const int n = loadStepText(s, small, sizeof(small));
  check(n == 7 && small[7] == '\0', "text: a short buffer is cut and ends with its NUL");
}

}  // namespace

void checkLoadRule() {
  checkWindowTrip();
  checkWindowSpan();
  checkWindowWorstBlock();
  checkWindowClear();
  checkWhichCard();
  checkAutotunePassedOver();
  checkSpacing();
  checkWaits();
  checkWaitsAreOwn();
  checkEnds();
  checkBottom();
  checkStepText();
}
