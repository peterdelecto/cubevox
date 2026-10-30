// Checks the boot splash on the host: the timer rule, the hand-over to the face, the input dropped
// while it shows, and the centred version text. Its proof goes to build/oled-splash.pgm.

#include "support.h"

#include <cstdint>

#include "core/canvas.h"
#include "core/fonts.h"
#include "core/layout.h"
#include "core/level.h"
#include "core/panel.h"
#include "core/settings.h"
#include "core/version.h"
#include "ui/face.h"
#include "ui/menu.h"
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
  check(splash.draw(100, kVersion, canvas) == SplashStep::Off, "splash: off until start()");

  splash.start(1000);
  check(splash.draw(1000, kVersion, canvas) == SplashStep::Up, "splash: up at once");
  check(splash.draw(1000 + kSplashMs - 1, kVersion, canvas) == SplashStep::Up, "splash: up until the time is spent");
  check(splash.draw(1000 + kSplashMs, kVersion, canvas) == SplashStep::Ended, "splash: reports Ended on the first pass after");
  check(splash.draw(1000 + kSplashMs + 10, kVersion, canvas) == SplashStep::Off, "splash: off after Ended");
  check(splash.draw(1000 + 10 * kSplashMs, kVersion, canvas) == SplashStep::Off, "splash: stays off");
}

void checkPaintedOnce() {
  Splash splash;
  Canvas canvas;
  splash.start(0);
  splash.draw(0, kVersion, canvas);
  clearAllDirty(canvas);
  splash.draw(500, kVersion, canvas);
  check(canvas.dirtyPages() == 0, "splash: the frame is painted once, not on every pass");
}

void checkCentred() {
  Splash splash;
  Canvas canvas;
  splash.start(0);
  splash.draw(0, kVersion, canvas);

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
  splash.draw(0, kVersion, canvas);
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
  splash.draw(0, wide, canvas);
  Canvas expected;
  expected.drawText(0, (kCanvasHeight - font9x12().height) / 2, font9x12(), wide);
  check(sameFrame(canvas, expected), "splash: text wider than the panel starts at the left edge");
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
    quiet = quiet && menu.update(click, now) == UiAction::None && menu.update(turn, now) == UiAction::None;
    quiet = quiet && splash.draw(now, kVersion, live) == SplashStep::Up;
    clearAllDirty(live);  // the panel takes the frame
  }
  check(quiet && !menu.open(), "splash: clicks and turns while it shows open no menu and flip no setting");

  const uint32_t end = 1000 + kSplashMs;
  check(splash.draw(end, kVersion, live) == SplashStep::Ended, "splash: hands over on time");
  face.invalidate();
  check(face.due(end + kUiTickMs, live), "splash: the face ticks once the splash has gone");
  face.tick(end + kUiTickMs, ctx, level::InputEnergy(), level::OutputEnergy(), menu, live);
  menu.update(click, end + kUiTickMs + 1);
  check(menu.open(), "splash: the first click once the face shows opens the menu");
}

// The splash as the box shows it, with this build's version, for the owner's proof.
void writeProof() {
  Splash splash;
  Canvas canvas;
  splash.start(0);
  splash.draw(0, kFirmwareVersion, canvas);
  writePgm(canvas, "oled-splash.pgm");
}

}  // namespace

void checkSplash() {
  checkTimerRule();
  checkSequence();
  checkPaintedOnce();
  checkCentred();
  checkReplacesWhatWasThere();
  checkTooWide();
  checkInputDuringSplash();
  writeProof();
}
