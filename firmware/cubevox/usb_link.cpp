#include "usb_link.h"

#include <Arduino.h>

#include "controls.h"
#include "pins.h"

#if !defined(USBCON) || !defined(USBD_USE_CDC)
#error "build with usb=CDCgen: Serial must be the USB CDC port"
#endif

// The core's PCD MSP configures every pin in this table. Only D+ and D- belong to the OTG
// core here; PA9 stays a plain input (native VBUS sensing is off), PA8 and PA10 are unused.
const PinMap PinMap_USB_OTG_FS[] = {
    {PA_11, USB_OTG_FS, STM_PIN_DATA(STM_MODE_AF_PP, LL_GPIO_PULL_UP, GPIO_AF10_OTG1_FS)},
    {PA_12, USB_OTG_FS, STM_PIN_DATA(STM_MODE_AF_PP, LL_GPIO_PULL_UP, GPIO_AF10_OTG1_FS)},
    {NC, NP, 0},
};

namespace {

DebouncedInput gVbus;
bool gStarted = false;
bool gTerminal = false;

bool vbusPresent() { return digitalRead(pins::kVbusSense) == HIGH; }

}  // namespace

void usbLinkInit() {
  pinMode(pins::kVbusSense, INPUT);  // no pull: the divider sets the level (Astra item 8)
  gVbus.reset(vbusPresent());
}

void usbLinkPoll(uint32_t nowMs) {
  gVbus.update(vbusPresent(), nowMs, kVbusDebounceMs);
  const bool wanted = gVbus.level();
  if (wanted == gStarted) return;

  if (wanted) {
    Serial.begin();  // USBD init holds SDIS, USBD_Start clears it
  } else {
    Serial.end();  // HAL_PCD_Stop sets SDIS before the device is torn down
    gTerminal = false;
  }
  gStarted = wanted;
}

bool usbLinkTerminalOpened() {
  const bool dtr = gStarted && Serial.dtr();
  const bool rose = dtr && !gTerminal;
  gTerminal = dtr;
  return rose;
}
