// SH1106 128 x 64 OLED on I2C2: PB10 SCL, PB11 SDA, AF4, 400 kHz, 4.7 k pull-ups on the board
// (firmware/cubevox/src/board/pins.h). The driver is firmware/cubevox/src/board/sh1106.c, the one the
// application runs, so update mode draws the same way up. A missing or stuck panel costs
// milliseconds, then this file stays quiet and update mode carries on without it.

#include "oled.h"

#include "boot_block.h"
#include "clock.h"
#include "sh1106.h"
#include "stm32h7xx_hal.h"

// 400 kHz Fast-mode from the 90 MHz rcc_pclk1 clock.c gives I2C2 (I2C123SEL keeps its reset value):
// PRESC 2 (33.3 ns), SCLDEL 12 (433 ns setup), SDADEL 7 (233 ns hold), SCLH 30, SCLL 44. The HSI
// fallback's slower PCLK1 only lengthens every one of these times, which Fast-mode allows.
#define CV_OLED_TIMINGR 0x20C71E2Cu
#define CV_OLED_TIMINGR_PCLK1_HZ 90000000u
CV_STATIC_ASSERT(oled_timing_matches_pclk1, CV_PCLK1_HZ == CV_OLED_TIMINGR_PCLK1_HZ);
CV_STATIC_ASSERT(oled_fallback_is_slower, CV_HSI_HZ <= CV_OLED_TIMINGR_PCLK1_HZ);
CV_STATIC_ASSERT(oled_frame_rows_are_pages,
                 CV_FRAME_WIDTH == CV_SH1106_PAGE_BYTES && CV_FRAME_BYTES == CV_SH1106_PAGES * CV_SH1106_PAGE_BYTES);

static const CvSh1106Bus kBus = {I2C2, GPIOB, GPIO_PIN_10, GPIO_PIN_11, GPIO_AF4_I2C2, CV_OLED_TIMINGR};

static bool gPresent;
static bool gDisplayOn;

bool cvOledStart(void) {
  __HAL_RCC_I2C2_CLK_ENABLE();
  gPresent = cvSh1106Start(&kBus);
  return gPresent;
}

void cvOledShow(const uint8_t frame[CV_FRAME_BYTES]) {
  for (uint32_t page = 0; gPresent && page < CV_SH1106_PAGES; ++page) {
    gPresent = cvSh1106Page(&kBus, (uint8_t)page, &frame[page * CV_FRAME_WIDTH]);
  }
  if (gPresent && !gDisplayOn) {
    gPresent = cvSh1106DisplayOn(&kBus);
    gDisplayOn = gPresent;
  }
}
