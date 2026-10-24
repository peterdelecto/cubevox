// SH1106 128 x 64 OLED on I2C2: PB10 SCL, PB11 SDA, AF4, 400 kHz, 4.7 k pull-ups on the board
// (firmware/cubevox/src/board/pins.h). The init sequence is the one U8x8's SH1106 128x64 NONAME
// driver sends for the firmware, with segment remap A1 and COM scan C8 (U8G2_R0), so update mode
// draws the same way up as the application. Every wait is bounded: a missing or stuck panel costs
// milliseconds, then the driver stays quiet and update mode carries on without it.

#include "oled.h"

#include "boot_block.h"
#include "clock.h"
#include "stm32h7xx_hal.h"

// 7-bit address, kOledAddress in firmware/cubevox/src/board/display_sh1106.h.
#define CV_OLED_ADDRESS 0x3Cu
// The SH1106 drives 132 columns; the 128-column panel starts at column 2.
#define CV_OLED_COLUMN 2u
// 400 kHz Fast-mode from the 90 MHz rcc_pclk1 clock.c gives I2C2 (I2C123SEL keeps its reset value):
// PRESC 2 (33.3 ns), SCLDEL 12 (433 ns setup), SDADEL 7 (233 ns hold), SCLH 30, SCLL 44. The HSI
// fallback's slower PCLK1 only lengthens every one of these times, which Fast-mode allows.
#define CV_OLED_TIMINGR 0x20C71E2Cu
#define CV_OLED_TIMINGR_PCLK1_HZ 90000000u
CV_STATIC_ASSERT(oled_timing_matches_pclk1, CV_PCLK1_HZ == CV_OLED_TIMINGR_PCLK1_HZ);
CV_STATIC_ASSERT(oled_fallback_is_slower, CV_HSI_HZ <= CV_OLED_TIMINGR_PCLK1_HZ);
// Status polls before a transfer counts as failed, a few milliseconds.
#define CV_OLED_POLLS 200000u
#define CV_OLED_COMMANDS 0x00u
#define CV_OLED_DATA 0x40u
#define CV_OLED_SCL GPIO_PIN_10
#define CV_OLED_SDA GPIO_PIN_11

static const uint8_t kInit[] = {
    0xAEu,         // display off
    0xD5u, 0x80u,  // clock divide and oscillator
    0xA8u, 0x3Fu,  // multiplex ratio, 64 rows
    0xD3u, 0x00u,  // display offset
    0x40u,         // start line 0
    0x8Du, 0x14u,  // charge pump, an SSD1306 command U8x8 sends to this panel too
    0x20u, 0x00u,  // addressing mode, likewise
    0xA1u,         // segment remap
    0xC8u,         // COM scan reversed
    0xDAu, 0x12u,  // COM pins
    0x81u, 0xCFu,  // contrast
    0xD9u, 0xF1u,  // pre-charge
    0xDBu, 0x40u,  // VCOMH deselect
    0x2Eu,         // scroll off
    0xA4u,         // pixels follow RAM
    0xA6u,         // not inverted
};
static const uint8_t kDisplayOn = 0xAFu;

static bool gPresent;
static bool gDisplayOn;

static void pause(void) {
  for (volatile uint32_t n = 0; n < 1000u; ++n) {}
}

// A reset in the middle of the firmware's own transfer can leave the panel holding SDA low. Up to
// nine SCL pulses let it finish its byte, then SDA rising while SCL is high is a STOP.
static void busRecover(void) {
  GPIO_InitTypeDef pins = {0};
  pins.Pin = CV_OLED_SCL | CV_OLED_SDA;
  pins.Mode = GPIO_MODE_OUTPUT_OD;
  pins.Pull = GPIO_NOPULL;
  pins.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_WritePin(GPIOB, CV_OLED_SCL | CV_OLED_SDA, GPIO_PIN_SET);
  HAL_GPIO_Init(GPIOB, &pins);
  pause();
  for (uint8_t i = 0; i < 9u && HAL_GPIO_ReadPin(GPIOB, CV_OLED_SDA) == GPIO_PIN_RESET; ++i) {
    HAL_GPIO_WritePin(GPIOB, CV_OLED_SCL, GPIO_PIN_RESET);
    pause();
    HAL_GPIO_WritePin(GPIOB, CV_OLED_SCL, GPIO_PIN_SET);
    pause();
  }
  HAL_GPIO_WritePin(GPIOB, CV_OLED_SDA, GPIO_PIN_RESET);
  pause();
  HAL_GPIO_WritePin(GPIOB, CV_OLED_SDA, GPIO_PIN_SET);
  pause();
}

static void i2cStart(void) {
  GPIO_InitTypeDef pins = {0};
  pins.Pin = CV_OLED_SCL | CV_OLED_SDA;
  pins.Mode = GPIO_MODE_AF_OD;
  pins.Pull = GPIO_NOPULL;
  pins.Speed = GPIO_SPEED_FREQ_LOW;
  pins.Alternate = GPIO_AF4_I2C2;
  HAL_GPIO_Init(GPIOB, &pins);
  __HAL_RCC_I2C2_CLK_ENABLE();
  I2C2->CR1 = 0u;
  I2C2->TIMINGR = CV_OLED_TIMINGR;
  I2C2->CR1 = I2C_CR1_PE;
}

// Polls until `flag` rises. False on a NACK or when the polls run out.
static bool waitFor(uint32_t flag) {
  for (uint32_t i = 0; i < CV_OLED_POLLS; ++i) {
    const uint32_t status = I2C2->ISR;
    if (status & flag) return true;
    if (status & I2C_ISR_NACKF) return false;
  }
  return false;
}

// One transfer: the control byte, then `length` bytes (at most 254). A failure disables I2C2, which
// drops the transfer and frees the lines.
static bool panelWrite(uint8_t control, const uint8_t* data, uint32_t length) {
  I2C2->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
  I2C2->CR2 = (CV_OLED_ADDRESS << 1) | ((length + 1u) << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START;
  bool ok = waitFor(I2C_ISR_TXIS);
  if (ok) I2C2->TXDR = control;
  for (uint32_t i = 0; ok && i < length; ++i) {
    ok = waitFor(I2C_ISR_TXIS);
    if (ok) I2C2->TXDR = data[i];
  }
  ok = ok && waitFor(I2C_ISR_STOPF);
  I2C2->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
  if (!ok) I2C2->CR1 = 0u;
  return ok;
}

bool cvOledStart(void) {
  busRecover();
  i2cStart();
  gPresent = panelWrite(CV_OLED_COMMANDS, kInit, sizeof kInit);
  return gPresent;
}

void cvOledShow(const uint8_t frame[CV_FRAME_BYTES]) {
  for (uint32_t page = 0; gPresent && page < CV_FRAME_BYTES / CV_FRAME_WIDTH; ++page) {
    const uint8_t address[3] = {(uint8_t)(0xB0u | page), (uint8_t)(CV_OLED_COLUMN & 0x0Fu),
                                (uint8_t)(0x10u | (CV_OLED_COLUMN >> 4))};
    gPresent = panelWrite(CV_OLED_COMMANDS, address, sizeof address) &&
               panelWrite(CV_OLED_DATA, &frame[page * CV_FRAME_WIDTH], CV_FRAME_WIDTH);
  }
  if (gPresent && !gDisplayOn) {
    gPresent = panelWrite(CV_OLED_COMMANDS, &kDisplayOn, 1u);
    gDisplayOn = gPresent;
  }
}
