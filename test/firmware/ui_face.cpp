// Checks the runtime face end to end on the host. Covers the dissolve frames against the proof
// renderer, the readout flowing in and out, the menu on top, the tick gate, the burn-in walk and
// the voice at three input levels. The proof PGMs go to build/oled-*.pgm.

#include "support.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/canvas.h"
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

  UiContext ctx() const { return UiContext{layout, panel, settings, switchOn}; }

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
};

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

void checkMenuOnTop() {
  FaceRig r;
  Menu ref;  // takes the same input, and draws the frame the face must show
  r.ticks(3);
  r.turn(4, 1, kTime160);
  r.ticks(4);
  const Canvas readout = r.live;

  UiInput click;
  click.click = true;
  r.menu.update(click, r.now);
  ref.update(click, r.now);
  Canvas menuScreen;
  ref.draw(r.ctx(), menuScreen);

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

  // The cursor moves at the next tick, as the menu draws it.
  UiInput down;
  down.detents = 1;
  r.menu.update(down, r.now);
  ref.update(down, r.now);
  Canvas moved;
  ref.draw(r.ctx(), moved);
  r.tick();
  check(sameCanvas(r.live, moved) && !sameCanvas(moved, menuScreen), "face: a cursor move shows at the next tick, with no dissolve");

  // Exit closes the menu, and the voice dissolves back from the last menu frame.
  UiInput toExit;
  toExit.detents = 5;
  r.menu.update(toExit, r.now);
  r.menu.update(click, r.now);
  check(!r.menu.open(), "face: Exit closed the menu");
  const Canvas lastMenu = r.live;
  r.tick();
  check(sameCanvas(r.live, blend(lastMenu, lineScreen(), kStep1Level)), "face: the closed menu dissolves back to the voice");
  r.ticks(2);
  check(sameCanvas(r.live, lineScreen()), "face: and the voice settles");
}

// The menu caches its last frame. Reopening it after another screen drew in between must still show it.
void checkMenuReopens() {
  FaceRig r;
  Menu ref;
  r.ticks(3);
  UiInput click;
  click.click = true;
  UiInput toExit;
  toExit.detents = 6;
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
  checkMutedReadout();
  checkBypassWord();
  checkMenuOnTop();
  checkMenuReopens();
  checkTickGate();
  checkQuietFrames();
  checkInvalidate();
  checkKnobMovedGuards();
  checkLateTick();
  checkWalkOnPanel();
  checkVoiceLevels();
}
