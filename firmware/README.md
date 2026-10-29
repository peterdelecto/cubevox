# cubevox firmware (STM32H743VIT6)

Needs arduino-cli 1.5.1 and the STMicroelectronics:stm32 3.0.0 core, which installs the xpack gcc
14.2.1-1.1. `build.sh` refuses any other version of the three and prints the gcc and firmware version it
builds with. `build.sh` compiles and never flashes; `flash.sh` flashes. The image runs from 0x08020000,
behind the bootloader in flash sector 0 (`docs/specs/2026-10-24-field-update-design.md`). The scripts assume
macOS (`/dev/cu.usbmodem*`, `/Volumes`, `~/Library/Arduino15`).

## Build and flash

    ./build.sh          # release image, FQBN STMicroelectronics:stm32:GenH7:pnum=GENERIC_H743VITX,usb=CDCgen
    BENCH=1 ./build.sh  # bench image (drills, panel test)
    OUT=/tmp/mine ./build.sh   # any output dir, so two checkouts or agents do not share one
    CLIP=loop.wav BENCH=1 ./build.sh   # bake a 48 kHz 16-bit WAV into the bench image for the clip op
    TAIL=5 ./build.sh   # last 5 lines of compiler output only
    CLEAN=1 ./build.sh  # rebuild everything, so the image matches what CI builds from the same commit
    ./flash.sh          # enter_dfu, copy cubevox.uf2 onto the CUBEVOX drive, print the boot report
    ./flash.sh --rom    # ST ROM DFU (hold BOOT0): dfu-util writes the .bin at 0x08020000

1. **Variant.** `build.sh` is the only place that decides it. `VARIANT=release|bench` names it outright,
   `BENCH=1` implies bench, and a conflicting `VARIANT` is an error. `./build.sh --variant`
   prints the variant and `./build.sh --outdir` prints the output dir for the current environment, then
   exit. The dir is `OUT` when set, else `/tmp/cubevox-fw-release` or `/tmp/cubevox-fw-bench`. `flash.sh`,
   `bringup.py` and CI ask it instead of repeating the rule. `./build.sh --help` prints the usage.
2. **Lock.** Each output dir has a `mkdir` lock holding the owner's pid. A second build into the same dir
   waits up to 10 minutes, builds into different dirs run side by side, and a lock whose owner died is
   removed.
3. **Pins.** `build.sh` holds the arduino-cli version (`CLI_VERSION`), the core version (`CORE_VERSION`) and
   the compiler (`XPACK_VERSION`), and stops on any other. The workflows install the same versions in their
   `env` blocks, and `tools/scripts_check.py` (ctest `tooling_scripts`) fails when they differ.
   `./build.sh --toolchain` prints the compiler's `bin/` dir, which is the way for the bootloader build to
   use the same compiler.
3. **Host-tool check.** Before compiling, `build.sh` runs `tools/bench_ops_check.py`. It compares the op
   table, the panel thresholds, the block rate and the SAI kernel clock that the Python tools carry against
   the firmware sources, so a drift fails the build.
4. **Link address.** `build.sh` reads `CV_APP_START` and `CV_APP_END` from `cubevox/src/board/boot_block.h` and
   passes `build.flash_offset=0x20000` to arduino-cli. That one property sets the FLASH origin and
   `-DVECT_TAB_OFFSET`, so the link address and the VTOR the core writes in `SystemInit` agree. Nothing else
   sets VTOR. `build.sh`, `flash.sh` and the release workflow read the header through `cv_define.sh`.
5. **Linker script.** `cubevox.ld` is the one script cubevox owns. It adds the RAM_D2 and DTCM data sections,
   `.cv_boot_block` at 0x2001FF00, `.cv_noinit` at 0x2001FFC0 and `.cv_image_hdr` at 0x08020400. It asserts
   that each data section ends inside its region, that the core's `.noinit` (which lands after `.cv_noinit`)
   ends inside DTCM, and that the image ends below flash bank 2. `ld` cannot include `boot_block.h`, so the
   addresses are copies with the macro named beside each. After the link `build.sh` compares `gBootBlock`,
   `kImageHeader` and `__cv_noinit_start` in the elf with the header and fails on a drift. It also fails
   when the core's `.noinit` (`_snoinit` to `_enoinit`) holds any data, because that section lands inside
   the fault record's slot.
