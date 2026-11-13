# cubevox bring-up, board 1

| Item | Value |
|---|---|
| Date | 2026-11-13-0258 |
| UID | 1E0049000351343434393332 |
| Bootloader | cubevox bootloader v0.09 |
| Firmware | v2.01.93 (release) |
| Failed | 0 |

## Checks

```text
PASS  flash bench image: build.sh VARIANT=bench + flash.sh
PASS  USB enumeration: /dev/cu.usbmodem327C395234341
NOTE  image: bench
NOTE  reset cause: software pin
PASS  fault record: none
PASS  silicon revision: rev V (0x2003), dev 0x450
NOTE  ROM bootloader ID at 0x1FF1E7FE: 0x92
PASS  crystal start-up (PLL on HSE): HSE 25 MHz
PASS  core clock: 480 MHz
PASS  USB clock: 48000000 Hz from PLL1Q
PASS  SAI kernel clock: 49152008 Hz (+0.2 ppm)
PASS  pin decode: [boot] pins XSMT C5 MUTE_N B5 SAI E2 E3 E4 E5 E6 OLED B10 B11
PASS  caches and FPU: on, on, on
PASS  audio init: [boot] audio running, SAI1 48 kHz
PASS  OLED: found at 0x3C
PASS  audio block rate (48 kHz SAI): 750.0 blocks/s against 750 blocks/s (-0.00 %)
PASS  late blocks and overruns: 0 late, 0 overruns in 21260 blocks
PASS  audio stall mutes: stall reported, output muted
PASS  audio stall resets: [boot] reset: software
PASS  watchdog reset: [boot] reset: watchdog
PASS  fault recorded across reset: [ERROR] last reset: fault bus pc=0x802A958 lr=0x802C067 cfsr=0x8200 hfsr=0x0 bfar=0xDEADBEE0 mmfar=0xDEADBEE0
```

## Update drills

| # | Drill | Result | Time | Detail |
|---|---|---|---|---|
| 1 | enter_dfu opens the update drive | PASS | 2.4 s | serial port gone, drive mounted in 2.4 s (limit 5 s); cubevox bootloader v0.09; installed v2.01.93-bench; option bytes set, BOR level 3, sector 0 protected |
| 2 | bench .uf2 copy restarts the application | PASS | 0.8 s | drive left 0.5 s after the copy, serial port back 0.8 s later (limit 10 s); hello build bench, fw v2.01.93 |
| 3 | USB pulled at 30 % of a copy | SKIPPED |  | needs the operator; --no-prompt |
| 4 | 9 V cut at 30 % of a copy | SKIPPED |  | needs the operator; --no-prompt |
| 5 | wrong base address refused | PASS |  | 1252 of 1252 blocks refused and the drive stayed up; layout unchanged after the restart (0.8 s) |
| 6 | other family refused | PASS |  | 1252 of 1252 blocks refused, last family 0x0 (file 0x0); drive still reports installed v2.01.93-bench; a full copy then restarted the box (0.8 s) |
| 7 | flipped byte lands in update mode | PASS |  | after the flipped copy the remounted drive reports installed 'invalid'; a full copy then restarted the box (0.8 s) |
| 8 | a new image's failed starts land in update mode | PASS | 3 starts | the new image faulted at 3 starts (want 3), then update mode; DIAG.TXT reason BootFailed, fault bus pc=0x802A958 lr=0x802C067 cfsr=0x8200 hfsr=0x0 bfar=0xDEADBEE0 mmfar=0xDEADBEE0; status boot_fails at each start [0, 1, 2]; the bench image then restarted the box (0.8 s) |
| 9 | layout survives an update | PASS |  | layout 'drill9' with slots 1 and 2 swapped survived the update (0.8 s); original layout set again |
| 10 | removed (no MENU path since bootloader v0.09) | SKIPPED |  | removed: bootloader v0.09 does not read MENU at a start |
| 11 | no computer, back to the application | SKIPPED |  | needs the operator; --no-prompt |
| 12 | menu entry and confirm screen | SKIPPED |  | needs the operator; --no-prompt |
| 13 | release .uf2 copy | PASS | 0.8 s | hello build release, fw v2.01.93, serial 1E0049000351343434393332 |
