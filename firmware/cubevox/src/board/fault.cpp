#include "board/fault.h"

#include <unistd.h>

#include <array>

#include "stm32yyxx_ll_iwdg.h"  // the core leaves the IWDG HAL module out; LL is what it uses

#include "board/audio.h"
#include "board/block_mem.h"
#include "board/boot_block.h"
#include "board/boot_health.h"
#include "board/diag_service.h"
#include "board/flash_bank2.h"
#include "board/image_header.h"
#include "board/mute.h"
#include "board/pins.h"
#include "board/planned_reset.h"
#include "board/serial_link.h"
#include "core/audio_stall.h"
#include "core/timing.h"

// faultCapture's own stack, so a fault from an overflowed main stack still records. The trampolines
// name it in assembly, which takes the size as text.
#define FAULT_STACK_BYTES 512
extern "C" {
__attribute__((used, aligned(8))) std::array<uint32_t, FAULT_STACK_BYTES / sizeof(uint32_t)> gFaultStack;
}
static_assert(sizeof(gFaultStack) == FAULT_STACK_BYTES, "the trampolines put the stack top at this offset");

namespace {

// Watchdog period. loop() kicks it; the audio interrupt never does.
constexpr uint32_t kWatchdogMs = 2000;
// LSI is about 32 kHz; / 64 gives 500 Hz, so the reload counts in 2 ms.
constexpr uint32_t kWatchdogReload = kWatchdogMs / 2 - 1;
// Longest the watchdog may take to accept its period.
constexpr uint32_t kWatchdogReadyMs = 100;
// Gap between two reports of new SAI or DMA transfer errors.
constexpr uint32_t kTransferErrorGapMs = 1000;

// faultInit fills the free part of the main stack with this word. The stack grows down from
// _estack toward the heap and every interrupt shares it, so a word that still holds the paint was
// never used.
constexpr uint32_t kStackPaint = 0xC5A5C5A5u;
// Left unpainted below the stack pointer, for the frames of the calls that paint.
constexpr uintptr_t kPaintMarginBytes = 256;

// Written by the fault handler, read on the next boot. .cv_noinit (firmware/cubevox.ld) sits at
// the top of DTCM, above the boot block and .dtcm_bss, and survives a system reset.
__attribute__((section(".cv_noinit"))) CvFaultRecord gStored;

CvFaultRecord gLast = {};  // copy taken at boot, printed by every report this run
uint32_t gResetFlags = 0;
bool gInitDone = false;
bool gWatchdogPeriodSet = false;  // the 2 s period read back as written
uintptr_t gPaintTop = 0;          // one past the highest painted word

AudioStallWatch gStallWatch;
uint32_t gReportedTransferErrors = 0;
uint32_t gTransferErrorReportMs = 0;

struct ResetFlag {
  uint32_t mask;
  const char* name;
};
constexpr std::array<ResetFlag, 7> kResetFlags = {{
    {RCC_RSR_IWDG1RSTF, "watchdog"},
    {RCC_RSR_WWDG1RSTF, "window-watchdog"},
    {RCC_RSR_SFTRSTF, "software"},
    {RCC_RSR_PORRSTF, "power-on"},
    {RCC_RSR_BORRSTF, "brown-out"},
    {RCC_RSR_PINRSTF, "pin"},
    {RCC_RSR_LPWRRSTF, "low-power"},
}};

// The first flag set, in table order, names the reset.
const ResetFlag* firstResetFlag() {
  for (const ResetFlag& f : kResetFlags) {
    if (gResetFlags & f.mask) return &f;
  }
  return nullptr;
}

// ECC_FA2 counts 32-byte flash words from the start of bank 2 (15 bits cover the 1 MB bank).
uint32_t bank2EccAddress(const CvFaultRecord& r) {
  return kBank2Base + (r.eccFa2 & FLASH_ECC_FA_FAIL_ECC_ADDR) * static_cast<uint32_t>(kFlashWordBytes);
}

bool bank2EccSet(const CvFaultRecord& r) { return (r.sr2 & FLASH_SR_DBECCERR) != 0; }

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
  if (gLast.kind == CvFaultKindClock) {
    out.print("[ERROR] last reset: clock setup failed in ");
    out.println(cvClockStepName(gLast.detail));
    return;
  }
  out.print("[ERROR] last reset: fault ");
  out.print(cvFaultKindName(gLast.kind));
  printHex(out, " pc", gLast.pc);
  printHex(out, " lr", gLast.lr);
  printHex(out, " cfsr", gLast.cfsr);
  printHex(out, " hfsr", gLast.hfsr);
  printHex(out, " bfar", gLast.bfar);
  printHex(out, " mmfar", gLast.mmfar);
  if (bank2EccSet(gLast)) printHex(out, " bank2-ecc", bank2EccAddress(gLast));
  out.println();
}

