// The updater: a program in the application's image format that replaces the bootloader in bank 1
// sector 0 with the one it carries (docs/specs/2026-11-13-bootloader-update-design.md, How it works
// item 2). Every bootloader takes it as an ordinary application and jumps to it with the core back
// on HSI, the caches off and IWDG1 running at 16 s (board_teardown2, board_app_jump).
//
// Interrupts stay masked from the first instruction to the reset; nothing here needs one. The
// watchdog is kicked at every step and inside every wait, and the waits are bounded, except the
// loop on sector 0 in updater_ram.c, which ends only on a sector that compares.

#include <string.h>

#include "board.h"
#include "boot_decide.h"
#include "image_check.h"
#include "oled.h"
#include "power_limits.h"
#include "screens.h"
#include "stm32h7xx_hal.h"
#include "text.h"
#include "updater_check.h"
#include "updater_ram.h"

// VA_SENSE, 5VA through 10 k / 10 k on PB1, ADC12_INP5 (pins.h kVaSense).
#define CV_RAIL_PORT     GPIOB
#define CV_RAIL_PIN      GPIO_PIN_1
#define CV_RAIL_CHANNEL  ADC_CHANNEL_5
#define CV_RAIL_READINGS 8u
#define CV_RAIL_READ_MS  10u
// The detector's output is valid 50 us after PVDEN (power_sense.cpp kPvdSettleUs). Two steps of
// the millisecond clock make at least one whole millisecond.
#define CV_PVD_SETTLE_MS 2u
// The option byte change and the flash idle before it (board_hooks.c CV_OPTION_TIMEOUT_MS).
#define CV_OPTION_LIMIT_MS 1000u
// How long the damaged-file screen stays before the bootloader's own screen follows.
#define CV_DAMAGED_SHOW_MS 4000u

#define CV_IMAGE_VERSION "boot-" CV_BOOT_VERSION
CV_STATIC_ASSERT(image_version_fits, sizeof CV_IMAGE_VERSION <= CV_IMAGE_VERSION_BYTES);

typedef void (*Vector)(void);

extern uint32_t _estack;
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _siramfunc, _sramfunc, _eramfunc;
extern Vector __init_array_start[], __init_array_end[];

extern const struct CvPayloadHeader cvPayloadHeader;
extern const uint8_t cvPayloadBytes[];
extern const uint8_t cvPayloadEnd[];

void Reset_Handler(void) __attribute__((noreturn));
int main(void) __attribute__((noreturn));
static void earlyFault(void);

// The vectors the bootloader checks and jumps through: a stack in DTCM and a reset handler in the
// application region. A fault before the RAM vectors take over resets; the old bootloader is whole.
__attribute__((section(".isr_vector"), used)) const Vector cvBootVectors[16] = {
    [0] = (Vector)&_estack,
    [1] = Reset_Handler,
    [2 ... 15] = earlyFault,
};

// Length and CRC stay zero here; tools/stamp_image.py fills them.
__attribute__((section(".cv_image_hdr"), used)) const struct CvImageHeader cvImageHeader = {
    CV_IMAGE_MAGIC, CV_IMAGE_HEADER_VERSION, 0u, 0u, CV_IMAGE_VERSION};

static char gSummary[sizeof(((struct CvBootBlock*)0)->appSummary)];

static void earlyFault(void) {
  NVIC_SystemReset();
}

static void wordsCopy(uint32_t* to, const uint32_t* from, const uint32_t* end) {
  while (to < end) *to++ = *from++;
}

void Reset_Handler(void) {
  __disable_irq();
  wordsCopy(&_sdata, &_sidata, &_edata);
  for (uint32_t* at = &_sbss; at < &_ebss; ++at) *at = 0u;
  wordsCopy(&_sramfunc, &_siramfunc, &_eramfunc);
  __DSB();
  __ISB();
  SCB->VTOR = (uint32_t)cvRamVectors;
  __DSB();
  __ISB();
  for (Vector* init = __init_array_start; init < __init_array_end; ++init) (*init)();
  main();
}

