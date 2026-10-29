# cubevox bring-up, board 1

| Item | Value |
|---|---|
| Date | 2026-10-29-1018 |
| UID | 1E0049000351343434393332 |
| Bootloader | cubevox bootloader v0.04 |
| Firmware | v2.00.38 (release) |
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
PASS  audio block rate (48 kHz SAI): 750.0 blocks/s against 750 blocks/s (+0.00 %)
PASS  late blocks and overruns: 0 late, 0 overruns in 21401 blocks
PASS  audio stall mutes: stall reported, output muted
PASS  audio stall resets: [boot] reset: software
PASS  watchdog reset: [boot] reset: watchdog
PASS  fault recorded across reset: [ERROR] last reset: fault bus pc=0x8026CF8 lr=0x80281D3 cfsr=0x8200 hfsr=0x0 bfar=0xDEADBEE0 mmfar=0xDEADBEE0
```

## Update drills

| # | Drill | Result | Time | Detail |
|---|---|---|---|---|
| 1 | enter_dfu opens the update drive | PASS | 2.4 s | serial port gone, drive mounted in 2.4 s (limit 5 s); cubevox bootloader v0.04; installed v2.00.38-bench; option bytes set, BOR level 3, sector 0 protected |
| 2 | bench .uf2 copy restarts the application | PASS | 0.3 s | drive left 0.2 s after the copy, serial port back 0.3 s later (limit 10 s); hello build bench, fw v2.00.38; the OS reported: [Errno 5] Input/output error |
| 3 | USB pulled at 30 % of a copy | SKIPPED |  | needs the operator; --no-prompt |
| 4 | 9 V cut at 30 % of a copy | SKIPPED |  | needs the operator; --no-prompt |
| 5 | wrong base address refused | PASS |  | 665 of 665 blocks refused and the drive stayed up; layout unchanged after the restart (0.3 s) |
| 6 | other family refused | PASS |  | 665 of 665 blocks refused, last family 0x0 (file 0x0); drive still reports installed v2.00.38-bench; a full copy then restarted the box (0.3 s) |
| 7 | flipped byte lands in update mode | PASS |  | after the flipped copy the restarted drive reports installed 'invalid'; a full copy then restarted the box (0.3 s) |
| 8 | failed starts land in update mode | PASS |  | started 3 times, then update mode; DIAG.TXT reason BootFailed, fault bus pc=0x8026CF8 lr=0x80281D3 cfsr=0x8200 hfsr=0x0 bfar=0xDEADBEE0 mmfar=0xDEADBEE0; status boot_fails at each start [0, 1, 2]; a full copy then restarted the box (0.3 s) |
| 9 | layout survives an update | PASS |  | layout 'drill9' with slots 1 and 2 swapped survived the update (0.3 s); original layout set again |
| 10 | MENU held at power-on | SKIPPED |  | needs the operator; --no-prompt |
| 11 | no computer, back to the application | SKIPPED |  | needs the operator; --no-prompt |
| 12 | menu entry and confirm screen | SKIPPED |  | needs the operator; --no-prompt |
| 13 | release .uf2 copy | PASS | 0.3 s | hello build release, fw v2.00.38, serial 1E0049000351343434393332; the OS reported: [Errno 5] Input/output error |
