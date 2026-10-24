// System clock for the bootloader. firmware/cubevox/src/board/sysclock.cpp is the reference: PLL1
// from the 25 MHz crystal, USB from PLL1Q at exactly 48 MHz, HSI48 off (free-running it is outside
// the USB tolerance without CRS), the USB voltage detector on. The bootloader runs that file's
// 360 MHz profile on every silicon revision instead of 480 MHz on VOS0: M 5, N 144, P 2 (SYSCLK
// 360 MHz), Q 15, R 2, VOS1, AHB and every APB at /2 (HCLK 180 MHz, PCLK 90 MHz), flash latency 2,
// programming delay 1.

#include "board_api.h"

#define CV_PLL_M 5u
#define CV_PLL_N 144u
#define CV_PLL_P 2u
#define CV_PLL_Q 15u
#define CV_PLL_R 2u
#define CV_VOS_READY_TIMEOUT_MS 100u

// A clock that will not start leaves nothing to run USB or the application on. The box stays
// dark until a power cycle; the shop path is BOOT0.
static void clockFail(void) {
  __disable_irq();
  for (;;) {}
}

static void supplyStart(void) {
  if (HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY) != HAL_OK) clockFail();
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) clockFail();
  const uint32_t startMs = HAL_GetTick();
  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
    if (HAL_GetTick() - startMs >= CV_VOS_READY_TIMEOUT_MS) clockFail();
  }
}

static void pll1Start(void) {
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
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) clockFail();
}

static void busesStart(void) {
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
  if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) clockFail();
  // The reset value is the slowest, so it changes after the switch.
  __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_1);
}

static void usbClockStart(void) {
  RCC_PeriphCLKInitTypeDef periph = {0};
  periph.PeriphClockSelection = RCC_PERIPHCLK_USB;
  periph.UsbClockSelection = RCC_USBCLKSOURCE_PLL;
  if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK) clockFail();
  HAL_PWREx_EnableUSBVoltageDetector();
}

void clock_init(void) {
  // The port never calls HAL_Init. The HAL's timeouts below need the tick, and
  // HAL_RCC_ClockConfig's closing HAL_InitTick needs a valid uwTickPrio.
  HAL_InitTick(TICK_INT_PRIORITY);
  supplyStart();
  pll1Start();
  busesStart();
  usbClockStart();
}
