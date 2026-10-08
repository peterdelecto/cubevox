# Astra packet: cubevox firmware step 0 clock and USB gating (2026-10-08)

Answer each item as you reach it and write it out before starting the next. Short answers. Do not read files; every fact you need is here.

Board: STM32H743VIT6 (LQFP100), HSE 25 MHz crystal, LDO supply (no SMPS), USB OTG_FS device only on PA11/PA12, VBUS on PA9 through a 33k/82k divider used as a plain GPIO input (native VBUS sensing off). Arduino STM32 core 3.0.0, HAL drivers from that core. SAI1 runs from PLL3 (M25 N196.608 P4 = 49.152 MHz), which assumes HSE as the shared PLL source.

Planned `SystemClock_Config` override:
- PWR_LDO_SUPPLY, VOS0 via `__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0)`, wait VOSRDY.
- HSE ON, HSI left at reset default, HSI48 OFF.
- PLL1: source HSE, M 5 (5 MHz ref, PLL1VCIRANGE_2), N 192, FRACN 0, P 2, Q 20, R 2, VCO wide. SYSCLK 480 MHz, HCLK 240 (AHB /2), APB1/2/3/4 /2 = 120 MHz, PLL1Q 48 MHz.
- `HAL_RCC_ClockConfig(..., FLASH_LATENCY_4)`.
- USB kernel clock = PLL1Q (RCC_USBCLKSOURCE_PLL). `HAL_PWREx_EnableUSBVoltageDetector()`.
- PLL2 for ADC: M 5, N 80, P 10 → 40 MHz, VCO medium. AdcClockSelection PLL2.

1. PLL1 at 480 MHz: is anything missing or wrong above (WRHIGHFREQ programming delay bits, PLL1 P even rule, 960 MHz VCO at the wide-range ceiling, VOS0 overdrive sequence on H743 rev V)? State the exact register or HAL call if something is missing.

2. Silicon revision fallback: 480 MHz needs rev V (DBGMCU REV_ID 0x2003). If REV_ID reads 0x1003 (rev Y, 400 MHz max, VOS1), I plan VOS1, N 144, P 2, Q 15 → 360 MHz core, 48 MHz PLL1Q, FLASH_LATENCY_2 (VOS1 at HCLK 180 MHz). Is that a valid combination, and is there a better integer combination at or under 400 MHz that still yields exactly 48 MHz on PLL1Q? State M/N/P/Q and latency.

3. ADC clock: the core builds the ADC with ADC_CLOCK_SYNC_PCLK_DIV4, so adc_ker_ck = HCLK/4 = 60 MHz at HCLK 240 MHz. What is the datasheet maximum adc_ker_ck for H743 rev V, and does 60 MHz exceed it? If so, should the build define ADC_CLOCK_DIV=ADC_CLOCK_ASYNC_DIV1 with PLL2 at 40 MHz as above, or a different PLL2 frequency? Give the number.

4. USB attach gating from firmware: when PA9 reads VBUS (50 ms debounce) call the core's `Serial.begin()`, which runs USBD_Init → HAL_PCD_Init (sets DCTL.SDIS, vbus_sensing disabled so GCCFG.VBDEN cleared and GOTGCTL BVALOEN|BVALOVAL set) → USBD_Start → USB_DevConnect (clears SDIS). When PA9 drops, call `Serial.end()` → HAL_PCD_Stop → SDIS set, then USBD_DeInit. Is this equivalent to holding DCTL.SDIS directly, and is repeated PCD init/deinit across many cable plug cycles safe on the OTG_FS core? Name any register that must be reset between cycles.
