// Checks the boot splash on the host: the timer rule, the hand-over to the face, the input dropped
// while it shows, the centred version text and the error text after an audio fault reset. Its
// proofs go to build/oled-splash.pgm and build/oled-splash-a1.pgm to -a3.pgm.

#include "support.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "core/audio_fault_cause.h"
#include "core/canvas.h"
#include "core/display.h"
#include "core/fonts.h"
#include "core/layout.h"
#include "core/level.h"
#include "core/panel.h"
#include "core/settings.h"
#include "core/version.h"
#include "ui/face.h"
#include "ui/menu.h"
#include "ui/screens.h"
#include "ui/splash.h"

namespace {

constexpr const char* kVersion = "v2.00.10";

bool sameFrame(const Canvas& a, const Canvas& b) {
  for (int p = 0; p < kCanvasPages; ++p) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (a.page(p)[x] != b.page(p)[x]) return false;
    }
  }
  return true;
}

void checkTimerRule() {
  check(kSplashMs == 2000u, "splash: shown for 2 s");
  check(!splashUp(false, 0, 0), "splash: not up before start()");
  check(splashUp(true, 500, 500), "splash: up at the start");
  check(splashUp(true, 500, 500 + kSplashMs - 1), "splash: up 1 ms before the end");
  check(!splashUp(true, 500, 500 + kSplashMs), "splash: over when the time is up");
  check(!splashUp(true, 500, 500 + 10 * kSplashMs), "splash: stays over");
  const uint32_t startMs = 0xFFFFFF00u;
  check(splashUp(true, startMs, startMs + kSplashMs - 1) && !splashUp(true, startMs, startMs + kSplashMs),
        "splash: the timer survives a millisecond wrap");
}

void checkSequence() {
  Splash splash;
  Canvas canvas;
  check(splash.draw(100, kVersion, canvas, false) == SplashStep::Off, "splash: off until start()");

  splash.start(1000);
  check(splash.draw(1000, kVersion, canvas, false) == SplashStep::Up, "splash: up at once");
  check(splash.draw(1000 + kSplashMs - 1, kVersion, canvas, false) == SplashStep::Up, "splash: up until the time is spent");
  check(splash.draw(1000 + kSplashMs, kVersion, canvas, false) == SplashStep::Ended, "splash: reports Ended on the first pass after");
  check(splash.draw(1000 + kSplashMs + 10, kVersion, canvas, false) == SplashStep::Off, "splash: off after Ended");
  check(splash.draw(1000 + 10 * kSplashMs, kVersion, canvas, false) == SplashStep::Off, "splash: stays off");
}

// The splash stays up past kSplashMs while the power-on wait holds, and ends on the pass it drops.
void checkHold() {
  Splash splash;
  Canvas canvas;
  check(splash.draw(100, kVersion, canvas, true) == SplashStep::Off, "splash: a hold before start() shows nothing");

  splash.start(1000);
  check(splash.draw(1000, kVersion, canvas, true) == SplashStep::Up, "splash: up at once with the hold");
  check(splash.draw(1000 + kSplashMs, kVersion, canvas, true) == SplashStep::Up, "splash: stays up past kSplashMs while held");
  check(splash.draw(1000 + 3 * kSplashMs, kVersion, canvas, true) == SplashStep::Up, "splash: stays up while held, however long");
  check(splash.draw(1000 + 3 * kSplashMs + 1, kVersion, canvas, false) == SplashStep::Ended,
        "splash: reports Ended on the pass the hold drops");
  check(splash.draw(1000 + 3 * kSplashMs + 2, kVersion, canvas, false) == SplashStep::Off, "splash: off after Ended");

  Splash early;
  early.start(0);
  check(early.draw(0, kVersion, canvas, true) == SplashStep::Up, "splash: up with the hold");
  check(early.draw(500, kVersion, canvas, false) == SplashStep::Up, "splash: a hold that drops early still lasts kSplashMs");
  check(early.draw(kSplashMs, kVersion, canvas, false) == SplashStep::Ended, "splash: and ends at kSplashMs");

  Splash plain;
  plain.start(0);
  check(plain.draw(kSplashMs - 1, kVersion, canvas, false) == SplashStep::Up, "splash: no hold, up until kSplashMs");
  check(plain.draw(kSplashMs, kVersion, canvas, false) == SplashStep::Ended, "splash: no hold, ends at kSplashMs");
}

