# cubevox firmware (STM32H743VIT6)

Needs arduino-cli and the STMicroelectronics:stm32 3.0.0 core. `build.sh` refuses any other core version
and prints the gcc and firmware version it builds with. `build.sh` compiles and never flashes; `flash.sh`
flashes. The image runs from 0x08020000, behind the bootloader in flash sector 0
(`docs/specs/2026-10-24-field-update-design.md`).

## Build and flash

    ./build.sh          # release image, FQBN STMicroelectronics:stm32:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen
    BENCH=1 ./build.sh  # bench image (drills, panel test)
    TAIL=5 ./build.sh   # last 5 lines of compiler output only
    ./flash.sh          # enter_dfu, copy cubevox.uf2 onto the CUBEVOX drive, print the boot report
    ./flash.sh --rom    # ST ROM DFU (hold BOOT0): dfu-util writes the .bin at 0x08020000

1. **Variant.** `build.sh` is the only place that decides it. `VARIANT=release|bench` names it outright,
   `BENCH=1` implies bench, and a conflicting `VARIANT` is an error. `./build.sh --variant`
   prints the variant and `./build.sh --outdir` prints the output dir (`/tmp/cubevox-fw-release` or
   `/tmp/cubevox-fw-bench`) for the current environment, then exit. `flash.sh`, `bringup.py` and CI ask it
   instead of repeating the rule.
2. **Lock.** Each output dir has a `mkdir` lock holding the owner's pid. A second build of the same variant
   waits up to 10 minutes, and a lock whose owner died is removed.
3. **Host-tool check.** Before compiling, `build.sh` runs `tools/bench_ops_check.py`. It compares the op
   table, the panel thresholds, the block rate and the SAI kernel clock that the Python tools carry against
   the firmware sources, so a drift fails the build.
4. **Link address.** `build.sh` reads `CV_APP_START` and `CV_APP_END` from `cubevox/src/board/boot_block.h` and
   passes `build.flash_offset=0x20000` to arduino-cli. That one property sets the FLASH origin and
   `-DVECT_TAB_OFFSET`, so the link address and the VTOR the core writes in `SystemInit` agree. Nothing else
   sets VTOR.
5. **Linker script.** `cubevox.ld` is the one script cubevox owns. It adds the RAM_D2 and DTCM data sections,
   `.cv_boot_block` at 0x2001FF00, `.cv_noinit` at 0x2001FFC0 and `.cv_image_hdr` at 0x08020400, and it asserts
   that the image ends below flash bank 2. `ld` cannot include `boot_block.h`, so the addresses are copies with
   the macro named beside each. After the link `build.sh` compares `gBootBlock`, `kImageHeader` and
   `__cv_noinit_start` in the elf with the header and fails on a drift.
6. **Image size.** The build fails when the .bin exceeds 917504 bytes, the room between the bootloader's sector
   and flash bank 2, which holds the layout record.
7. **Stamp and update file.** `tools/stamp_image.py` writes the length and CRC-32 into the image header, in place.
   It then writes `cubevox.uf2` next to the .bin, in the cubevox family, and reads that file back the way the
   bootloader will. `build.sh` prints the stamped version.
8. **Version.** The release name is `v` plus `VERSION.txt`. `build.sh` passes it as `CUBEVOX_FW_VERSION`, and
   the image header, `hello`, the boot report, the boot splash and the confirm screen all show that one string.
   The bench image adds `-bench` to the header only.
9. **Warning gate.** `warn_gate.py` recompiles every source under the sketch (sketch, `src/**`) with
   `-Wall -Wextra -Wshadow -Wdouble-promotion -Wundef -Wformat=2 -Werror`. Third-party includes are `-isystem`.
   It fails when fewer files are checked than the sketch holds. Both variants must pass; CI builds, gates and
   uploads both. Check the split with
   `nm /tmp/cubevox-fw-release/cubevox.ino.elf | grep -E 'audioStopForBench|recordEraseAll|faultForceBusFault'`,
   which finds nothing.
10. **Serial port.** `cubevox_port.py` is the one place that finds and opens the CDC port. `term.py`,
    `bringup.py`, `flash.sh` and `tools/protocol_check.py` use it. With more than one `cu.usbmodem*` device,
    name the port with `CUBEVOX_PORT=/dev/cu.usbmodemXXXX` or the tool's `--port` option (`term.py` takes it as
    the argument).