// The HAL's waits (GPIO, ADC) count on this. SysTick stays off, so the cycle counter keeps time.
uint32_t HAL_GetTick(void) {
  return cvRamMs();
}

//--------------------------------------------------------------------+
// Summary, screens, waits
//--------------------------------------------------------------------+

// `first` then `second` into out[size], cut to fit and NUL padded.
static const char* textJoin(char* out, uint32_t size, const char* first, const char* second) {
  memset(out, 0, size);
  strncpy(out, first, size - 1u);
  strncat(out, second, size - 1u - strlen(out));
  return out;
}

// The boot block's appSummary text DIAG.TXT shows at the next update mode: what the updater did.
static const char* summarySet(const char* first, const char* second) {
  return textJoin(gSummary, sizeof gSummary, first, second);
}

static void screenShow(CvScreen screen) {
  static uint8_t frame[CV_FRAME_BYTES];
  cvScreenDraw(frame, screen, NULL, false);
  cvOledShow(frame);
}

static void hold(uint32_t ms) {
  const uint32_t startMs = cvRamMs();
  while (cvRamMs() - startMs < ms) cvWatchdogKick();
}

// True once `flags` clear in *reg within `limitMs`.
static bool waitClear(volatile uint32_t* reg, uint32_t flags, uint32_t limitMs) {
  const uint32_t startMs = cvRamMs();
  while ((*reg & flags) != 0u) {
    cvWatchdogKick();
    if (cvRamMs() - startMs >= limitMs) return false;
  }
  return true;
}

//--------------------------------------------------------------------+
// Step 2: the carried bootloader
//--------------------------------------------------------------------+

// Checks the bytes in flash, copies them into the RAM image with 0xFF after them, and checks the
// copy, which is what goes to sector 0.
static CvPayloadState payloadPrepare(void) {
  const uint32_t available = (uint32_t)((uintptr_t)cvPayloadEnd - (uintptr_t)cvPayloadBytes);
  const CvPayloadState state = cvPayloadCheck(&cvPayloadHeader, cvPayloadBytes, available);
  if (state != CvPayloadGood) return state;
  memset(cvSector0Image, 0xFF, sizeof cvSector0Image);
  memcpy(cvSector0Image, cvPayloadBytes, cvPayloadHeader.length);
  return cvPayloadCheck(&cvPayloadHeader, cvSector0Image, cvPayloadHeader.length);
}

//--------------------------------------------------------------------+
// Step 3: the chip and the power
//--------------------------------------------------------------------+

// The state the bootloader's jump leaves: HSI, so the OLED timing and the flash programming delay
// hold; 0x08000000 is bank 1; BOR level 3; FLASH_CR1 unlocks. NULL when all hold.
static const char* chipRefusal(void) {
  if ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) return "clock not HSI";
  // WRHIGHFREQ 0 is too tight for HSI at VOS3 (RM0433 Table 17); a larger value only programs slower.
  if ((FLASH->ACR & FLASH_ACR_WRHIGHFREQ) == 0u) return "flash delay 0";
  if ((FLASH->OPTCR & FLASH_OPTCR_SWAP_BANK) != 0u || (FLASH->OPTSR_CUR & FLASH_OPTSR_SWAP_BANK_OPT) != 0u) {
    return "banks swapped";
  }
  if ((FLASH->OPTSR_CUR & CV_OPTSR_BOR_LEV_MASK) != (CV_BOR_LEVEL_WANTED << CV_OPTSR_BOR_LEV_POS)) {
    return "BOR not level 3";
  }
  if (!cvBank1Unlock()) return "flash locked";
  return NULL;
}

// VDD against the PVD at level 6, 2.85 V, as power_sense.cpp arms it, read without its interrupt.
static bool vddLow(void) {
  PWR->CR1 = (PWR->CR1 & ~PWR_CR1_PLS) | PWR_CR1_PLS_LEV6 | PWR_CR1_PVDEN;
  hold(CV_PVD_SETTLE_MS);
  return (PWR->CSR1 & PWR_CSR1_PVDO) != 0u;
}