// A warm start after an audio error keeps the splash 6 s and ends it early on a touch. Without an
// error code the splash is the plain one.
void checkWarmError() {
  check(kSplashWarmErrorMs == 6000u, "splash warm: the error splash stays 6 s");
  check(splashUp(true, 500, 500 + kSplashWarmErrorMs - 1, kSplashWarmErrorMs) &&
            !splashUp(true, 500, 500 + kSplashWarmErrorMs, kSplashWarmErrorMs),
        "splash warm: the timer takes its own duration");

  Splash splash;
  Canvas canvas;
  splash.startAfterWarmError(1000);
  check(splash.draw(1000, kVersion, canvas, false, AudioFaultCause::Stalled) == SplashStep::Up, "splash warm: up at once");
  check(splash.draw(1000 + kSplashMs, kVersion, canvas, false, AudioFaultCause::Stalled) == SplashStep::Up,
        "splash warm: still up after the normal 2 s");
  check(splash.draw(1000 + kSplashWarmErrorMs - 1, kVersion, canvas, false, AudioFaultCause::Stalled) == SplashStep::Up,
        "splash warm: up until 6 s");
  check(splash.draw(1000 + kSplashWarmErrorMs, kVersion, canvas, false, AudioFaultCause::Stalled) == SplashStep::Ended,
        "splash warm: ends at 6 s");

  Splash touched;
  touched.startAfterWarmError(0);
  touched.draw(0, kVersion, canvas, false, AudioFaultCause::RepeatedFaults);
  touched.noteTouch();
  check(touched.draw(100, kVersion, canvas, false, AudioFaultCause::RepeatedFaults) == SplashStep::Ended,
        "splash warm: a touch ends it at once");
  check(touched.draw(200, kVersion, canvas, false, AudioFaultCause::RepeatedFaults) == SplashStep::Off,
        "splash warm: and it stays ended");

  Splash held;
  held.startAfterWarmError(0);
  held.draw(0, kVersion, canvas, true, AudioFaultCause::RestartFailed);
  held.noteTouch();
  check(held.draw(10, kVersion, canvas, true, AudioFaultCause::RestartFailed) == SplashStep::Ended,
        "splash warm: a touch ends it through the power-on hold");

  Splash cold;
  cold.start(0);
  cold.draw(0, kVersion, canvas, false, AudioFaultCause::Stalled);
  cold.noteTouch();
  check(cold.draw(100, kVersion, canvas, false, AudioFaultCause::Stalled) == SplashStep::Up,
        "splash warm: a cold start's splash ignores a touch");
  check(cold.draw(kSplashMs, kVersion, canvas, false, AudioFaultCause::Stalled) == SplashStep::Ended,
        "splash warm: and keeps its 2 s");

  Splash again;
  again.startAfterWarmError(0);
  again.noteTouch();
  again.start(5000);
  check(again.draw(5000, kVersion, canvas, false) == SplashStep::Up, "splash warm: a fresh start() clears the touch");

  Splash plainWarm;
  plainWarm.start(0);  // a warm start with no error code keeps the plain splash
  check(plainWarm.draw(kSplashMs - 1, kVersion, canvas, false) == SplashStep::Up &&
            plainWarm.draw(kSplashMs, kVersion, canvas, false) == SplashStep::Ended,
        "splash warm: no error code keeps the normal 2 s");
}

void checkPaintedOnce() {
  Splash splash;
  Canvas canvas;
  splash.start(0);
  splash.draw(0, kVersion, canvas, false);
  clearAllDirty(canvas);
  splash.draw(500, kVersion, canvas, false);
  check(canvas.dirtyPages() == 0, "splash: the frame is painted once, not on every pass");
}

