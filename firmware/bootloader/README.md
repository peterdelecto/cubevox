# cubevox bootloader

TinyUF2 (Adafruit, MIT, on TinyUSB) for the STM32H7 port, with a cubevox board. It lives in bank 1
sector 0 (0x08000000, 128 KB) and starts the application at 0x08020000. In update mode the box is a
drive named `CUBEVOX`; dragging the release `.uf2` onto it updates the firmware. Design:
`docs/specs/2026-10-24-field-update-design.md`. Shared contract: `firmware/cubevox/src/board/boot_block.h`.
Run-or-update policy: `boards/cubevox/boot_decide.c` (pure C, covered by `ctest`).

## Pin
| Item | Value |
|---|---|
| TinyUF2 | adafruit/tinyuf2 `29d9ce231bada513a7200c087f10a91c832aec42` (0.35.0-145) |
| TinyUSB | submodule `lib/tinyusb` `ea1d867ced5259d49679f481e5d4e21a36dc11b0` (0.21.0) |
| STM32H7 HAL | `lib/mcu/st/stm32h7xx_hal_driver` `d8461b980b59b1625207d8c4f2ce0a9c2a7a3b04`, pinned by upstream `tools/get_deps.py` |
| CMSIS device H7 | `lib/mcu/st/cmsis_device_h7` `60dc2c913203dc8629dc233d4384dcc41c91e77f`, same |
| Compiler | `xpack-arm-none-eabi-gcc` 14.2.1-1.1, the one the Arduino core installs |

The commit and its version strings are `TINYUF2_COMMIT`, `TINYUF2_DESCRIBE` and `TINYUSB_DESCRIBE`
in `build.sh`. The compiler version is `XPACK_VERSION` in `firmware/build.sh`, which this build reads through
`firmware/build.sh --toolchain`.

## Build
```
./build.sh                # cubevox board: $OUT/cubevox-bootloader.{bin,elf}, OUT=/tmp/cubevox-boot-cubevox by default
./build.sh --outdir       # the output dir
./build.sh --proofs       # host build of screens.c: build/proofs/boot/NN-<state>.png and diag-sample.txt at the repo root
./build.sh --selftest     # host checks, listed below
./build.sh --help         # the usage
```
1. The checkout is `build/tinyuf2`, gitignored. A missing checkout is cloned; one off the pin is
   fetched and checked out; a checkout still off the pin stops the build.
2. Every build resets the checkout to the pin with `git reset --hard`, so edits made there are lost.
   Only `lib/tinyusb` is fetched as a submodule; `tools/get_deps.py stm32h7` fetches the HAL and the
   CMSIS device files. `lib/CMSIS_5` ships in the tree.
3. `boards/cubevox` is symlinked into `ports/stm32h7/boards/`, not copied. Any other link there,
   left by an older build for a retired board, is removed.
4. `patches/*.patch` apply in name order, each with `git apply --check` first.
5. `make BOARD=cubevox clean all` runs in `ports/stm32h7` with `CROSS_COMPILE` set to the compiler
   above and the pinned version strings in place of `git describe`. `board.mk` fixes the compile
   date and time, so two builds of one commit are the same bytes; the script prints the `.bin`'s
   SHA-256. A `.bin` over `CV_FLASH_SECTOR_SIZE` (boot_block.h, 131072) fails the build. The script
   ends with `arm-none-eabi-size` and the output dir.
6. The bootloader version is `CV_BOOT_VERSION` in `boards/cubevox/board.h`. Bump it with every
   bootloader change; it is independent of `VERSION.txt`.
7. `--selftest` holds `image_check.c` against `tools/stamp_image.py` (a later header version
   included), the screens against the approved wording and rows (`tools/proof.c --check`), the drive
   texts of seven sample boxes (`tools/drivetext.c`), flash.sh's option byte edit
   (`tools/option_bytes.py selftest`), and runs upstream's `ports/test_ghostfat` selftest on the
   patched tree for boards 4k, 4k_favicon, 32k and 512b_favicon. The policy in `boot_decide.c`,
   the decision, the fault record, the option byte rule, the UF2 filters and the screen choice, is
   covered by `ctest` (`test/firmware/boot_decide.cpp`).

## Patches
Each is small, has one purpose and a one-line reason at its top, and is written to go upstream.