// The release name and CRC verdict of the running image, the failed starts the bootloader counted
// before this one, and why the bootloader last stayed in update mode.
void reportImage(Print& out) {
  out.print("[boot] image ");
  out.print(imageVersion());
  out.print(imageCrcOk() ? " crc ok" : " crc BAD");
  out.print(", boot fails ");
  out.print(bootHealthFails());
  out.print(", last update reason ");
  out.println(bootHealthLastReason());
}

void reportWatchdog(Print& out) {
  if (gWatchdogPeriodSet) {
    out.print("[boot] watchdog ");
    out.print(kWatchdogMs / 1000);
    out.println(" s");
    return;
  }
  out.print("[ERROR] watchdog did not take its ");
  out.print(kWatchdogMs / 1000);
  out.println(" s period and runs on the one it had");
}

// Waits about `ms` with no timer, which may be what faulted. One pass of the loop takes at least a
// cycle of SystemCoreClock, the core clock the HAL last set, so the wait is never short; a few
// cycles per pass make it a few times longer.
void spinMs(uint32_t ms) {
  const uint32_t passes = ms * (SystemCoreClock / 1000u);
  for (volatile uint32_t i = 0; i < passes; i = i + 1) {}
}

// The heap's end, where the free stack begins.
uintptr_t heapTop() { return (reinterpret_cast<uintptr_t>(sbrk(0)) + 3u) & ~uintptr_t{3}; }

void paintStack() {
  const uintptr_t top = (__get_MSP() - kPaintMarginBytes) & ~uintptr_t{3};
  for (uintptr_t at = heapTop(); at < top; at += sizeof(uint32_t)) *reinterpret_cast<uint32_t*>(at) = kStackPaint;
  gPaintTop = top;
}

}  // namespace

// The trampoline picks the stack the fault came from and hands the exception frame to
// faultCapture. Frame layout: r0 r1 r2 r3 r12 lr pc xpsr.
extern "C" __attribute__((used, noreturn)) void faultCapture(const uint32_t* frame, uint32_t kind) {
  // XSMT first, so the DAC ramps to silence before K102 opens, the order muteForReset keeps. By
  // register write (BSRR upper half clears the pin), because the HAL may be mid-call. The DMA keeps
  // the SAI clocked through the waits, since its interrupts rank below this handler.
  pins::portOf(pins::kXsmt)->BSRR = pins::maskOf(pins::kXsmt) << 16;

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
  gStored.magic = CV_FAULT_MAGIC;

  spinMs(kXsmtLeadMs);
  pins::portOf(pins::kMuteN)->BSRR = pins::maskOf(pins::kMuteN) << 16;
  spinMs(kRelaySettleMs);
  NVIC_SystemReset();
  for (;;) {}
}

// FAULT_TRAMPOLINE hands the kind to the assembler as a literal. These keep the literals equal to
// enum CvFaultKind.
static_assert(CvFaultKindHard == 0 && CvFaultKindMemManage == 1 && CvFaultKindBus == 2 &&
                  CvFaultKindUsage == 3,
              "the FAULT_TRAMPOLINE literals must equal enum CvFaultKind");

#define FAULT_STRINGIFY(x) #x
#define FAULT_TEXT(x) FAULT_STRINGIFY(x)

// r0 takes the frame from the stack in use at the fault, then MSP moves to the top of gFaultStack.
#define FAULT_TRAMPOLINE(handler, kind)                                         \
  extern "C" __attribute__((naked)) void handler() {                            \
    __asm volatile(                                                             \
        "tst lr, #4                                                \n"          \
        "ite eq                                                    \n"          \
        "mrseq r0, msp                                             \n"          \
        "mrsne r0, psp                                             \n"          \
        "movw r2, #:lower16:gFaultStack+" FAULT_TEXT(FAULT_STACK_BYTES) "\n"    \
        "movt r2, #:upper16:gFaultStack+" FAULT_TEXT(FAULT_STACK_BYTES) "\n"    \
        "msr msp, r2                                               \n"          \
        "mov r1, #" #kind "                                        \n"          \
        "b faultCapture                                            \n");        \
  }

FAULT_TRAMPOLINE(HardFault_Handler, 0)
FAULT_TRAMPOLINE(MemManage_Handler, 1)
FAULT_TRAMPOLINE(BusFault_Handler, 2)
FAULT_TRAMPOLINE(UsageFault_Handler, 3)

void faultClockFailure(ClockStep step) {
  gStored = CvFaultRecord{};
  gStored.kind = CvFaultKindClock;
  gStored.detail = static_cast<uint32_t>(step);
  gStored.magic = CV_FAULT_MAGIC;
  NVIC_SystemReset();
  for (;;) {}
}

void faultInit() {
  gResetFlags = RCC->RSR;
  RCC->RSR |= RCC_RSR_RMVF;

  if (gStored.magic == CV_FAULT_MAGIC) {
    gLast = gStored;
    gStored.magic = 0;
  }

  // Without these the three lesser faults escalate to HardFault and lose their kind.
  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
  paintStack();
  gInitDone = true;
}

