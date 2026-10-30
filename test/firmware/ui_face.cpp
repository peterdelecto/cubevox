// Checks the runtime face end to end on the host. Covers the dissolve frames against the proof
// renderer, the readout flowing in and out and leaving with its card, the menu on top and the input
// it takes once the face draws, the tick gate, the burn-in walk, the DEBUG load figure, the voice at
// three levels and the input bar beside the output ribbon against the proof renderer. The proof
// PGMs go to build/oled-*.pgm.

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

// The input meter, drawn from its definition. The box is columns 1 to 9 and rows 8 to 56, 1 px wide.
// Each segment is 2 rows tall at columns 2 to 8, the lowest on rows 54 and 55, with 1 row of gap
// above it. The walk moves the whole meter by (dx, dy).
void addMeterBox(Canvas& c, int dx = 0, int dy = 0) {
  c.fillRect(1 + dx, 8 + dy, 9, 1, true);
  c.fillRect(1 + dx, 56 + dy, 9, 1, true);
  c.fillRect(1 + dx, 8 + dy, 1, 49, true);
  c.fillRect(9 + dx, 8 + dy, 1, 49, true);
}

void addSegments(Canvas& c, int lit, int dx = 0, int dy = 0) {
  for (int n = 0; n < lit; ++n) c.fillRect(2 + dx, 54 - 3 * n + dy, 7, 2, true);
}