| Patch | Change |
|---|---|
| `01-linker-board-layout` | `BOARD_NOINIT_ORIGIN`/`_SIZE` place NOINIT; `BOARD_PFLASH_APP_END` ends the app flash; the two temporary boot words are `PROVIDE`d so a board can define them |
| `02-ghostfat-optional-index-current` | `TINYUF2_INDEX_HTM` and `TINYUF2_CURRENT_UF2` (default 1) drop either file |
| `03-ghostfat-board-files` | `TINYUF2_BOARD_INFO_UF2` serves the board's INFO_UF2.TXT; `TINYUF2_BOARD_FILES` adds fixed-size files |
| `04-ghostfat-block-filter` | `TINYUF2_BOARD_BLOCK_FILTER`: `board_uf2_block_accept()` sees each block and the transfer it would join; a refused block is neither written nor counted toward the end of the transfer |
| `05-main-dfu-task-hook` | weak `board_dfu_task()` on every pass of the DFU loop |
| `06-boards-weak-dfu-complete` | `board_dfu_complete()` is weak, so a board records the update before the reset |
| `07-boards-dbl-tap-default` | the port's `TINYUF2_DBL_TAP_DFU 1` becomes a default a board can turn off |
| `08-msc-write-state-reset-on-mount` | `tud_mount_cb` calls `uf2_write_state_reset()`, which clears the UF2 block count and calls `board_flash_init()` to forget the erased sectors, so each enumeration starts a clean copy |
| `09-ghostfat-failed-write-not-counted` | a block `board_flash_write()` refuses does not count toward the end of the transfer; a board with the filter hears the result through `board_uf2_block_written()` |
| `10-ghostfat-root-dir-hook` | `TINYUF2_BOARD_DIR_HOOK`: every root directory sector the host writes reaches `board_uf2_dir_written()` |
| `11-stm32h7-systick-1ms` | SysTick stays at the 1 ms the HAL's tick needs; the board timer divides it for the LED pattern |

With every define at its default the drive is byte-identical to upstream: the upstream
`ports/test_ghostfat` selftest passes on the patched tree (boards 4k, 4k_favicon, 32k, 512b_favicon),
and `./build.sh --selftest` runs it.

## Board
| Item | Value |
|---|---|
| Hardware | cubevox v4, STM32H743VIT6 |
| LED | PB7 `USER_LED` |
| OLED | SH1106 on I2C2 (PB10/PB11) |
| Cable | VBUS_SENSE on PA9 (R6/R7 divider), a plain input debounced 50 ms |
| Double tap | off: the sealed box has no reachable reset button |
| UF2 family | `CV_UF2_FAMILY_CUBEVOX` |
| Volume label | `CUBEVOX` |
| Board-ID | `cubevox-h743-v4` |

25 MHz crystal, USB OTG_FS on PA11/PA12 with the B-session override, VID:PID
`CV_USB_VID_BOOT`:`CV_USB_PID_BOOT` (1209:0001), manufacturer `cubevox`, product `cubevox update`,
serial the 96-bit UID as 24 uppercase hex characters, bytes in address order (the firmware's
`deviceSerial()` prints the same text). MENU on PE13 with a pull-up, sampled 20 ms after start.

## Boot
1. `clock.c` mirrors `firmware/cubevox/src/board/sysclock.cpp` with its 360 MHz profile on every
   silicon revision: PLL1 from HSE (M 5, N 144, P 2, Q 15, R 2), VOS1, HCLK 180 MHz, flash latency 2,
   USB 48 MHz from PLL1Q. When a step fails (a missing crystal, an unlocked PLL1) the core goes back
   to HSI at 64 MHz and USB runs from HSI48, which CRS trims to the host's start-of-frame packets.
   The decision then stays in update mode with `ClockFailed`, so the screen says so and DIAG.TXT
   names the step.
2. `board_init2` reads RCC_RSR without clearing it and adopts the boot block (`cvBootAdopt`). A pure
   power-on (PORRSTF or BORRSTF, and none of SFTRSTF, IWDG1RSTF, WWDG1RSTF) clears the block whole,
   so power cycles never count as failed starts. The bootloader's own resets keep the power-on flags
   until the application clears them, and SFTRSTF tells those apart.
3. Next the option bytes: BOR level 3 and write protection of bank 1 sector 0. When either is
   missing they are programmed in one change (unlock, OPTSR_PRG and WPSN_PRG1, OPTSTART, a bounded
   wait on OPT_BUSY, lock), once per power-on. The result is `optionBytes` in the block and a line
   in DIAG.TXT; a failure waits for the next power-on.
4. The decision runs before upstream's `check_dfu_mode` reads any flash. A pending request may
   name an unreadable word, so the image is read only after `cvBootDecide` has erased that word's
   sector, and not at all when a read fault named no sector. `board_app_valid2` returns the decision.
5. The image check is the header (magic, a header version from 1 up, length in the region), the
   CRC-32 over the image with its CRC field zero, then upstream's stack and reset-vector check, so
   upstream never disagrees.
6. Before the jump the bootloader starts IWDG1 at 16 s (`CV_WATCHDOG_BOOT_MS`). The application
   shortens it to 2 s at the end of `setup()`; a hang before that resets and counts as a failed start.
   Update mode never runs the watchdog.
