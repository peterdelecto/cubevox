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

The commit and the compiler version are `TINYUF2_COMMIT` and `XPACK_VERSION` in `build.sh`.

## Build
```
./build.sh                # cubevox board: /tmp/cubevox-boot-cubevox/cubevox-bootloader.{bin,elf}
BOARD=weact ./build.sh    # WeAct H743 test board: /tmp/cubevox-boot-weact/
./build.sh --outdir       # the output dir for the current BOARD
./build.sh --proofs       # host build of screens.c: build/proofs/boot/NN-<state>.png at the repo root
./build.sh --selftest     # host run of the image check against tools/stamp_image.py, and the drive texts
```
1. The checkout is `build/tinyuf2`, gitignored. A missing checkout is cloned; one off the pin is
   fetched and checked out; a checkout still off the pin stops the build.
2. Every build resets the checkout to the pin with `git reset --hard`, so edits made there are lost.
   Only `lib/tinyusb` is fetched as a submodule; `tools/get_deps.py stm32h7` fetches the HAL and the
   CMSIS device files. `lib/CMSIS_5` ships in the tree.
3. `boards/cubevox` and `boards/weact` are symlinked into `ports/stm32h7/boards/`, not copied.
4. `patches/*.patch` apply in name order, each with `git apply --check` first.
5. `make BOARD=<board> clean all` runs in `ports/stm32h7` with `CROSS_COMPILE` set to the compiler
   above. A `.bin` over 131072 bytes fails the build. The script ends with `arm-none-eabi-size` and
   the output dir.
6. The bootloader version is `CV_BOOT_VERSION` in `boards/cubevox/board.h`. Bump it with every
   bootloader change; it is independent of `VERSION.txt`.

## Patches
Each is small, has one purpose and a one-line reason at its top, and is written to go upstream.

| Patch | Change |
|---|---|
| `01-linker-board-layout` | `BOARD_NOINIT_ORIGIN`/`_SIZE` place NOINIT; `BOARD_PFLASH_APP_END` ends the app flash; the two temporary boot words are `PROVIDE`d so a board can define them |
| `02-ghostfat-optional-index-current` | `TINYUF2_INDEX_HTM` and `TINYUF2_CURRENT_UF2` (default 1) drop either file |
| `03-ghostfat-board-files` | `TINYUF2_BOARD_INFO_UF2` serves the board's INFO_UF2.TXT; `TINYUF2_BOARD_FILES` adds fixed-size files |
| `04-ghostfat-block-filter` | `TINYUF2_BOARD_BLOCK_FILTER`: a block the board refuses is neither written nor counted toward the end of the transfer |
| `05-main-dfu-task-hook` | weak `board_dfu_task()` on every pass of the DFU loop |
| `06-boards-weak-dfu-complete` | `board_dfu_complete()` is weak, so a board records the update before the reset |
| `07-boards-dbl-tap-default` | the port's `TINYUF2_DBL_TAP_DFU 1` becomes a default a board can turn off |

With every define at its default the drive is byte-identical to upstream: the upstream
`ports/test_ghostfat` selftest passes on the patched tree (boards 4k, 4k_favicon, 32k, 512b_favicon).

## Boards
| | cubevox | weact |
|---|---|---|
| Hardware | cubevox v4, STM32H743VIT6 | WeAct MiniSTM32H743, test only |
| LED | PB7 `USER_LED` | PE3 |
| OLED | SH1106 on I2C2 (PB10/PB11) | none |
| Double tap | off: the sealed box has no reachable reset button | on |
| UF2 family | `CV_UF2_FAMILY_CUBEVOX` | `CV_UF2_FAMILY_WEACT` |
| Volume label | `CUBEVOX` | `WEACTBOOT` |
| Board-ID | `cubevox-h743-v4` | `cubevox-weact-h743` |

Both: 25 MHz crystal, USB OTG_FS on PA11/PA12 with the B-session override, VID:PID
`CV_USB_VID_BOOT`:`CV_USB_PID_BOOT` (1209:0001), manufacturer `cubevox`, product `cubevox update`,
serial the 96-bit UID as 24 uppercase hex characters, bytes in address order (the firmware's
`deviceSerial()` prints the same text). MENU on PE13 with a pull-up, sampled 20 ms after start.

## Boot
1. `clock.c` mirrors `firmware/cubevox/src/board/sysclock.cpp` with its 360 MHz profile on every
   silicon revision: PLL1 from HSE (M 5, N 144, P 2, Q 15, R 2), VOS1, HCLK 180 MHz, flash latency 2,
   USB 48 MHz from PLL1Q, HSI48 off.
2. `board_init2` decides before upstream's `check_dfu_mode` reads any flash. A pending request may
   name an unreadable word, so the image is read only after `cvBootDecide` has erased that word's
   sector. `board_app_valid2` returns the decision.
3. The image check is the header (magic, version, length in the region), the CRC-32 over the image
   with its CRC field zero, then upstream's stack and reset-vector check, so upstream never disagrees.
4. NMI, HardFault, MemManage, BusFault and UsageFault reset the stack to the top of RAM, decode a
   double ECC error (bank 1: the word's address; bank 2: the log's address, nothing erased), call
   `cvBootRecordReadFault` and reset.
5. NOINIT is the boot block, 0xC0 bytes at 0x2001FF00, so `_board_dfu_dbl_tap` is its `request` word.
   The fault record above it at 0x2001FFC0 is only read.

## Update mode
1. A UF2 block is written only with this board's family, inside [0x08020000, 0x08100000), in whole
   32-byte flash words. Anything else is refused and counted; the OLED shows WRONG FILE and DIAG.TXT
   the count and the last family. A wrong file never completes, so the box stays in update mode.
2. After the last block the screen shows DONE, `cvBootMarkUpdated` clears the counter and the reason,
   the D-cache is cleaned and the box resets.
3. With reason MenuHeld or AppRequest, a valid image, no host enumerated and no block written, update
   mode resets after 60 s; the request is consumed, so the decision then runs the application.
4. Screens follow the spec's table (`boards/cubevox/screens.c`); `./build.sh --proofs` renders them.

## The drive
| File | Content |
|---|---|
| `INFO_UF2.TXT` | bootloader version, Model, Board-ID, Serial, Installed (`v2.00.10`, `none` or `invalid`), TinyUF2 and TinyUSB versions with their MIT notices |
| `DIAG.TXT` | 1024 bytes, space padded: bootloader version, board, serial, reason, fault address, app summary, the fault record as `faultReport` prints it, refused blocks and the last refused family, `flash log: not in this build` |

DIAG.TXT is rewritten at every mount and whenever a block is refused.

## Flash (shop only)
1. Enclosure open, 9 V on. Hold BOOT0, tap NRST, release BOOT0.
2. `./flash.sh` waits for 0483:df11 and runs `dfu-util -a 0 -s 0x08000000:leave -D cubevox-bootloader.bin`.
   It refuses a file over 131072 bytes.
3. Expected within 3 s: OLED `UPDATE NEEDED`, drive `CUBEVOX`, `INFO_UF2.TXT` says `Installed: none`.

## Only hardware can verify
1. USB enumeration as 1209:0001 and the `CUBEVOX` drive mounting on macOS.
2. A UF2 write, the lazy sector erase, the reset into the new image.
3. The read-fault path: a power cut mid-copy, the ECC fault, the sector erase, `ImageUnreadable`.
4. The OLED driver: I2C timing at 400 kHz, the init sequence, orientation against the firmware.
5. The MENU switch at power-on, the 60 s timeout, the LED, the WeAct double tap.

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