6. **Image size.** The build fails when the .bin exceeds 917504 bytes, the room between the bootloader's sector
   and flash bank 2, which holds the layout record.
7. **Stamp and update file.** `tools/stamp_image.py` writes the length and CRC-32 into the image header, in place.
   It then writes `cubevox.uf2` next to the .bin, in the cubevox family, and reads that file back the way the
   bootloader will. `build.sh` prints the stamped version. A build that starts compiling and does not finish
   deletes the .bin and .uf2 it found, so `flash.sh` cannot install an image that failed a check or came from
   an earlier build, and `flash.sh` also refuses an image whose version differs from `VERSION.txt`. Equal
   sources give an identical image, CRC included, in a clean build whatever the output dir or checkout path.
   An incremental build over an older output dir can order the link differently, which moves functions and
   changes the CRC, so `CLEAN=1` builds the image to compare with the check line in a release's notes.
8. **Budget.** After the stamp `build.sh` prints one `[budget]` block of true headroom, from the link map and
   the elf. App flash counts against the 917504 B region, DTCM against the boot block address, D1 against the
   stack top and D2 against its end. At v2.00.35 the release image uses 164960 B of flash (18.0 %) and
   51968 B of DTCM (39.7 %, 78848 B free), 167624 B of D1 and 2048 B of D2. arduino-cli prints its own
   summary above it, which divides by the whole 2 MB flash and by D1 alone.
9. **Version.** The release name is `v` plus `VERSION.txt`. `build.sh` passes it as `CUBEVOX_FW_VERSION`, and
   the image header, `hello`, the boot report, the boot splash and the confirm screen all show that one string.
   The bench image adds `-bench` to the header only. `tools/bump_version.sh` raises the last field and carries
   into the minor field; every field is read as decimal.
10. **Warning gate and symbol check.** `warn_gate.py` recompiles every `.c` and `.cpp` source under the sketch
    (sketch, `src/**`) with `-Wall -Wextra -Wshadow -Wdouble-promotion -Wundef -Wformat=2 -Wstack-usage=1152
    -Werror`. Third-party includes are `-isystem`. It fails when fewer files are checked than the sketch holds. It
    runs last, and a failure removes the image the stamp step wrote. `tools/check_bench_symbols.sh` runs after the
    link. The release image must hold none of five bench-only functions and the bench image must hold all five,
    which keeps the list tied to real code. Both variants must pass; CI builds, gates and uploads both.
11. **Serial port.** `cubevox_port.py` is the one place that finds and opens the CDC port. `term.py`,
    `bringup.py`, `flash.sh` and `tools/protocol_check.py` use it. With more than one `cu.usbmodem*` device,
    name the port with `CUBEVOX_PORT=/dev/cu.usbmodemXXXX` or the tool's `--port` option (`term.py` takes it as
    the argument). `term.py --expect-image VERSION` exits 1 unless the boot report says
    `[boot] image VERSION crc ok`, which is how `flash.sh` judges a flash.
12. **Editor.** `cubevox/.clangd` and `bootloader/.clangd` give clangd the include paths and defines of each
    program. clangd has no variable expansion, so the paths are absolute. `python3 -I tools/clangd_paths.py`
    rewrites the Arduino and checkout prefixes for this machine.