11. **Editor.** `cubevox/.clangd` gives clangd the include paths and defines of the board tier, so `Arduino.h`,
    the HAL and the variant headers resolve. The paths are absolute and the file says how to regenerate them.

Design: `docs/specs/2026-10-08-firmware-foundation-design.md` (tiers, tests, fault handling),
`2026-10-08-effect-registry-design.md` (cards), `2026-10-08-update-and-slots-design.md` (roadmap).

## Layout

`cubevox/cubevox.ino` holds `setup()`, `loop()`, the request dispatch and the reply wiring. Every decision it
calls is a pure function or class in `core/` or `ui/`. Sources sit under `cubevox/src/` in three tiers;
includes are tier-qualified (`core/registry.h`, `board/pins.h`, `ui/menu.h`). Module map and ownership rules:
`docs/specs/2026-10-12-firmware-structure-design.md`.

**`src/core/`** is pure C++17 with no Arduino or HAL include. `test/firmware/*.cpp` compiles it on the Mac
(`ctest` target `firmware`), so an Arduino include here breaks the host build.

1. Cards and layout: `registry` (cards, knob descriptors with curves, apply, format), `costs.h`, `chain_params`,
   `slots.h` (slot and pot counts), `layout` (card per slot, validation with a reason, `recoverLayout`),
   `layout_fade` (50 ms out/in sequencer for a live layout change, engines to reset), `unison_rate`, `settings.h`.
2. Panel and input: `panel.h` (`PanelReading`), `knob_filter` (stepping, per-knob smoothing, `KnobBank` for the
   sixteen pots, the end-of-travel dead band), `debounce`, `jack_switch` (mute dip around K103),
   `mute_sequence`, `panel_test`, `bench_ops` (the op table), `timing.h` (switch, relay and mute times, `Every`),
   `loop_policy.h` (`PublishGate`, `CanvasHandoff`).
3. Sound path: `compose` (`ChainSnapshot` from layout, panel, settings and bench overrides).
4. Display: `canvas` (128 x 64 page-layout frame buffer with dirty pages), `fonts` (the font editor's export as
   `FontDesc`), `display.h` (backend interface, `flushStep`).
5. Protocol and storage: `protocol` (JSON line framing, request parse, reply formatting into a `Writer`),
   `tx_queue` (bounded line queue for serial output), `record` (layout record codec, newest-valid scan),
   `record_store` (append log, sector switch, debounce, as a state machine over `FlashIo`), `crc32` (the one
   CRC-32, shared by the record and the image header), `version.h`.
6. Update support: `boot_health_rule.h` (when a start counts as healthy, the failed-start count). The names
   of the update reasons, fault kinds and clock steps live in the contract header `board/boot_block.h`.

**`src/ui/`** is pure and also host-compiled: `menu` (state machine, `UiInput` in, `UiAction` out, draws the
screens on the canvas), `actions` (what a `UiAction` does to the settings) and `splash` (the version, centred,
for 2 s after `setup()`).

**`src/board/`** has one hardware concern per file.

1. Pins and clocks: `pins.h` and `pins.cpp` (every pin, port decode, the boot pin report), `clocks.h` (HSE),
   `sysclock` (the `SystemClock_Config` override and the clock report), `slots` (toggle pin and mux channels per
   slot), `build_kind.h` (`CUBEVOX_BENCH`).
2. Audio: `audio` (SAI, DMA, the audio IRQ, chain walk over the published snapshot, fade and engine reset on a
   layout change, SAI and DMA error count).
3. Panel: `controls` (mux scan, ADC, toggles), `input` (encoder and click to `UiInput`), `mute`, `input_jack`
   (PAD_EN for relay K103).
4. Display: `display_sh1106` (U8x8 driver), `display_stub` (ASCII render over Serial when no panel answers).
5. USB and protocol: `usb_link` (CDC gated on VBUS), `serial_link` (CDC bytes to JSON lines, reply writer, output
   queue), `boot_report` (the report a terminal gets on open).
