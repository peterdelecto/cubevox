# Bench checklist

These checks need a person at the bench with a board, a scope or a PA. They come from the "Bench
checks" sections of the 2026-10-24 firmware audits, grouped by the slice each audit covered.
`firmware/bringup.py` names this file in its final summary and automates none of it.

Each row has an id for citing it, the check, what a pass looks like, and the audit item it answers.
Audit items are written as the slice name and the item (audio A1 is item A1 of the audio and I/O
audit). The behaviour a pass describes is the behaviour after the fixes of the same pass. Rows that
carry a measured number from the audit keep that number.

## Audio and I/O

| Id | Check | Pass when | Answers |
|---|---|---|---|
| AU1 | Pull the barrel and switch SWPWR1 off, once while singing and once in silence. Scope the output and time 5VA's fall against K102's release. | The order on the scope is XSMT low at least 3.4 ms before the rails go, then MUTE_N low, then K102 open. No thump is audible through the PA and no step shows above the noise floor, in both cases. | audio A1, the power-fail mute (hardware spec F2 and F3) |
| AU2 | Hold the box in reset, in the UF2 bootloader and in the ROM DFU. | K102 stays open and XSMT stays low in all three states, with both pins undriven. | audio answer 4, mute at start; same measurement as BO10 |
| AU3 | Read DMA1_Stream0->NDTR in the RX half callback. | The lead of the TX DMA over the RX half is measured and written down. A lead above zero confirms the finding, and the late count then has to count inside that lead. | audio B4, late count (unsure item) |
| AU4 | Run the stop_audio op with signal playing and listen to the 100 ms before the latch. | The 375 Hz buzz of the looped ring is absent. The mute latches or the stream restarts, and status and the next boot report show the stall. | audio A3, DMA error ends the audio |
| AU5 | Read the silicon revision in the boot report. | Rev V (0x2003) runs 480 MHz. Any other revision runs 360 MHz, where the default layout's worst sum rises from 72 % to about 96 % of the block. The revision of each unit is logged. | audio answer 9, budget; same as EN1 and HE7 |
| AU6 | Turn DRIVE to 0 with the Distortion toggle on and listen for fuzz. Repeat with Mix at 0 on Octave and on Reverb. | No fuzz at DRIVE 0 and no effect at Mix 0, and the readout agrees with the sound. | audio A2, knob rest value |
| AU7 | Sweep RATE above 68 % at full DEPTH and listen for ticks. Flip OUTPUT on a sine and scope the step. | No tick on the sweep. The OUTPUT flip ramps over a block or more and shows no single-sample 30 dB step. | audio B2, Unison swing, and audio B1, OUTPUT step |
| AU8 | Read every pot at full clockwise with panel_dump. | All 16 print 1.00. The pots run on 3V3A and VREF+ comes from a separate supply, so the end margin of 1 % has to cover the difference (hardware spec items 12 and 13). | audio answer 5, end dead band |
| AU9 | Time 3V3A's rise after power-on against kMuxSelectHoldMs (1,100 ms). Compare the first published knob values with the values one second later. | 3V3A is up before the hold ends, with the margin logged. No knob moves by more than the 1.5 % move threshold between the first publish and one second later. | audio answer 5, mux timing |
| AU10 | Measure the longest loop() pass and the 16-pot scan period, with analogRead's init and calibration included. | Both numbers are logged. The scan keeps the one mux step per pass at least 500 microseconds apart, and the longest pass stays far under the 2 s watchdog. | audio C4, analogRead cost |
| AU11 | Run the stats op on the default layout with both polish stages on. | The COMP and soft ceiling load in percent is read and entered in core/costs.h. The default layout stays under the 75 % mean and 95 % worst lines with those terms added. | audio C1, kFixed misses the polish tail; engine B7 |
| AU12 | Plug and unplug USB in the middle of a song. | The late count in status does not move and the audio does not glitch. | audio answer 8, USB attach |

## Bootloader and update path

