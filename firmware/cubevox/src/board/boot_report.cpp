#include "board/boot_report.h"

#include <Arduino.h>

#include "board/audio.h"
#include "board/build_kind.h"
#include "board/display_sh1106.h"
#include "board/fault.h"
#include "board/pins.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "board/sysclock.h"
#include "core/version.h"

namespace {

constexpr uint32_t kFpuAccessMask = 0xFu << 20;  // CPACR CP10 and CP11 full access

const char* onOff(bool on) { return on ? "on" : "OFF"; }

void reportCpu(Print& out) {
  out.print("[boot] icache ");
  out.print(onOff((SCB->CCR & SCB_CCR_IC_Msk) != 0));
  out.print(", dcache ");
  out.print(onOff((SCB->CCR & SCB_CCR_DC_Msk) != 0));
  out.print(", fpu ");
  out.println(onOff((SCB->CPACR & kFpuAccessMask) == kFpuAccessMask));
}

void reportOled(Print& out, bool found) {
  if (!found) {
    out.println("[WARN] OLED not found, serial display");
    return;
  }
  out.print("[boot] OLED at 0x");
  out.println(kOledAddress, HEX);
}

}  // namespace

void bootReportPrint(bool oledFound) {
  Print& out = serialOut();
  out.print("[boot] cubevox firmware ");
  out.println(kFirmwareVersion);
  out.println(kBenchBuild ? "[boot] build bench" : "[boot] build release");
  faultReport(out);
  recordReportBoot(out);
  clockReport(out);
  pins::report(out);
  reportCpu(out);
  reportOled(out, oledFound);
  audioReport(out);
}
