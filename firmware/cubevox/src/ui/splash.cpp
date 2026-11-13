#include "ui/splash.h"

#include <string.h>

#include "core/fonts.h"
#include "ui/screens.h"

namespace {

// Centres `text` across the panel. Text wider than the panel starts at the left edge.
int centredX(const FontDesc& font, const char* text) {
  const int width = Canvas::textWidth(font, text);
  return width < kCanvasWidth ? (kCanvasWidth - width) / 2 : 0;
}

// Centres a 5x7 line on its ink. The font keeps its fixed pitch, so the end cells carry blank
// columns, and a closing period leaves most of its cell blank.
int inkCentredX(const char* text) {
  const FontDesc& font = font5x7();
  const int last = static_cast<int>(strlen(text)) - 1;
  const int first = glyphIndex(font, text[0]);
  const int end = glyphIndex(font, text[last]);
  const int inkStart = font.inkLeft[first];
  const int inkEnd = last * kFont5x7Advance + font.inkLeft[end] + font.inkWidth[end];
  return (kCanvasWidth - (inkEnd - inkStart)) / 2 - inkStart;
}

// A 5x7 line on text row `row`, kScreenPad below the row's top, so row 0 keeps 1 px from the edge.
void paintLine(int row, const char* text, Canvas& canvas) {
  canvas.drawText(inkCentredX(text), row * kDisplayRowPx + kScreenPad, font5x7(), text);
}

// The version sits in the centre of the panel, with or without a fault's text.
void paint(const char* version, AudioFaultCause cause, Canvas& canvas) {
  const FontDesc& font = font9x12();
  canvas.clear();
  canvas.drawText(centredX(font, version), (kCanvasHeight - font.height) / 2, font, version);
  const SplashFaultText* text = splashFaultText(cause);
  if (text == nullptr) return;
  paintLine(0, text->explanation[0], canvas);
  paintLine(2, text->explanation[1], canvas);
  paintLine(6, text->code, canvas);
}

}  // namespace

SplashStep Splash::draw(uint32_t nowMs, const char* version, Canvas& canvas, bool hold, AudioFaultCause cause) {
  const bool up = started_ && !touched_ && (splashUp(started_, startMs_, nowMs, durationMs_) || hold);
  if (handoff_.update(up)) return SplashStep::Ended;
  if (!up) return SplashStep::Off;
  if (!painted_) {
    paint(version, cause, canvas);
    painted_ = true;
  }
  return SplashStep::Up;
}
