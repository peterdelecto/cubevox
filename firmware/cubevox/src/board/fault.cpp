#include "board/fault.h"

#include "stm32yyxx_ll_iwdg.h"  // the core leaves the IWDG HAL module out; LL is what it uses

#include "board/audio.h"
#include "board/mute.h"

namespace {

constexpr uint32_t kMagic = 0x46415531;  // "FAU1"

enum Kind : uint32_t { kHard = 0, kMemManage = 1, kBus = 2, kUsage = 3 };
const char* const kKindName[] = {"hard", "memmanage", "bus", "usage"};

// Written by the fault handler, read on the next boot. .noinit survives a system reset.
struct FaultRecord {
  uint32_t magic;
  uint32_t kind;
  uint32_t pc;
  uint32_t lr;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t bfar;
  uint32_t mmfar;
};
__attribute__((section(".noinit"))) FaultRecord gStored;

FaultRecord gLast = {};  // copy taken at boot, printed by every report this run
uint32_t gResetFlags = 0;

uint32_t gLastBlocks = 0;
uint32_t gLastAdvanceMs = 0;
bool gStalled = false;

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

void printHex(Print& out, const char* label, uint32_t v) {
  out.print(label);
  out.print("=0x");
  out.print(v, HEX);
}

}  // namespace

// The trampoline picks the stack the fault came from and hands the exception frame to
// faultCapture. Frame layout: r0 r1 r2 r3 r12 lr pc xpsr.
extern "C" __attribute__((used, noreturn)) void faultCapture(const uint32_t* frame, uint32_t kind) {
  // Mute by register write: pins::kXsmt is PC5, pins::kMuteN is PB5. No HAL calls here.
  GPIOC->BSRR = static_cast<uint32_t>(GPIO_PIN_5) << 16;
  GPIOB->BSRR = static_cast<uint32_t>(GPIO_PIN_5) << 16;

  gStored.kind = kind;
  gStored.pc = frame[6];
  gStored.lr = frame[5];
  gStored.cfsr = SCB->CFSR;
  gStored.hfsr = SCB->HFSR;
  gStored.bfar = SCB->BFAR;
  gStored.mmfar = SCB->MMFAR;
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

void faultInit() {
  gResetFlags = RCC->RSR;
  RCC->RSR |= RCC_RSR_RMVF;

  if (gStored.magic == kMagic) {
    gLast = gStored;
    gStored.magic = 0;
  }

  // Without these the three lesser faults escalate to HardFault and lose their kind.
  SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
}

void faultWatchdogStart() {
  // LSI is about 32 kHz; / 64 gives 500 Hz, so the reload counts in 2 ms. Starting the
  // watchdog forces LSI on. Once enabled it cannot be stopped short of a reset.
  LL_IWDG_Enable(IWDG1);
  LL_IWDG_EnableWriteAccess(IWDG1);
  LL_IWDG_SetPrescaler(IWDG1, LL_IWDG_PRESCALER_64);
  LL_IWDG_SetReloadCounter(IWDG1, kWatchdogMs / 2 - 1);
  while (!LL_IWDG_IsReady(IWDG1)) {}
  LL_IWDG_ReloadCounter(IWDG1);
}

void faultWatchdogKick() { LL_IWDG_ReloadCounter(IWDG1); }

void faultAudioWatch(uint32_t nowMs, bool audioRunning) {
  if (!audioRunning || gStalled) return;
  const uint32_t blocks = audioStats().blocks;
  if (blocks != gLastBlocks) {
    gLastBlocks = blocks;
    gLastAdvanceMs = nowMs;
    return;
  }
  if (nowMs - gLastAdvanceMs < kAudioStallMs) return;
  gStalled = true;
  muteLatch();
  Serial.print("[ERROR] audio stalled after ");
  Serial.print(blocks);
  Serial.println(" blocks, output muted");
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
    out.print("[ERROR] last reset: fault ");
    out.print(kKindName[gLast.kind < 4 ? gLast.kind : 0]);
    printHex(out, " pc", gLast.pc);
    printHex(out, " lr", gLast.lr);
    printHex(out, " cfsr", gLast.cfsr);
    printHex(out, " hfsr", gLast.hfsr);
    printHex(out, " bfar", gLast.bfar);
    printHex(out, " mmfar", gLast.mmfar);
    out.println();
  } else {
    out.println("[boot] fault: none");
  }

  if (gStalled) {
    out.print("[ERROR] audio stalled after ");
    out.print(gLastBlocks);
    out.println(" blocks, output muted");
  }
}
