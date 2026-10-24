#include "ui/splash.h"

#include "core/fonts.h"

namespace {

// Centres the text box on the panel. Text wider than the panel starts at the left edge.
void paint(const char* version, Canvas& canvas) {
  const FontDesc& font = font9x12();
  const int width = Canvas::textWidth(font, version);
  const int x = width < kCanvasWidth ? (kCanvasWidth - width) / 2 : 0;
  const int y = (kCanvasHeight - font.height) / 2;
  canvas.clear();
  canvas.drawText(x, y, font, version);
}

}  // namespace

SplashStep Splash::draw(uint32_t nowMs, const char* version, Canvas& canvas) {
  const bool up = splashUp(started_, startMs_, nowMs);
  if (handoff_.update(up)) return SplashStep::Ended;
  if (!up) return SplashStep::Off;
  if (!painted_) {
    paint(version, canvas);
    painted_ = true;
  }
  return SplashStep::Up;
}
