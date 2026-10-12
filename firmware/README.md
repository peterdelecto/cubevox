# cubevox firmware (STM32H743VIT6)

Build only, nothing here flashes. Needs arduino-cli and the STMicroelectronics:stm32 3.0.0 core;
`build.sh` refuses any other core version and prints the gcc and firmware version it builds with.

    ./build.sh        # FQBN STMicroelectronics:stm32:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen
    TAIL=5 ./build.sh # last 5 lines only

After the build, `warn_gate.py` recompiles our sources (sketch, `src/**`, engine headers) with `-Wall -Wextra -Werror`, third-party and `platform/` includes as `-isystem`, so any warning of ours fails the script.

Design: `docs/specs/2026-10-08-firmware-foundation-design.md` (tiers, tests, fault handling),
`2026-10-08-effect-registry-design.md` (cards), `2026-10-08-update-and-slots-design.md` (roadmap).

## Layout

`cubevox/cubevox.ino` is `setup()` and `loop()`. Sources sit under `cubevox/src/` in three tiers;
includes are tier-qualified (`core/registry.h`, `board/pins.h`, `ui/menu.h`).

Module map and ownership rules: `docs/specs/2026-10-12-firmware-structure-design.md`.

- `src/core/`: pure C++17, no Arduino or HAL include. Compiled on the Mac by `test/firmware_test.cpp`
  (`ctest` target `firmware`), so an Arduino include here breaks the host build.
  `registry` (cards, knob descriptors with curves, apply, format), `costs.h`, `layout` (card per slot,
  validation), `settings.h` (output level, input jack), `panel.h` (`PanelReading`), `compose`
  (`ChainSnapshot` from layout, panel, settings and bench overrides), `chain_params`, `knob_filter`
  (debounce, stepping, smoothing), `jack_switch` (mute dip around K103), `canvas` (128 x 64 page-layout
  frame buffer with dirty pages), `fonts` (the font editor's export as `FontDesc`), `display.h`
  (backend interface, `flushStep`), `record` (layout record codec, CRC32, newest-valid scan), `record_store` (append log, sector switch, debounce, as a state machine over `FlashIo`), `unison_rate.h`, `version.h`.
- `src/board/`: one hardware concern per file. `pins.h`, `slots` (toggle pin and mux channels per
  slot), `sysclock`, `usb_link`, `audio` (SAI, DMA, the audio IRQ, chain walk over the published
  snapshot), `controls` (mux scan, ADC, toggles), `input` (encoder and click to `UiInput`), `mute`,
  `input_jack` (PAD_EN for relay K103), `fault` (watchdog, fault handlers, reset cause, audio stall
  watch), `flash_bank2` (bank 2 erase and flash-word program by register, non-blocking erase), `record_service` (store wiring: boot load, ECC recovery, save request, bench hooks), `display_sh1106`, `display_stub` (ASCII render over Serial), `platform` (DFU entry).
- `src/ui/`: pure, also host-compiled. `menu` (state machine, `UiInput` in, `UiAction` out, draws
  on the canvas).

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
- `board/display_sh1106.cpp`: U8x8 init on I2C2 PB10/PB11; `flushStep` sends the lowest dirty canvas page with `drawTile`, once per loop pass.
  The fonts come from `tools/font-editor/fonts/cubevox_fonts.h` through an include path, no copy.
- `board/fault.cpp`: IWDG1 at 2 s, armed at the end of `setup()` and kicked only from `loop()`. HardFault,
  MemManage, BusFault and UsageFault drive XSMT and MUTE_N low by register write, store PC, LR, CFSR, HFSR,
  BFAR and MMFAR in a `.cv_noinit` record (`cubevox_noinit.ld`, top of DTCM) and reset; the next boot report prints `[ERROR] last reset: fault ...`
  or `[boot] fault: none`, plus the decoded `RCC->RSR` reset cause. If the audio block counter stops for
  100 ms while audio runs, the mute latches (`muteLatch()`, MUTE_SW ignored until reset) and one error line
  prints.
- `board/flash_bank2.cpp` and `board/record_service.cpp`: the layout record lives in bank 2 at 0x08100000 and 0x08120000
  (spec `2026-10-12-layout-record-design.md`). A save runs 2 s after the last settings change, one erase or flash-word
  program per `loop()` pass. `build.sh` fails when the .bin exceeds 1 MB so DFU never reaches bank 2. The fault record
  keeps FLASH->SR2 and ECC_FA2; a double ECC fault in a record sector is erased at boot before the scan.
- Bench commands (serial, one letter): `1`-`8` toggle slot off, `0` all on, `k` pin knobs, `c` Chasm reverb, `r` boot
  report, `p` stats, `f` bus fault, `w` watchdog spin, `a` stop audio, `m` reset peaks, `v` vocal clip, `d` DFU,
  `s` save the layout record now, `q` print record status (sector, slot, sequence, name, settings), `e` erase both bank 2 sectors.
- Panel self-test (bench, `core/panel_test`): `t` on/off, `l` summary (pots and toggles still to exercise), `g` raw dump of
  all 16 pots and 8 toggles. While on, the box prints `[panel] pot NN slot S A|B v.vv` when a pot moves and
  `[panel] toggle slot S on|off` on each edge. `i` steps K103 MIC/PEDAL through the jack-switch mute dip, `x` holds and
  releases the output mute through the mute sequence, `o` holds an OLED pattern (border, checkerboard, 5x7, 9x12 and
  16x24 text) with the menu draw paused. `i`, `x`, `o` and `l` need the test on; leaving it restores jack, mute and menu.
  `./bringup.py --board 1 --panel` walks slots 1 to 8 (knob A and B left then right, toggle both ways), fails a pot
  that does not travel, reads backwards, or moves with another channel, then asks about the K103 click, the mute click
  and the OLED pattern. `--no-prompt` skips it.

Real: slot hardware table, default card layout, mux scan (selects held low 1.1 s after boot for U107's 3V3A
delay), smoothing, KEY/SEMITONES stepping, toggles, mute sequence, menu logic and DFU entry, clock tree,
VBUS-gated USB, PLL3 and SAI1 TX/RX DMA setup, engine chain in slot order, watchdog and fault handling.
Stubbed or unverified on hardware: OLED (SH1106 driver compiled, falls back to `stubDisplay()` when no panel
answers at 0x3C), layout persistence (flash bank 2 record written; chip behavior unverified until the WeAct bench run), VA_SENSE shutdown, EQ menu, costs (estimates
until the bench runs on a board). Audio, clocks and mux timing have never run on a board.
