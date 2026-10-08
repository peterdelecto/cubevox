# cubevox firmware (STM32H743VIT6)

Build only, nothing here flashes. Needs arduino-cli and the STMicroelectronics:stm32 3.0.0 core.

    ./build.sh        # FQBN STMicroelectronics:stm32:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen

Layout: `cubevox/` is the sketch. `platform/` holds verbatim copies from Teensy-H7-Port (first line names the source).
Engine headers come from `../engine` by include path. No emulator files are included.

- `cubevox/display_sh1106.h`: SH1106 OLED interface (`sh1106Present()`, `sh1106Display()`).
- `cubevox/display_sh1106.cpp`: U8x8 text driver on I2C2 PB10/PB11, 8 rows of 16 characters.
- `cubevox/sysclock.cpp`: `SystemClock_Config` override (spec 2026-10-08 step 0). HSE 25 MHz, PLL1 M5 N192 P2 Q20
  for 480 MHz on rev V silicon (360 MHz, N144 Q15, on older revisions), USB from PLL1Q, HSI48 off, PLL2 40 MHz for
  the ADC. `clockReport()` prints silicon rev, ROM bootloader ID at 0x1FF1E7FE and the clock tree.
- `cubevox/usb_link.cpp`: USB CDC starts only while PA9 reads VBUS and stops when it drops (self-powered box,
  `-DUSBD_SELF_POWERED=1` in build.sh). The boot report prints each time a terminal opens the port.
  Astra's review of the clock numbers and the gating: `docs/audits/2026-10-08-astra-clock-response.md`.

Real: slot table, mux scan (selects held low 1.1 s after boot for U107's 3V3A delay), smoothing, KEY/SEMITONES
stepping, toggles, mute sequence, menu logic and DFU entry, clock tree, VBUS-gated USB, PLL3 and SAI1 TX/RX DMA
setup, engine chain in slot order.
Stubbed or unverified on hardware: OLED (SH1106 driver compiled, falls back to `stubDisplay()` when no panel answers at 0x3C), settings
persistence (flash bank 2, RAM only),
VA_SENSE shutdown, EQ menu. Audio, clocks and mux timing have never run on a board.