static ADC_HandleTypeDef gAdc;

// ADC1 on VA_SENSE as adc_setup.cpp sets it up, but clocked from HCLK / 4: the jump leaves PLL2,
// the application's ADC kernel clock, off.
static bool railStart(void) {
  __HAL_RCC_ADC12_CLK_ENABLE();
  GPIO_InitTypeDef pin = {0};
  pin.Pin = CV_RAIL_PIN;
  pin.Mode = GPIO_MODE_ANALOG;
  pin.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(CV_RAIL_PORT, &pin);
  gAdc.Instance = ADC1;
  gAdc.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  gAdc.Init.Resolution = ADC_RESOLUTION_12B;  // CV_RAIL_BITS
  gAdc.Init.ScanConvMode = ADC_SCAN_DISABLE;
  gAdc.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  gAdc.Init.LowPowerAutoWait = DISABLE;
  gAdc.Init.ContinuousConvMode = DISABLE;
  gAdc.Init.NbrOfConversion = 1;
  gAdc.Init.DiscontinuousConvMode = DISABLE;
  gAdc.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  gAdc.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  gAdc.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  gAdc.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  gAdc.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
  gAdc.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&gAdc) != HAL_OK) return false;
  if (HAL_ADCEx_Calibration_Start(&gAdc, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED) != HAL_OK) return false;
  ADC_ChannelConfTypeDef channel = {0};
  channel.Channel = CV_RAIL_CHANNEL;
  channel.Rank = ADC_REGULAR_RANK_1;
  channel.SamplingTime = ADC_SAMPLETIME_387CYCLES_5;
  channel.SingleDiff = ADC_SINGLE_ENDED;
  channel.OffsetNumber = ADC_OFFSET_NONE;
  return HAL_ADC_ConfigChannel(&gAdc, &channel) == HAL_OK;
}

static bool railRead(uint32_t* counts) {
  if (HAL_ADC_Start(&gAdc) != HAL_OK) return false;
  const bool ok = HAL_ADC_PollForConversion(&gAdc, CV_RAIL_READ_MS) == HAL_OK;
  if (ok) *counts = HAL_ADC_GetValue(&gAdc);
  (void)HAL_ADC_Stop(&gAdc);
  return ok;
}

// The power sense has a reading and no trip: VDD above the PVD level and every one of a few 5VA
// readings at or above the level that releases the application's mute. NULL when all hold.
static const char* powerRefusal(void) {
  if (vddLow()) return "VDD low";
  if (!railStart()) return "5VA no reading";
  uint32_t lowest = UINT32_MAX;
  for (uint32_t i = 0; i < CV_RAIL_READINGS; ++i) {
    uint32_t counts = 0u;
    if (!railRead(&counts)) return "5VA no reading";
    if (counts < lowest) lowest = counts;
  }
  if (lowest >= cvRailMinCounts) return NULL;
  static char text[24];
  char digits[11];
  char mv[16];
  textJoin(mv, sizeof mv, cvTextDecimal(digits, cvRailMv(lowest)), " mV");
  return textJoin(text, sizeof text, "5VA ", mv);
}

//--------------------------------------------------------------------+
// Step 4: the write protection of sector 0
//--------------------------------------------------------------------+

static bool sector0WritableNow(void) {
  return (FLASH->WPSN_CUR1 & CV_WPSN_SECTOR0) != 0u &&
         (FLASH->OPTSR_CUR & CV_OPTSR_BOR_LEV_MASK) == (CV_BOR_LEVEL_WANTED << CV_OPTSR_BOR_LEV_POS);
}

