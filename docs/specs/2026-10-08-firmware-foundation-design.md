# Firmware foundation design (2026-10-08)

Owner asked whether the firmware plan is proper and prudent now that it is built from the
ground up. Review of `firmware/` at a5dec89 against `2026-10-08-update-and-slots-design.md`.
This spec adds what that roadmap leaves out. The roadmap's order stands.

## Assessment

Already prudent, left alone:

1. Build order with a checkable gate per step (clock, registry, record, serial, app, page, release).
2. Audio runs in a software-pended low-priority IRQ with DWT cycle counts, overrun counting and a double-buffered atomic parameter publish (`audio.cpp`).
3. Mute is asserted first in `setup()`; the relay sequence is time-based (`mute.cpp`).
4. Flash record is ping-pong with CRC and schema version; the image stays in bank 1 so DFU cannot erase the layout.
5. Wire ids are stable; firmware is the single source of truth for the registry.
6. Every init failure sets a string the boot report prints as `[ERROR]` or `[WARN]`.

Missing:

1. Tests for firmware logic. The engine has 14 host tests; the firmware has none. Registry apply, layout rules, the record codec and the serial protocol are pure logic that can run on the Mac.
2. A fault story. No watchdog, no fault handler, no reset-cause report, no action when the audio IRQ stalls.
3. Toolchain pinning in the build. README names core 3.0.0; `build.sh` does not check it.
4. Two step-3 decisions: JSON handling on the box and the source of the version string.
5. Directory shape. The sketch is one flat folder where HAL code and pure logic sit side by side, so nothing marks which files may touch hardware.

## Directory layout

Arduino compiles every source under the sketch's `src/` tree, so the sketch can be split
by dependency tier. Files move; nothing is rewritten in the move.

```
firmware/
  build.sh                  pins the core, sets the version, builds
  README.md
  platform/                 verbatim Teensy-H7-Port copies (unchanged)
  cubevox/
    cubevox.ino             setup() and loop() only
    src/
      core/                 pure C++17, no Arduino or HAL include, host-tested
        registry.h/.cpp     cards, knob descriptors, apply, format
        costs.h             per-release cost percents
        chain_params.h/.cpp ChainParams and its defaults
        layout.h/.cpp       runtime slot-to-card array, validation (step 2)
        record.h/.cpp       bank 2 record codec, CRC32 (step 2)
        protocol.h/.cpp     JSON request parse, reply format (step 3)
        unison_rate.h
        version.h           CUBEVOX_FW_VERSION (step 3)
      board/                Arduino and HAL; one hardware concern per file
        pins.h
        slots.h/.cpp        toggle pin and mux channels per slot
        sysclock.h/.cpp
        usb_link.h/.cpp
        audio.h/.cpp        SAI, DMA, the audio IRQ, runChain
        controls.h/.cpp     mux scan, smoothing, stepping, toggles
        mute.h/.cpp
        display_sh1106.h/.cpp
        fault.h/.cpp        watchdog, fault handlers, reset cause
        flash_bank2.h/.cpp  erase and program only (step 2)
        platform.h, platform_shim.cpp
      ui/
        menu.h/.cpp         OLED screens; deliberately unplanned beyond today
```

Rules:

1. `core/` never includes `<Arduino.h>`, HAL headers or anything from `board/`. The host test target compiles every `core/*.cpp` and fails the build if one does.
2. `board/` may include `core/`. `ui/` may include both.
3. Includes are tier-qualified: `#include "core/registry.h"`, `#include "board/pins.h"`.
4. `build.sh` adds `-I$FW/cubevox/src` so the same include lines work on the box and in the test.

## Decisions

