# The emulator, and the LED that shows the load

Owner 2026-11-01: "make an emulator that faithfully reproduces the experience of the box, showing
the cpu percent. use toggles to switch between unison or mob or spring and chasm for those slots.
no debug sliders needed otherwise." Same day: "the mcu has a flashing led that doesn't appear to
indicate anything other than that the mcu is on. there is already a power indicator led. would be
better if the led flashes to show mcu processing." Build order: "just build it now."

## 1. What it is

1. A Mac app, `cubevox-emulator`, that runs the box's firmware code unchanged: `core/`, `ui/` and
   `engine/*.h`. Mac code stands in for `board/` only. Source in `firmware/host/`.
2. It replaces the prototype emulator (`emulator/`, `cubevox-proto`) as the thing the owner opens.
   The prototype stays in the tree but builds only with `-DCUBEVOX_BUILD_PROTO=ON` (owner
   2026-11-07); its `--layout` probe (ctest `proto_layout`) exists only in that build and is a
   gate only when `emulator/` changes.
3. What the box does, the emulator does, because it is the same code: splash, face, readout, menu,
   MUTED and BYPASS screens, the knob filter and its stepped knobs, the slot crossfades, the layout
   fade on a card swap, the mute sequence, the cost table and the rule that a layout over the CPU
   lines does not load.

## 2. Decisions

1. CPU percent is the cost table (`core/costs.h`), `layoutCost(layout)` mean and worst against the
   75 and 95 lines. Owner picked it over the Mac's own timing on 2026-11-01: it is the number the
   box uses to accept or refuse a layout, and the Mac's timing converts at a ratio that differs
   between the pitch code and the rest. The window shows it on one line; with DEBUG on in the menu,
   the voice screen's figure is the same table's worst. Host timing is measured for nothing and
   shown nowhere.
2. Five card switches, the only controls the box lacks, one for each SLOT row of the menu (SLOT 3,
   4, 6, 7, 8): under slot 3 `OCTAVE`, `FORMANT` and `APRAXIA`, under slot 4 `UNISON | MOB`, under
   slot 6 `DISTORTION | DEEPTHROAT`, under slot 7 a grid of `GATE`, `LO-FI` and `CHATTER`, under
   slot 8 `SPRING | CHASM`. Each goes through the menu's own action (`UiAction::SwapOctaveFormant`,
   `UiAction::SwapUnisonCrowd`, `UiAction::SwapDistortionGrowl`, `UiAction::CycleSlot7Card`,
   `UiAction::SwapReverb`), so the
   layout fade and the engine resets run as on the box, and a swap the box would not load
   leaves the layout unchanged. The switch position follows the layout, so a swap made in the
   menu moves it.
3. One source switch, `CLIP | MIC`. The clip starts as `samples/adam_vox_sample3_5s.wav`, looped;
   File > Open (Cmd+O) in the Mac's menu bar loads another wav or mp3 in its place (what the
   decoder takes) and sets the switch to CLIP, and the window title carries the clip's name (owner 2026-11-01: "i need a
   file>open for the file"). MIC is whatever macOS has chosen as input; no picker; no mic or no
   permission dims the switch. The audio device opens playback-only until MIC is picked, because a
   duplex open takes a Bluetooth headset's capture side and drops every Mac sound to phone quality.
   Playback is the default output, both channels the same.
4. Nothing else on the window. No tuning panels, sliders, state file or menus. Layout and settings
   live in memory for the run.
5. The window is 1440x840, fixed, dark only, every colour and size from one token header
   (`firmware/host/theme.h`), text and background pairs checked for contrast before the window
   is called done.
6. The LED on the box shows the processing load (section 5). The emulator shows the same LED from
   the cost table's mean.

## 3. Host seams

The firmware's `core/` and `ui/` include nothing from `board/`, so the host supplies:

| Board piece | Host piece |
|---|---|
| `cubevox.ino` `loop()` | `firmware/host/box.cpp`, `Box::tick(PanelInput, nowMs)` once per frame |
| `board/audio.cpp` `processHalf` | `firmware/host/audio_block.cpp`, `process(in, out, 64)` in the audio callback |
| `audioScope` | `AudioBlock::scope`, the same `ScopeRing` in `BlockFlow`, so the emulator draws the scope trace from the box's own code (`2026-11-06-scope-design.md`); the two threads share it through its block count |
| `board/controls.cpp` | the window's knobs and toggles into `KnobBank` and `PanelReading` |
| `board/input.cpp` (MENU) | the MENU knob: drag or scroll turns; a click on the knob, or Enter or Space, is a click when it ends and a hold after 600 ms (`MenuButton`, the same class the box uses), and a hold closes an open menu |
| `board/mute.cpp` | `MuteSequence` as the board runs it, minus the 6 s power-on wait |
| `board/display_sh1106.cpp` | a `Display` whose `flushStep` copies the dirty page into a 128x64 buffer drawn at 4x |
| SAI in and out | miniaudio duplex, 48 kHz, 64-frame period, mono in, stereo out |
| `board/record_service.cpp`, flash bank 2 | memory for the run |
| DWT cycle counter | a steady-clock lambda for `runChain`; its result is unused |
| user LED | a dot under the MENU knob |