| Id | Check | Pass when | Answers |
|---|---|---|---|
| BO1 | Power on cold 20 times with no debugger attached. | The box reaches the app 20 times out of 20. | boot answer 5, DWT CYCCNT spin, and answer 3, byte stores into random DTCM |
| BO2 | Measure MENU released at power-on. | MENU reads high 20 ms after a cold power-on on the internal pull-up alone. An RC on ENC_MENU_SW would read as held at every power-on, so a box that does is a fail. | boot answer 3, MENU read |
| BO3 | Power on cold with MENU held, 20 times. | The OLED answers its first init every time. The driver never retries, so one miss shows as a dark screen. | boot answer 5, OLED waits |
| BO4 | Pull the cable in the middle of a copy and record the screen. | The screen says the update stopped (field-update spec failure table) and the 60 s timeout runs again. Before the fix the screen keeps "Do not unplug." | boot B1, pulled cable |
| BO5 | Force a double ECC error in log sector 2. | DIAG.TXT shows `flash log: unreadable at 0x814xxxx`, which confirms the FAIL_ECC_ADDR decode, and the box stays in update mode. | boot B8 (unsure item) and answer 4 |
| BO6 | Cut 9 V during sector 1's erase, then drag the release once. | The first copy completes with no reset in the middle. | boot B9, torn word survives |
| BO7 | Enter update mode by the menu, by the app request and by MENU at power-on, many times over. | The CUBEVOX drive appears within 3 s on every entry. | boot answer 5, TinyUSB core reset loops |
| BO8 | Cold-start every board and watch for the update screen or the app. | HSE starts inside its 100 ms timeout on every board. A miss is a dark box. | boot answer 6 and B13, missing crystal |
| BO9 | Scope I2C2 SCL while the bootloader draws a screen. | SCL runs near 400 kHz. | boot C7, CV_OLED_TIMINGR |
| BO10 | Hold the box in update mode and watch K102 and XSMT. | K102 stays open and XSMT stays low through update mode. The bootloader never drives PB5 or PC5. | boot pins; same measurement as AU2 |
| BO11 | Time a full update of the current release image, and time the update-mode timeout against a stopwatch. | The copy takes about 3 s for a 200 KB image with the erase included (field-update spec). The timeout fires at 60 s. | field-update spec timing table |
| BO12 | Drag the release onto the drive on Adam's macOS version. | The only dialog is the "not ejected properly" notice after the reset (field-update spec, Writes item 4). Any other error dialog before the reset is logged as a finding. | field-update spec, Writes item 4 |

## App boot, bank 2, faults and watchdog

| Id | Check | Pass when | Answers |
|---|---|---|---|
| HE1 | Boot the PCB with the OLED fitted ten times, once with a blank diag log and once with a full one. | The box starts with sound 10 times out of 10, and no boot report shows "audio stalled after 0 blocks". | health A1, stall watch armed at boot |
| HE2 | Cut the 9 V for 50 ms, 0.5 s and 3 s, three times each, within 5 s of a start. Read boot_fails from status and watch for "Startup failed.". | boot_fails reads 0 after each power-on and "Startup failed." never appears after a power interruption. | health A2, start counter and power dips |
| HE3 | Read BOR_LEV from FLASH->OPTSR_CUR on a fresh unit and again after the shop step. | It reads 0 on the fresh unit and 3 after the shop step. | health A4, brown-out level |
| HE4 | Cut the 9 V during a run of bench record_save ops. Read the boot report for an ECC recovery and the layout the box comes back on. | The box comes back on the newest intact record, not the default layout, and the boot report names the recovery. | health A3, torn record word |
| HE5 | Listen through a PA at gain to the bench fault and spin drills. | The fault drill makes no click. | health B1, fault path mute order |
| HE6 | Leave the drive from enter_dfu mounted past 2 s and up to the 60 s timeout. | No watchdog reset happens in update mode, which shows the IWDG stops at a system reset. | health answer 1, watchdog |
| HE7 | Read "[clock] silicon rev" on every unit. | Rev V runs 480 MHz. A unit on other silicon runs 360 MHz, where the default chain nears 96 % worst. The revision of each unit is logged. | health answer 6, clocks; same as AU5 and EN1 |
| HE8 | Hold the OLED's SDA low and watch the heartbeat LED. | The LED toggles every 0.5 s. A toggle of 0.7 to 1.4 s measures the loop pass, and it stays under the 2 s watchdog with no reset. | health answer 1, loop pass against the watchdog |
| HE9 | Cold-start the crystal against the HAL's 100 ms HSE timeout. | HSE starts inside 100 ms on every cold start tried. A miss leaves the bootloader dark until a power cycle. | health answer 6 and boot B13 |
| HE10 | Scope MUTE_N, XSMT and the K102 contacts through a watchdog reset. | The board pulls hold the mute while the MCU pins float. MUTE_N and XSMT stay low and K102 stays open. | health answer 7, power order |
| HE11 | Time a boot-time recovery erase of a bank 2 sector. | The erase takes 1 to 2 s against the 5 s bound and ends before the watchdog starts. | health answer 1, flash waits |

## Engines and CPU

