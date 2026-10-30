// Checks the runtime face end to end on the host. Covers the dissolve frames against the proof
// renderer, the readout flowing in and out and leaving with its card, the menu on top and the input
// it takes once the face draws, the tick gate, the burn-in walk, the DEBUG load figure and the voice at
// three input levels.
// The proof PGMs go to build/oled-*.pgm.

#include "support.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/canvas.h"
#include "core/fonts.h"
#include "core/layout.h"
#include "core/level.h"
#include "core/panel.h"
#include "core/registry.h"
#include "core/settings.h"
#include "ui/face.h"
#include "ui/gui.h"
#include "ui/menu.h"
#include "ui/screens.h"

namespace {

bool lit(const Canvas& c, int x, int y) { return ((c.page(y / 8)[x] >> (y % 8)) & 1) != 0; }

bool sameCanvas(const Canvas& a, const Canvas& b) {
  for (int p = 0; p < kCanvasPages; ++p) {
    if (std::memcmp(a.page(p), b.page(p), kCanvasWidth) != 0) return false;
  }
  return true;
}

// Each pixel of the new screen where its Bayer threshold is below `level` of 64, the rest from the
// old one. This is the per-pixel definition the per-byte masks must agree with.
Canvas blend(const Canvas& from, const Canvas& to, int level) {
  Canvas r;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) r.setPixel(x, y, bayerLit(x, y, level) ? lit(to, x, y) : lit(from, x, y));
  }
  return r;
}

// Step 1 and step 2 of a 3-part dissolve light 21 and 43 of the 64 thresholds.
constexpr int kStep1Level = 21;
constexpr int kStep2Level = 43;

Canvas lineScreen() {
  Canvas c;
  c.fillRect(0, 32, kCanvasWidth, 1, true);
  return c;
}

Canvas fullScreen() {
  Canvas c;
  c.fillRect(0, 0, kCanvasWidth, kCanvasHeight, true);
  return c;
}

level::InputEnergy energyOfRms(float rms) {
  level::InputEnergy e;
  e.samples = 1920;  // one tick of audio at 48 kHz
  e.sumSquares = rms * rms * static_cast<float>(e.samples);
  return e;
}

float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

// Slapback time of 160 ms, as a knob position.
constexpr float kTime160 = (160.0f - 60.0f) / 190.0f;

// The face with the state the sketch gives it.
struct FaceRig {
  Face face;
  Menu menu;
  Layout layout = defaultLayout();
  PanelReading panel;
  Settings settings;
  Canvas live;
  uint32_t now = 1000;
  bool switchOn = false;
  int loadPercent = 0;

  UiContext ctx() const { return UiContext{layout, panel, settings, switchOn, loadPercent}; }

  // One UI tick, 40 ms on, with the input at `rms`.
  void tick(float rms = 0.0f) {
    now += kUiTickMs;
    face.tick(now, ctx(), energyOfRms(rms), menu, live);
  }

  void ticks(int n, float rms = 0.0f) {
    for (int i = 0; i < n; ++i) tick(rms);
  }

  // Ticks until the next one would land `ms` after `since`.
  void runUntil(uint32_t since, uint32_t ms) {
    while (now + kUiTickMs - since < ms) tick();
  }

  // Moves a knob to `reading` (0..1, or a step index) and tells the face.
  void turn(int slot, int knob, float reading) {
    panel.knob[static_cast<size_t>(slot)][static_cast<size_t>(knob)] = reading;
    LastMoved moved;
    moved.slot = static_cast<int8_t>(slot);
    moved.knobB = knob == 1;
    face.knobMoved(layout, panel, moved, now);
  }

  // Flips a slot's toggle and tells the face.
  void flip(int slot, bool on) {
    panel.toggle[static_cast<size_t>(slot)] = on;
    face.toggleFlipped(layout, ToggleFlip{static_cast<int8_t>(slot), on}, now);
  }
};

// The toggle readout screen of `slot`.
Canvas toggleScreen(const Layout& layout, int slot, bool on) {
  ReadoutView v;
  v.slot = slot;
  v.card = cardDesc(layout.card[static_cast<size_t>(slot)]).name;
  v.toggle = true;
  v.on = on;
  Canvas screen;
  renderReadout(screen, v);
  return screen;
}