7. NMI, HardFault, MemManage, BusFault and UsageFault reset the stack to the top of RAM, keep the
   stacked pc and CFSR, HFSR and BFAR in the block, and call `cvBootRecordFault`, then reset. A double
   ECC error in bank 1, or any fault while the image is read, sets the resume request with
   `ImageUnreadable`, and the next start erases that word's sector. A bank 2 address can only come
   from reading the log: it is kept for DIAG.TXT, which then skips the log, and update mode resumes
   with its reason. Any other fault resumes update mode with `BootFault`, which times out to a valid
   image; a second one inside that update mode lets the next start decide afresh.
8. NOINIT is the boot block, 0xC0 bytes at 0x2001FF00, so `_board_dfu_dbl_tap` is its `request` word.
   The fault record above it at 0x2001FFC0 is only read.

## Update mode
1. Before USB starts, every flash word of the application region is read once, so a word a power
   cut tore anywhere in the region faults now and its sector is erased at the next start, instead of
   halfway through a copy.
2. A UF2 block is written only with this board's family, inside [0x08020000, 0x08100000), in whole
   32-byte flash words. Anything else is refused and counted; the OLED shows the wrong-file screen and
   DIAG.TXT the count and the last family. A wrong file never completes, so the box stays in update
   mode. A refusal after accepted blocks in the same host session, such as a file larger than the
   region, shows "The update stopped." instead.
3. A block the flash fails to take is not counted (patch 09), so the transfer cannot complete over
   it; the screen shows "The update stopped." and DIAG.TXT counts it with the first failing address.
4. A second file in one host session restarts the transfer (`uf2_write_state_reset()`): a block of
   another block count, or one landing on other data in a sector the transfer already erased. A copy
   cancelled on the host with the cable in place therefore does not stop the next one. After a
   failed write or a refusal, the file's first block arriving again restarts the transfer too, so
   every sector is erased afresh and the same file dropped again gets a clean try.
5. A root directory entry for a raw image, a hex or elf file or an archive (BIN, HEX, ELF, DFU, ZIP,
   GZ, TGZ, TAR, 7Z) shows the wrong-file screen and goes into DIAG.TXT by its 8.3 name. Finder's own
   files never carry these extensions. Such a file has no UF2 blocks, so nothing else would notice it.
6. After the last block the screen shows DONE, `cvBootMarkUpdated` clears the counter and the reason,
   the D-cache is cleaned and the box resets.
7. The D+ pull-up follows the cable on VBUS_SENSE. A pull after a host had connected posts the unplug
   TinyUSB's own VBUS sensing would, so the host session ends; with nothing written the 60 s timeout
   runs again from the pull. With a write begun, or no image to return to, the screen says the cable
   came out.
8. With reason MenuHeld, AppRequest or BootFault, a valid image, no host enumerated since the cable
   went in and no block written, update mode resets after 60 s; the request is consumed, so the
   decision then runs the application.
9. Screens follow the spec's table (`boards/cubevox/screens.c`), chosen by `cvUpdateScreen` in
   `boot_decide.c`. Every line is a full sentence in the 5x7 font, a screen holds four lines of 21
   characters on rows 0, 2, 4 and 6, and the file extension never appears. `./build.sh --proofs`
   renders them and `./build.sh --selftest` checks the wording and the rows.
10. Every enumeration clears the UF2 block count and the flash driver's record of erased sectors
    (patch 08), and rebuilds both text files.

## The drive
Both files have a fixed size, space padded and ending in CR LF, so the directory never changes while
their text does.

| File | Content |
|---|---|
| `INFO_UF2.TXT` | 512 bytes: bootloader version, Model, Board-ID, Serial, Installed (`v2.00.10`, `none` or `invalid`), TinyUF2 and TinyUSB versions with their MIT notices |
| `DIAG.TXT` | 4096 bytes: bootloader version, board, serial, installed image, reason, fault address, the bootloader's own fault registers, the clock, the option bytes, app summary, the fault record as `faultReport` prints it, blocks written, refused and failed, a wrong file by name, then the `flash log:` lines |

Both are rebuilt at every mount; once a write has begun the image is inspected again first, so
`Installed` says what the flash holds. DIAG.TXT is also rebuilt, at most every 250 ms, after a
refused block, a failed write or a wrong file. Each rewrite reads the log again, which takes a few
milliseconds.

