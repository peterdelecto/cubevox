#include "board/fault.h"

#include <array>

#include "stm32yyxx_ll_iwdg.h"  // the core leaves the IWDG HAL module out; LL is what it uses

#include "board/audio.h"
#include "board/flash_bank2.h"
#include "board/mute.h"
#include "board/pins.h"
#include "board/serial_link.h"
#include "h7_block_mem.h"

namespace {

constexpr uint32_t kMagic = 0x46415533;  // "FAU3"

// Watchdog period. loop() kicks it; the audio interrupt never does.
constexpr uint32_t kWatchdogMs = 2000;
// Longest the watchdog may take to accept its period.
constexpr uint32_t kWatchdogReadyMs = 100;
// Longest the audio block counter may sit still while audio is running.
constexpr uint32_t kAudioStallMs = 100;
// Gap between two reports of new SAI or DMA transfer errors.
constexpr uint32_t kTransferErrorGapMs = 1000;

// Indexed by the literal each FAULT_TRAMPOLINE passes (0 to 3) and by kKindClock.
constexpr std::array<const char*, 5> kKindName = {"hard", "memmanage", "bus", "usage", "clock"};
constexpr uint32_t kKindClock = 4;
static_assert(kKindClock == kKindName.size() - 1, "kKindClock must index the last kind name");

// Indexed by ClockStep; entry 0 covers an unknown value.
constexpr std::array<const char*, 6> kClockStepName = {"?", "supply", "voltage scaling", "oscillator and PLL1",
                                                       "bus clocks", "peripheral clocks"};

// Written by the fault handler, read on the next boot. .cv_noinit (firmware/cubevox_noinit.ld)
// sits at the top of DTCM, clear of .dtcm_bss, and survives a system reset.
struct FaultRecord {
  uint32_t magic;
  uint32_t kind;
  uint32_t pc;
  uint32_t lr;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t bfar;
  uint32_t mmfar;
  uint32_t sr2;     // FLASH->SR2 at the fault, for DBECCERR
  uint32_t eccFa2;  // FLASH->ECC_FA2 at the fault, bank 2 ECC fail address
  uint32_t detail;  // the ClockStep for a clock failure
};
__attribute__((section(".cv_noinit"))) FaultRecord gStored;

FaultRecord gLast = {};  // copy taken at boot, printed by every report this run
uint32_t gResetFlags = 0;
bool gInitDone = false;

uint32_t gLastBlocks = 0;
uint32_t gLastAdvanceMs = 0;
bool gStalled = false;
uint32_t gReportedTransferErrors = 0;
uint32_t gTransferErrorReportMs = 0;

struct ResetFlag {
  uint32_t mask;
  const char* name;
};
constexpr ResetFlag kResetFlags[] = {
    {RCC_RSR_IWDG1RSTF, "watchdog"}, {RCC_RSR_WWDG1RSTF, "window-watchdog"},
    {RCC_RSR_SFTRSTF, "software"},   {RCC_RSR_PORRSTF, "power-on"},
    {RCC_RSR_BORRSTF, "brown-out"},  {RCC_RSR_PINRSTF, "pin"},
    {RCC_RSR_LPWRRSTF, "low-power"},
};

// ECC_FA2 counts 32-byte flash words from the start of bank 2 (15 bits cover the 1 MB bank).
uint32_t bank2EccAddress(const FaultRecord& r) {
  return kBank2Base + (r.eccFa2 & FLASH_ECC_FA_FAIL_ECC_ADDR) * static_cast<uint32_t>(kFlashWordBytes);
}

bool bank2EccSet(const FaultRecord& r) { return (r.sr2 & FLASH_SR_DBECCERR) != 0; }

void printHex(Print& out, const char* label, uint32_t v) {
  out.print(label);
  out.print("=0x");
  out.print(v, HEX);
}

bool inRam(const void* address) {
  switch (h7_block_mem_region_of(address)) {
    case H7_MEM_DTCM:
    case H7_MEM_RAM_D1:
    case H7_MEM_RAM_D2:
    case H7_MEM_RAM_D3: return true;
    default: return false;
  }
}

// A stack overflow leaves the stack pointer outside RAM, and reading the frame would fault again
// into lockup. The frame is eight words.
bool frameReadable(const uint32_t* frame) {
  return (reinterpret_cast<uintptr_t>(frame) & 3u) == 0 && inRam(frame) && inRam(frame + 7);
}

// The prescaler and reload writes take a few LSI cycles to cross into the watchdog domain.
bool watchdogAcceptedPeriod() {
  const uint32_t startMs = millis();
  while (!LL_IWDG_IsReady(IWDG1)) {
    if (millis() - startMs >= kWatchdogReadyMs) return false;
  }
  return true;
}

void reportTransferErrors(uint32_t nowMs) {
  const AudioErrors errors = audioErrors();
  if (errors.count == gReportedTransferErrors || nowMs - gTransferErrorReportMs < kTransferErrorGapMs) return;
  gReportedTransferErrors = errors.count;
  gTransferErrorReportMs = nowMs;
  serialOut().print("[ERROR] audio SAI/DMA transfer errors: ");
  serialOut().print(errors.count);
  printHex(serialOut(), ", HAL error bits", errors.codes);
  serialOut().println();
}

void reportFault(Print& out) {
  if (gLast.kind == kKindClock) {
    out.print("[ERROR] last reset: clock setup failed in ");
    out.println(kClockStepName[gLast.detail < kClockStepName.size() ? gLast.detail : 0]);
    return;
  }
  out.print("[ERROR] last reset: fault ");
  out.print(gLast.kind < kKindName.size() ? kKindName[gLast.kind] : "unknown");
  printHex(out, " pc", gLast.pc);
  printHex(out, " lr", gLast.lr);
  printHex(out, " cfsr", gLast.cfsr);
  printHex(out, " hfsr", gLast.hfsr);
  printHex(out, " bfar", gLast.bfar);
  printHex(out, " mmfar", gLast.mmfar);
  if (bank2EccSet(gLast)) printHex(out, " bank2-ecc", bank2EccAddress(gLast));
  out.println();
}

}  // namespace