// The readout screen of `slot` and `knob` at `shown`.
Canvas readoutScreen(const Layout& layout, int slot, int knob, float shown) {
  const Card c = layout.card[static_cast<size_t>(slot)];
  ReadoutView v;
  v.slot = slot;
  v.card = cardDesc(c).name;
  v.knob = knobDesc(c, knob);
  v.shown = shown;
  v.target = static_cast<int>(std::lround(shown));
  Canvas screen;
  renderReadout(screen, v);
  return screen;
}

void checkMuteDissolveAgainstProof() {
  FaceRig r;
  r.tick();
  check(sameCanvas(r.live, lineScreen()), "face: a silent voice is the horizon line");
  r.switchOn = true;
  r.tick();
  check(matchesGolden(r.live, "muted-1"), "face: mute on, dissolve step 1 matches the proof frame");
  r.tick();
  check(matchesGolden(r.live, "muted-2"), "face: mute on, dissolve step 2 matches the proof frame");
  r.tick();
  check(matchesGolden(r.live, "muted-3"), "face: mute on, the dissolve ends on the MUTED frame");
  r.ticks(10);
  check(matchesGolden(r.live, "muted-3"), "face: MUTED holds still");
}

void checkReadoutReplacedAgainstProof() {
  FaceRig r;
  r.tick();
  r.turn(4, 1, kTime160);
  r.ticks(5);
  check(sameCanvas(r.live, readoutScreen(r.layout, 4, 1, 160.0f)), "face: the first readout settles on 160 MS");

  r.turn(2, 0, 12.0f);  // octave semitones, step 12 is 0 ST
  r.tick();
  check(matchesGolden(r.live, "switch-1"), "face: one slot's readout replaced by another's, step 1 matches the proof frame");
  r.tick();
  check(matchesGolden(r.live, "switch-2"), "face: that replacement, step 2 matches the proof frame");
  r.tick();
  check(matchesGolden(r.live, "switch-3"), "face: that replacement ends on the new readout");
}

// The per-byte masks against the per-pixel definition, and step 2 mixing from the old screen.
void checkReadoutInAndOut() {
  FaceRig r;
  r.ticks(3);
  const Canvas voice = r.live;
  r.turn(4, 1, kTime160);
  const uint32_t touched = r.now;
  const Canvas readout = readoutScreen(r.layout, 4, 1, 160.0f);
  r.tick();
  check(sameCanvas(r.live, blend(voice, readout, kStep1Level)), "face: readout in, step 1 lights 21 of 64 thresholds of the new screen");
  r.tick();
  check(sameCanvas(r.live, blend(voice, readout, kStep2Level)), "face: readout in, step 2 mixes from the old screen, not from step 1");
  r.tick();
  check(sameCanvas(r.live, readout), "face: readout in ends on the readout");
  writePgm(r.live, "oled-readout-160ms.pgm");

  r.runUntil(touched, kReadoutHoldMs);
  check(r.now - touched == kReadoutHoldMs - kUiTickMs && sameCanvas(r.live, readout), "face: the readout is still up 1960 ms after the move");
  r.tick();
  check(sameCanvas(r.live, blend(readout, lineScreen(), kStep1Level)), "face: readout out, step 1 at 2000 ms");
  r.tick();
  check(sameCanvas(r.live, blend(readout, lineScreen(), kStep2Level)), "face: readout out, step 2");
  r.tick();
  check(sameCanvas(r.live, lineScreen()), "face: readout out ends on the voice");
}