6. Safety and storage: `fault` (watchdog, fault handlers, reset cause, clock failure record, audio stall and
   transfer error watch), `flash_bank2` (bank 2 erase and flash-word program by register, non-blocking erase),
   `record_service` (store wiring: boot load, ECC recovery, save request, flush before reset), `block_mem`
   (where audio memory lives: the RAM_D2 MPU region, the DTCM section, adapted from Teensy-H7-Port).
7. Bootloader contract: `boot_block.h` (the addresses, magics, name tables and the four structs both programs
   read: boot block, image header, diagnostic entry, fault record; C-compatible, the bootloader and
   `tools/stamp_image.py` read it too), `boot_block` (the shared DTCM block as a variable),
   `image_header` (the 32-byte header at +0x400 and the CRC check of the running image), `boot_health` (zeroes
   the bootloader's start counter once the audio interrupt has run for 5 s), `dfu` (the one way into update
   mode), `device_serial` (the MCU id as 24 hex characters).
8. Bench: `bench` (query ops and stats), `bench_drill` (slot and knob overrides, faults, record drills),
   `bench_panel` (panel self-test). The drill and panel code is compiled in the bench image only.

Engine headers come from `../engine` by include path. No emulator files are included. Teensy-H7-Port and
DrumSynthV3 are references to read, and nothing in `firmware/` is copied from them verbatim.

## Notes

- `flash.sh` takes the path Adam takes: with a serial port present it sends `enter_dfu`, waits for the
  `CUBEVOX` drive, copies `<out dir>/cubevox.uf2` onto it and prints the boot report once the port returns.
  `--rom` is the shop fallback for a box whose application is unreachable: dfu-util writes
  `<out dir>/cubevox.ino.bin` at 0x08020000 (`-s 0x08020000:leave`), above the bootloader that
  `bootloader/flash.sh` puts in sector 0. `./flash.sh --help` lists the knobs.
- `bringup.py`: first-article checks for a new board over USB, optional fault, ROM-DFU and update drills
  (`--faults`, `--dfu`, `--update`, the last one being spec drills 1 to 13), operator questions, log to
  `hardware/bringup/`. `./bringup.py --board 1 --faults --update`. It checks its panel thresholds, block rate
  and SAI clock against the firmware through `tools/bench_ops_check.py`.
- `board/sysclock.cpp`: `SystemClock_Config` override (spec 2026-10-08 step 0). HSE 25 MHz, PLL1 M5 N192 P2 Q20
  for 480 MHz on rev V silicon (360 MHz, N144 Q15, on older revisions), USB from PLL1Q, HSI48 off, PLL2 40 MHz as
  the ADC kernel clock. A failing step records its name in the fault record and resets; the next boot report
  says which step failed. `clockReport()` prints silicon rev, ROM bootloader ID at 0x1FF1E7FE and the clock tree.
- ADC: `build.sh` sets the 810.5-cycle sample window. The core configures the ADC from the AHB clock divided by 4,
  so the PLL2 kernel clock is not the one in use. Each `analogRead` blocks `loop()` for one conversion.
- `board/usb_link.cpp`: USB CDC starts only while PA9 reads VBUS and stops when it drops (self-powered box,
  `-DUSBD_SELF_POWERED=1` in build.sh). The boot report prints each time a terminal opens the port. The CDC
  receive queue holds two maximum-length requests, and `serial_link.cpp` asserts it at compile time.
  The clock numbers and the gating were reviewed in `docs/audits/2026-10-08-astra-clock-response.md`.
- `board/display_sh1106.cpp`: U8x8 init on I2C2 PB10/PB11; `flushStep` sends the lowest dirty canvas page with
  `drawTile`, once per loop pass, and keeps the page dirty when the write fails. The fonts come from
  `tools/font-editor/fonts/cubevox_fonts.h` through an include path, no copy.
- `board/fault.cpp`: IWDG1 at 2 s, armed at the end of `setup()` and kicked only from `loop()` (and from
  `recordFlush`). HardFault, MemManage, BusFault and UsageFault drive XSMT and MUTE_N low by register write, store
  PC, LR, CFSR, HFSR, BFAR and MMFAR in a `.cv_noinit` record (`cubevox.ld`, top of DTCM) and reset; the
  next boot report prints `[ERROR] last reset: fault ...` or `[boot] fault: none`, plus the decoded `RCC->RSR`
  reset cause. A stack pointer outside RAM stores zero for PC and LR. If the audio block counter stops for 100 ms
  while audio runs, the mute latches (`muteLatch()`, MUTE_SW ignored until reset) and one error line prints.
  SAI and DMA transfer errors are counted in `audio.cpp` and reported at most once a second.