// The trampoline picks the stack the fault came from and hands the exception frame to
// faultCapture. Frame layout: r0 r1 r2 r3 r12 lr pc xpsr.
extern "C" __attribute__((used, noreturn)) void faultCapture(const uint32_t* frame, uint32_t kind) {
  // Mute by register write (BSRR upper half clears the pin). No HAL calls here.
  pins::portOf(pins::kXsmt)->BSRR = pins::maskOf(pins::kXsmt) << 16;
  pins::portOf(pins::kMuteN)->BSRR = pins::maskOf(pins::kMuteN) << 16;

  const bool readable = frameReadable(frame);
  gStored.kind = kind;
  gStored.pc = readable ? frame[6] : 0;
  gStored.lr = readable ? frame[5] : 0;
  gStored.cfsr = SCB->CFSR;
  gStored.hfsr = SCB->HFSR;
  gStored.bfar = SCB->BFAR;
  gStored.mmfar = SCB->MMFAR;
  gStored.sr2 = FLASH->SR2;
  gStored.eccFa2 = FLASH->ECC_FA2;
  gStored.detail = 0;
  gStored.magic = kMagic;
  NVIC_SystemReset();
  for (;;) {}
}

#define FAULT_TRAMPOLINE(handler, kind)                   \
  extern "C" __attribute__((naked)) void handler() {      \
    __asm volatile(                                       \
        "tst lr, #4        \n"                            \
        "ite eq            \n"                            \
        "mrseq r0, msp     \n"                            \
        "mrsne r0, psp     \n"                            \
        "mov r1, #" #kind "\n"                            \
        "b faultCapture    \n");                          \
  }