// Slot 6 is Distortion in the default layout. A flip dissolves the readout in, the next flip on
// the same slot changes the word in place, and the hold ends on the voice.
void checkToggleAgainstProof() {
  FaceRig r;
  r.ticks(3);
  r.flip(5, true);
  const Canvas on = toggleScreen(r.layout, 5, true);
  r.tick();
  check(sameCanvas(r.live, blend(lineScreen(), on, kStep1Level)), "face: a flip dissolves the ON readout in, step 1");
  r.tick();
  check(sameCanvas(r.live, blend(lineScreen(), on, kStep2Level)), "face: a flip dissolves the ON readout in, step 2");
  r.tick();
  check(matchesGolden(r.live, "toggle-on"), "face: the ON readout matches the proof frame");
  check(sameCanvas(r.live, on), "face: the ON readout is the renderer's toggle screen");

  r.ticks(10);
  r.flip(5, false);
  const uint32_t flipped = r.now;
  r.tick();
  check(matchesGolden(r.live, "toggle-off"), "face: a second flip on the slot shows OFF in place, matching the proof frame");

  r.runUntil(flipped, kReadoutHoldMs);
  check(matchesGolden(r.live, "toggle-off"), "face: OFF is still up 1960 ms after the flip");
  r.ticks(3);
  check(sameCanvas(r.live, lineScreen()), "face: the toggle readout leaves for the voice 2 s after the flip");
}

void checkToggleOverMuted() {
  FaceRig r;
  r.switchOn = true;
  r.ticks(5);
  const Canvas word = r.live;
  r.flip(3, false);
  const uint32_t flipped = r.now;
  r.ticks(3);
  check(sameCanvas(r.live, toggleScreen(r.layout, 3, false)), "face: a flip while muted shows its readout");
  r.runUntil(flipped, kReadoutHoldMs);
  r.ticks(3);
  check(sameCanvas(r.live, word), "face: the toggle readout falls back to MUTED");
}

void checkToggleGuards() {
  FaceRig r;
  r.ticks(3);
  r.layout.card[3] = Card::Empty;
  r.flip(3, true);
  r.ticks(3);
  check(sameCanvas(r.live, lineScreen()), "face: a flip on a slot with no card raises no readout");

  const ToggleFlip none;  // slot -1
  r.face.toggleFlipped(r.layout, none, r.now);
  r.face.toggleFlipped(r.layout, ToggleFlip{static_cast<int8_t>(kSlotCount), true}, r.now);
  r.ticks(3);
  check(sameCanvas(r.live, lineScreen()), "face: a flip with no slot or a slot past the panel raises no readout");

  r.layout = defaultLayout();
  r.flip(5, true);
  r.ticks(4);
  const Canvas readout = r.live;
  r.layout.card[2] = Card::Unison;
  r.ticks(2);
  check(sameCanvas(r.live, readout), "face: a new card in another slot leaves the toggle readout up");
  r.layout.card[5] = Card::Gate;
  r.tick();
  check(sameCanvas(r.live, blend(readout, lineScreen(), kStep1Level)), "face: a new card in the toggle's slot dissolves the readout away at once");
  r.ticks(2);
  check(sameCanvas(r.live, lineScreen()), "face: and the voice settles with no readout of the new card");
}

void checkMutedReadout() {
  FaceRig r;
  r.switchOn = true;
  r.ticks(5);
  check(matchesGolden(r.live, "muted-3"), "face: muted rests on the word");
  const Canvas word = r.live;
  r.turn(6, 0, 0.5f);
  const uint32_t touched = r.now;
  r.ticks(3);
  check(sameCanvas(r.live, readoutScreen(r.layout, 6, 0, -40.0f)), "face: a knob turned while muted shows its readout");
  r.runUntil(touched, kReadoutHoldMs);
  r.ticks(3);
  check(sameCanvas(r.live, word), "face: the readout falls back to MUTED, not to the voice");
}

void checkBypassWord() {
  FaceRig r;
  r.settings.topSwitch = TopSwitch::Bypass;
  r.switchOn = true;
  r.ticks(4);
  Canvas want;
  renderMuted(want, "BYPASS");
  check(sameCanvas(r.live, want), "face: with TOP SWITCH on BYPASS the engaged switch shows BYPASS");
  writePgm(r.live, "oled-bypass-face.pgm");
}

UiInput clickInput() {
  UiInput click;
  click.click = true;
  return click;
}

