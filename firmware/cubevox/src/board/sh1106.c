// The init sequence is the one U8x8's SH1106 128x64 NONAME driver sends, with segment remap A1 and
// COM scan C8 (U8G2_R0), so the panel draws the way up the enclosure expects.

#include "sh1106.h"

// The SH1106 drives 132 columns; the 128-column panel starts at column 2.
#define CV_SH1106_COLUMN 2u
// Status polls before a wait counts as failed, a few milliseconds.
#define CV_SH1106_POLLS 200000u
#define CV_SH1106_COMMANDS 0x00u
#define CV_SH1106_DATA 0x40u

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

static void pause(void) {
  for (volatile uint32_t n = 0; n < 1000u; ++n) {}
}

// A reset in the middle of a transfer can leave the panel holding SDA low. Up to nine SCL pulses let
// it finish its byte, then SDA rising while SCL is high is a STOP.
static void busRecover(const CvSh1106Bus* bus) {
  GPIO_InitTypeDef pins = {0};
  pins.Pin = (uint32_t)bus->scl | bus->sda;
  pins.Mode = GPIO_MODE_OUTPUT_OD;
  pins.Pull = GPIO_NOPULL;
  pins.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_WritePin(bus->port, (uint16_t)(bus->scl | bus->sda), GPIO_PIN_SET);
  HAL_GPIO_Init(bus->port, &pins);
  pause();
  for (uint8_t i = 0; i < 9u && HAL_GPIO_ReadPin(bus->port, bus->sda) == GPIO_PIN_RESET; ++i) {
    HAL_GPIO_WritePin(bus->port, bus->scl, GPIO_PIN_RESET);
    pause();
    HAL_GPIO_WritePin(bus->port, bus->scl, GPIO_PIN_SET);
    pause();
  }
  HAL_GPIO_WritePin(bus->port, bus->sda, GPIO_PIN_RESET);
  pause();
  HAL_GPIO_WritePin(bus->port, bus->sda, GPIO_PIN_SET);
  pause();
}

static void i2cStart(const CvSh1106Bus* bus) {
  GPIO_InitTypeDef pins = {0};
  pins.Pin = (uint32_t)bus->scl | bus->sda;
  pins.Mode = GPIO_MODE_AF_OD;
  pins.Pull = GPIO_NOPULL;
  pins.Speed = GPIO_SPEED_FREQ_LOW;
  pins.Alternate = bus->alternate;
  HAL_GPIO_Init(bus->port, &pins);
  bus->i2c->CR1 = 0u;
  bus->i2c->TIMINGR = bus->timingr;
  bus->i2c->CR1 = I2C_CR1_PE;
}

// Polls until `flag` rises. False on a NACK or when the polls run out.
static bool waitFor(const CvSh1106Bus* bus, uint32_t flag) {
  for (uint32_t i = 0; i < CV_SH1106_POLLS; ++i) {
    const uint32_t status = bus->i2c->ISR;
    if (status & flag) return true;
    if (status & I2C_ISR_NACKF) return false;
  }
  return false;
}

// One transfer: the control byte, then `length` bytes (at most 254). A failure turns the I2C off,
// which drops the transfer and frees the lines.
static bool panelWrite(const CvSh1106Bus* bus, uint8_t control, const uint8_t* data, uint32_t length) {
  I2C_TypeDef* i2c = bus->i2c;
  if ((i2c->CR1 & I2C_CR1_PE) == 0u) return false;
  i2c->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
  i2c->CR2 = (CV_SH1106_ADDRESS << 1) | ((length + 1u) << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START;
  bool ok = waitFor(bus, I2C_ISR_TXIS);
  if (ok) i2c->TXDR = control;
  for (uint32_t i = 0; ok && i < length; ++i) {
    ok = waitFor(bus, I2C_ISR_TXIS);
    if (ok) i2c->TXDR = data[i];
  }
  ok = ok && waitFor(bus, I2C_ISR_STOPF);
  i2c->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
  if (!ok) i2c->CR1 = 0u;
  return ok;
}

bool cvSh1106Start(const CvSh1106Bus* bus) {
  busRecover(bus);
  i2cStart(bus);
  return panelWrite(bus, CV_SH1106_COMMANDS, kInit, sizeof kInit);
}

bool cvSh1106Page(const CvSh1106Bus* bus, uint8_t page, const uint8_t data[CV_SH1106_PAGE_BYTES]) {
  const uint8_t address[3] = {(uint8_t)(0xB0u | page), (uint8_t)(CV_SH1106_COLUMN & 0x0Fu),
                              (uint8_t)(0x10u | (CV_SH1106_COLUMN >> 4))};
  return panelWrite(bus, CV_SH1106_COMMANDS, address, sizeof address) &&
         panelWrite(bus, CV_SH1106_DATA, data, CV_SH1106_PAGE_BYTES);
}

bool cvSh1106DisplayOn(const CvSh1106Bus* bus) { return panelWrite(bus, CV_SH1106_COMMANDS, &kDisplayOn, 1u); }
