// cubevox pin constants. Source of truth is hardware/PINMAP.md (U1, STM32H743VIT6).
// Net names follow the schematic.

#pragma once

#include <Arduino.h>

namespace pins {

// Pot mux (CD74HC4067 on 3V3A). Keep the selects low until 3V3A is up, and at the lowest GPIO speed.
constexpr uint32_t kMuxS0 = PE12;  // MUX_S0
constexpr uint32_t kMuxS1 = PE10;  // MUX_S1
constexpr uint32_t kMuxS2 = PE8;   // MUX_S2
constexpr uint32_t kMuxS3 = PE9;   // MUX_S3
constexpr uint32_t kPotMuxOut = PC0;  // POT_MUX_OUT (MUX1_SIG_ADC), ADC1/2/3_INP10, 100 R / 1 nF at the pin

// Slot bypass toggles, 10 k pull-up and 100 nF on the board, one throw to GND.
constexpr uint32_t kToggle1 = PA7;  // TOGGLE1
constexpr uint32_t kToggle2 = PC4;  // TOGGLE2
constexpr uint32_t kToggle3 = PB0;  // TOGGLE3
constexpr uint32_t kToggle4 = PA6;  // TOGGLE4
constexpr uint32_t kToggle5 = PB2;  // TOGGLE5
constexpr uint32_t kToggle6 = PA5;  // TOGGLE6
constexpr uint32_t kToggle7 = PE7;  // TOGGLE7
constexpr uint32_t kToggle8 = PA4;  // TOGGLE8

// Global mute and output stage.
constexpr uint32_t kMuteSw = PC6;  // MUTE_SW, low = muted, R120 pull-up on the board
constexpr uint32_t kMuteN = PB5;   // MUTE_N, high closes relay K102, low at reset
constexpr uint32_t kXsmt = PC5;    // XSMT, PCM5102A soft mute, low = muted, R16 pull-down

// Indicators and sense.
constexpr uint32_t kUserLed = PB7;   // USER_LED
constexpr uint32_t kVaSense = PB1;   // VA_SENSE, ADC12_INP5 (not read yet)
constexpr uint32_t kVbusSense = PA9;  // VBUS_SENSE, R6/R7 divider, native OTG sensing off

// USB OTG_FS: PA11 USB_DM, PA12 USB_DP, owned by the core's CDC driver (usb_link.cpp).

// OLED on I2C2 (AF4), SH1106, 4.7 k pull-ups on the board.
constexpr uint32_t kOledScl = PB10;  // I2C_SCL
constexpr uint32_t kOledSda = PB11;  // I2C_SDA

// MENU encoder, EXTI lines 13, 14, 15.
constexpr uint32_t kEncMenuA = PE15;   // ENC_MENU_A
constexpr uint32_t kEncMenuB = PE14;   // ENC_MENU_B
constexpr uint32_t kEncMenuSw = PE13;  // ENC_MENU_SW

// SAI1, AF6. Owned by the audio driver (HAL), listed here for reference.
//   PE2 MCLK_A (ADC_SCKI_SRC), PE3 SD_B (ADC_DOUT), PE4 FS_A (I2S_LRCLK),
//   PE5 SCK_A (I2S_BCLK_SRC), PE6 SD_A (I2S_DATA)

// BOOT0 (pin 94) is the DFU recovery button, not a GPIO the firmware uses.
// PB8, PB9 and PE0 are spare (freed 2026-10-07 when KEY and SEMITONES became pots).
// PB6, PE1, PE11 and PB4 are also unconnected.

}  // namespace pins
