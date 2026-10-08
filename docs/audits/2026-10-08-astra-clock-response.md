1. **480 MHz configuration is valid on rev V.** P=2 is legal; 960 MHz VCO is within specification. Add:
   ```c
   __HAL_RCC_SYSCFG_CLK_ENABLE(); // before VOS0 macro
   MODIFY_REG(FLASH->ACR, FLASH_ACR_WRHIGHFREQ,
              FLASH_ACR_WRHIGHFREQ_1); // 0b10
   ```
   Use `HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY)` first. The H743 VOS0 macro handles VOS1 → ready → `SYSCFG_PWRCR.ODEN` → ready. `FLASH_LATENCY_4` is correct. [ST reference manual](https://www.st.com/resource/en/reference_manual/rm0433-stm32h742-stm32h743-753-and-stm32h750-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)

2. **360 MHz is valid and the highest integer solution:** **M5/N144/P2/Q15**, VOS1, `FLASH_LATENCY_2`, `WRHIGHFREQ=01`. Although 384 MHz would permit Q16, generating its 768 MHz VCO from 25 MHz requires integer N≥768, exceeding N≤512. [PLL constraints](https://www.st.com/resource/en/reference_manual/dm00314099-stm32h742-stm32h743753-and-stm32h750-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)

3. **The rev V conversion-clock limit is 50 MHz.** Its additional fixed ÷2 makes your synchronous configuration **30 MHz**, so it does **not** exceed the limit. Async ÷1 with PLL2P=40 MHz gives **20 MHz**; PLL2P=80 MHz gives **40 MHz**. With M5, select **`RCC_PLL2VCOWIDE`**, because medium requires a 1–2 MHz reference. Rev Y lacks the extra ÷2 and requires conversion clock ≤36 MHz. [Datasheet](https://www.st.com/resource/en/datasheet/stm32h743vi.pdf), [reference manual](https://www.st.com/resource/en/reference_manual/rm0433-stm32h742-stm32h743753-and-stm32h750-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)

4. **Equivalent for electrical attach gating; different for software state.** `begin/end` also rebuilds/tears down the USB stack. Repeated initialization is supported, provided teardown finishes and IRQs/transfers cannot race it. **No additional manual register reset is required:** initialization performs `GRSTCTL.CSRST`, clears `DIEPTXF[]`, flushes FIFOs, and reinitializes endpoint/interrupt state. No per-cycle clearing of `GOTGCTL.BVALOEN/BVALOVAL` is needed with sensing permanently disabled. Call begin/end once per debounced transition. [ST USB driver](https://github.com/STMicroelectronics/stm32h7xx-hal-driver/blob/master/Src/stm32h7xx_ll_usb.c)