13. **CI and release.** `.github/workflows/ci.yml` runs the host suite (`ctest --no-tests=error`), both firmware
    builds, the bootloader build and its selftest. `release.yml` runs on a tag `vX.YY.ZZ` that equals `v` plus
    `VERSION.txt`. Its `tests` job (host suite, bootloader selftest, bench build) must pass before `publish`
    creates a draft release. The draft holds `cubevox-vX.YY.ZZ.uf2` for Adam, `cubevox-rom-0x08020000.bin` for
    the ROM DFU and `cubevox-rom-0x08020000.elf`, which resolves the addresses in a `DIAG.TXT`. The notes
    carry the image check line of the .uf2. The owner publishes the draft after the bench look, and a rerun
    stops when the release already holds a file.

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
   `mute_sequence`, `power_fail` (the 5VA trip and clear levels and the hold before a power-fail mute releases),
   `panel_test`, `bench_ops` (the op table), `timing.h` (switch, relay and mute times, `Every`),
   `loop_policy.h` (`PublishGate`, `FallingEdge` with its `CanvasHandoff` name, `ResetWait`).
3. Sound path: `compose` (`ChainSnapshot` from layout, panel, settings and bench overrides), `chain_run` (the
   slot walk, the NaN guard, the polish tail and the BYPASS mix, over the engines and buffers the board hands in),
   `output_stage` (the NaN guard and the output gain ramp), `audio_recovery` (when a faulted stream restarts: at
   most 3 times in 60 s, then the mute latches).