bool faultInitDone() { return gInitDone; }

uint32_t faultResetFlags() { return gResetFlags; }

bool faultResetWasPowerOn() {
  const ResetFlag* first = firstResetFlag();
  return first != nullptr && first->mask == RCC_RSR_PORRSTF;
}

bool faultRecordPresent() { return gLast.magic == CV_FAULT_MAGIC; }

bool faultLastWasBank2Ecc(uint32_t& address) {
  if (gLast.magic != CV_FAULT_MAGIC || !bank2EccSet(gLast)) return false;
  address = bank2EccAddress(gLast);
  return true;
}

// The bootloader started the watchdog at 16 s right before the jump. With 0x5555 in KR the
// prescaler and the reload take new values while it runs, SR's PVU and RVU clear once they have
// crossed into the LSI domain, and the refresh then loads the new reload (RM0433, IWDG). Reading
// both registers back after that proves the period took. Under a bootloader that never started it,
// Enable starts it here and forces LSI on; once running it stops only at a reset.
void faultWatchdogStart() {
  LL_IWDG_Enable(IWDG1);
  LL_IWDG_EnableWriteAccess(IWDG1);
  LL_IWDG_SetPrescaler(IWDG1, LL_IWDG_PRESCALER_64);
  LL_IWDG_SetReloadCounter(IWDG1, kWatchdogReload);
  // The wait gives up rather than hang, because a hang here would stop the box before the watchdog
  // could help. A watchdog that missed the update keeps the period it had.
  gWatchdogPeriodSet = watchdogAcceptedPeriod() && LL_IWDG_GetPrescaler(IWDG1) == LL_IWDG_PRESCALER_64 &&
                       LL_IWDG_GetReloadCounter(IWDG1) == kWatchdogReload;
  LL_IWDG_ReloadCounter(IWDG1);
  if (!gWatchdogPeriodSet) reportWatchdog(serialOut());
}

void faultWatchdogKick() { LL_IWDG_ReloadCounter(IWDG1); }

// A stream the audio module latched has stopped on purpose and is not a stall; a restart arms the
// watch afresh. A stall mutes at once and resets once the flash is idle, so DIAG.TXT gets the entry
// and the sound comes back about a second later.
void faultAudioWatch(uint32_t nowMs, bool audioRunning) {
  if (!audioRunning) return;
  reportTransferErrors(nowMs);
  const AudioHealth health = audioHealth();
  if (!gStallWatch.update(nowMs, !health.latched, health.restarts, audioBlockCount())) return;
  muteLatch();
  serialOut().print("[ERROR] audio stalled after ");
  serialOut().print(gStallWatch.blocks());
  serialOut().println(" blocks, output muted; resetting");
  diagNoteAudioFault();
  plannedResetStart(PlannedReset::AudioStall, nowMs);
}

bool faultAudioStalled() { return gStallWatch.stalled(); }

uint32_t faultStackUntouchedBytes() {
  const uintptr_t bottom = heapTop();
  uintptr_t at = bottom;
  while (at < gPaintTop && *reinterpret_cast<const volatile uint32_t*>(at) == kStackPaint) at += sizeof(uint32_t);
  return static_cast<uint32_t>(at - bottom);
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

  if (gLast.magic == CV_FAULT_MAGIC) {
    reportFault(out);
  } else {
    out.println("[boot] fault: none");
  }

  reportImage(out);
  reportWatchdog(out);
  out.print("[boot] stack: ");
  out.print(faultStackUntouchedBytes());
  out.println(" bytes never used");

  if (gStallWatch.stalled()) {
    out.print("[ERROR] audio stalled after ");
    out.print(gStallWatch.blocks());
    out.println(" blocks, output muted");
  }
}

void faultSummaryText(char* out, size_t size) {
  if (size == 0) return;
  out[0] = '\0';
  if (gLast.magic != CV_FAULT_MAGIC) return;
  if (gLast.kind == CvFaultKindClock) {
    snprintf(out, size, "clock setup failed in %s", cvClockStepName(gLast.detail));
    return;
  }
  snprintf(out, size, "%s pc=0x%lX lr=0x%lX cfsr=0x%lX bfar=0x%lX", cvFaultKindName(gLast.kind),
           static_cast<unsigned long>(gLast.pc), static_cast<unsigned long>(gLast.lr),
           static_cast<unsigned long>(gLast.cfsr), static_cast<unsigned long>(gLast.bfar));
}

void faultResetText(char* out, size_t size) {
  const ResetFlag* first = firstResetFlag();
  const char* reset = first != nullptr ? first->name : "none";
  const char* fault = "none";
  if (gLast.magic == CV_FAULT_MAGIC) fault = cvFaultKindName(gLast.kind);
  snprintf(out, size, "%s %s", reset, fault);
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