void checkCentred() {
  Splash splash;
  Canvas canvas;
  splash.start(0);
  splash.draw(0, kVersion, canvas, false);

  const FontDesc& font = font9x12();
  const int width = Canvas::textWidth(font, kVersion);
  const int x = (kCanvasWidth - width) / 2;
  const int y = (kCanvasHeight - font.height) / 2;
  Canvas expected;
  expected.drawText(x, y, font, kVersion);
  check(sameFrame(canvas, expected), "splash: the version is drawn in the 9x12 font at the centre");
  check(x >= 0 && (kCanvasWidth - width) - 2 * x <= 1, "splash: equal margins left and right, give or take a pixel");
  check((kCanvasHeight - font.height) - 2 * y <= 1, "splash: equal margins above and below, give or take a pixel");
}

void checkReplacesWhatWasThere() {
  Splash splash;
  Canvas canvas;
  canvas.fillRect(0, 0, kCanvasWidth, kCanvasHeight, true);
  splash.start(0);
  splash.draw(0, kVersion, canvas, false);
  Canvas expected;
  expected.drawText((kCanvasWidth - Canvas::textWidth(font9x12(), kVersion)) / 2, (kCanvasHeight - font9x12().height) / 2,
                    font9x12(), kVersion);
  check(sameFrame(canvas, expected), "splash: it clears the panel before it draws");
}

// A version wider than the panel starts at the left edge and clips instead of drawing at a negative x.
void checkTooWide() {
  Splash splash;
  Canvas canvas;
  splash.start(0);
  const char* wide = "v2.00.10-bench-with-a-long-tail";
  splash.draw(0, wide, canvas, false);
  Canvas expected;
  expected.drawText(0, (kCanvasHeight - font9x12().height) / 2, font9x12(), wide);
  check(sameFrame(canvas, expected), "splash: text wider than the panel starts at the left edge");
}

bool lit(const Canvas& canvas, int x, int y) { return ((canvas.page(y / 8)[x] >> (y % 8)) & 1u) != 0; }

// The lit pixels' bounds over rows [top, bottom). An empty band has right < left.
struct Ink {
  int left = kCanvasWidth;
  int right = -1;
  int top = kCanvasHeight;
  int bottom = -1;
};

Ink inkIn(const Canvas& canvas, int top, int bottom) {
  Ink ink;
  for (int y = top; y < bottom; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) {
      if (!lit(canvas, x, y)) continue;
      ink.left = std::min(ink.left, x);
      ink.right = std::max(ink.right, x);
      ink.top = std::min(ink.top, y);
      ink.bottom = std::max(ink.bottom, y);
    }
  }
  return ink;
}

bool centredInk(const Ink& ink) {
  const int leftMargin = ink.left;
  const int rightMargin = kCanvasWidth - 1 - ink.right;
  return ink.right >= ink.left && std::abs(leftMargin - rightMargin) <= 1;
}

Canvas splashFrame(AudioFaultCause cause, const char* version = kVersion) {
  Splash splash;
  Canvas canvas;
  splash.start(0);
  splash.draw(0, version, canvas, false, cause);
  return canvas;
}

// Without a cause the splash is the version alone, as it was before the error text existed.
void checkPlainUnchanged() {
  Splash splash;
  Canvas byDefault;
  splash.start(0);
  splash.draw(0, kVersion, byDefault, false);
  Canvas expected;
  expected.drawText((kCanvasWidth - Canvas::textWidth(font9x12(), kVersion)) / 2, (kCanvasHeight - font9x12().height) / 2,
                    font9x12(), kVersion);
  check(sameFrame(splashFrame(AudioFaultCause::None), expected) && sameFrame(byDefault, expected),
        "splash: with no cause it shows the version alone, pixel for pixel");
}

void check(bool ok, const std::string& what) { ::check(ok, what.c_str()); }

// The 5x7 ink on text row `row`.
Ink rowInk(const Canvas& canvas, int row) { return inkIn(canvas, row * kDisplayRowPx, (row + 1) * kDisplayRowPx); }

bool keepsPad(const Ink& ink) {
  return ink.top >= kScreenPad && ink.left >= kScreenPad && ink.right <= kCanvasWidth - 1 - kScreenPad &&
         ink.bottom <= kCanvasHeight - 1 - kScreenPad;
}