FAULT_TRAMPOLINE(HardFault_Handler, 0)
FAULT_TRAMPOLINE(MemManage_Handler, 1)
FAULT_TRAMPOLINE(BusFault_Handler, 2)
FAULT_TRAMPOLINE(UsageFault_Handler, 3)

void faultClockFailure(ClockStep step) {
  gStored = FaultRecord{};
  gStored.kind = kKindClock;
  gStored.detail = static_cast<uint32_t>(step);
  gStored.magic = kMagic;
  NVIC_SystemReset();
  for (;;) {}
}

void faultInit() {
  gResetFlags = RCC->RSR;
  RCC->RSR |= RCC_RSR_RMVF;

  if (gStored.magic == kMagic) {
    gLast = gStored;
    gStored.magic = 0;
  }

  // Without these the three lesser faults escalate to HardFault and lose their kind.
  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
  gInitDone = true;
}

bool faultInitDone() { return gInitDone; }

bool faultLastWasBank2Ecc(uint32_t& address) {
  if (gLast.magic != kMagic || !bank2EccSet(gLast)) return false;
  address = bank2EccAddress(gLast);
  return true;
}

void faultWatchdogStart() {
  // LSI is about 32 kHz; / 64 gives 500 Hz, so the reload counts in 2 ms. Starting the
  // watchdog forces LSI on. Once enabled it cannot be stopped short of a reset.
  LL_IWDG_Enable(IWDG1);
  LL_IWDG_EnableWriteAccess(IWDG1);
  LL_IWDG_SetPrescaler(IWDG1, LL_IWDG_PRESCALER_64);
  LL_IWDG_SetReloadCounter(IWDG1, kWatchdogMs / 2 - 1);

  if (!watchdogAcceptedPeriod()) {
    // Without the update the watchdog keeps its power-on period of about half a second, which
    // loop() still meets. The wait gives up and says so, because a hang here would stop the box
    // before the watchdog could help.
    serialOut().println("[ERROR] watchdog did not accept its 2 s period, running on its default");
    return;
  }
  LL_IWDG_ReloadCounter(IWDG1);
}

void faultWatchdogKick() { LL_IWDG_ReloadCounter(IWDG1); }

void faultAudioWatch(uint32_t nowMs, bool audioRunning) {
  if (!audioRunning) return;
  reportTransferErrors(nowMs);
  if (gStalled) return;
  const uint32_t blocks = audioBlockCount();
  if (blocks != gLastBlocks) {
    gLastBlocks = blocks;
    gLastAdvanceMs = nowMs;
    return;
  }
  if (nowMs - gLastAdvanceMs < kAudioStallMs) return;
  gStalled = true;
  muteLatch();
  serialOut().print("[ERROR] audio stalled after ");
  serialOut().print(blocks);
  serialOut().println(" blocks, output muted");
}

void faultReport(Print& out) {
  out.print("[boot] reset:");
  for (const ResetFlag& f : kResetFlags) {
    if (gResetFlags & f.mask) {
      out.print(' ');
      out.print(f.name);
    }
  }
  out.println();

  if (gLast.magic == kMagic) {
    reportFault(out);
  } else {
    out.println("[boot] fault: none");
  }

  if (gStalled) {
    out.print("[ERROR] audio stalled after ");
    out.print(gLastBlocks);
    out.println(" blocks, output muted");
  }
}

#if CUBEVOX_BENCH
void faultForceBusFault() {
  serialOut().println("[bench] forcing a bus fault");
  serialFlush(kFlushBeforeResetMs);
  (void)*reinterpret_cast<volatile uint32_t*>(0xDEADBEE0);
}

[[noreturn]] void faultSpinForWatchdog() {
  serialOut().print("[bench] spinning; the watchdog resets in ");
  serialOut().print(kWatchdogMs / 1000);
  serialOut().println(" s");
  serialFlush(kFlushBeforeResetMs);
  for (;;) {}
}
#endif