// Opens the menu over a settled readout and returns the frame the face must then show, which `ref`
// draws from the same input. `readout` gets the frame the panel held before.
Canvas openMenuOverReadout(FaceRig& r, Menu& ref, Canvas& readout) {
  ref.listen();
  r.ticks(3);
  r.turn(4, 1, kTime160);
  r.ticks(4);
  readout = r.live;
  r.menu.update(clickInput(), r.now);
  ref.update(clickInput(), r.now);
  Canvas menuScreen;
  ref.draw(r.ctx(), menuScreen);
  return menuScreen;
}

void checkMenuOnTop() {
  FaceRig r;
  Menu ref;
  Canvas readout;
  const Canvas menuScreen = openMenuOverReadout(r, ref, readout);
  r.tick();
  check(sameCanvas(r.live, blend(readout, menuScreen, kStep1Level)), "face: an open menu dissolves in over the readout, step 1");
  r.tick();
  check(sameCanvas(r.live, blend(readout, menuScreen, kStep2Level)), "face: an open menu dissolves in over the readout, step 2");
  r.tick();
  check(sameCanvas(r.live, menuScreen), "face: the open menu settles on the menu's own frame");
  writePgm(r.live, "oled-face-menu.pgm");

  // A knob under the open menu leaves it alone, and so does the hold running out.
  r.turn(4, 1, kTime160 + 0.1f);
  r.ticks(80);
  check(sameCanvas(r.live, menuScreen), "face: the menu stays while a knob moves and the hold runs out");
}

// The menu's own redraws show at the next tick, and closing it dissolves back to the voice.
void checkMenuMovesAndCloses() {
  FaceRig r;
  Menu ref;
  Canvas readout;
  const Canvas menuScreen = openMenuOverReadout(r, ref, readout);
  r.ticks(80);  // the readout's hold runs out under the menu

  UiInput down;
  down.detents = 1;
  r.menu.update(down, r.now);
  ref.update(down, r.now);
  Canvas moved;
  ref.draw(r.ctx(), moved);
  r.tick();
  check(sameCanvas(r.live, moved) && !sameCanvas(moved, menuScreen), "face: a cursor move shows at the next tick, with no dissolve");

  UiInput toExit;
  toExit.detents = 7;  // from the second item to Exit, the ninth
  r.menu.update(toExit, r.now);
  r.menu.update(clickInput(), r.now);
  check(!r.menu.open(), "face: Exit closed the menu");
  const Canvas lastMenu = r.live;
  r.tick();
  check(sameCanvas(r.live, blend(lastMenu, lineScreen(), kStep1Level)), "face: the closed menu dissolves back to the voice");
  r.ticks(2);
  check(sameCanvas(r.live, lineScreen()), "face: and the voice settles");
}

// The menu listens once the face has drawn. Before that the boot splash is up and nobody could see
// what a click opened.
void checkMenuListensOnceFaceDraws() {
  FaceRig r;
  check(r.menu.update(clickInput(), r.now) == UiAction::None && !r.menu.open(), "face: before its first tick a click opens no menu");
  r.tick();
  r.menu.update(clickInput(), r.now);
  check(r.menu.open(), "face: after its first tick a click opens the menu");
}

// A readout belongs to the card its knob was on. When a new layout puts another card in that slot, the
// readout leaves at the next tick instead of showing the old number under the new card's name.
void checkReadoutLeavesWithItsCard() {
  FaceRig r;
  r.ticks(3);
  r.turn(4, 1, kTime160);
  r.ticks(4);
  const Canvas readout = r.live;
  r.layout.card[2] = Card::Unison;
  r.ticks(2);
  check(sameCanvas(r.live, readout), "face: a new card in another slot leaves the readout up");
  r.layout.card[4] = Card::Distortion;
  r.tick();
  check(sameCanvas(r.live, blend(readout, lineScreen(), kStep1Level)), "face: a new card in the readout's slot dissolves it away at once");
  r.ticks(2);
  check(sameCanvas(r.live, lineScreen()), "face: and the voice settles with no readout of the new card");
}

