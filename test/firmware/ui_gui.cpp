// Checks the runtime screen state machine on the host. Covers the resting screens, the readout and
// its 2 s hold, what wins over what, the 2-frame dissolve, the value easing and the 60 s walk.

#include "support.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "core/settings.h"
#include "ui/gui.h"

namespace {

// A gui driven at its own tick, with the switch and menu states a test sets.
struct GuiRig {
  Gui gui;
  uint32_t now = 1000;
  bool switchOn = false;
  bool menuOpen = false;

  GuiFrame tick() {
    now += kUiTickMs;
    return gui.tick(now, switchOn, menuOpen);
  }

  GuiFrame ticks(int n) {
    GuiFrame last;
    for (int i = 0; i < n; ++i) last = tick();
    return last;
  }

  // Ticks until the next tick would land `ms` after `since`. Returns the frame of the last tick run.
  GuiFrame runUntil(uint32_t since, uint32_t ms) {
    GuiFrame last;
    while (now + kUiTickMs - since < ms) last = tick();
    return last;
  }

  void move(int slot, int knob, float target) { gui.knobMoved(slot, knob, target, now); }

  void flip(int slot, bool on) { gui.toggleFlipped(slot, on, now); }
};

bool isScreen(const GuiFrame& f, Screen s, int dissolve) { return f.screen == s && f.dissolve == dissolve; }

// A screen change shows dissolve steps 1 and 2, then settles at 0.
bool dissolvesInto(GuiRig& g, Screen s) {
  const GuiFrame a = g.tick();
  const GuiFrame b = g.tick();
  const GuiFrame c = g.tick();
  return isScreen(a, s, 1) && isScreen(b, s, 2) && isScreen(c, s, 0);
}

bool sameWalk(Walk a, Walk b) { return a.x == b.x && a.y == b.y; }

void checkConstants() {
  check(kUiFps == 25 && kUiTickMs == 40, "gui: the UI ticks at 25 fps, every 40 ms");
  check(kReadoutHoldMs == 2000 && kWalkStepMs == 60000, "gui: the readout holds 2 s and the walk steps every 60 s");
  check(kDissolveSteps == 2, "gui: a dissolve is 2 mixed frames");
}

void checkResting() {
  GuiRig g;
  check(isScreen(g.tick(), Screen::Voice, 0), "gui: the first tick shows the voice with no dissolve");
  check(isScreen(g.ticks(10), Screen::Voice, 0), "gui: the voice stays settled");

  g.switchOn = true;
  check(dissolvesInto(g, Screen::Muted), "gui: the switch on dissolves from the voice to MUTED in 2 frames");
  check(isScreen(g.ticks(100), Screen::Muted, 0), "gui: MUTED stays settled");
  g.switchOn = false;
  check(dissolvesInto(g, Screen::Voice), "gui: the switch off dissolves back to the voice");
}

void checkReadoutLifecycle() {
  GuiRig g;
  g.ticks(3);
  g.move(4, 1, 120.0f);
  const uint32_t touched = g.now;
  const GuiFrame in = g.tick();
  check(isScreen(in, Screen::Readout, 1) && in.slot == 4 && in.knob == 1, "gui: a knob move starts the readout with dissolve step 1");
  check(in.shown == 120.0f && in.target == 120.0f, "gui: a new readout starts at the knob's reading");
  const GuiFrame second = g.tick();
  const GuiFrame third = g.tick();
  check(isScreen(second, Screen::Readout, 2) && isScreen(third, Screen::Readout, 0), "gui: the readout dissolves in over 2 frames");

  const GuiFrame still = g.runUntil(touched, kReadoutHoldMs);
  check(g.now - touched == kReadoutHoldMs - kUiTickMs && isScreen(still, Screen::Readout, 0),
        "gui: the readout is still up at 1960 ms after the last move");
  check(isScreen(g.tick(), Screen::Voice, 1), "gui: at 2000 ms it leaves with a dissolve back to the voice");
  const GuiFrame out2 = g.tick();
  const GuiFrame out3 = g.tick();
  check(isScreen(out2, Screen::Voice, 2) && isScreen(out3, Screen::Voice, 0), "gui: the dissolve out takes 2 frames");
}

void checkHoldRestarts() {
  GuiRig g;
  g.move(0, 0, 10.0f);
  g.ticks(33);  // 1.3 s
  g.move(0, 0, 20.0f);
  const uint32_t touched = g.now;
  const GuiFrame f = g.runUntil(touched, kReadoutHoldMs);
  check(f.screen == Screen::Readout, "gui: a move 1.3 s in gives the readout 2 s from that move");
  check(isScreen(g.tick(), Screen::Voice, 1), "gui: the readout leaves 2 s after the last move");
}

void checkMuteAndReadout() {
  GuiRig g;
  g.switchOn = true;
  g.ticks(5);
  g.move(2, 0, 7.0f);
  const uint32_t touched = g.now;
  check(isScreen(g.tick(), Screen::Readout, 1), "gui: a knob turned while muted raises the readout");
  const GuiFrame still = g.runUntil(touched, kReadoutHoldMs);
  check(isScreen(still, Screen::Readout, 0), "gui: muted does not shorten the hold");
  check(isScreen(g.tick(), Screen::Muted, 1), "gui: after the hold the readout dissolves to MUTED, not the voice");
  check(isScreen(g.tick(), Screen::Muted, 2) && isScreen(g.tick(), Screen::Muted, 0), "gui: and settles on MUTED");
}

void checkMenuWins() {
  GuiRig g;
  g.move(1, 0, 3.0f);
  g.ticks(3);
  g.menuOpen = true;
  check(isScreen(g.tick(), Screen::Menu, 1), "gui: an open menu replaces a live readout");
  g.ticks(2);
  g.move(1, 0, 4.0f);
  check(isScreen(g.ticks(5), Screen::Menu, 0), "gui: a knob moved under the open menu does not show");
  g.menuOpen = false;
  check(isScreen(g.tick(), Screen::Readout, 1), "gui: closing the menu inside the hold shows the readout again");

  GuiRig h;
  h.menuOpen = true;
  h.ticks(5);
  h.switchOn = true;
  check(isScreen(h.ticks(3), Screen::Menu, 0), "gui: the open menu also wins over MUTED");
  h.move(1, 0, 4.0f);
  h.menuOpen = false;
  h.ticks(kReadoutHoldMs / kUiTickMs + 5);
  check(isScreen(h.tick(), Screen::Muted, 0), "gui: closing the menu after the hold lands on MUTED");
}

void checkReadoutReplaced() {
  GuiRig g;
  g.move(4, 1, 160.0f);
  g.ticks(5);
  g.move(2, 0, 0.0f);
  const GuiFrame f = g.tick();
  check(isScreen(f, Screen::Readout, 1) && f.slot == 2 && f.knob == 0, "gui: another slot's knob replaces the readout with a dissolve");
  check(f.shown == 0.0f && f.target == 0.0f, "gui: the new readout starts at its own reading, not the old one");
  g.ticks(2);
  g.move(2, 1, 50.0f);
  check(isScreen(g.tick(), Screen::Readout, 1), "gui: the other knob of the same slot also dissolves");
  g.ticks(2);
  g.move(2, 1, 60.0f);
  const GuiFrame same = g.tick();
  check(isScreen(same, Screen::Readout, 0) && same.slot == 2 && same.knob == 1, "gui: the same knob moving again does not dissolve");

  GuiRig mid;
  mid.move(0, 0, 1.0f);
  mid.ticks(1);
  mid.move(1, 0, 1.0f);
  const GuiFrame restart = mid.tick();
  const GuiFrame next = mid.tick();
  check(isScreen(restart, Screen::Readout, 1) && restart.slot == 1 && isScreen(next, Screen::Readout, 2),
        "gui: a change in the middle of a dissolve restarts it at step 1");
}

void checkToggleLifecycle() {
  GuiRig g;
  g.ticks(3);
  g.flip(5, true);
  const uint32_t flipped = g.now;
  const GuiFrame in = g.tick();
  check(isScreen(in, Screen::Readout, 1) && in.slot == 5 && in.toggle && in.on,
        "gui: a flip starts the readout of its slot with dissolve step 1 and the new state");
  check(isScreen(g.tick(), Screen::Readout, 2) && isScreen(g.tick(), Screen::Readout, 0), "gui: the toggle readout dissolves in over 2 frames");

  const GuiFrame still = g.runUntil(flipped, kReadoutHoldMs);
  check(g.now - flipped == kReadoutHoldMs - kUiTickMs && isScreen(still, Screen::Readout, 0) && still.toggle,
        "gui: the toggle readout is still up at 1960 ms after the flip");
  check(isScreen(g.tick(), Screen::Voice, 1), "gui: at 2000 ms it leaves with a dissolve back to the voice");
  check(isScreen(g.tick(), Screen::Voice, 2) && isScreen(g.tick(), Screen::Voice, 0), "gui: and the voice settles");

  GuiRig m;
  m.switchOn = true;
  m.ticks(5);
  m.flip(0, false);
  const uint32_t mutedFlip = m.now;
  const GuiFrame over = m.tick();
  check(isScreen(over, Screen::Readout, 1) && over.toggle && !over.on, "gui: a flip raises the readout over MUTED");
  check(isScreen(m.runUntil(mutedFlip, kReadoutHoldMs), Screen::Readout, 0) && isScreen(m.tick(), Screen::Muted, 1),
        "gui: after the hold the toggle readout dissolves to MUTED, not the voice");
}

void checkToggleRefresh() {
  GuiRig g;
  g.flip(5, true);
  g.ticks(5);
  g.flip(5, false);
  const GuiFrame off = g.tick();
  check(isScreen(off, Screen::Readout, 0) && off.slot == 5 && off.toggle && !off.on,
        "gui: the same slot's toggle flipped again refreshes in place with no dissolve");

  GuiRig hold;
  hold.flip(5, true);
  hold.ticks(33);  // 1.3 s
  hold.flip(5, false);
  const uint32_t flipped = hold.now;
  check(isScreen(hold.runUntil(flipped, kReadoutHoldMs), Screen::Readout, 0), "gui: a flip 1.3 s in gives the readout 2 s from that flip");
  check(isScreen(hold.tick(), Screen::Voice, 1), "gui: the toggle readout leaves 2 s after the last flip");
}

void checkToggleReplaced() {
  GuiRig g;
  g.flip(5, true);
  g.ticks(5);
  g.flip(2, true);
  const GuiFrame other = g.tick();
  check(isScreen(other, Screen::Readout, 1) && other.slot == 2 && other.toggle, "gui: another slot's flip replaces the readout with a dissolve");

  g.ticks(2);
  g.move(2, 0, 7.0f);
  const GuiFrame knob = g.tick();
  check(isScreen(knob, Screen::Readout, 1) && !knob.toggle && knob.knob == 0 && knob.shown == 7.0f,
        "gui: a knob on the toggle's slot replaces it with a dissolve and starts at its own reading");
  g.ticks(2);
  g.flip(2, false);
  const GuiFrame back = g.tick();
  check(isScreen(back, Screen::Readout, 1) && back.toggle && !back.on, "gui: a flip after a knob on the same slot dissolves to the toggle");

  GuiRig mid;
  mid.move(0, 0, 1.0f);
  mid.ticks(1);
  mid.flip(1, true);
  const GuiFrame restart = mid.tick();
  check(isScreen(restart, Screen::Readout, 1) && restart.slot == 1 && restart.toggle,
        "gui: a flip in the middle of a dissolve restarts it at step 1");
}

void checkToggleMenuWins() {
  GuiRig g;
  g.flip(5, true);
  g.ticks(3);
  g.menuOpen = true;
  check(isScreen(g.tick(), Screen::Menu, 1), "gui: an open menu replaces a live toggle readout");
  g.ticks(2);
  g.flip(5, false);
  check(isScreen(g.ticks(5), Screen::Menu, 0), "gui: a flip under the open menu does not show");
  g.menuOpen = false;
  const GuiFrame back = g.tick();
  check(isScreen(back, Screen::Readout, 1) && back.toggle && !back.on, "gui: closing the menu inside the hold shows the new toggle state");

  GuiRig h;
  h.menuOpen = true;
  h.ticks(3);
  h.flip(5, true);
  h.menuOpen = false;
  h.ticks(kReadoutHoldMs / kUiTickMs + 5);
  check(isScreen(h.tick(), Screen::Voice, 0), "gui: closing the menu after the hold lands on the voice");
}

void checkEasing() {
  GuiRig g;
  g.move(0, 0, 100.0f);
  g.ticks(5);
  g.move(0, 0, 200.0f);
  const float coefficient = 1.0f - std::exp(-0.04f / 0.02f);
  const GuiFrame one = g.tick();
  check(std::fabs(coefficient - 0.8647f) < 1e-3f, "gui: a 20 ms time constant at 25 fps closes 86.5 % of the gap per tick");
  check(std::fabs(one.shown - (100.0f + 100.0f * coefficient)) < 1e-3f && one.target == 200.0f,
        "gui: the value closes 86.5 % of the gap in one tick");
  const GuiFrame five = g.ticks(4);
  check(std::fabs(five.shown - 200.0f) < 0.01f, "gui: the value settles on the reading within five ticks");
  g.move(0, 0, 50.0f);
  const GuiFrame down = g.tick();
  check(down.shown < 200.0f && down.shown > 50.0f, "gui: the value eases down as well as up");
}

void checkForceChange() {
  GuiRig g;
  g.ticks(5);
  g.gui.forceChange();
  check(dissolvesInto(g, Screen::Voice), "gui: forceChange dissolves even to the screen already shown");
  check(isScreen(g.ticks(3), Screen::Voice, 0), "gui: and only once");
}

void checkWalkTable() {
  const int8_t expect[10][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}};
  bool table = true;
  for (uint32_t s = 0; s < 10; ++s) table = table && walkAt(s).x == expect[s][0] && walkAt(s).y == expect[s][1];
  check(table, "walk: the shift starts centred, then circles the 8 outer cells of a 3 by 3 block");

