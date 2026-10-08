#include "board/sysclock.h"

#include <Arduino.h>

// Replaces the generic H743 variant's SystemClock_Config, which runs PLL1 from HSI and USB from
// the free-running HSI48 (outside the USB FS tolerance without CRS). All three PLLs share one
// source, so HSE here also makes audio.cpp's PLL3 numbers (M 25 from 25 MHz) correct.
//
// Rev V (REV_ID 0x2003): VOS0, 480 MHz. Older silicon tops out at 400 MHz on VOS1, so it gets
// 360 MHz, the highest integer PLL1 setting that still puts exactly 48 MHz on PLL1Q.

namespace {

constexpr uint32_t kRevIdV = 0x2003;
constexpr uint32_t kHseHz = 25000000;
constexpr uint32_t kPllM = 5;  // 5 MHz reference, PLL1VCIRANGE_2
constexpr uint32_t kUsbHz = 48000000;
constexpr uint32_t kRomBootloaderIdAddr = 0x1FF1E7FE;

// Flash wait states and programming delay follow RM0433 table 16 for the HCLK each profile gives.
struct CoreProfile {
  uint32_t pllN;
  uint32_t pllQ;
  uint32_t voltageScale;
  uint32_t flashLatency;
  uint32_t flashProgramDelay;
};

constexpr CoreProfile kProfileRevV = {192, 20, PWR_REGULATOR_VOLTAGE_SCALE0, FLASH_LATENCY_4,
                                      FLASH_PROGRAMMING_DELAY_2};  // 480 MHz, HCLK 240
constexpr CoreProfile kProfileOlder = {144, 15, PWR_REGULATOR_VOLTAGE_SCALE1, FLASH_LATENCY_2,
                                       FLASH_PROGRAMMING_DELAY_1};  // 360 MHz, HCLK 180

const CoreProfile& profileForSilicon() { return clockIsRevV() ? kProfileRevV : kProfileOlder; }

const char* revName(uint32_t rev) {
  switch (rev) {
    case 0x1001: return "Z";
    case 0x1003: return "Y";
    case 0x2001: return "X";
    case 0x2003: return "V";
  }
  return "?";
}

}  // namespace

bool clockIsRevV() { return HAL_GetREVID() == kRevIdV; }

extern "C" void SystemClock_Config(void) {
  const CoreProfile& p = profileForSilicon();

  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);
  __HAL_RCC_SYSCFG_CLK_ENABLE();  // VOS0 is VOS1 plus SYSCFG overdrive
  if (HAL_PWREx_ControlVoltageScaling(p.voltageScale) != HAL_OK) Error_Handler();
  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
  }

  RCC_OscInitTypeDef osc = {};
  osc.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI48;
  osc.HSEState = RCC_HSE_ON;
  osc.HSI48State = RCC_HSI48_OFF;
  osc.PLL.PLLState = RCC_PLL_ON;
  osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  osc.PLL.PLLM = kPllM;
  osc.PLL.PLLN = p.pllN;
  osc.PLL.PLLFRACN = 0;
  osc.PLL.PLLP = 2;
  osc.PLL.PLLQ = p.pllQ;
  osc.PLL.PLLR = 2;
  osc.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  osc.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

  RCC_ClkInitTypeDef clk = {};
  clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                  RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
  clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  clk.SYSCLKDivider = RCC_SYSCLK_DIV1;
  clk.AHBCLKDivider = RCC_HCLK_DIV2;
  clk.APB3CLKDivider = RCC_APB3_DIV2;
  clk.APB1CLKDivider = RCC_APB1_DIV2;
  clk.APB2CLKDivider = RCC_APB2_DIV2;
  clk.APB4CLKDivider = RCC_APB4_DIV2;
  if (HAL_RCC_ClockConfig(&clk, p.flashLatency) != HAL_OK) Error_Handler();
  __HAL_FLASH_SET_PROGRAM_DELAY(p.flashProgramDelay);  // reset value is the slowest, so after the switch

  // USB from PLL1Q. ADC from PLL2 at 40 MHz (5 MHz * 80 / 10, wide VCO for a 5 MHz reference),
  // used once the build selects an asynchronous ADC clock; the other kernel muxes keep their
  // reset sources.
  RCC_PeriphCLKInitTypeDef periph = {};
  periph.PeriphClockSelection = RCC_PERIPHCLK_USB | RCC_PERIPHCLK_ADC;
  periph.UsbClockSelection = RCC_USBCLKSOURCE_PLL;
  periph.PLL2.PLL2M = kPllM;
  periph.PLL2.PLL2N = 80;
  periph.PLL2.PLL2P = 10;
  periph.PLL2.PLL2Q = 10;
  periph.PLL2.PLL2R = 10;
  periph.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_2;
  periph.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
  periph.PLL2.PLL2FRACN = 0;
  periph.AdcClockSelection = RCC_ADCCLKSOURCE_PLL2;
  if (HAL_RCCEx_PeriphCLKConfig(&periph) != HAL_OK) Error_Handler();

  HAL_PWREx_EnableUSBVoltageDetector();
}

void clockReport(Print& out) {
  const uint32_t rev = HAL_GetREVID();
  out.print("[clock] silicon rev ");
  out.print(revName(rev));
  out.print(" (0x");
  out.print(rev, HEX);
  out.print(") dev 0x");
  out.print(HAL_GetDEVID(), HEX);
  out.print(" rom bootloader id 0x");
  out.println(*reinterpret_cast<const volatile uint8_t*>(kRomBootloaderIdAddr), HEX);
  if (!clockIsRevV()) out.println("[WARN] not rev V, core limited to 360 MHz");

  const bool pllFromHse = (RCC->PLLCKSELR & RCC_PLLCKSELR_PLLSRC) == RCC_PLLCKSELR_PLLSRC_HSE;
  out.print("[clock] pll source ");
  out.print(pllFromHse ? "HSE " : "NOT HSE ");
  out.print(kHseHz / 1000000);
  out.print(" MHz, sysclk ");
  out.print(HAL_RCC_GetSysClockFreq() / 1000000);
  out.print(" MHz, hclk ");
  out.print(HAL_RCC_GetHCLKFreq() / 1000000);
  out.println(" MHz");

  const uint32_t usbHz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USB);
  out.print("[clock] usb ");
  out.print(usbHz);
  out.print(usbHz == kUsbHz ? " Hz from PLL1Q, sai1 " : " Hz WRONG, sai1 ");
  out.print(HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SAI1));
  out.print(" Hz, adc ");
  out.print(HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_ADC));
  out.println(" Hz");
}