// The menu caches its last frame. Reopening it after another screen drew in between must still show it.
void checkMenuReopens() {
  FaceRig r;
  Menu ref;
  ref.listen();
  r.ticks(3);
  UiInput click;
  click.click = true;
  UiInput toExit;
  toExit.detents = 8;  // from the first item to Exit, the ninth
  for (int round = 0; round < 3; ++round) {
    r.menu.update(click, r.now);
    ref.update(click, r.now);
    Canvas menuScreen;
    ref.invalidate();
    ref.draw(r.ctx(), menuScreen);
    r.ticks(4);
    check(sameCanvas(r.live, menuScreen), "face: the menu shows again each time it opens");

    r.menu.update(toExit, r.now);
    r.menu.update(click, r.now);
    ref.update(toExit, r.now);
    ref.update(click, r.now);
    r.ticks(4);
    check(!r.menu.open() && sameCanvas(r.live, lineScreen()), "face: the voice shows again each time it closes");
  }
}

void checkTickGate() {
  FaceRig r;
  check(!r.face.due(10, r.live), "face: no tick while the panel still has pages to send");
  clearAllDirty(r.live);
  check(!r.face.due(10, r.live), "face: no tick before 40 ms have passed");
  check(r.face.due(40, r.live), "face: a tick is due 40 ms in once the pages are sent");
  check(!r.face.due(41, r.live) && !r.face.due(79, r.live), "face: one tick per 40 ms");
  r.live.fillRect(0, 0, 4, 4, true);
  check(!r.face.due(80, r.live), "face: a due tick waits while a page is dirty");
  clearAllDirty(r.live);
  check(r.face.due(81, r.live), "face: and runs as soon as the page is sent");
}

void checkQuietFrames() {
  FaceRig r;
  r.ticks(5);
  clearAllDirty(r.live);
  r.ticks(3);
  check(r.live.dirtyPages() == 0, "face: a silent voice sends no page to the panel");

  r.turn(4, 1, kTime160);
  r.ticks(4);
  clearAllDirty(r.live);
  r.ticks(10);
  check(r.live.dirtyPages() == 0, "face: a settled readout sends no page to the panel");
}

void checkInvalidate() {
  FaceRig r;
  r.ticks(4);
  const Canvas other = fullScreen();  // something else drew on the panel
  r.live.copyFrom(other);
  r.face.invalidate();
  r.tick();
  check(sameCanvas(r.live, blend(other, lineScreen(), kStep1Level)), "face: invalidate dissolves from what the other owner left, step 1");
  r.tick();
  check(sameCanvas(r.live, blend(other, lineScreen(), kStep2Level)), "face: invalidate, step 2");
  r.tick();
  check(sameCanvas(r.live, lineScreen()), "face: and the dissolve ends on the voice");
}

void checkKnobMovedGuards() {
  FaceRig r;
  r.ticks(3);
  r.layout.card[3] = Card::Empty;
  r.turn(3, 0, 0.5f);
  r.ticks(3);
  check(sameCanvas(r.live, lineScreen()), "face: a slot with no card raises no readout");

  const LastMoved none;  // slot -1
  r.face.knobMoved(r.layout, r.panel, none, r.now);
  LastMoved far;
  far.slot = kSlotCount;
  r.face.knobMoved(r.layout, r.panel, far, r.now);
  r.ticks(3);
  check(sameCanvas(r.live, lineScreen()), "face: a move with no slot or a slot past the panel raises no readout");

  r.layout = defaultLayout();
  r.turn(2, 0, 24.0f);  // +12 ST
  r.ticks(4);
  check(sameCanvas(r.live, readoutScreen(r.layout, 2, 0, 12.0f)), "face: a stepped knob reads its step as a signed number");
}

// The 120 ms release runs on real time, so a tick that comes late lets the ribbon fall further.
void checkLateTick() {
  FaceRig onTime;
  FaceRig late;
  onTime.ticks(3);
  late.ticks(3);
  onTime.tick(1.0f);
  late.tick(1.0f);
  onTime.tick();
  late.now += 80;  // this tick lands 120 ms after the loud one
  late.tick();

  // The newest column is solid to `h` either side of the horizon, and the rim lies one row beyond.
  const auto reaches = [](const Canvas& c, int h) {
    return lit(c, 127, 32 - h) && lit(c, 127, 32 + h) && !lit(c, 127, 32 - h - 2) && !lit(c, 127, 32 + h + 2);
  };
  check(reaches(onTime.live, 17), "face: 40 ms after a full-scale tick the ribbon has fallen to 17 px");
  check(reaches(late.live, 10), "face: 120 ms after it, when the tick came late, the ribbon has fallen to 10 px");
}