// One cause's splash: the wording, the code that DIAG.TXT also names, and each line on its row,
// centred, clear of the edge, of the version and of each other, with the version where it was.
void checkFaultText(AudioFaultCause cause, const char* line0, const char* line1, const char* code) {
  const std::string name = std::string("splash ") + audioFaultCode(cause) + ": ";
  const SplashFaultText* text = splashFaultText(cause);
  check(text != nullptr && std::string(text->explanation[0]) == line0 && std::string(text->explanation[1]) == line1 &&
            std::string(text->code) == code,
        name + "the wording");
  check(std::string(code) == std::string("ERROR ") + audioFaultCode(cause) &&
            std::string(audioFaultDiagText(cause)).rfind(std::string(audioFaultCode(cause)) + " ", 0) == 0,
        name + "the screen and DIAG.TXT name the same code");

  const Canvas frame = splashFrame(cause);
  const Canvas plain = splashFrame(AudioFaultCause::None);
  const Ink version = inkIn(plain, 0, kCanvasHeight);
  const Ink first = rowInk(frame, 0);
  const Ink second = rowInk(frame, 2);
  const Ink codeInk = rowInk(frame, 6);

  // Each line starts where its first glyph's ink lands.
  const FontDesc& font = font5x7();
  const auto lineX = [&](const Ink& ink, const char* s) { return ink.left - font.inkLeft[glyphIndex(font, s[0])]; };
  Canvas expected;
  expected.copyFrom(plain);
  expected.drawText(lineX(first, line0), kScreenPad, font, line0);
  expected.drawText(lineX(second, line1), 2 * kDisplayRowPx + kScreenPad, font, line1);
  expected.drawText(lineX(codeInk, code), 6 * kDisplayRowPx + kScreenPad, font, code);
  check(sameFrame(frame, expected), name + "the lines in 5x7 on rows 0, 2 and 6 around the version, nothing else");

  check(keepsPad(first) && keepsPad(second) && keepsPad(codeInk), name + "every line keeps 1 px from the panel edge");
  check(centredInk(first) && centredInk(second) && centredInk(codeInk), name + "each line is centred, give or take a pixel");
  check(second.top - first.bottom - 1 >= 2, name + "the two explanation lines stand clear of each other");
  check(version.top - second.bottom - 1 >= 2 && codeInk.top - version.bottom - 1 >= 2,
        name + "at least 2 px clear above and below the version");
}

void checkFaultTexts() {
  checkFaultText(AudioFaultCause::RepeatedFaults, "THE AUDIO LINK FAILED", "4 TIMES IN A MINUTE.", "ERROR A1");
  checkFaultText(AudioFaultCause::RestartFailed, "THE AUDIO LINK DID", "NOT START AGAIN.", "ERROR A2");
  checkFaultText(AudioFaultCause::Stalled, "THE AUDIO STREAM", "STOPPED MOVING.", "ERROR A3");
  checkFaultText(AudioFaultCause::StartFailed, "THE AUDIO DID NOT", "START. TRYING AGAIN.", "ERROR A4");
  checkFaultText(AudioFaultCause::Crashed, "THE BOX CRASHED AND", "RESTARTED ITSELF.", "ERROR C1");
  checkFaultText(AudioFaultCause::Hung, "THE BOX FROZE AND", "RESTARTED ITSELF.", "ERROR W1");
}

