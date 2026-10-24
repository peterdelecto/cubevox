// The canvas goes out one page per flushStep(). core/display.h's PanelLink decides each pass whether
// to send, restart the panel or wait, so a missing or failing panel never holds the loop longer than
// one bounded transfer.

#include "board/display_sh1106.h"

#include <Arduino.h>

#include "board/pins.h"
#include "board/serial_link.h"

namespace {

// 400 kHz Fast-mode from a 120 MHz I2C2 kernel clock, the PCLK1 of sysclock.cpp's 480 MHz profile
// (I2C123SEL keeps its reset value): PRESC 2 (25 ns), SCLDEL 14 (375 ns setup), SDADEL 2 (50 ns hold),
// SCLH 31, SCLL 51, the value the STM32 core's Wire computed for that clock. The 360 MHz profile's
// 90 MHz PCLK1 only lengthens every one of these times, which Fast-mode allows. A faster clock would
// overrun the panel, so panelBus() refuses one.
constexpr uint32_t kTimingr = 0x20E21F33;
constexpr uint32_t kTimingrKernelHz = 120000000;

static_assert(kCanvasWidth == CV_SH1106_PAGE_BYTES && kCanvasPages == CV_SH1106_PAGES,
              "a canvas page must be one panel page");

// The bus from pins.h, with its clocks on. False, with the reason printed, when the pins or the clock
// do not fit the driver; the panel then stays untouched.
bool panelBus(CvSh1106Bus& bus) {
  const uint32_t port = pins::portIndexOf(pins::kOledScl);
  if (pins::portIndexOf(pins::kOledSda) != port) {
    serialOut().println("[ERROR] OLED: SCL and SDA must share a port, so the panel stays off");
    return false;
  }
  const uint32_t kernelHz = HAL_RCC_GetPCLK1Freq();
  if (__HAL_RCC_GET_I2C2_SOURCE() != RCC_I2C2CLKSOURCE_D2PCLK1 || kernelHz > kTimingrKernelHz) {
    serialOut().print("[ERROR] OLED: I2C2 needs PCLK1 at 120 MHz or less and has ");
    serialOut().print(kernelHz);
    serialOut().println(" Hz, so the panel stays off");
    return false;
  }
  set_GPIO_Port_Clock(port);
  __HAL_RCC_I2C2_CLK_ENABLE();
  bus.i2c = I2C2;
  bus.port = pins::portOf(pins::kOledScl);
  bus.scl = static_cast<uint16_t>(pins::maskOf(pins::kOledScl));
  bus.sda = static_cast<uint16_t>(pins::maskOf(pins::kOledSda));
  bus.alternate = GPIO_AF4_I2C2;  // PB10 and PB11 reach I2C2 on AF4
  bus.timingr = kTimingr;
  return true;
}

class Sh1106Display : public Display {
 public:
  bool begin();
  bool up() const { return ready_ && link_.up(); }
  void flushStep(Canvas& canvas) override;

 private:
  void restart(Canvas& canvas, uint32_t nowMs);
  void send(Canvas& canvas, uint32_t nowMs);
  void note(PanelEvent event) const;

  CvSh1106Bus bus_ = {};
  PanelLink link_;
  bool ready_ = false;      // the bus fits the driver
  bool displayOn_ = false;  // since the last start
};

// Several starts at boot cover a panel that is still powering up. Then the retry window takes over.
bool Sh1106Display::begin() {
  ready_ = panelBus(bus_);
  if (!ready_) return false;
  bool ok = false;
  for (int i = 0; i < kPanelStartTries && !ok; ++i) {
    if (i > 0) delay(kPanelStartGapMs);
    ok = cvSh1106Start(&bus_);
  }
  note(link_.started(ok, millis()));
  return ok;
}

void Sh1106Display::flushStep(Canvas& canvas) {
  if (!ready_) return;
  const uint32_t nowMs = millis();
  switch (link_.step(nowMs)) {
    case PanelStep::Send: send(canvas, nowMs); break;
    case PanelStep::Restart: restart(canvas, nowMs); break;
    case PanelStep::Wait: break;
  }
}

// A start leaves the display off and its RAM unknown, so the whole canvas goes out again.
void Sh1106Display::restart(Canvas& canvas, uint32_t nowMs) {
  displayOn_ = false;
  const bool ok = cvSh1106Start(&bus_);
  if (ok) canvas.markAllDirty();
  note(link_.started(ok, nowMs));
}

// The lowest dirty page goes out, and a failed one stays dirty for the next try. After a start the
// display turns on in the pass that sends the frame's last page, so a face that draws again at once
// cannot hold it off.
void Sh1106Display::send(Canvas& canvas, uint32_t nowMs) {
  const int page = canvas.firstDirtyPage();
  if (page >= 0) {
    const bool ok = cvSh1106Page(&bus_, static_cast<uint8_t>(page), canvas.page(page));
    note(link_.wrote(ok, nowMs));
    if (!ok) return;
    canvas.clearDirty(page);
  }
  if (displayOn_ || canvas.firstDirtyPage() >= 0) return;
  displayOn_ = cvSh1106DisplayOn(&bus_);
  note(link_.shown(displayOn_, nowMs));
}

void Sh1106Display::note(PanelEvent event) const {
  if (event == PanelEvent::Lost) {
    serialOut().println("[WARN] OLED not answering, retrying once a second");
  } else if (event == PanelEvent::Back) {
    serialOut().print("[oled] OLED answering again after ");
    serialOut().print(link_.failures());
    serialOut().println(" failed tries");
  }
}

Sh1106Display& panel() {
  static Sh1106Display display;
  static bool begun = false;
  if (!begun) {
    begun = true;
    display.begin();
  }
  return display;
}

}  // namespace

bool sh1106Present() { return panel().up(); }

Display& sh1106Display() { return panel(); }