// The voice screen in silence, which is the horizon line from the ribbon's first column, column 13,
// to its last, column 126, and the empty meter box. The walk moves both.
Canvas lineScreen(int dx = 0, int dy = 0) {
  Canvas c;
  c.fillRect(13 + dx, 32 + dy, 114, 1, true);
  addMeterBox(c, dx, dy);
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

  // One UI tick, 40 ms on, with the output at `outRms` (the ribbon) and the input at `inRms` (the bar).
  void tick(float outRms = 0.0f, float inRms = 0.0f) {
    now += kUiTickMs;
    face.tick(now, ctx(), energyOfRms(inRms), energyOfRms(outRms), menu, live);
  }

  void ticks(int n, float outRms = 0.0f, float inRms = 0.0f) {
    for (int i = 0; i < n; ++i) tick(outRms, inRms);
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
  toExit.detents = 10;  // from the second item to Exit, the twelfth
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
  toExit.detents = 11;  // from the first item to Exit, the twelfth
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

// The window top falls in real time, so a tick that comes late reads against a lower top and stands
// taller. After a full-scale tick the top falls 0.4 dB in 40 ms and 4.8 dB in 480 ms, so a tick at
// -6 dBFS reads 0.525 (12 px) on time and 0.871 (19 px) late.
void checkLateTick() {
  FaceRig onTime;
  FaceRig late;
  onTime.ticks(3);
  late.ticks(3);
  onTime.tick(1.0f);
  late.tick(1.0f);
  onTime.tick(dbToLinear(-6.0f));
  late.now += 440;  // this tick lands 480 ms after the loud one
  late.tick(dbToLinear(-6.0f));

  // The newest column is solid to `h` either side of the horizon, and the rim lies one row beyond.
  const auto reaches = [](const Canvas& c, int h) {
    return lit(c, 126, 32 - h) && lit(c, 126, 32 + h) && !lit(c, 126, 32 - h - 2) && !lit(c, 126, 32 + h + 2);
  };
  check(reaches(onTime.live, 12), "face: 40 ms after a full-scale tick, a tick at -6 dBFS reads 12 px");
  check(reaches(late.live, 19), "face: 480 ms after it, when the tick came late, a tick at -6 dBFS reads 19 px");
}

void checkWalkOnPanel() {
  FaceRig r;
  r.ticks(10);
  check(sameCanvas(r.live, lineScreen()), "face: the voice starts unshifted");
  r.ticks(static_cast<int>(kWalkStepMs / kUiTickMs) + 10);
  check(sameCanvas(r.live, lineScreen(1, 0)), "face: after 60 s the voice and its meter box move 1 px, with no dissolve");
  r.ticks(static_cast<int>(kWalkStepMs / kUiTickMs));
  check(sameCanvas(r.live, lineScreen(1, 1)), "face: after 120 s they move 1 px more");
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
  Canvas shifted = lineScreen(1, 0);
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

// True when the panel's last column, which the ribbon leaves blank, holds no lit pixel.
bool darkFrom127(const Canvas& c) {
  for (int y = 0; y < kCanvasHeight; ++y) {
    if (lit(c, 127, y)) return false;
  }
  return true;
}

// Columns 0 to 12 hold the meter box and `segments` lit segments, the lowest ones, and nothing else.
bool meterShows(const Canvas& c, int segments) {
  Canvas want;
  addMeterBox(want);
  addSegments(want, segments);
  bool ok = true;
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < 13; ++x) ok = ok && lit(c, x, y) == lit(want, x, y);
  }
  return ok;
}

// The voice at three levels, through the whole path. The output feeds the ribbon, which reads each
// tick's amplitude over the window top's. The top sits at -30 dBFS or at the level, whichever is
// louder, so a steady level under -30 dBFS shows its amplitude over that mark's (-40 dBFS is 0.316
// and 7 px, -36 dBFS is 0.501 and 11 px) and a steady level over it shows full. The input feeds the
// meter, which reads absolute dB from -50 to 0 on 16 segments.
void checkVoiceLevels() {
  struct Case {
    float outDb;
    int half;
    float inDb;
    int segments;
    const char* pgm;
  };
  const Case cases[] = {
      {-40.0f, 7, -40.0f, 3, "oled-voice-quiet.pgm"},
      {-36.0f, 11, -30.0f, 6, "oled-voice-mid.pgm"},
      {-8.0f, 22, -6.0f, 14, "oled-voice-loud.pgm"},
  };
  for (const Case& c : cases) {
    const std::string out = std::to_string(static_cast<int>(c.outDb)) + " dBFS out";
    const std::string in = std::to_string(static_cast<int>(c.inDb)) + " dBFS in";
    level::OutputMeter probe;
    check(ribbonHalfHeight(probe.update(energyOfRms(dbToLinear(c.outDb)), 40.0f)) == c.half,
          ("voice: " + out + " reads " + std::to_string(c.half) + " px").c_str());
    check(meterSegments(level::displayLevel(dbToLinear(c.inDb))) == c.segments,
          ("voice: " + in + " reads " + std::to_string(c.segments) + " segments").c_str());

    // 20 ticks of silence, 30 at the levels, 14 of silence again fill the 57 levels of history.
    FaceRig r;
    r.ticks(20);
    r.ticks(30, dbToLinear(c.outDb), dbToLinear(c.inDb));
    bool solid = true;
    for (int y = 32 - c.half; y <= 32 + c.half; ++y) solid = solid && lit(r.live, 125, y) && lit(r.live, 126, y);
    check(solid && !lit(r.live, 126, 32 - c.half - 2) && !lit(r.live, 126, 32 + c.half + 2) && darkFrom127(r.live),
          ("voice: the plateau at " + out + " is solid to its height and no taller").c_str());
    check(meterShows(r.live, c.segments),
          ("voice: the meter at " + in + " lights the lowest " + std::to_string(c.segments) + " segments inside its box").c_str());

    r.ticks(14);
    check(lit(r.live, 126, 32) && !lit(r.live, 126, 31) && !lit(r.live, 126, 33) && !lit(r.live, 125, 31),
          ("voice: 14 ticks after " + out + " the ribbon is back on the line").c_str());
    check(meterShows(r.live, 0), ("voice: 14 ticks after " + in + " the meter has released to its empty box").c_str());
    writePgm(r.live, c.pgm);
  }
}

// The input reaches the bar and the output reaches the ribbon, each alone.
void checkEachOwnsItsSource() {
  FaceRig inputOnly;
  inputOnly.ticks(10, 0.0f, dbToLinear(-6.0f));
  Canvas lit14 = lineScreen();
  addSegments(lit14, 14);
  check(sameCanvas(inputOnly.live, lit14), "face: an input with a silent output lights 14 segments and leaves the ribbon on the line");

  // Ten levels of 22 px fill columns 107 to 126.
  FaceRig outputOnly;
  outputOnly.ticks(10, dbToLinear(-6.0f), 0.0f);
  check(meterShows(outputOnly.live, 0) && lit(outputOnly.live, 107, 32 - 22) && lit(outputOnly.live, 126, 32 + 22) && !lit(outputOnly.live, 106, 32 - 22) &&
            !lit(outputOnly.live, 127, 32 + 22),
        "face: an output with a silent input fills the ribbon and leaves the meter an empty box");
}

// The idle hiss of the output, under -60 dBFS, draws no ribbon at all. A tick at -70 dBFS every tick
// for longer than the ribbon holds leaves the screen as it is in silence.
void checkOutputHissIsSilence() {
  FaceRig r;
  r.ticks(70, dbToLinear(-70.0f), 0.0f);
  check(sameCanvas(r.live, lineScreen()), "face: an output hiss at -70 dBFS draws the horizon line and nothing else");
}

// The scripted drive of proof scene 12 in tools/oled-proof/oled_motion.cpp. The input holds
// -23 dBFS (9 segments) and the output cycles through twelve levels, one per tick. The golden is the
// frame after tick 80, when the 57 levels of the ribbon all come from the cycle.
constexpr float kScriptInDb = -23.0f;
constexpr float kScriptOutDb[12] = {-30.0f, -24.0f, -19.0f, -12.0f, -8.0f, -14.0f, -20.0f, -26.0f, -32.0f, -40.0f, -52.0f, -60.0f};
constexpr int kScriptTicks = 80;

void checkBarAndRibbonAgainstProof() {
  FaceRig r;
  for (int k = 0; k < kScriptTicks; ++k) r.tick(dbToLinear(kScriptOutDb[k % 12]), dbToLinear(kScriptInDb));
  check(matchesGolden(r.live, "voice-meter-ribbon"), "face: the boxed meter beside the ribbon matches the proof frame");
  check(meterShows(r.live, 9), "face: that frame's meter lights 9 segments inside its box");
  writePgm(r.live, "oled-voice-meter.pgm");
}

// Screens off the voice carry no meter. Each settles to the same frame with a loud input as with a
// silent one, and the meter's place, columns 0 to 5 down to row 56, stays dark on the ones that leave
// it free. The readout and toggle screens light rows 0 to 13 with their title bar, so they leave the
// place free from row 14 down.
Canvas settledOff(void (*setUp)(FaceRig&), float inRms) {
  FaceRig r;
  r.ticks(4, 0.0f, inRms);
  setUp(r);
  r.ticks(6, 0.0f, inRms);
  return r.live;
}

bool meterPlaceDark(const Canvas& c, int fromRow = 8) {
  for (int y = fromRow; y <= 56; ++y) {
    for (int x = 0; x < 6; ++x) {
      if (lit(c, x, y)) return false;
    }
  }
  return true;
}

void checkNoBarOffTheVoice() {
  struct Scene {
    const char* name;
    void (*setUp)(FaceRig&);
    int freeFrom;  // first row from which the screen draws nothing of its own at the meter's columns, -1 for none
  };
  const Scene scenes[] = {
      {"readout", [](FaceRig& r) { r.turn(4, 1, kTime160); }, 14},
      {"toggle", [](FaceRig& r) { r.flip(5, true); }, 14},
      {"MUTED", [](FaceRig& r) { r.switchOn = true; }, 8},
      {"BYPASS", [](FaceRig& r) { r.settings.topSwitch = TopSwitch::Bypass; r.switchOn = true; }, 8},
      {"menu", [](FaceRig& r) { r.menu.update(clickInput(), r.now); }, -1},
  };
  const float loud = dbToLinear(-6.0f);
  for (const Scene& s : scenes) {
    const Canvas withInput = settledOff(s.setUp, loud);
    const Canvas silent = settledOff(s.setUp, 0.0f);
    check(sameCanvas(withInput, silent), (std::string("face: the ") + s.name + " screen is the same with a loud input and a silent one").c_str());
    if (s.freeFrom >= 0) {
      check(meterPlaceDark(withInput, s.freeFrom),
            (std::string("face: the ") + s.name + " screen has nothing at the meter's columns from row " + std::to_string(s.freeFrom)).c_str());
    }
  }

  FaceRig voice;
  voice.ticks(4, 0.0f, loud);
  check(!meterPlaceDark(voice.live) && meterShows(voice.live, 14), "face: the same input does light 14 segments of the meter on the voice screen");
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
  checkEachOwnsItsSource();
  checkOutputHissIsSilence();
  checkBarAndRibbonAgainstProof();
  checkNoBarOffTheVoice();
}