void checkWalkOnPanel() {
  FaceRig r;
  r.ticks(10);
  check(sameCanvas(r.live, lineScreen()), "face: the voice starts unshifted");
  r.ticks(static_cast<int>(kWalkStepMs / kUiTickMs) + 10);
  Canvas shifted;
  shifted.fillRect(1, 32, kCanvasWidth - 1, 1, true);
  check(sameCanvas(r.live, shifted), "face: after 60 s the voice moves 1 px, with no dissolve");
  r.ticks(static_cast<int>(kWalkStepMs / kUiTickMs));
  Canvas again;
  again.fillRect(1, 33, kCanvasWidth - 1, 1, true);
  check(sameCanvas(r.live, again), "face: after 120 s it moves 1 px more");
}

// Pixels of `a` that differ from `b`, as the box (x0, y0, x1, y1) that holds them all, or false when
// the canvases are the same. Lets a test say where a figure went without drawing it again.
bool diffBox(const Canvas& a, const Canvas& b, int& x0, int& y0, int& x1, int& y1) {
  x0 = kCanvasWidth;
  y0 = kCanvasHeight;
  x1 = -1;
  y1 = -1;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (lit(a, x, y) == lit(b, x, y)) continue;
      x0 = x < x0 ? x : x0;
      y0 = y < y0 ? y : y0;
      x1 = x > x1 ? x : x1;
      y1 = y > y1 ? y : y1;
    }
  }
  return x1 >= 0;
}

// The DEBUG load figure on the voice screen: its text in 5x7 with the last column at 126 and the top
// row at 56, so it keeps 1 px from the right and bottom edges. The text goes at literal places, x 116,
// 110 and 104 for 7%, 42% and 100%, which are 11, 17 and 23 px wide on the 6 px pitch.
void checkLoadFigure() {
  struct Case {
    int percent;
    const char* text;
    int x;
  };
  const Case cases[] = {{7, "7%", 116}, {42, "42%", 110}, {100, "100%", 104}};
  const Canvas line = lineScreen();

  FaceRig r;
  r.settings.debug = true;
  r.ticks(3);
  for (const Case& c : cases) {
    r.loadPercent = c.percent;
    r.tick();  // the same screen, so the new figure shows at once with no dissolve
    Canvas want = line;
    want.drawText(c.x, 56, font5x7(), c.text);
    const std::string what = std::string("face: DEBUG ON, ") + c.text;
    check(sameCanvas(r.live, want), (what + " shows at its place on the next tick").c_str());

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    check(diffBox(r.live, line, x0, y0, x1, y1) && x0 >= c.x && x1 <= 126 && y0 >= 56 && y1 <= 62,
          (what + " lies inside columns " + std::to_string(c.x) + " to 126 and rows 56 to 62").c_str());
    writePgm(r.live, (std::string("oled-voice-load-") + std::to_string(c.percent) + ".pgm").c_str());
  }

  r.loadPercent = 42;
  r.ticks(static_cast<int>(kWalkStepMs / kUiTickMs) + 10);
  Canvas shifted;
  shifted.fillRect(1, 32, kCanvasWidth - 1, 1, true);
  shifted.drawText(110, 56, font5x7(), "42%");
  check(sameCanvas(r.live, shifted), "face: the ribbon walks 1 px after 60 s and the figure stays where it was");
}

