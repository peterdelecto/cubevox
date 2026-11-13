// The updater's RAM code. See updater_ram.h.
//
// The flash sequences follow RM0433 section 4.3.9 (single write sequence) and 4.3.10 (sector erase
// sequence) register by register, as the HAL's HAL_FLASH_Program, FLASH_Erase_Sector and
// FLASH_WaitForLastOperation do them. Only FLASH_CR1, SR1 and CCR1 are written: bank 2 is never
// unlocked, erased or written. Interrupts stay off, so every wait polls and kicks the watchdog.

#include "updater_ram.h"

#include <stdbool.h>

#include "boot_decide.h"
#include "stm32h7xx_hal.h"

// Bank 1 sector 0, the bootloader's, and sector 1, the first of the application region.
#define CV_SECTOR0_ADDR    FLASH_BANK1_BASE
#define CV_SECTOR0_NUMBER  0u
#define CV_APP_SECTOR      1u
CV_STATIC_ASSERT(app_starts_at_sector_1, CV_APP_START == FLASH_BANK1_BASE + CV_APP_SECTOR * CV_FLASH_SECTOR_SIZE);
CV_STATIC_ASSERT(flash_word_matches_hal, CV_FLASH_WORD_BYTES == 4u * FLASH_NB_32BITWORD_IN_FLASHWORD);

// Bounds on a flash operation. A sector erase takes about 2 s and a flash word microseconds; the
// bounds sit far above both and far below nothing, because every wait kicks the watchdog.
#define CV_ERASE_LIMIT_MS  10000u
#define CV_WORD_LIMIT_MS   100u

// IWDG1's reload key (RM0433, IWDG).
#define CV_IWDG_KEY_RELOAD 0xAAAAu

// The H743's 166 vectors (boot_block.h).
#define CV_VECTOR_COUNT 166u

// A pass that leaves sector 0 as the old bootloader is tried this often before the updater leaves
// with the old bootloader kept.
#define CV_UNCHANGED_PASSES 3u

uint8_t cvSector0Image[CV_FLASH_SECTOR_SIZE] __attribute__((section(".cv_sector0_image"), aligned(32)));
uint8_t cvSector0Old[CV_FLASH_SECTOR_SIZE] __attribute__((section(".cv_sector0_image"), aligned(32)));

typedef enum {
  // Sector 0 still holds the old bootloader.
  CvPhaseChecks,
  // The first erase of sector 0 has started; only a sector that compares ends it.
  CvPhaseSector0,
  // Sector 0 compares; what is left is the erase of sector 1 and the reset.
  CvPhaseLeave
} Phase;

static volatile Phase gPhase = CvPhaseChecks;
static uint32_t gWriteBytes;
static const char* gSummary;
static uint32_t gCyclesPerMs = 1u;
static uint32_t gMs;
static uint32_t gMark;
static uint32_t gCarry;

static const char kFaultSummary[] = "bootloader kept: updater fault";
static const char kUnchangedSummary[] = "bootloader kept: sector 0 unchanged";

void cvUpdaterFaulted(void) __attribute__((used, noreturn));
static void faultEntry(void);

//--------------------------------------------------------------------+
// Time and the watchdog
//--------------------------------------------------------------------+

void cvRamClockStart(uint32_t cyclesPerMs) {
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->LAR = 0xC5ACCE55u;  // the Cortex-M7 DWT ignores writes until unlocked
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  gCyclesPerMs = cyclesPerMs != 0u ? cyclesPerMs : 1u;
  gMark = DWT->CYCCNT;
  gCarry = 0u;
  gMs = 0u;
}

uint32_t cvRamMs(void) {
  const uint32_t now = DWT->CYCCNT;
  gCarry += now - gMark;
  gMark = now;
  gMs += gCarry / gCyclesPerMs;
  gCarry %= gCyclesPerMs;
  return gMs;
}

void cvWatchdogKick(void) {
  IWDG1->KR = CV_IWDG_KEY_RELOAD;
}

//--------------------------------------------------------------------+
// Bank 1
//--------------------------------------------------------------------+

// True once QW1 clears within `limitMs` (RM0433 4.3.9 step 5, 4.3.10 step 5).
static bool bank1Wait(uint32_t limitMs) {
  const uint32_t startMs = cvRamMs();
  while ((FLASH->SR1 & FLASH_SR_QW) != 0u) {
    cvWatchdogKick();
    if (cvRamMs() - startMs >= limitMs) return false;
  }
  return true;
}

// PGSERR and INCERR must be clear before a write (RM0433 4.3.9), and a read error the compare left
// would fail the next check.
static void bank1ErrorsClear(void) {
  FLASH->CCR1 = FLASH_FLAG_ALL_ERRORS_BANK1;
}

static bool bank1Clean(void) {
  return (FLASH->SR1 & FLASH_FLAG_ALL_ERRORS_BANK1) == 0u;
}

// RM0433 4.5.1: a second key sequence on an unlocked register locks it until the next reset, so
// the keys go in only while LOCK is set.
bool cvBank1Unlock(void) {
  if ((FLASH->CR1 & FLASH_CR_LOCK) != 0u) {
    FLASH->KEYR1 = FLASH_KEY1;
    FLASH->KEYR1 = FLASH_KEY2;
  }
  return (FLASH->CR1 & FLASH_CR_LOCK) == 0u;
}

static void bank1Lock(void) {
  FLASH->CR1 |= FLASH_CR_LOCK;
}

