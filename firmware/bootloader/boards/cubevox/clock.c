// System clock for the bootloader. See clock.h. firmware/cubevox/src/board/sysclock.cpp is the
// reference for the crystal profile. HSI48 runs USB only when the crystal path fails, trimmed by CRS
// from the host's start-of-frame packets; free-running it is outside the USB tolerance.

#include "clock.h"

#include "board_api.h"
#include "boot_block.h"

CV_STATIC_ASSERT(sysclk_360_mhz, CV_SYSCLK_HZ == 360000000u);
CV_STATIC_ASSERT(usb_48_mhz, CV_USB_HZ == 48000000u);

#define CV_VOS_READY_TIMEOUT_MS 100u
// USB start-of-frame packets arrive once a millisecond.
#define CV_USB_SOF_HZ 1000u

static uint32_t gFailedStep;

// Without even HSI48 nothing can run USB or the application. The box stays dark until a power
// cycle; the shop path is BOOT0.
static void clockFail(void) {
  __disable_irq();
  for (;;) {}
}

static bool supplyStart(void) {
  return HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY) == HAL_OK;
}

static bool voltageStart(void) {
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) return false;
  const uint32_t startMs = HAL_GetTick();
  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
    if (HAL_GetTick() - startMs >= CV_VOS_READY_TIMEOUT_MS) return false;
  }
  return true;
}

static bool pll1Start(void) {
  RCC_OscInitTypeDef osc = {0};
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI48;
  osc.HSEState = RCC_HSE_ON;
  osc.HSI48State = RCC_HSI48_OFF;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = CV_PLL_M;
  osc.PLL.PLLN = CV_PLL_N;
  osc.PLL.PLLFRACN = 0u;
  osc.PLL.PLLP = CV_PLL_P;
  osc.PLL.PLLQ = CV_PLL_Q;
  osc.PLL.PLLR = CV_PLL_R;
  osc.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;  // 5 MHz reference
  osc.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  return HAL_RCC_OscConfig(&osc) == HAL_OK;
}

static bool busesStart(void) {
  RCC_ClkInitTypeDef clk = {0};
  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                  RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.SYSCLKDivider = RCC_SYSCLK_DIV1;
  clk.AHBCLKDivider = RCC_HCLK_DIV2;
  clk.APB3CLKDivider = RCC_APB3_DIV2;
  clk.APB1CLKDivider = RCC_APB1_DIV2;
  clk.APB2CLKDivider = RCC_APB2_DIV2;
  clk.APB4CLKDivider = RCC_APB4_DIV2;
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) return false;
  // The reset value is the slowest, so it changes after the switch.
  __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_1);
  return true;
}

static bool usbClockStart(void) {
  RCC_PeriphCLKInitTypeDef periph = {0};
  periph.PeriphClockSelection = RCC_PERIPHCLK_USB;
  periph.UsbClockSelection = RCC_USBCLKSOURCE_PLL;
  return HAL_RCCEx_PeriphCLKConfig(&periph) == HAL_OK;
}

// The crystal profile, step by step. Returns the step that failed, 0 when all ran.
static uint32_t crystalStart(void) {
  if (!supplyStart()) return CvClockStepSupply;
  if (!voltageStart()) return CvClockStepVoltageScale;
  if (!pll1Start()) return CvClockStepOscillator;
  if (!busesStart()) return CvClockStepClockTree;
  if (!usbClockStart()) return CvClockStepPeripheralClocks;
  return 0u;
}

// Back to HSI on every bus, then USB from HSI48 with CRS locking it to the host's frames.
static void fallbackStart(void) {
  if (HAL_RCC_DeInit() != HAL_OK) clockFail();
  RCC_OscInitTypeDef osc = {0};
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  osc.HSI48State = RCC_HSI48_ON;
  osc.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) clockFail();
  RCC_PeriphCLKInitTypeDef periph = {0};
  periph.PeriphClockSelection = RCC_PERIPHCLK_USB;
  periph.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK) clockFail();
  __HAL_RCC_CRS_CLK_ENABLE();
  RCC_CRSInitTypeDef crs = {0};
  crs.Prescaler = RCC_CRS_SYNC_DIV1;
  crs.Source = RCC_CRS_SYNC_SOURCE_USB2;
  crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000u, CV_USB_SOF_HZ);
  crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
  crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
  HAL_RCCEx_CRSConfig(&crs);
}

void clock_init(void) {
  // The port never calls HAL_Init. The HAL's timeouts below need the tick, and
  // HAL_RCC_ClockConfig's closing HAL_InitTick needs a valid uwTickPrio.
  HAL_InitTick(TICK_INT_PRIORITY);
  gFailedStep = crystalStart();
  if (gFailedStep != 0u) fallbackStart();
  HAL_PWREx_EnableUSBVoltageDetector();
}

uint32_t cvClockFailedStep(void) {
  return gFailedStep;
}