| Id | Check | Pass when | Answers |
|---|---|---|---|
| EN1 | Read REV_ID at boot. | Rev V runs 480 MHz. Older silicon runs 360 MHz and puts the default layout at 81 % mean and 96 % worst. | engine budget; same as AU5 and HE7 |
| EN2 | Take per-slot cycles for the default layout with every knob at its top. Include the kStatComp and kStatEq buckets, Unison on the parked path, and the block total minus the slots. | The table is entered in core/costs.h. The default layout sum, with the polish tail, the ceiling and the BYPASS mix included, stays under the 75 % mean and 95 % worst lines. | engine B7, cost table |
| EN3 | Read the worst block while toggling Autotune on with the voice clip, and while sweeping reverb MIX and SEMITONES. | No block reaches 100 % of its budget and the worst stays under the 95 % line. | engine B7 and B4, engage block and glides |
| EN4 | Count each gate's and Slapback's cycles before and after the fastmath swap. | The saving per stage is logged. About 10 % of the block in total is the estimate to confirm. | engine B4, libm per sample |
| EN5 | Measure the cycles of the AXI SRAM engines, which have a 143,596 B working set against the 16 KB D-cache. | The cycles match the figures in core/costs.h, or costs.h is updated with the new ones. | engine budget, state in AXI SRAM |
| EN6 | Check the `.dtcm_bss` end in the link map against 0x2001FF00. | The end is at or below 0x2001FF00. build.sh prints the margin as the `[budget] DTCM` line (PitchFx takes 112,920 B at v2.00.27). | engine C1, PitchFx DTCM budget |
| EN7 | Measure round-trip latency with Autotune off and with it on, and ask Adam whether he accepts the Autotune latency in his monitors. | Off reads about 2.7 ms plus the codec delay. On reads 25 to 39 ms. Adam's answer is logged. | engine answer 9, latency |
| EN8 | Listen for clicks at RATE above 68 %, with SEMITONES through 0 at high MIX, with OUTPUT LINE/MIC on a held note, and with BYPASS both ways. | No click in any of the four. | engine B2, B3 and B6, and answer 3 for BYPASS |

## OLED and UI

| Id | Check | Pass when | Answers |
|---|---|---|---|
| UI1 | Lift the panel's SDA or VCC for 50 ms while singing, then watch the 500 ms heartbeat LED and the knob response. | The heartbeat holds and the knobs answer within one pass. Before the driver fix the passes last about 0.8 s until power-off. | ui A1, I2C error locks the handle |
| UI2 | Hold SCL low for 200 ms with the voice screen running and time the loop with the stats op. | The longest pass stays under the driver's own timeout and no watchdog reset happens. | ui A1, timeout path |
| UI3 | Scope SCL and SDA at 400 kHz with the fitted pull-ups and cable. | The rise time is under 300 ns and one page takes about 3.4 ms. | ui answer 1, flush cost |
| UI4 | Measure loop pass time and the UI tick rate with the default chain running and a loud voice screen. | The UI holds 25 fps. No page write stretches past 5 ms. The UI load in percent at 480 MHz is logged with the face on and off. | ui answer 2 and OLED GUI spec Bench 1 and 3 |
| UI5 | Power up cold 20 times. | The splash shows every time. | ui B1, probe timing |
| UI6 | Run 20 bench fault resets with the voice screen running. | The panel returns every time. | ui B1, SDA held after a reset |
| UI7 | Check the picture orientation in the enclosure and the column offset of 2 in the SH1106 table. | The picture is upright and no stray column shows at either edge. | ui answer 1, display driver |
| UI8 | Watch the scrolling ribbon while pages go out one per pass, top to bottom. | The ribbon shows no visible shear. If it does, the owner decides what to change. | ui answer 1, one page per pass |
| UI9 | Leave the pots untouched for 10 minutes. | No readout appears. | ui answer 2 with audio A2, readout trigger |
| UI10 | Sing so that the ribbon fills (about -6 dBFS RMS) and watch whether the ADC already clips on peaks. | No clipping on peaks while the ribbon is full. If the ADC clips, the owner decides whether the meter needs a clip mark. | ui answer 7, level meter |
| UI11 | Cycle TOP SWITCH in the menu with the switch engaged. | The output changes at once and the new word shows after the menu closes. | ui answer 2, TOP SWITCH |

## Tooling

The tooling audit has no Bench checks section. These rows cover the script changes of this pass that
only a real box can exercise.

| Id | Check | Pass when | Answers |
|---|---|---|---|
| TO1 | Build the release image and run `firmware/flash.sh` on a box with the bootloader in. | The last lines show `[boot] image <version> crc ok` with the version that was built, and `echo $?` prints 0. | tooling B3, flash read-back |
| TO2 | With the box running, run `firmware/term.py --expect-image v0.00.00`. | It prints the `[ERROR] no '[boot] image v0.00.00 crc ok' line` message and `echo $?` prints 1. | tooling B3, negative control |
| TO3 | Build the release image with `CLEAN=1 firmware/build.sh` and compare its image check line with the line in the release notes of the same commit. | Version, byte count and CRC are equal. A difference means CI and the bench build different images from one commit. | tooling B6 (unsure item) |
