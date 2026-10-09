# cubevox firmware (STM32H743VIT6)

Build only, nothing here flashes. Needs arduino-cli and the STMicroelectronics:stm32 3.0.0 core;
`build.sh` refuses any other core version and prints the gcc and firmware version it builds with.

    ./build.sh        # FQBN STMicroelectronics:stm32:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen
    TAIL=5 ./build.sh # last 5 lines only

Design: `docs/specs/2026-10-08-firmware-foundation-design.md` (tiers, tests, fault handling),
`2026-10-08-effect-registry-design.md` (cards), `2026-10-08-update-and-slots-design.md` (roadmap).

## Layout

`cubevox/cubevox.ino` is `setup()` and `loop()`. Sources sit under `cubevox/src/` in three tiers;
includes are tier-qualified (`core/registry.h`, `board/pins.h`, `ui/menu.h`).

- `src/core/`: pure C++17, no Arduino or HAL include. Compiled on the Mac by `test/firmware_test.cpp`
  (`ctest` target `firmware`), so an Arduino include here breaks the host build.
  `registry` (cards, knob descriptors, apply, format), `costs.h`, `chain_params` (the parameter
  bundle the audio IRQ reads), `display.h` (text display interface), `unison_rate.h`, `version.h`.
- `src/board/`: one hardware concern per file. `pins.h`, `slots` (toggle pin and mux channels per
  slot), `sysclock`, `usb_link`, `audio` (SAI, DMA, the audio IRQ, chain walk), `controls` (mux
  scan, smoothing, stepping, toggles), `mute`, `fault` (watchdog, fault handlers, reset cause, audio
  stall watch), `display_sh1106`, `platform` (DFU entry).
- `src/ui/`: `menu` (encoder state machine and OLED screens).

`platform/` holds verbatim copies from Teensy-H7-Port (first line names the source); they are on
the include path and come into the build through `board/platform_shim.cpp`. Engine headers come
from `../engine` by include path. No emulator files are included.

## Notes

- `bringup.py`: first-article checks for a new board over USB, optional fault and DFU drills, operator
  questions, log to `hardware/bringup/`. `./bringup.py --board 1 --faults --dfu`; on the WeAct add
  `--weact --no-oled`.
- `board/sysclock.cpp`: `SystemClock_Config` override (spec 2026-10-08 step 0). HSE 25 MHz, PLL1 M5 N192 P2 Q20
  for 480 MHz on rev V silicon (360 MHz, N144 Q15, on older revisions), USB from PLL1Q, HSI48 off, PLL2 40 MHz for
  the ADC. `clockReport()` prints silicon rev, ROM bootloader ID at 0x1FF1E7FE and the clock tree.
- `board/usb_link.cpp`: USB CDC starts only while PA9 reads VBUS and stops when it drops (self-powered box,
  `-DUSBD_SELF_POWERED=1` in build.sh). The boot report prints each time a terminal opens the port.
  Astra's review of the clock numbers and the gating: `docs/audits/2026-10-08-astra-clock-response.md`.
- `board/display_sh1106.cpp`: U8x8 text driver on I2C2 PB10/PB11, 8 rows of 16 characters.
- `board/fault.cpp`: IWDG1 at 2 s, armed at the end of `setup()` and kicked only from `loop()`. HardFault,
  MemManage, BusFault and UsageFault drive XSMT and MUTE_N low by register write, store PC, LR, CFSR, HFSR,
  BFAR and MMFAR in a `.cv_noinit` record (`cubevox_noinit.ld`, top of DTCM) and reset; the next boot report prints `[ERROR] last reset: fault ...`
  or `[boot] fault: none`, plus the decoded `RCC->RSR` reset cause. If the audio block counter stops for
  100 ms while audio runs, the mute latches (`muteLatch()`, MUTE_SW ignored until reset) and one error line
  prints.

Real: slot hardware table, default card layout, mux scan (selects held low 1.1 s after boot for U107's 3V3A
delay), smoothing, KEY/SEMITONES stepping, toggles, mute sequence, menu logic and DFU entry, clock tree,
VBUS-gated USB, PLL3 and SAI1 TX/RX DMA setup, engine chain in slot order, watchdog and fault handling.
Stubbed or unverified on hardware: OLED (SH1106 driver compiled, falls back to `stubDisplay()` when no panel
answers at 0x3C), layout persistence (flash bank 2, RAM only), VA_SENSE shutdown, EQ menu, costs (estimates
until the bench runs on a board). Audio, clocks and mux timing have never run on a board.