The snapshot hand-off from the main thread to the audio callback is the board's own lock-free
scheme (`audioPublish`, `syncSnapshot`), ported. The host never marks a block late and never
plays a dry block.

## 4. Window

Top band: MUTE toggle at the left; the OLED at 4x in the centre; the MENU knob at the right with
the LED and the CPU line under it: `CPU  67 % mean   76 % worst   lines 75 / 95`, a figure in the
danger colour when over its line.

Toggle row: the eight bypass toggles, one above each slot strip, drawn as the box's bat toggles,
a vertical lever with its tip up for on and down for off (owner 2026-11-01: "vertical toggles
above the effect strip as they are laid out on the actual unit"). MUTE is the same toggle, top
left. No ON or OFF words on a toggle; the lever and its colour say it.

Slot band: eight strips, slot 1 to 8, each with the card's name, the upper knob and its label,
and the lower knob and its label. Slots 4 and 8 carry the card switch at the foot. Knobs show no
value text; the OLED readout shows the value, as on the box.

Bottom band: the source switch at the left and the version under it.

## 5. The LED

1. Today the LED toggles every 500 ms from the main loop. That says only that the loop runs, and
   the 2 s watchdog already guards that. The board has a separate power LED.
2. The LED shows the load as brightness, with no blink (owner 2026-11-05: "why does the mcu
   indicator light just flash regularly at regular intervals? that provides no information on
   whether it is processing"). The first build lit it for the mean load's share of each half
   second, and at a steady load that is a regular blink whose information sits in an on-time the
   eye cannot read.
3. The mean is the mean block time over the last whole second as a percent of the block budget,
   from the same one-second window that feeds the DEBUG figure's worst (`core/load_window.h`).
   The level is 0.03 + 0.97 x share squared, where share is the mean over the 75 % average line
   that a layout is gated against (`costs::kBudgetAvgPercent`), held to 0..1. So the LED is a
   faint glow at no load, about a quarter bright at half the limit and full at the limit. A PWM
   duty is linear in light and the eye reads light on roughly a square-root scale, so a duty
   that goes with the square of the load looks like an even climb.
4. A late, dry or overrun block holds the LED full for 100 ms, so a box in trouble shows it at
   low load too. The LED is dark while audio is not running, has stalled, or is latched after a
   fault, and the flash does not light it then.
5. The rule is `loadLedLevel(meanPercent, limitPercent, audioLive, msSinceTrouble)` in
   `core/load_led.h`, which returns the level 0..1 and is shared by the box and the emulator.
   `BlockTroubleClock` turns the running count of late, dry and overrun blocks
   (`audioTroubleBlocks()`) into the time since it last changed.
6. On the box, `cubevox.ino` `driveLoadLed` writes the level to PB7 (`USER_LED`) as PWM:
   TIM4_CH2, 1 kHz, 12 bits, `analogWrite` on the STM32duino core. It writes only when the duty
   changes. The bootloader's own LED use is unchanged.
7. The emulator passes the cost table's mean and no trouble, because the host never marks a
   block late. It draws the dot from the unlit grey to full green along level^(1/2.2), so the
   light on the screen goes with the duty on the box.

## 6. Checks

1. `cubevox-emulator --check`, registered in ctest as `emulator_check`: no window, no audio
   device; the clip through the block path for 3 s on the default layout, then both swaps and 2 s
   more; asserts finite output, output above -40 dBFS RMS in the last second, the cost sums
   before and after the swaps derived from the `costs::` rows, the layout reading `crowd` and
   `reverb_chasm`, a rendered face, and a MENU hold that closes the open list. Prints `emulator check: ok`. Adam's clips stay out of git,
   so where `samples/` is absent (CI) the check writes two synthetic voiced clips (150 Hz for 4 s,
   220 Hz for 3 s, harmonics under three formant peaks, 5 Hz vibrato) to a temp directory, runs the
   same checks on them, and prints a `clip:` line naming the set it used.
2. `cubevox-emulator --proof <file.ppm>`: one frame of the window after the splash, rendered to a
   hidden window and written as a PPM, for looking at. `--on <slot>` (1 to 8, repeatable) runs with
   that slot's toggle on, every other off; `--at <ms>` takes the frame after that much run time
   instead of 2500 ms. `--slot3`, `--slot6`, `--slot7` and `--slot8` pick the card in a swappable slot.
3. `ctest` as a whole (`proto_layout` joins it only with `-DCUBEVOX_BUILD_PROTO=ON`).
4. The LED: host tests for the window mean, the `loadLedLevel` curve and flash and
   `BlockTroubleClock` (`core_load.cpp`); the release and bench firmware builds; the box flashed
   with the merged release and watched for the PWM.

## 7. Out of scope

The serial protocol and bench ops on the host, saving the layout between runs, a device picker,
measured host CPU time, and removing the prototype.

## 8. Log

- 2026-11-01: designed and built. Owner picked the cost table for the CPU figure (A over B) and
  ordered the build before the spec review.
- 2026-11-06: the voice screen's output trace is the scope (`2026-11-06-scope-design.md`). The
  host's output-energy tally went with the board's; both read the ring in `BlockFlow`.