// Every code's splash stays 6 s on a warm start and ends on a touch, and no two codes share a frame.
void checkEveryCodeWarm() {
  const AudioFaultCause codes[] = {AudioFaultCause::RepeatedFaults, AudioFaultCause::RestartFailed,
                                   AudioFaultCause::Stalled,        AudioFaultCause::StartFailed,
                                   AudioFaultCause::Crashed,        AudioFaultCause::Hung};
  bool sixSeconds = true;
  bool touchEnds = true;
  bool distinct = true;
  Canvas canvas;
  for (size_t i = 0; i < 6; ++i) {
    Splash timed;
    timed.startAfterWarmError(0);
    sixSeconds = sixSeconds && timed.draw(kSplashWarmErrorMs - 1, kVersion, canvas, false, codes[i]) == SplashStep::Up &&
                 timed.draw(kSplashWarmErrorMs, kVersion, canvas, false, codes[i]) == SplashStep::Ended;
    Splash touched;
    touched.startAfterWarmError(0);
    touched.draw(0, kVersion, canvas, false, codes[i]);
    touched.noteTouch();
    touchEnds = touchEnds && touched.draw(1, kVersion, canvas, false, codes[i]) == SplashStep::Ended;
    for (size_t j = i + 1; j < 6; ++j) distinct = distinct && !sameFrame(splashFrame(codes[i]), splashFrame(codes[j]));
  }
  check(sixSeconds, "splash codes: each of the six stays 6 s on a warm start");
  check(touchEnds, "splash codes: a touch ends each of the six");
  check(distinct, "splash codes: the six screens differ");
}

// The sketch's order each pass: the menu takes the encoder's input, then the splash draws, and the
// face ticks once the splash has gone and the panel has taken its frame. A click or a turn while the
// splash shows opens nothing; the first click once the face shows opens the menu.
void checkInputDuringSplash() {
  Splash splash;
  Face face;
  Menu menu;
  Canvas live;
  const Layout layout = defaultLayout();
  const PanelReading panel{};
  const Settings settings{};
  const UiContext ctx{layout, panel, settings, false};
  UiInput click;
  click.click = true;
  UiInput turn;
  turn.detents = 2;

  splash.start(1000);
  bool quiet = true;
  for (uint32_t now = 1000; now < 1000 + kSplashMs; now += 50) {
    quiet = quiet && menu.update(click, now, layout) == UiAction::None && menu.update(turn, now, layout) == UiAction::None;
    quiet = quiet && splash.draw(now, kVersion, live, false) == SplashStep::Up;
    clearAllDirty(live);  // the panel takes the frame
  }
  check(quiet && !menu.open(), "splash: clicks and turns while it shows open no menu and flip no setting");

  const uint32_t end = 1000 + kSplashMs;
  check(splash.draw(end, kVersion, live, false) == SplashStep::Ended, "splash: hands over on time");
  face.invalidate();
  check(face.due(end + kUiTickMs, live), "splash: the face ticks once the splash has gone");
  const ScopeRing ring;
  face.tick(end + kUiTickMs, ctx, level::InputEnergy(), ring, menu, live);
  menu.update(click, end + kUiTickMs + 1, layout);
  check(menu.open(), "splash: the first click once the face shows opens the menu");
}

// The splash as the box shows it, with this build's version, for the owner's proof.
void writeProof() {
  Splash splash;
  Canvas canvas;
  splash.start(0);
  splash.draw(0, kFirmwareVersion, canvas, false);
  writePgm(canvas, "oled-splash.pgm");

  writePgm(splashFrame(AudioFaultCause::RepeatedFaults, kFirmwareVersion), "oled-splash-a1.pgm");
  writePgm(splashFrame(AudioFaultCause::RestartFailed, kFirmwareVersion), "oled-splash-a2.pgm");
  writePgm(splashFrame(AudioFaultCause::Stalled, kFirmwareVersion), "oled-splash-a3.pgm");
  writePgm(splashFrame(AudioFaultCause::StartFailed, kFirmwareVersion), "oled-splash-a4.pgm");
  writePgm(splashFrame(AudioFaultCause::Crashed, kFirmwareVersion), "oled-splash-c1.pgm");
  writePgm(splashFrame(AudioFaultCause::Hung, kFirmwareVersion), "oled-splash-w1.pgm");
}

}  // namespace

void checkSplash() {
  checkTimerRule();
  checkSequence();
  checkHold();
  checkWarmError();
  checkPaintedOnce();
  checkCentred();
  checkReplacesWhatWasThere();
  checkTooWide();
  checkPlainUnchanged();
  checkFaultTexts();
  checkEveryCodeWarm();
  checkInputDuringSplash();
  writeProof();
}
