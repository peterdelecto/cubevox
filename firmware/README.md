# cubevox firmware (STM32H743VIT6)

Build only, nothing here flashes. Needs arduino-cli and the STMicroelectronics:stm32 3.0.0 core.

    ./build.sh        # FQBN STMicroelectronics:stm32:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen

Layout: `cubevox/` is the sketch. `platform/` holds verbatim copies from Teensy-H7-Port (first line names the source).
Engine headers come from `../engine` by include path. No emulator files are included.

- `cubevox/display_sh1106.h`: SH1106 OLED interface (`sh1106Present()`, `sh1106Display()`).
- `cubevox/display_sh1106.cpp`: U8x8 text driver on I2C2 PB10/PB11, 8 rows of 16 characters.

Real: slot table, mux scan, smoothing, KEY/SEMITONES stepping, toggles, mute sequence, menu logic and DFU entry,
PLL3 and SAI1 TX/RX DMA setup, engine chain in slot order.
Stubbed or unverified on hardware: OLED (SH1106 driver compiled, falls back to `stubDisplay()` when no panel answers at 0x3C), settings
persistence (flash bank 2, RAM only),
VA_SENSE shutdown, EQ menu. Audio, clocks and mux timing have never run on a board.