// With DEBUG off nothing is drawn, and with it on only the voice screen shows the figure. The readout,
// the toggle readout, MUTED, BYPASS and the menu keep their own frames.
void checkLoadFigureAbsent() {
  FaceRig off;
  off.loadPercent = 42;
  off.ticks(4);
  check(sameCanvas(off.live, lineScreen()), "face: with DEBUG off the voice has no figure");

  FaceRig knob;
  knob.settings.debug = true;
  knob.loadPercent = 42;
  knob.ticks(3);
  knob.turn(4, 1, kTime160);
  knob.ticks(4);
  check(sameCanvas(knob.live, readoutScreen(knob.layout, 4, 1, 160.0f)), "face: the knob readout has no figure with DEBUG on");

  FaceRig toggle;
  toggle.settings.debug = true;
  toggle.loadPercent = 42;
  toggle.ticks(3);
  toggle.flip(5, true);
  toggle.ticks(4);
  check(sameCanvas(toggle.live, toggleScreen(toggle.layout, 5, true)), "face: the toggle readout has no figure with DEBUG on");

  FaceRig muted;
  muted.settings.debug = true;
  muted.loadPercent = 42;
  muted.switchOn = true;
  muted.ticks(5);
  check(matchesGolden(muted.live, "muted-3"), "face: the MUTED screen has no figure with DEBUG on");

  FaceRig bypass;
  bypass.settings.debug = true;
  bypass.settings.topSwitch = TopSwitch::Bypass;
  bypass.loadPercent = 42;
  bypass.switchOn = true;
  bypass.ticks(5);
  Canvas word;
  renderMuted(word, "BYPASS");
  check(sameCanvas(bypass.live, word), "face: the BYPASS screen has no figure with DEBUG on");

  FaceRig menu;
  Menu ref;
  ref.listen();
  menu.settings.debug = true;
  menu.loadPercent = 42;
  menu.ticks(3);
  menu.menu.update(clickInput(), menu.now);
  ref.update(clickInput(), menu.now);
  Canvas menuScreen;
  ref.draw(menu.ctx(), menuScreen);
  menu.ticks(4);
  check(sameCanvas(menu.live, menuScreen), "face: the menu has no figure with DEBUG on");
}

// The voice at three input levels, as an RMS through the whole path: energy, meter, ribbon.
void checkVoiceLevels() {
  struct Case {
    float db;
    int half;
    const char* pgm;
  };
  const Case cases[] = {
      {-40.0f, 11, "oled-voice-quiet.pgm"},
      {-24.0f, 17, "oled-voice-mid.pgm"},
      {-8.0f, 22, "oled-voice-loud.pgm"},
  };
  for (const Case& c : cases) {
    const std::string db = std::to_string(static_cast<int>(c.db)) + " dBFS";
    check(ribbonHalfHeight(level::displayLevel(dbToLinear(c.db))) == c.half, ("voice: " + db + " reads " + std::to_string(c.half) + " px").c_str());

    // 20 ticks of silence, 30 at the level, 14 of silence again fill the 64 levels of history.
    FaceRig r;
    r.ticks(20);
    r.ticks(30, dbToLinear(c.db));
    bool solid = true;
    for (int y = 32 - c.half; y <= 32 + c.half; ++y) solid = solid && lit(r.live, 126, y) && lit(r.live, 127, y);
    check(solid && !lit(r.live, 127, 32 - c.half - 2) && !lit(r.live, 127, 32 + c.half + 2),
          ("voice: the plateau at " + db + " is solid to its height and no taller").c_str());

    r.ticks(14);
    check(lit(r.live, 127, 32) && !lit(r.live, 127, 31) && !lit(r.live, 127, 33) && !lit(r.live, 126, 31),
          ("voice: 14 ticks after " + db + " the 120 ms release is back on the line").c_str());
    writePgm(r.live, c.pgm);
  }
}

}  // namespace

void checkFace() {
  checkMuteDissolveAgainstProof();
  checkReadoutReplacedAgainstProof();
  checkReadoutInAndOut();
  checkToggleAgainstProof();
  checkToggleOverMuted();
  checkToggleGuards();
  checkMutedReadout();
  checkBypassWord();
  checkMenuOnTop();
  checkMenuMovesAndCloses();
  checkMenuListensOnceFaceDraws();
  checkReadoutLeavesWithItsCard();
  checkMenuReopens();
  checkTickGate();
  checkQuietFrames();
  checkInvalidate();
  checkKnobMovedGuards();
  checkLateTick();
  checkWalkOnPanel();
  checkLoadFigure();
  checkLoadFigureAbsent();
  checkVoiceLevels();
}