// One option byte change, the bootloader's (board_hooks.c optionsProgram, RM0433 4.4.3) with the
// sector 0 bit set instead of cleared: a set WRPSn bit leaves its sector writable (RM0433 4.9.16).
// BOR level 3 is written back with it. The bootloader protects the sector again at its next start.
static uint32_t sector0Unprotect(void) {
  if (sector0WritableNow()) return CvOptionAlreadySet;
  if (!waitClear(&FLASH->SR1, FLASH_SR_QW, CV_OPTION_LIMIT_MS) ||
      !waitClear(&FLASH->SR2, FLASH_SR_QW, CV_OPTION_LIMIT_MS)) {
    return CvOptionTimeout;
  }
  if ((FLASH->OPTCR & FLASH_OPTCR_OPTLOCK) != 0u) {
    FLASH->OPTKEYR = FLASH_OPT_KEY1;
    FLASH->OPTKEYR = FLASH_OPT_KEY2;
  }
  if ((FLASH->OPTCR & FLASH_OPTCR_OPTLOCK) != 0u) return CvOptionLocked;
  FLASH->OPTCCR = FLASH_OPTCCR_CLR_OPTCHANGEERR;
  FLASH->OPTSR_PRG = cvOptionsOptsr(FLASH->OPTSR_PRG);
  FLASH->WPSN_PRG1 = cvSector0WritableWpsn(FLASH->WPSN_PRG1);
  FLASH->OPTCR |= FLASH_OPTCR_OPTSTART;
  uint32_t result = CvOptionProgrammed;
  if (!waitClear(&FLASH->OPTSR_CUR, FLASH_OPTSR_OPT_BUSY, CV_OPTION_LIMIT_MS)) {
    result = CvOptionTimeout;
  } else if ((FLASH->OPTSR_CUR & FLASH_OPTSR_OPTCHANGEERR) != 0u) {
    result = CvOptionChangeError;
  } else if (!sector0WritableNow()) {
    result = CvOptionNotApplied;
  }
  FLASH->OPTCR |= FLASH_OPTCR_OPTLOCK;
  return result;
}

//--------------------------------------------------------------------+
// The run
//--------------------------------------------------------------------+

static void leaveKept(const char* why) {
  cvUpdaterLeave(summarySet("bootloader kept: ", why));
}

int main(void) {
  cvWatchdogKick();
  SystemCoreClockUpdate();
  cvRamClockStart(SystemCoreClock / 1000u);
  // The bootloader turns both caches off before the jump; the compare must read the flash itself.
  if ((SCB->CCR & SCB_CCR_DC_Msk) != 0u) SCB_DisableDCache();

  // Step 1. The jump turned the GPIO clocks off; I2C2 runs from PCLK1, HSI's 64 MHz.
  __HAL_RCC_GPIOB_CLK_ENABLE();
  (void)cvOledStart();
  screenShow(CvScreenBootUpdating);
  cvWatchdogKick();

  // Step 2.
  const CvPayloadState payload = payloadPrepare();
  if (payload != CvPayloadGood) {
    screenShow(CvScreenBootDamaged);
    hold(CV_DAMAGED_SHOW_MS);
    cvUpdaterLeave(summarySet("bootloader file damaged: ", cvPayloadStateName(payload)));
  }
  cvWatchdogKick();

  // Step 3.
  const char* refusal = chipRefusal();
  if (refusal == NULL) refusal = powerRefusal();
  if (refusal != NULL) leaveKept(refusal);
  cvWatchdogKick();

  // Sector 0 as it stands, so the RAM code can tell a pass that never changed it.
  memcpy(cvSector0Old, (const void*)FLASH_BANK1_BASE, sizeof cvSector0Old);

  // Step 4.
  const uint32_t options = sector0Unprotect();
  if (options != CvOptionProgrammed && options != CvOptionAlreadySet) {
    char text[sizeof gSummary];
    leaveKept(textJoin(text, sizeof text, "option bytes ", cvOptionResultName(options)));
  }

  // Steps 5 to 8.
  const uint32_t writeBytes =
      (cvPayloadHeader.length + CV_FLASH_WORD_BYTES - 1u) / CV_FLASH_WORD_BYTES * CV_FLASH_WORD_BYTES;
  cvSector0Install(writeBytes, summarySet("bootloader v" CV_BOOT_VERSION " written", ""));
}