- `board/dfu.cpp`: `enterDfu()` is the only way into update mode, where the bootloader serves the `CUBEVOX` drive.
  It writes a summary of this run into the boot block (version, uptime, reset cause, last fault kind), queues a
  diagnostic log entry and waits up to 3 s for it, writes a pending record save, waits up to 5 s for the store to go idle, lets the serial queue drain for 100 ms, mutes
  through `muteForReset()`, writes the request word and resets. The menu action and the `enter_dfu` command both
  call it. The confirm screen shows the installed version, and the box shows the same version for 2 s after
  every start.
- `board/boot_health.cpp`: the bootloader counts every start that reaches the application. `bootHealthInit()` at
  the end of `setup()` reads that count, and `bootHealthPoll()` zeroes it 5 s later if the audio interrupt has
  advanced blocks since `setup()` ended. A fault, a watchdog reset or a hang inside those 5 s leaves the count
  standing, and after three such starts in a row the next reset stays in update mode. The boot report line
  `[boot] image <version> crc ok|BAD, boot fails <n>, last update reason <text>` and the `status` fields
  `boot_fails` and `update_reason` give the earlier failed starts and why the bootloader last stayed in update
  mode. The CRC covers the running image and is computed once, on the first report.
- `board/flash_bank2.cpp` and `board/record_service.cpp`: the layout record lives in bank 2 at 0x08100000 and
  0x08120000 (spec `2026-10-12-layout-record-design.md`). A save runs 2 s after the last settings change, one erase
  or flash-word program per `loop()` pass. Bank 2 sector 2 at 0x08140000 holds the diagnostic log, which
  `core/entry_log` appends to over `core/flash_io`, `board/diag_service` fills and polls, and the bootloader only
  reads for `DIAG.TXT`. The flash driver checks the unlock, rejects a sector outside the record's two and the log's
  one, and bounds every wait (5 s for an erase, 5 ms for one program word); a failure sets an error flag
  and `recordPoll` prints `[ERROR] record save failed`. `build.sh` and the linker script keep the image below
  bank 2, and the bootloader writes only inside the image region. The fault record keeps FLASH->SR2 and ECC_FA2;
  a double ECC fault in a record or log sector is erased at
  boot before the scan, and a failed erase is reported in the boot report. A record whose layout fails validation
  gives the default layout with a `[WARN]` and a rewrite. The boot report also warns about an unknown card id, a
  sector with data after a blank entry, and torn entries.
- Serial protocol (spec `2026-10-12-serial-protocol-design.md`): one JSON object per line on the same CDC port,
  requests `{"id":N,"cmd":"..."}` with `hello`, `get_layout`, `set_layout`, `status` and `enter_dfu`; a `{` outside a
  line starts a JSON line, any other byte is ignored and counted, lines over 768 bytes get an error reply with id 0.
  `set_layout` validates (`layoutValid` gives the reason), requests a record save, and marks the snapshot stale. The
  same pass re-seeds every knob whose step count changed, then publishes. The audio block fades the output out over
  50 ms, swaps the snapshot at zero, resets the engines whose slot changed card, and fades back in. The layout name
  is 1 to 31 printable ASCII characters. `status` reports CPU mean and worst since the previous `status` (its own
  window; the bench peaks are untouched), cumulative `late`, `muted`, the record `seq` and `rec_errors`,
  `serial_dropped`, `stray_bytes`, `boot_fails` and `update_reason`. All output (replies, boot report, bench prints) goes through `serialOut()`, a bounded queue
  (`core/tx_queue`) that `serialLinkPoll` drains each pass with non-blocking writes sized by
  `Serial.availableForWrite()`; a line that does not fit is dropped whole and counted, and with no host connected
  the queue is discarded. `enter_dfu` and the bench fault and spin ops give the queue 100 ms (`serialFlush`) before
  they act. Engine clears happen on the silent swap block; `kResetsPerSilentBlock` and `kFadeHoldBlocks` spread them
  over several blocks if the bench shows that block late, and the `stats` op's line `layout swaps` prints the swap
  block and the worst block through the last fade-in. `./tools/protocol_check.py` (repo root `tools/`, no pyserial
  needed) runs the checks against a box and prints PASS or FAIL per check; `--no-reset` skips the reset-and-reload
  check, which uses the bench `spin` op (so a release build skips it). On a release build it also checks that bench
  `fault`, `record_erase` and `stop_audio` are refused; on both builds it sends raw `faewdx1k` outside a line and
  checks that nothing acts and `stray_bytes` grows by 8.
