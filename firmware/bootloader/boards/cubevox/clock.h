// The bootloader's clocks. clock.c runs firmware/cubevox/src/board/sysclock.cpp's 360 MHz profile
// on every silicon revision: PLL1 from the 25 MHz crystal (HSE_VALUE, board.mk) with M 5, N 144,
// P 2, Q 15, R 2, VOS1, AHB and every APB at /2, flash latency 2, USB at exactly 48 MHz from PLL1Q.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CV_PLL_M 5u
#define CV_PLL_N 144u
#define CV_PLL_P 2u
#define CV_PLL_Q 15u
#define CV_PLL_R 2u
#define CV_SYSCLK_HZ (HSE_VALUE / CV_PLL_M * CV_PLL_N / CV_PLL_P)
#define CV_HCLK_HZ   (CV_SYSCLK_HZ / 2u)
#define CV_PCLK1_HZ  (CV_HCLK_HZ / 2u)
#define CV_USB_HZ    (HSE_VALUE / CV_PLL_M * CV_PLL_N / CV_PLL_Q)
// With the crystal or PLL1 down the core stays on HSI at its reset 64 MHz, every divider at 1.
#define CV_HSI_HZ    64000000u

// Called by the port's board_init before anything else runs.
void clock_init(void);

// Saves RCC_RSR for this start and clears it (board_hooks.c). clock_init calls it first.
void cvResetFlagsTake(void);

// The CvClockStep that failed at this start, 0 when the crystal and PLL1 run. After a failure the
// core runs from HSI and USB from HSI48 trimmed by CRS, so update mode still serves DIAG.TXT.
uint32_t cvClockFailedStep(void);

#ifdef __cplusplus
}
#endif