| Item | Decision |
|---|---|
| Host tests | CMake target `firmware_test` (`test/firmware_test.cpp`) compiles `firmware/cubevox/src/core/*.cpp` with `CUBEVOX_ENGINE_FLAGS` against `engine/`. In `ctest` alongside the engine tests. |
| Test content, step 1 | Each card's two knobs move the expected `ChainParams` field at 0 and 1. Stepped knobs report their positions. `formatKnob` per unit. `applyPlacement` selects the reverb engine. Default layout matches CLAUDE.md. Ids unique and snake_case. Default layout cost sum is under `costs::kBudgetPercent`. |
| Test content, later | `layout`: reverb rejection, unknown id to `empty`. `record`: encode/decode round trip, CRC, newest valid wins, corrupt record falls back. `protocol`: each command parses, each reply formats, bad input gives an error reply. Each module joins the target the day it is written. |
| Watchdog | IWDG, 2 s, kicked from `loop()` only, never from the audio IRQ. Armed after `audioInit()`. |
| Fault handlers | HardFault, BusFault, UsageFault, MemManage: drive MUTE_N low, store PC, LR, CFSR, HFSR and a magic word in a `.noinit` struct, reset. Next boot report and `status` print `[ERROR] last reset: fault pc=... cfsr=...` then clear it. |
| Reset cause | Boot report prints `RCC->RSR` decoded (power-on, pin, IWDG, software) and clears it. `[boot] fault: none` when clean, for the first-article checklist. |
| Audio stall | `loop()` watches `audioStats().blocks`. No advance in 100 ms while audio is running: mute, set the audio error string, report once. |
| Toolchain | `build.sh` reads `arduino-cli core list`, refuses any core version but the pinned one, prints the first line of `arm-none-eabi-gcc --version`. Release action (step 6) pins the board index URL and the same core version. |
| Version | `-DCUBEVOX_FW_VERSION="$(git describe --tags --always --dirty)"` from `build.sh`; `core/version.h` exposes it; the release action passes the tag. Shown on the OLED and in `hello`. |
| JSON | Hand-written. Requests are flat objects with few keys; a small tokenizer extracts `cmd`, `id` and a fixed field set. Replies are built with `snprintf`. No ArduinoJson. Lives in `core/protocol.cpp`, host-tested. CDC transport in `board/usb_link.cpp` stays thin. |
| Record schema v1 | magic, schema, sequence, name[32], 8 × (id[16], 8 reserved bytes), menu settings, CRC32 over everything before it. Codec pure in `core/record.cpp`; `board/flash_bank2.cpp` erases and programs only. |
| Record fallbacks | Unknown card id becomes `empty`. A second reverb card becomes `empty` with a `[WARN]`. No valid record gives the default layout. |
| Harmony engine | Still compiles into `PitchFx` with no card (RAM 48 %). Remove from the firmware build only if RAM becomes a problem. |
| Daisy bench | Deferred (owner 2026-10-08). |

## Recommended next steps

Build order, each step checkable before the next. Steps 1 to 3 are the clean-up; building resumes after them.

1. Move files into `src/core`, `src/board`, `src/ui`; tier-qualify includes; `ChainParams` constructor from `slots.cpp` to `core/chain_params.cpp`; `-I$FW/cubevox/src` in `build.sh`. Check: `cd firmware && ./build.sh` gives the same sketch size as a5dec89 (129720 bytes) with no warnings.
2. `firmware_test` target with the step 1 test content. Check: `ctest` lists and passes it; a deliberate `#include <Arduino.h>` in a `core/` file breaks the host build.
3. `build.sh` core pin and version define; `core/version.h`. Check: build prints the gcc line and the version string; a wrong core version is refused.
4. `board/fault.cpp`: watchdog, fault handlers, reset cause, audio stall watch. Check: builds; hardware checks go on the first-article list (forced fault behind a step 3 serial command reproduces the `[ERROR] last reset` line; a stalled audio IRQ mutes).
5. Resume the roadmap at step 1 part 2 (runtime layout) in `core/layout.cpp`, with its tests.
6. Update `firmware/README.md` for the new layout and `2026-10-08-update-and-slots-design.md` next steps with a pointer here.