4. Display: `canvas` (128 x 64 page-layout frame buffer with dirty pages, ordered dissolve), `fonts` (the font
   editor's export as `FontDesc`), `display.h` (backend interface, `flushStep`, and `PanelLink`, the restart rule
   for a panel that is missing or stops answering), `level` (input energy to the voice ribbon's 0..1 level).
5. Protocol and storage: `protocol` (JSON line framing, request parse, reply formatting into a `Writer`),
   `tx_queue` (bounded line queue for serial output), `record` (layout record codec, newest-valid scan),
   `record_store` (append log, sector switch, debounce, as a state machine over `FlashIo`), `entry_log` (the
   same append-log machine for the diagnostic log's fixed-size entries), `flash_io` (the flash interface both
   stores use, with the word and sector sizes), `le_bytes.h` (little-endian field access for records and
   entries), `crc32` (the one CRC-32, shared by the record, the log and the image header), `version.h`.
6. Update support and health: `boot_health_rule.h` (when a start counts as healthy, the failed-start count),
   `audio_stall.h` (when the running stream counts as stalled). The names of the update reasons, fault kinds
   and clock steps live in the contract header `board/boot_block.h`.

**`src/ui/`** is pure and also host-compiled: `menu` (state machine, `UiInput` in, `UiAction` out, draws the open
menu and the update's starting screen on the canvas, and drops every click and turn until the face has ticked),
`actions` (what a `UiAction` does to the settings), `splash` (the version, centred, for 2 s after `setup()`), and
the runtime screens of `docs/specs/2026-10-24-oled-gui-design.md`: `gui` (which screen and
dissolve step), `screens` (voice ribbon, knob readout, MUTED or BYPASS) and `face` (the 25 fps tick that draws them
and dissolves between them).

**`src/board/`** has one hardware concern per file.

1. Pins and clocks: `pins.h` and `pins.cpp` (every pin, port decode, the boot pin report), `clocks.h` (HSE),
   `sysclock` (the `SystemClock_Config` override and the clock report), `slots` (toggle pin and mux channels per
   slot), `build_kind.h` (`CUBEVOX_BENCH`).
2. Audio: `audio` (SAI, DMA, the audio IRQ, the engines and the published snapshot, fade and engine reset on a
   layout change, per-stage cycle counts, SAI and DMA error handling with the stream restart).
3. Panel: `controls` (mux scan on ADC3, toggles), `adc_setup` (one ADC brought up once, for `controls` and
   `power_sense`), `input` (encoder and click to `UiInput`), `mute`, `input_jack` (PAD_EN for relay K103).
4. Display: `sh1106.c` (the SH1106 driver the bootloader compiles too) and `display_sh1106` (the bus from
   `pins.h`, boot starts with bus recovery, one page per loop pass under `PanelLink`).
5. USB and protocol: `usb_link` (CDC gated on VBUS), `serial_link` (CDC bytes to JSON lines, reply writer, output
   queue), `boot_report` (the report a terminal gets on open).
6. Safety and storage: `fault` (watchdog, fault handlers, reset cause, clock failure record, audio stall and
   transfer error watch), `power_sense` (VA_SENSE on ADC1: its analog watchdog interrupt mutes at once when 5VA
   falls under the trip level, and the poll releases the mute once the rail holds), `flash_bank2` (bank 2 erase
   and flash-word program by register, non-blocking erase),
   `record_service` (store wiring: boot load, the ECC recovery that carries the newest intact record out of a
   faulted sector, save requests and the debounce skip), `diag_service` (the diagnostic log in bank 2 sector 2:
   an entry for a start that follows a fault, a reset that was not a plain power-on or a failed option byte
   step, one before each update-mode request, and one per audio fault; the bootloader lists them in
   `DIAG.TXT`), `planned_reset` (the reset into update mode and the reset after an audio stall wait in
   `loop()` until both stores are idle, then mute and reset), `block_mem` (where audio memory lives: the RAM_D2
   MPU region, the DTCM section, adapted from Teensy-H7-Port).
7. Bootloader contract: `boot_block.h` (the addresses, magics, name tables and the four structs both programs
   read: boot block, image header, diagnostic entry, fault record; C-compatible, the bootloader and
   `tools/stamp_image.py` read it too), `boot_block` (the shared DTCM block as a variable) with
   `boot_block_access.h` (`bootBlock()` and the check that the bootloader wrote the block since power-on),
   `image_header` (the 32-byte header at +0x400 and the CRC check of the running image), `boot_health` (zeroes
   the bootloader's start counter once the audio interrupt has run for 5 s), `dfu` (the one way into update
   mode), `device_serial` (the MCU id as 24 hex characters).
8. Bench: `bench` (query ops and stats, which also print the stack never used), `bench_drill` (slot and knob
   overrides, faults, record drills), `bench_panel` (panel self-test). The drill and panel code is compiled in the
   bench image only.

Engine headers come from `../engine` by include path. No emulator files are included. Teensy-H7-Port and
DrumSynthV3 are references to read, and nothing in `firmware/` is copied from them verbatim.

## Notes

- `flash.sh` takes the path Adam takes: with a serial port present it sends `enter_dfu`, waits for the
  `CUBEVOX` drive, copies `<out dir>/cubevox.uf2` onto it and prints the boot report once the port returns.
  `--rom` is the shop fallback for a box whose application is unreachable: dfu-util writes
  `<out dir>/cubevox.ino.bin` at 0x08020000 (`-s 0x08020000:leave`), above the bootloader that
  `bootloader/flash.sh` puts in sector 0. Both scripts judge a dfu-util log with `dfu_unexpected_errors`
  from `dfu_util.sh`. `./flash.sh --help` lists the knobs. The script refuses an image
  whose header version differs from `VERSION.txt` and exits 1 unless the boot report that follows names the
  version it flashed.
- `bringup.py`: first-article checks for a new board over USB, optional fault, ROM-DFU and update drills
  (`--faults`, `--dfu`, `--update`, the last one being spec drills 1 to 13), operator questions, log to
  `hardware/bringup/`. `./bringup.py --board 1 --faults --update`. It checks its panel thresholds, block rate
  and SAI clock against the firmware through `tools/bench_ops_check.py`. `--dfu` stops at once when dfu-util is
  missing. Drill 1 fails a unit whose option bytes are not BOR level 3 with sector 0 protected. The final summary
  names `hardware/bringup/bench-checklist.md`, the checks that need a person at the bench with a scope or a PA.
- `board/sysclock.cpp`: `SystemClock_Config` override (spec 2026-10-08 step 0). HSE 25 MHz, PLL1 M5 N192 P2 Q20
  for 480 MHz on rev V silicon (360 MHz, N144 Q15, on older revisions), USB from PLL1Q, HSI48 off, PLL2 40 MHz as
  the ADC kernel clock. A failing step records its name in the fault record and resets; the next boot report
  says which step failed. `clockReport()` prints silicon rev, ROM bootloader ID at 0x1FF1E7FE and the clock tree.
- ADC: pots on ADC3 and VA_SENSE on ADC1, set up once by `board/adc_setup` (PLL2 40 MHz / 2). The scan never
  waits; a failed sample drops with a `[WARN]`. Nothing calls `analogRead`, which resets ADC1 and ADC2.
- `board/usb_link.cpp`: USB CDC starts only while PA9 reads VBUS and stops when it drops (self-powered box,
  `-DUSBD_SELF_POWERED=1` in build.sh). The boot report prints each time a terminal opens the port. The CDC
  receive queue holds two maximum-length requests, and `serial_link.cpp` asserts it at compile time.
  The clock numbers and the gating were reviewed in `docs/audits/2026-10-08-astra-clock-response.md`.
- `board/display_sh1106.cpp`: drives the panel through `board/sh1106.c`, the driver the bootloader shares, on I2C2
  PB10/PB11 at 400 kHz. At boot the panel gets 5 starts 20 ms apart, each after nine SCL clocks and a STOP.
  `flushStep` sends the lowest dirty canvas page once per loop pass and keeps a failed page dirty. `PanelLink`
  (`core/display.h`) restarts the panel at once after the first failure, then once a second, and prints one
  `[WARN]` per run and one `[oled]` line when the panel is back. The face draws a new frame only once every page
  of the last one has gone out. The fonts come from `tools/font-editor/fonts/cubevox_fonts.h` through an include
  path, no copy.
- `board/fault.cpp`: the bootloader starts IWDG1 at 16 s before the jump; `faultWatchdogStart()` sets 2 s at the
  end of `setup()` and reads it back, and the boot report prints `[boot] watchdog 2 s`. `loop()` kicks it, and
  `setup()` refreshes the bootloader's period once after the ECC recovery. HardFault, MemManage, BusFault and
  UsageFault move to a 512-byte stack of their own, drive XSMT low by register write, store PC, LR, CFSR, HFSR,
  BFAR, MMFAR, SR2 and ECC_FA2 in a `.cv_noinit` record (`cubevox.ld`, top of DTCM), drive MUTE_N low 5 ms later
  and reset 20 ms after that. The next boot report prints `[ERROR] last reset: fault ...` or `[boot] fault: none`,
  plus the decoded `RCC->RSR` reset cause. A stack pointer outside RAM stores zero for PC and LR. `faultInit()`
  paints the free stack, and the boot report prints how much was never used. The stall watch arms at each stream
  start. If the block counter then stops for 100 ms while the stream runs and the audio module has not latched,
  the mute latches, an `audio` entry goes to the diagnostic log, and a planned reset brings the sound back. SAI and
  DMA transfer errors are counted in `audio.cpp` and reported at most once a second. `SAI1_IRQn` delivers `OVRUDR`,
  `AFSDET`, `LFSDET` and `WCKCFG`. A stream-stopping error or 8 overruns in a row mutes; `audioPoll` restarts at
  most 3 times a minute, then latches. TX halves are zeroed once read.
- `board/dfu.cpp`: `enterDfu()` is the only way into update mode, where the bootloader serves the `CUBEVOX` drive.
  It writes a summary of this run into the boot block (version, uptime, reset cause, last fault kind), queues a
  diagnostic log entry and starts the planned reset (`board/planned_reset.cpp`). `loop()` keeps running until a
  pending record save and the entry are in flash (3 s at most, a running erase not counted), and the menu takes
  no input meanwhile. Then the serial queue drains for 100 ms, `muteForReset()` mutes, the request word is written
  and the box resets. The menu action and the `enter_dfu` command both call it. The confirm screen shows the
  installed version, the box shows "Starting the update." until the reset, and it shows the same version for 2 s
  after every start.
- `board/boot_health.cpp`: the bootloader counts every start that reaches the application. `bootHealthInit()` at
  the end of `setup()` reads that count, and `bootHealthPoll()` zeroes it 5 s later if the audio interrupt has
  advanced blocks since `setup()` ended. A fault, a watchdog reset or a hang inside those 5 s leaves the count
  standing, and after three such starts in a row the next reset stays in update mode. The boot report line
  `[boot] image <version> crc ok|BAD, boot fails <n>, last update reason <text>` and the `status` fields
  `boot_fails` and `update_reason` give the earlier failed starts and why the bootloader last stayed in update
  mode. The CRC covers the running image and is computed once, on the first report.
- `board/flash_bank2.cpp` and `board/record_service.cpp`: the layout record lives in bank 2 at 0x08100000 and
  0x08120000 (spec `2026-10-12-layout-record-design.md`). A save runs 500 ms after the last change, or at once when
  the menu closes, and writes nothing when flash already holds the same state; one erase or flash-word program per
  `loop()` pass. Bank 2 sector 2 at 0x08140000 holds the diagnostic log, which
  `core/entry_log` appends to over `core/flash_io`, `board/diag_service` fills and polls, and the bootloader only
  reads for `DIAG.TXT`. The flash driver checks the unlock, rejects a sector outside the record's two and the log's
  one, and bounds every wait (5 s for an erase, 5 ms for one program word); a failure sets an error flag
  and `recordPoll` prints `[ERROR] record save failed`. `build.sh` and the linker script keep the image below
  bank 2, and the bootloader writes only inside the image region. The fault record keeps FLASH->SR2 and ECC_FA2;
  a double ECC fault is recovered at boot before the scan. In a record sector the newest record that ends below the
  faulted word moves to the other sector first when nothing there is newer, then the faulted sector is erased; the
  log's sector is simply erased. A sector that will not erase stays unread for that start, the log stays closed,
  and the boot report names what happened. A record whose layout fails validation
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
  `serial_dropped`, `stray_bytes`, `boot_fails`, `update_reason`, the audio counters `overruns`, `sai_errors`,
  `restarts` and `nan_blocks`, and `power_trips`, `stalled`, `audio_latched` and `reply_overflows`. All output
  (replies, boot report, bench prints) goes through `serialOut()`, a bounded queue (`core/tx_queue`) that
  `serialLinkPoll` drains each pass with non-blocking writes sized by `Serial.availableForWrite()`; a line that does not fit is dropped whole and counted, and with no host connected
  the queue is discarded. `enter_dfu` and the bench fault and spin ops give the queue 100 ms (`serialFlush`) before
  they act. Engine clears happen on the silent swap block; `kResetsPerSilentBlock` and `kFadeHoldBlocks` spread them
  over several blocks if the bench shows that block late, and the `stats` op's line `layout swaps` prints the swap
  block and the worst block through the last fade-in. `python3 -I tools/protocol_check.py` (repo root `tools/`, no
  pyserial needed) runs the checks against a box and prints PASS or FAIL per check; `--no-reset` skips the reset-and-reload
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

Costs in `core/costs.h` are marked measured or estimated. The measured rows come from runs of the engine chain on
an H743 test board, and the POLISH COMP, soft ceiling and BYPASS rows stay estimates until a cubevox board runs
them; the method is in that file. The audio path, clock tree, USB gating and mux timing have run on the test board
and not yet on a cubevox board. The bring-up logs are in `hardware/bringup/`. The OLED driver starts the panel at
boot and retries one that does not answer once a second, with a `[WARN]` in the boot report, so the box plays on
without a panel. `board/power_sense` mutes the output when 5VA, read on VA_SENSE (PB1), falls under
`kPowerTripMv` (4750 mV) and releases it after 500 ms above 4850 mV, and `status` reports its trips as
`power_trips`. The trip level is provisional until the bench sets it from the measured fall of 5VA.
