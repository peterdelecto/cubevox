#include "board/boot_report.h"

#include <Arduino.h>

#include "board/audio.h"
#include "board/boot_block_access.h"
#include "board/build_kind.h"
#include "board/controls.h"
#include "board/device_serial.h"
#include "board/diag_service.h"
#include "board/display_sh1106.h"
#include "board/fault.h"
#include "board/pins.h"
#include "board/power_sense.h"
#include "board/record_service.h"
#include "board/serial_link.h"
#include "board/sysclock.h"
#include "core/version.h"

namespace {

constexpr uint32_t kFpuAccessMask = 0xFu << 20;  // CPACR CP10 and CP11 full access
// The bootloader's option byte step sets BOR level 3 (2.7 V) and write-protects bank 1 sector 0.
// A set bit in WPSN_CUR1 leaves its sector writable (RM0433, FLASH_WPSN_CUR1R).
constexpr uint32_t kBorLevelWanted = 3;
constexpr uint32_t kSector0Writable = 1u << FLASH_WPSN_WRPSN_Pos;

const char* onOff(bool on) { return on ? "on" : "OFF"; }

void printHex(Print& out, const char* label, uint32_t v) {
  out.print(label);
  out.print("=0x");
  out.print(v, HEX);
}

// The MCU id, so each bench log names its box.
void reportSerial(Print& out) {
  out.print("[boot] serial ");
  out.println(deviceSerial());
}

// The option byte step's result and the values in force, in the words DIAG.TXT uses.
void reportOptionBytes(Print& out) {
  const uint32_t result = bootBlockWritten() ? bootBlock().optionBytes : CV_DIAG_UNKNOWN_U32;
  const uint32_t borLevel = (FLASH->OPTSR_CUR & FLASH_OPTSR_BOR_LEV) >> FLASH_OPTSR_BOR_LEV_Pos;
  const bool sector0Protected = (FLASH->WPSN_CUR1 & kSector0Writable) == 0u;
  const bool asWanted = !cvOptionResultFailed(result) && borLevel == kBorLevelWanted && sector0Protected;
  out.print(asWanted ? "[boot] option bytes: " : "[WARN] option bytes: ");
  out.print(cvOptionResultName(result));
  out.print(", BOR level ");
  out.print(borLevel);
  out.println(sector0Protected ? ", sector 0 protected" : ", sector 0 writable");
}

// The bootloader's own last fault, in the words DIAG.TXT uses, when it left one.
void reportBootloaderFault(Print& out) {
  if (!bootBlockWritten()) return;
  const CvBootBlock& block = bootBlock();
  if ((block.bootFaultPc | block.bootFaultCfsr | block.bootFaultHfsr | block.bootFaultBfar) == 0u) return;
  out.print("[WARN] boot fault:");
  printHex(out, " pc", block.bootFaultPc);
  printHex(out, " cfsr", block.bootFaultCfsr);
  printHex(out, " hfsr", block.bootFaultHfsr);
  printHex(out, " bfar", block.bootFaultBfar);
  out.println();
}

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

void bootReport(bool oledFound) {
  Print& out = serialOut();
  out.print("[boot] cubevox firmware ");
  out.println(kFirmwareVersion);
  out.println(kBenchBuild ? "[boot] build bench" : "[boot] build release");
  reportSerial(out);
  faultReport(out);
  reportOptionBytes(out);
  reportBootloaderFault(out);
  recordReportBoot(out);
  diagReportBoot(out);
  clockReport(out);
  pins::report(out);
  reportCpu(out);
  reportOled(out, oledFound);
  audioReport(out);
  powerSenseReport(out);
  controlsReport(out);
}