## Flash log in DIAG.TXT
The application appends 256-byte entries to bank 2 sector 2 and the bootloader only reads them
(contract: `boot_block.h`, section Diagnostic log). `cvDiagLogLines` in `diag.c` classifies all 512
slots. A slot is blank when every byte is 0xFF, valid when its magic and its trailing CRC-32 hold
(`cvCrc32`, the image check's), and torn otherwise.

| Text | Meaning |
|---|---|
| `#12 boot v2.00.10 up=? rst=0x4 fails=3 BootFailed fa=0x0 watchdog bus sai=0 ovr=0` | One valid entry. Fields in order: sequence, kind (`boot` or `update`), firmware, uptime in seconds, reset flags in hex, boot fails, last reason, fault address in hex, fault text, SAI errors, overruns. `?` marks a field the application did not know. |
| `... sai=0 ovr=3 ob=failed, timeout` | The option byte step failed at that boot; the entry's `optionBytes` holds the result. Absent when the step succeeded or the entry does not carry it. |
| `flash log: 3 valid, 1 torn, 508 blank` | The counts over all slots, after the entries. |
| `flash log: empty` | No valid entry. The counts line follows only when a slot is torn. |
| `flash log: unreadable at 0x8141230` | A read fault hit bank 2, which the bootloader reads only for the log, so the sector is not read at all. |

1. The newest 16 valid entries come first. The application only appends and erases the whole
   sector, so the highest slot is the newest.
2. A line that does not fit whole in the 4096 bytes is left out with every older line. Typical lines
   are 85 to 91 bytes with the line break. The longest, with every field at its widest, a 63
   character fault text and a failed option byte step, are 201 bytes, and 16 of those still fit
   under a header with every register and count at full width (`DIAG-crowded.TXT` in the selftest).
3. Text fields print printable bytes only, anything else as `?`.
4. `./build.sh --proofs` writes `build/proofs/boot/diag-sample.txt`, the sample box with three valid
   entries, one torn entry and blank slots. `./build.sh --selftest` checks it and six variants.

## Flash (shop only)
1. Enclosure open, 9 V on. Hold BOOT0 down for the whole run, tap NRST.
2. `./flash.sh` waits for 0483:df11. It reads the option bytes through the ROM DFU's option byte
   area (`tools/option_bytes.py`); when a bootloader has write protected sector 0, it clears that one
   bit, because the ROM DFU skips a protected sector without an error. The chip restarts after an
   option byte write and BOOT0 brings it back into the ROM DFU.
3. It writes `cubevox-bootloader.bin` at 0x08000000, reads the sector back, compares it with the file,
   and starts the bootloader. A file over `CV_FLASH_SECTOR_SIZE` (131072 bytes) is refused; a
   read-back that differs is an [ERROR]. `./flash.sh --help` prints the usage.
4. Let go of BOOT0 when flash.sh says so. Expected within 3 s: OLED `The update stopped.` /
   `Drop the firmware on` / `the CUBEVOX drive.`, drive `CUBEVOX`, `INFO_UF2.TXT` says
   `Installed: none`. The new bootloader sets BOR level 3 and protects its sector at that start;
   DIAG.TXT says `option bytes: programmed, BOR level 3, sector 0 protected`.

## Only hardware can verify
1. USB enumeration as 1209:0001 and the `CUBEVOX` drive mounting on macOS.
2. A UF2 write, the lazy sector erase, the reset into the new image.
3. The read-fault path: a power cut mid-copy, the ECC fault, the sector erase, `ImageUnreadable`.
4. The OLED driver: I2C timing at 400 kHz, the init sequence, orientation against the firmware.
5. The MENU switch at power-on, the 60 s timeout, the LED.
6. Real log entries listed after a fault reboot, and the `unreadable` line after a double ECC error in
   bank 2 (one reset, then the sector is skipped).
7. A different file copied after a cable pulled mid-copy, and after a copy cancelled in Finder, with
   no power cycle between.
8. The cable on PA9: pulled mid-copy, the cable screen and the drive back on replug; pulled before a
   copy, the 60 s timeout from the pull.
9. The option byte step on a fresh part (BOR_LEV 0, sector 0 writable) and flash.sh's clearing of
   the protection through the ROM DFU.
10. A missing crystal: the clock screen, the drive on HSI48 with CRS, DIAG.TXT's clock line.
11. The 16 s watchdog before the jump and the application's 2 s after `setup()`.

## Licences
cubevox is MIT. The bootloader binary contains TinyUF2 and TinyUSB under these notices:

```
MIT License

Copyright (c) 2020 Ha Thach

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

```
The MIT License (MIT)

Copyright (c) 2012-2026, Ha Thach (tinyusb.org)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

The binary also links STMicroelectronics' STM32H7 HAL (BSD 3-Clause, `lib/mcu/st/stm32h7xx_hal_driver/License.md`)
and the CMSIS device and core files (Apache 2.0, `lib/mcu/st/cmsis_device_h7`, `lib/CMSIS_5`).