- Bench operations (spec `2026-10-12-serial-protocol-design.md`, "Bench operations"): the box takes no single-byte
  commands. A byte outside a JSON line is ignored and counted (`stray_bytes` in `status`). Every bench action is a
  framed request, `{"id":N,"cmd":"bench","op":"<name>"}`, plus `"slot":1..8` for `slot_off`. The reply comes after
  the op runs: `{"id":N,"ok":true}` or an error such as `record store busy`. Only `fault` and `spin`, which never
  return, reply first. Diagnostic text (`[audio]`, `[record]`, `[panel]`) stays on log lines. One table
  (`core/bench_ops`) holds each op's name and class.
  1. Query ops (every build): `report` (boot report), `stats`, `record_status`.
  2. Bench ops (bench image only; the parser refuses them in a release image with `op not in this build: <op>`, and
     the release image does not contain the code): `slot_off`, `slots_on`, `pin_knobs`, `chasm`, `clip`,
     `reset_peaks`, `stop_audio`, `fault`, `spin`, `record_save`, `record_erase`, `panel_test`, `panel_summary`,
     `panel_dump`, `panel_jack`, `panel_mute`, `panel_oled`.
  3. DFU is `{"id":N,"cmd":"enter_dfu"}` in every build.
  `hello` reports the box's `serial` (the MCU id the update drive also serves), `build` (`release` or `bench`)
  and `bench_ops` (what this build accepts); the boot report prints `[boot] build release|bench`.
- Letter map for the host tools (`firmware/bench_ops.py`, used by `term.py -s`, `flash.sh`, `bringup.py` and
  `tools/protocol_check.py`): `1`-`8` `slot_off` N, `0` `slots_on`, `k` `pin_knobs`, `c` `chasm`, `r` `report`, `p`
  `stats`, `f` `fault`, `w` `spin`, `a` `stop_audio`, `m` `reset_peaks`, `v` `clip`, `s` `record_save`, `q`
  `record_status`, `e` `record_erase`, `d` the `enter_dfu` command, and the panel letters below. `term.py -s p`
  sends the JSON for `stats`; `term.py -s '{"id":1,"cmd":"status"}'` sends a raw request.
- Panel self-test (bench image, `core/panel_test`): `t` `panel_test` on/off, `l` `panel_summary` (pots and toggles
  still to exercise), `g` `panel_dump` raw dump of all 16 pots and 8 toggles. While on, the box prints
  `[panel] pot NN slot S A|B v.vv` when a pot moves and `[panel] toggle slot S on|off` on each edge. `i`
  `panel_jack` steps K103 MIC/LINE through the jack-switch mute dip, `x` `panel_mute` holds and releases the output
  mute through the mute sequence, `o` `panel_oled` holds an OLED pattern (border, checkerboard, 5x7, 9x12 and 16x24
  text) with the menu draw paused. The `panel_summary`, `panel_jack`, `panel_mute` and `panel_oled` ops error with
  `panel test is off` unless the test is on; leaving it restores jack, mute and menu. `./bringup.py --board 1
  --panel` walks slots 1 to 8 (knob A and B left then right, toggle both ways), fails a pot that does not travel,
  reads backwards, or moves with another channel, then asks about the K103 click, the mute click and the OLED
  pattern. `--no-prompt` skips it. `bringup.py` flashes the bench image first, runs its checks, then flashes the
  release image and checks that `hello` reports `release`.

## Status

Costs in `core/costs.h` are marked measured, from runs of the engine chain on an H743 test board; the method
is in that file. The audio path, clock tree, USB gating and mux timing have run on the test board and not yet on a
cubevox board. The bring-up logs are in `hardware/bringup/`. The OLED driver is compiled and falls back to
`stubDisplay()` when nothing answers at 0x3C. VA_SENSE shutdown is not implemented, and no code reads PB1.
