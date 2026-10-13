# cubevox firmware (STM32H743VIT6)

Build only, nothing here flashes. Needs arduino-cli and the STMicroelectronics:stm32 3.0.0 core;
`build.sh` refuses any other core version and prints the gcc and firmware version it builds with.

    ./build.sh          # release image; FQBN STMicroelectronics:stm32:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen
    BENCH=1 ./build.sh  # bench image (drills, panel test)
    TAIL=5 ./build.sh   # last 5 lines only

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
  validation with a reason), `layout_fade` (50 ms out/in sequencer for a live layout change, engines to reset),
  `protocol` (JSON line framing, request parse, reply formatting into a `Writer`), `tx_queue` (bounded line queue for serial output), `settings.h` (output level, input jack), `panel.h` (`PanelReading`), `compose`
  (`ChainSnapshot` from layout, panel, settings and bench overrides), `chain_params`, `knob_filter`
  (debounce, stepping, smoothing), `jack_switch` (mute dip around K103), `canvas` (128 x 64 page-layout
  frame buffer with dirty pages), `fonts` (the font editor's export as `FontDesc`), `display.h`
  (backend interface, `flushStep`), `record` (layout record codec, CRC32, newest-valid scan), `record_store` (append log, sector switch, debounce, as a state machine over `FlashIo`), `unison_rate.h`, `version.h`.
- `src/board/`: one hardware concern per file. `pins.h`, `slots` (toggle pin and mux channels per
  slot), `sysclock`, `usb_link`, `audio` (SAI, DMA, the audio IRQ, chain walk over the published
  snapshot, fade and engine reset on a layout change), `serial_link` (CDC bytes to JSON lines, reply writer), `bench` (query ops), `bench_drill` and `bench_panel` (bench-image ops),
  `controls` (mux scan, ADC, toggles), `input` (encoder and click to `UiInput`), `mute`,
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
- Serial protocol (spec `2026-10-12-serial-protocol-design.md`): one JSON object per line on the same CDC port, requests
  `{"id":N,"cmd":"..."}` with `hello`, `get_layout`, `set_layout`, `status` and `enter_dfu`; a `{` outside a line starts a
  JSON line, any other byte is ignored and counted, lines over 768 bytes get an error reply with id 0. `set_layout` validates
  (`layoutValid` gives the reason), publishes, and requests a record save; the audio block fades the output out over 50 ms,
  swaps the snapshot at zero, resets the engines whose slot changed card, and fades back in. The layout name is 1 to 31 printable
  ASCII characters. `status` reports CPU mean and worst since the previous `status` (its own window; the bench peaks are
  untouched), cumulative `late`, `muted`, the record `seq` and `rec_errors`, and `serial_dropped`. All output (replies, boot
  report, bench prints) goes through `serialOut()`, a bounded queue (`core/tx_queue`) that `serialLinkPoll` drains each pass
  with non-blocking writes sized by `Serial.availableForWrite()`; a line that does not fit is dropped whole and counted, and
  with no host connected the queue is discarded. `enter_dfu` and the bench fault and spin ops give the queue 100 ms
  (`serialFlush`) before they act. Engine clears happen on the silent swap block; `kResetsPerSilentBlock` and
  `kFadeHoldBlocks` spread them over several blocks if the bench shows that block late, and the `stats` op's line `layout swaps`
  prints the swap block and the worst block through the last fade-in. `./tools/protocol_check.py` (repo root
  `tools/`, no pyserial needed) runs the checks against a box and prints PASS or FAIL per check; `--no-reset` skips the
  reset-and-reload check, which uses the bench `spin` op (so a release build skips it). On a release build it also checks
  that bench `fault`, `record_erase` and `stop_audio` are refused; on both builds it sends raw `faewdx1k` outside a line and
  checks that nothing acts and `stray_bytes` grows by 8.
- Bench operations (spec `2026-10-12-serial-protocol-design.md`, "Bench operations"): the box takes no single-byte
  commands. A byte outside a JSON line is ignored and counted (`stray_bytes` in `status`). Every bench action is a framed
  request, `{"id":N,"cmd":"bench","op":"<name>"}`, plus `"slot":1..8` for `slot_off`. The reply comes after the op runs:
  `{"id":N,"ok":true}` or an error such as `record store busy`. Only `fault` and `spin`, which never return, reply first.
  Diagnostic text (`[audio]`, `[record]`, `[panel]`) stays on log lines. One table (`core/bench_ops`) holds each op's
  name and class.
  1. Query ops (every build): `report` (boot report), `stats`, `record_status`.
  2. Bench ops (bench image only; a release image replies `op not in this build: <op>` and does not contain the code):
     `slot_off`, `slots_on`, `pin_knobs`, `chasm`, `clip`, `reset_peaks`, `stop_audio`, `fault`, `spin`, `record_save`,
     `record_erase`, `panel_test`, `panel_summary`, `panel_dump`, `panel_jack`, `panel_mute`, `panel_oled`.
  3. DFU is `{"id":N,"cmd":"enter_dfu"}` in every build.
  `hello` reports `build` (`release` or `bench`) and `bench_ops` (what this build accepts); the boot report prints
  `[boot] build release|bench`.
- Build variants: `./build.sh` makes the release image in `/tmp/cubevox-fw-release`. `BENCH=1 ./build.sh` adds
  `-DCUBEVOX_BENCH` and makes the bench image in `/tmp/cubevox-fw-bench`; `BOARD=weact` implies bench. A `mkdir` lock per
  output dir makes a second build of the same variant wait up to 10 minutes. `./flash.sh` takes `VARIANT=release|bench`
  (bench when `BOARD=weact`, release otherwise). CI builds, warn-gates and uploads both. Check the split with
  `nm /tmp/cubevox-fw-release/cubevox.ino.elf | grep -E 'audioStopForBench|recordEraseAll|faultForceBusFault'`, which finds nothing.
- Letter map for the host tools (`firmware/bench_ops.py`, used by `term.py -s`, `flash.sh`, `bringup.py` and
  `tools/protocol_check.py`): `1`-`8` `slot_off` N, `0` `slots_on`, `k` `pin_knobs`, `c` `chasm`, `r` `report`, `p` `stats`,
  `f` `fault`, `w` `spin`, `a` `stop_audio`, `m` `reset_peaks`, `v` `clip`, `s` `record_save`, `q` `record_status`,
  `e` `record_erase`, `d` the `enter_dfu` command, and the panel letters below. `term.py -s p` sends the JSON for `stats`;
  `term.py -s '{"id":1,"cmd":"status"}'` sends a raw request.
- Panel self-test (bench image, `core/panel_test`): `t` `panel_test` on/off, `l` `panel_summary` (pots and toggles still to
  exercise), `g` `panel_dump` raw dump of
  all 16 pots and 8 toggles. While on, the box prints `[panel] pot NN slot S A|B v.vv` when a pot moves and
  `[panel] toggle slot S on|off` on each edge. `i` `panel_jack` steps K103 MIC/PEDAL through the jack-switch mute dip, `x`
  `panel_mute` holds and releases the output mute through the mute sequence, `o` `panel_oled` holds an OLED pattern
  (border, checkerboard, 5x7, 9x12 and 16x24 text) with the menu draw paused. The `panel_summary`, `panel_jack`,
  `panel_mute` and `panel_oled` ops error with `panel test is off` unless the test is on; leaving it restores jack, mute and menu.
  `./bringup.py --board 1 --panel` walks slots 1 to 8 (knob A and B left then right, toggle both ways), fails a pot
  that does not travel, reads backwards, or moves with another channel, then asks about the K103 click, the mute click
  and the OLED pattern. `--no-prompt` skips it. On a real board `bringup.py` flashes the bench image first, runs its
  checks, then flashes the release image and checks that `hello` reports `release`; `--weact` stays on the bench image.

Real: slot hardware table, default card layout, mux scan (selects held low 1.1 s after boot for U107's 3V3A
delay), smoothing, KEY/SEMITONES stepping, toggles, mute sequence, menu logic and DFU entry, clock tree,
VBUS-gated USB, PLL3 and SAI1 TX/RX DMA setup, engine chain in slot order, watchdog and fault handling.
Stubbed or unverified on hardware: OLED (SH1106 driver compiled, falls back to `stubDisplay()` when no panel
answers at 0x3C), layout persistence (flash bank 2 record written; chip behavior unverified until the WeAct bench run), VA_SENSE shutdown, EQ menu, costs (estimates
until the bench runs on a board). Audio, clocks and mux timing have never run on a board.