// One sector of bank 1, 64-bit parallelism as the bootloader erases (FLASH_VOLTAGE_RANGE_4).
static bool sectorErase(uint32_t sector) {
  if (!bank1Wait(CV_ERASE_LIMIT_MS)) return false;
  bank1ErrorsClear();
  FLASH->CR1 &= ~(FLASH_CR_PSIZE | FLASH_CR_SNB);
  FLASH->CR1 |= FLASH_CR_SER | FLASH_VOLTAGE_RANGE_4 | (sector << FLASH_CR_SNB_Pos) | FLASH_CR_START;
  const bool done = bank1Wait(CV_ERASE_LIMIT_MS);
  FLASH->CR1 &= ~(FLASH_CR_SER | FLASH_CR_SNB);
  return done && bank1Clean();
}

// One 32-byte flash word from the RAM image.
static bool wordProgram(uint32_t address, const uint32_t* words) {
  if (!bank1Wait(CV_WORD_LIMIT_MS)) return false;
  bank1ErrorsClear();
  FLASH->CR1 |= FLASH_CR_PG;
  __ISB();
  __DSB();
  volatile uint32_t* target = (volatile uint32_t*)address;
  for (uint32_t i = 0; i < FLASH_NB_32BITWORD_IN_FLASHWORD; ++i) target[i] = words[i];
  __ISB();
  __DSB();
  const bool done = bank1Wait(CV_WORD_LIMIT_MS);
  FLASH->CR1 &= ~FLASH_CR_PG;
  return done && bank1Clean();
}

// The whole of sector 0 against `bytes`, the erased tail included. The D-cache is off, so these
// reads come from the flash.
static bool sector0Holds(const uint8_t* bytes) {
  const volatile uint32_t* flash = (const volatile uint32_t*)CV_SECTOR0_ADDR;
  const uint32_t* image = (const uint32_t*)bytes;
  bool same = true;
  for (uint32_t i = 0; i < CV_FLASH_SECTOR_SIZE / 4u; ++i) same = same && flash[i] == image[i];
  return same;
}

// Step 5: erase, write, read back. The compare alone decides, so a write that reports an error but
// lands is kept, and one that reports nothing but differs is done again.
static bool sector0Pass(void) {
  bool ok = cvBank1Unlock() && sectorErase(CV_SECTOR0_NUMBER);
  for (uint32_t at = 0u; ok && at < gWriteBytes; at += CV_FLASH_WORD_BYTES) {
    ok = wordProgram(CV_SECTOR0_ADDR + at, (const uint32_t*)&cvSector0Image[at]);
  }
  bank1Lock();
  return sector0Holds(cvSector0Image);
}

//--------------------------------------------------------------------+
// Leaving
//--------------------------------------------------------------------+

static void summaryWrite(const char* text) {
  struct CvBootBlock* block = (struct CvBootBlock*)CV_BOOT_BLOCK_ADDR;
  uint32_t i = 0u;
  for (; i + 1u < sizeof block->appSummary && text[i] != '\0'; ++i) block->appSummary[i] = text[i];
  for (; i < sizeof block->appSummary; ++i) block->appSummary[i] = '\0';
}

// Steps 7 and 8. A failed erase still resets: the bootloader then finds the updater again and runs
// it, and its start counter ends that after three runs.
static void leave(void) __attribute__((noreturn));
static void leave(void) {
  gPhase = CvPhaseLeave;
  cvWatchdogKick();
  if (cvBank1Unlock()) {
    (void)sectorErase(CV_APP_SECTOR);
    bank1Lock();
  }
  __DSB();
  NVIC_SystemReset();
}

// Steps 5 and 6, then 7 and 8. Never resets on a sector 0 that holds neither bootloader whole. A
// sector the passes never changed (the erase refused, the register stayed locked) still holds the
// old bootloader, which starts as before, so after a few such passes the updater leaves.
static void sector0Loop(void) __attribute__((noreturn));
static void sector0Loop(void) {
  gPhase = CvPhaseSector0;
  uint32_t unchanged = 0u;
  while (!sector0Pass()) {
    cvWatchdogKick();
    unchanged = sector0Holds(cvSector0Old) ? unchanged + 1u : 0u;
    if (unchanged >= CV_UNCHANGED_PASSES) {
      summaryWrite(kUnchangedSummary);
      leave();
    }
  }
  cvBootUpdatedMarkSet((struct CvBootBlock*)CV_BOOT_BLOCK_ADDR);
  summaryWrite(gSummary);
  leave();
}

void cvSector0Install(uint32_t writeBytes, const char* summary) {
  gWriteBytes = writeBytes;
  gSummary = summary;
  sector0Loop();
}

void cvUpdaterLeave(const char* summary) {
  summaryWrite(summary);
  leave();
}

//--------------------------------------------------------------------+
// Faults
//--------------------------------------------------------------------+

// Every fault restarts the stack at its top and lands here. Before the first erase the old
// bootloader is whole, so the updater leaves; from it on, only a sector that compares ends the run.
void cvUpdaterFaulted(void) {
  if (gPhase == CvPhaseSector0) sector0Loop();
  if (gPhase == CvPhaseChecks) summaryWrite(kFaultSummary);
  leave();
}

__attribute__((naked)) static void faultEntry(void) {
  __asm volatile(
      "ldr r0, =_estack     \n"
      "msr msp, r0          \n"
      "b cvUpdaterFaulted   \n"
      ".ltorg               \n");
}

extern uint32_t _estack;

// Every exception and interrupt goes to faultEntry. Interrupts stay masked, so only the faults and
// NMI can arrive.
__attribute__((section(".cv_ram_vectors"), used, aligned(1024))) const uint32_t cvRamVectors[CV_VECTOR_COUNT] = {
    [0] = (uint32_t)&_estack,
    [1 ... CV_VECTOR_COUNT - 1u] = (uint32_t)&faultEntry,
};
