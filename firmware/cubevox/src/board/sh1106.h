// SH1106 128 x 64 OLED on an STM32H7 I2C, write only and polled. The application
// (board/display_sh1106.cpp) and the bootloader (firmware/bootloader/boards/cubevox/oled.c) both
// compile sh1106.c, so the panel starts and draws the same way in both. Each program passes the bus
// its own pin map and clock give it. Every wait is bounded: a missing or stuck panel costs
// milliseconds and a false, never a hang.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stm32h7xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

// 7-bit address of the panel.
#define CV_SH1106_ADDRESS 0x3Cu
// A page is one byte per column, bit 0 on top, and the panel is 8 pages tall.
#define CV_SH1106_PAGE_BYTES 128u
#define CV_SH1106_PAGES 8u

// The bus the panel hangs on. The caller enables the I2C kernel clock and the GPIO port clock first.
typedef struct {
  I2C_TypeDef* i2c;
  GPIO_TypeDef* port;  // SCL and SDA share this port
  uint16_t scl;        // GPIO_PIN_x masks
  uint16_t sda;
  uint8_t alternate;   // the GPIO_AFx value that routes both pins to `i2c`
  uint32_t timingr;    // 400 kHz Fast-mode from the program's I2C kernel clock
} CvSh1106Bus;

// Frees a bus that a reset left mid-transfer (up to nine SCL pulses, then a STOP), starts the I2C
// and sends the panel's init sequence, which leaves the display off. False when the panel does not
// answer.
bool cvSh1106Start(const CvSh1106Bus* bus);

// Writes page `page` at the panel's first visible column. False on a NACK or a timeout, and false at
// once while the I2C is off. A failure turns the I2C off, so only a new start writes again.
bool cvSh1106Page(const CvSh1106Bus* bus, uint8_t page, const uint8_t data[CV_SH1106_PAGE_BYTES]);

// Turns the display on. The caller sends it once a whole frame is in the panel's RAM, so the panel
// never shows what a power-up left there.
bool cvSh1106DisplayOn(const CvSh1106Bus* bus);

#ifdef __cplusplus
}
#endif