  bool unit = true;
  bool inside = true;
  for (uint32_t s = 0; s < 40; ++s) {
    const Walk a = walkAt(s);
    const Walk b = walkAt(s + 1);
    unit = unit && std::abs(a.x - b.x) + std::abs(a.y - b.y) == 1;
    inside = inside && std::abs(a.x) <= 1 && std::abs(a.y) <= 1;
  }
  check(unit && inside, "walk: every step moves 1 px along one axis and stays inside the 3 by 3 block");
  check(sameWalk(walkAt(1), walkAt(9)) && sameWalk(walkAt(8), Walk{1, -1}), "walk: the circle repeats every 8 steps");

  bool visits = true;
  for (int x = -1; x <= 1; ++x) {
    for (int y = -1; y <= 1; ++y) {
      bool seen = false;
      for (uint32_t s = 0; s < 9; ++s) seen = seen || (walkAt(s).x == x && walkAt(s).y == y);
      visits = visits && seen;
    }
  }
  check(visits, "walk: one lap visits all 9 cells");
}

void checkWalkTiming() {
  GuiRig g;
  g.now = 5000;
  const uint32_t start = g.now + kUiTickMs;  // the first tick anchors the walk
  const GuiFrame first = g.tick();
  check(sameWalk(first.walk, Walk{0, 0}), "walk: the first tick is unshifted");
  const GuiFrame before = g.runUntil(start, kWalkStepMs);
  check(g.now - start == kWalkStepMs - kUiTickMs && sameWalk(before.walk, Walk{0, 0}), "walk: 59.96 s in, the screen has not moved");
  check(sameWalk(g.tick().walk, Walk{1, 0}) && g.now - start == kWalkStepMs, "walk: at 60 s the screen moves 1 px");
  g.runUntil(start, 2 * kWalkStepMs);
  check(sameWalk(g.tick().walk, Walk{1, 1}), "walk: at 120 s it moves 1 px more");

  // The millisecond counter wraps after 49 days. The walk goes on across it.
  GuiRig w;
  w.now = 0xFFFFFFFFu - 30000u;
  const uint32_t wrapStart = w.now + kUiTickMs;
  w.tick();
  const GuiFrame wrapped = w.runUntil(wrapStart, kWalkStepMs);
  check(w.now < 40000u && sameWalk(wrapped.walk, Walk{0, 0}), "walk: the counter wrapped and the screen has not moved yet");
  check(sameWalk(w.tick().walk, Walk{1, 0}), "walk: the first step comes on time across the wrap");
}

void checkTopSwitchEngaged() {
  check(topSwitchEngaged(TopSwitch::Mute, true, false) && !topSwitchEngaged(TopSwitch::Mute, false, true),
        "switch: under MUTE the mute sequence says whether it is engaged");
  check(topSwitchEngaged(TopSwitch::Bypass, false, true) && !topSwitchEngaged(TopSwitch::Bypass, true, false),
        "switch: under BYPASS the debounced switch says whether it is engaged");
}

}  // namespace

void checkGui() {
  checkConstants();
  checkResting();
  checkReadoutLifecycle();
  checkHoldRestarts();
  checkMuteAndReadout();
  checkMenuWins();
  checkReadoutReplaced();
  checkToggleLifecycle();
  checkToggleRefresh();
  checkToggleReplaced();
  checkToggleMenuWins();
  checkEasing();
  checkForceChange();
  checkWalkTable();
  checkWalkTiming();
  checkTopSwitchEngaged();
}
