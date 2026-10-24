# Field firmware update — design

Owner decisions 2026-10-24. The Mac app is parked. The box ships to Adam with the default chain
and he updates its firmware himself over USB-C with nothing to install. cubevox is open hardware
and open source. A handful of hand-built units will exist. The OLED tells Adam what the box is
doing in update mode. A diagnostics file on the update drive is how he sends us what went wrong.
Firmware is not signed. Adam will later keep his own data on the box, so flash is reserved for it.
When the Mac app returns, one click in it also updates the firmware by copying the same file to the
drive. Going back to an older firmware is dragging the older release file; the box keeps one image.
The repository licence is MIT.

This spec supersedes the ROM-DFU items of `2026-10-02-hardware-design.md` (lines 151 and 180) and
the box side of the Update section of `2026-10-15-mac-app-design.md`. When the app returns it
copies the same `.uf2` file to the same drive. The firmware structure rules of
`2026-10-12-firmware-structure-design.md` apply to every new file here.

## What Adam does
1. Downloads `cubevox-vX.YY.ZZ.uf2` from the release page. One file.
2. With the box powered on as usual, plugs USB-C into the Mac. Turns MENU to `Update firmware`,
   clicks, reads the version he has on the confirm screen (`You have vX.YY.ZZ.`), clicks again.
3. The OLED says `Drop the firmware on / the CUBEVOX drive.` A drive named `CUBEVOX` appears on
   the desktop. He drags the file onto it.
4. The OLED says `Writing the firmware. / Do not unplug.`, then `The update is done. / Restarting
   now.` The drive goes away by itself, the box restarts, the boot screen shows the new version,
   and the layout and settings are as before.
5. If the copy is interrupted, the box comes back in update mode on its own, says why, and he drags
   again. If the box ever fails to start three times in a row it also comes back in update mode.
   Holding MENU while switching the power on forces update mode. No action from outside the box
   can leave it without a way back.

## Why a bootloader
1. The ROM DFU needs dfu-util on the host and a BOOT0 press to recover from a bad image. BOOT0 is
   inside the sealed enclosure. Neither is a path Adam can take.
2. A bootloader in its own flash sector never changes in the field. Whatever state the application
   is in, the update path still works, and the OLED still explains it.
3. UF2 is the file format that survives a drag onto a Finder drive. Every 512-byte block names its
   own target address and a family id, so Finder's dot files, out-of-order writes and a wrong file
   are all ignored instead of written.
4. TinyUF2 (Adafruit, MIT, on TinyUSB) has an STM32H7 port whose Nucleo-H743 board runs the
   application from internal flash at 0x08020000. We add a cubevox board to that port, keep the
   upstream commit pinned, and keep our additions as small, upstreamable patches.

## Principles
1. One definition per fact. Addresses, magics, the family id and the header layout live in
   `firmware/cubevox/src/board/boot_block.h`; the bootloader board and the Python tools read that
   file. Fonts come from `tools/font-editor/fonts/cubevox_fonts.h`. The version comes from
   `VERSION.txt`. The toolchain is the one the Arduino core already installs.
2. Every refusal is visible. A rejected block, a wrong file, a failed flash write, an invalid image,
   a read fault, a failed boot and a failed clock show on the OLED and in `DIAG.TXT`. Nothing is
   swallowed.
3. No path leaves the box unbootable. Power loss at any instant, a wrong file, a corrupt file, an
   image that crashes or hangs, a dead menu, all end in update mode with instructions on the screen.
4. The bootloader is frozen and minimal. It owns the first 128 KB and the update drive, nothing
   else. Besides those it sets two option bytes once (section Option bytes) and arms the watchdog
   for the jump (section Boot sequence), because only code that runs before the application can.
   Features that can live in the application live there.
5. The decision logic is pure C with no HAL, so the host test suite exercises every branch of
   "run the app or stay in update mode" without a board.

## Flash map
| Region | Address | Size | Content |
|---|---|---|---|
| Bank 1 sector 0 | 0x08000000 | 128 KB | TinyUF2 bootloader, cubevox board |
| Bank 1 sectors 1–7 | 0x08020000 | 896 KB | Application image, header at +0x400 |
| Bank 2 sectors 0–1 | 0x08100000 | 256 KB | Layout record, `2026-10-12-layout-record-design.md`, unchanged |
| Bank 2 sector 2 | 0x08140000 | 128 KB | Diagnostic log (section Diagnostics) |
| Bank 2 sectors 3–7 | 0x08160000 | 640 KB | Reserved for Adam's data (presets, alternate layouts), its own spec later |

1. The bootloader writes only inside the application region. The cubevox board sets the writable
   bound to 0x08100000; a block addressed anywhere else is refused and reported. Upstream hard-codes
   2048K in `ports/stm32h7/linker/memory.ld`, so the board build carries a patch that takes the
   bound from the board, sent upstream.
2. The owned linker script asserts the image ends below 0x08100000 and `build.sh` fails when the
   `.bin` exceeds 917504 bytes. The linker guards the link, the script guards the shipped file.
3. Today's release image is about 180 KB. The bootloader is about 35 KB of its 128 KB sector; the
   sector is the erase unit, so nothing else may live there.
4. Bank 2 is never written by the bootloader. It reads sector 2 for `DIAG.TXT` only.
5. Bank 1 sector 0 is write protected by the bootloader's option byte step, so no application bug
   can erase it. `firmware/bootloader/flash.sh` clears the protection before a shop reflash.

## Shared boot block
Both programs agree on 256 bytes at the top of DTCM. `boot_block.h` defines the struct, the
addresses and the magics; the bootloader board includes the same header. DTCM survives a system
reset and is random after power-on, so every field is validated before use.

| Address | Field | Meaning |
|---|---|---|
| 0x2001FF00 | `request` | TinyUF2's double-tap register. `0xF01669EF` (`CV_REQUEST_UPDATE`): the application asks for update mode, then resets. `0x53525643` (`CV_REQUEST_RESUME`): the bootloader's fault handler returns to update mode for `lastReason`. Every decision clears it |
| 0x2001FF04 | `bootFails` | Jumps to the application with no healthy report since the last good run |
| 0x2001FF08 | `bootFailsCheck` | `~bootFails`. Disagreement means power-on garbage, read as 0 |
| 0x2001FF0C | `blockVersion` | 1. The bootloader's alone: names the bootloader fields that exist |
| 0x2001FF10 | `lastReason` | Why the bootloader last stayed in update mode (enum, section Boot sequence) |
| 0x2001FF14 | `faultAddress` | Flash address the bootloader faulted on while reading, else 0 |
| 0x2001FF18 | `appSummary[40]` | Written by the application before it requests update mode: version, uptime s, reset cause, last fault kind |
| 0x2001FF40 | `bootFaultPc`, `bootFaultCfsr`, `bootFaultHfsr`, `bootFaultBfar` | The bootloader's own last fault: stacked pc, CFSR, HFSR, BFAR. 0 when none since power-on or the last update |
| 0x2001FF50 | `optionBytes` | What the option byte step found or did since power-on (enum `CvOptionResult`) |
| 0x2001FF54 | `bootReserved` (44 B) | Zero. New bootloader fields |
| 0x2001FF80 | `appFieldsVersion` | 0 until the application adds a field. The bootloader never reads it |
| 0x2001FF84 | `appReserved` (60 B) | Zero. New application fields |
| 0x2001FFC0 | fault record (64 B) | `.cv_noinit`, `FAU3`, unchanged. The bootloader reads it for `DIAG.TXT` and never clears it |

1. The bootloader's NOINIT origin moves to 0x2001FF00 (board linker override, same patch set).
   The application's `.dtcm_bss` assert drops to `<= 0x2001FF00`.
2. Rule for the future. Existing offsets never move. A bootloader field takes `bootReserved` and
   bumps `blockVersion`; an application field takes `appReserved` and bumps `appFieldsVersion`.
   Readers accept every version from 1 up, because a newer writer never changes an older field.
   The bootloader in Adam's box is the oldest reader there will ever be.
3. Power-on rule. The bootloader reads `RCC->RSR` and never clears it; the application clears it
   for its boot report. A pure power-on (PORRSTF or BORRSTF, with none of SFTRSTF, IWDG1RSTF and
   WWDG1RSTF) clears the whole block, so power cycles and brown-outs never count as failed starts.
   The flags pile up until the application clears them, so a reset the bootloader makes after a
   power-on still shows PORRSTF, and SFTRSTF tells it apart. A block with `blockVersion` 0 is
   cleared too. Otherwise the bootloader leaves the application's area alone.

## Image header and UF2 family
A 32-byte header sits at application offset 0x400, after the 0x298-byte vector table.
`firmware/cubevox.ld`, the one owned linker script, places section `.cv_image_hdr` at that address
(`ALIGN(0x400)` after `.isr_vector`, with a linker assert on 0x08020400).

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `CVFW` |
| 4 | 4 | header version, 1 today; readers accept every version from 1 up |
| 8 | 4 | image length in bytes, the whole `.bin` |
| 12 | 4 | CRC-32 (zlib polynomial) over the whole image with this field as zero |
| 16 | 16 | version string from `VERSION.txt`, `v2.00.10` or `v2.00.10-bench`, NUL padded |

1. The compiler emits the header with length and CRC zero. `tools/stamp_image.py` fills both after
   the link, verifies them (`stamp_image.py check`), and writes the `.uf2`. It is the only UF2
   writer in the project; the format is 512-byte blocks with a 32-byte header and 256 bytes of
   payload, and vendoring `uf2conv.py` would add a second definition of the same constants.
2. The UF2 family id is cubevox's own, `CV_UF2_FAMILY_CUBEVOX` in `boot_block.h`, not the generic
   STM32H7 id. The board's block filter refuses every block whose family differs, before writing
   anything. One constant therefore rejects a wrong file and any other project's UF2, while the
   installed image stays intact.
3. The image check (`image_check.c`) takes magic, header version, length inside the region and the
   CRC. TinyUF2's own test is a stack-pointer and reset-vector sanity check that a half-written
   image passes; the header is what makes an incomplete image refuse to boot. The upstream weak
   hook `board_app_valid2` returns the boot decision, which uses the check.
4. The CRC runs in software from a table at the bootloader's 360 MHz. The hardware CRC unit is not
   used so the Python and C definitions stay one and the same.
5. The 32 header bytes never change their meaning. A later header puts new fields after them and
   bumps the header version. Every reader, the frozen bootloader and the application's own check
   in `image_header.cpp` alike, accepts every version from 1 up and reads the version 1 fields,
   because the CRC already covers every byte of the image. A reader that refused a later version
   would turn the first release that follows this rule into `The update stopped.` on every box.

## Boot sequence
1. Reset runs TinyUF2 from sector 0. Clocks come up from the 25 MHz crystal with the 360 MHz
   profile `board/sysclock.cpp` uses on older silicon: PLL1 (M 5, N 144, P 2, Q 15), VOS1, HCLK
   180 MHz, USB 48 MHz from PLL1Q. When the crystal or PLL1 fails, the core goes back to HSI at
   64 MHz and USB runs from HSI48, which CRS trims to the host's start-of-frame packets, so the
   drive and `DIAG.TXT` stay reachable.
2. `board_init2` reads `RCC->RSR` and adopts the block (power-on rule, section Shared boot block),
   then runs the option byte step (section Option bytes).
3. `cvBootDecide()` (pure C, `firmware/bootloader/boards/cubevox/boot_decide.c`, shared with the
   host tests) returns one of two actions from the block, the image check, the MENU switch and the
   clock. `board_app_valid2` hands the action to TinyUF2. The first matching row wins.

| Order | Condition | Action | `lastReason` |
|---|---|---|---|
| 1 | the crystal or PLL1 failed | update mode on HSI48 | `ClockFailed` |
| 2 | MENU switch held at reset (PE13 low after 20 ms) | update mode | `MenuHeld` |
| 3 | `request` = `0x53525643` after a read fault in the image | update mode, erase the faulting sector | `ImageUnreadable` (`ImageInvalid` when the fault named no sector) |
| 4 | `request` = `0x53525643` otherwise | update mode | the stored reason, `BootFault` when none |
| 5 | `request` = `0xF01669EF` | update mode | `AppRequest` |
| 6 | header or CRC invalid | update mode | `ImageInvalid` |
| 7 | `bootFails >= 3` | update mode | `BootFailed` |
| 8 | otherwise | `bootFails += 1`, write the check word, arm the watchdog, jump | — |

4. With a request pending the image is read only after the decision, because the request may name
   an unreadable word. The decision erases that word's sector first, and when the fault named no
   sector the image is not read at all.
5. The double-tap window is off. The box has no reset button Adam can reach, so the 500 ms wait
   would cost boot time for nothing. `TINYUF2_DBL_TAP_DFU` is 0; the register checks in rows 3 to 5
   stay.
6. Row 3 exists because H7 flash words carry ECC, and a word whose programming was cut by a power
   loss raises a fault when read. The bootloader installs its own fault handlers for NMI, HardFault,
   MemManage, BusFault and UsageFault. They reset the stack, keep the stacked pc, CFSR, HFSR and
   BFAR in the block, decode the failing address from `FLASH->ECC_FA1` or `ECC_FA2` when SR1 or SR2
   shows DBECCERR, call `cvBootRecordFault` and reset.
   1. A double ECC error in bank 1, or any fault while the image is read, sets the resume request
      with `ImageUnreadable` and the address. On the way back up the bootloader erases that sector
      before enumerating, so every later read of the region is clean.
   2. A bank 2 address can only come from reading the log for `DIAG.TXT`. The address is kept, so
      `DIAG.TXT` skips the log from then on, and update mode resumes with its reason. The
      application erases that sector at its next start.
   3. Any other fault resumes update mode with `BootFault`. A second one inside that update mode
      lets the next start decide afresh, so a fault in update mode never loops.
7. Jump. `HAL_RCC_DeInit`, caches off, `SCB->VTOR = 0x08020000`, MSP from the image, branch to the
   reset vector. The STM32duino core then sets VTOR again from `VECT_TAB_OFFSET`
   (`system_stm32h7xx.c:291`), so both sides agree without either trusting the other.
8. Watchdog. Right before the jump the bootloader starts IWDG1 at 16 s (`CV_WATCHDOG_BOOT_MS`). The
   application's `faultWatchdogStart` rewrites the prescaler and the reload to its own 2 s at the
   end of `setup()`; IWDG1 takes new values at any time once 0x5555 is in KR (RM0433). A hang in
   `setup()` therefore resets with IWDG1RSTF and counts as a failed start. A system reset stops
   IWDG1, so update mode never runs it: a stuck update mode is recovered by a power cycle, and a
   watchdog reset during a sector program would only manufacture the row 3 case.
9. Healthy. The application reports healthy once `setup()` has finished, the watchdog is running and
   the audio ISR has advanced blocks for 5 s. It then writes `bootFails = 0` with its check word. A
   fault, a watchdog reset or a hang inside those 5 s leaves the count standing. A crash after the
   window does not count, by design; a box that runs for minutes is a box Adam can still update
   from the menu, and a box that never reaches the menu is what the counter is for.
10. Update mode leaves on its own after 60 s when no USB host has enumerated and the image is valid
    and `lastReason` is `MenuHeld`, `AppRequest` or `BootFault`. That covers a confirm with no
    computer attached. A cable pulled before anything was written starts the 60 s again. Once a
    host has enumerated, or a write has begun, or the image is invalid, there is no timeout.
11. The mute relay K102 is open while the bootloader runs (MUTE_N idle low), so update mode is
    silent.

## Option bytes
1. Once per power-on, before the decision, the bootloader reads `FLASH->OPTSR_CUR` and
   `FLASH->WPSN_CUR1`. It wants BOR level 3 (2.7 V), as the hardware spec asks, so the core resets
   before a sagging 9 V can tear a flash write. It also wants bank 1 sector 0 write protected, so
   no application bug can erase the bootloader.
2. When either is missing it programs both in one change: unlock the option registers, clear
   OPTCHANGEERR, write `OPTSR_PRG` with BOR_LEV 3 and `WPSN_PRG1` with sector 0 protected, every
   other bit as it was, set OPTSTART, wait at most 1 s on OPT_BUSY, check OPTCHANGEERR, read the
   current registers back, lock.
3. The result goes to `optionBytes` in the block (`set`, `programmed`, or `failed` with the cause)
   and to a line in `DIAG.TXT`; the application's boot report prints it with BOR_LEV and the
   sector 0 protection it reads itself. The step marks itself interrupted before it programs, so a
   fault or reset mid-change reads as a failure, and a failure is not tried again until the next
   power-on.
4. The ROM DFU skips a protected sector without an error. `firmware/bootloader/flash.sh` therefore
   clears the protection through the ROM's option byte area before it writes, and the new
   bootloader sets it again at its first start.

## Update mode
1. Before USB starts, the bootloader reads every word of the application region once. A word a power
   cut tore anywhere in the region then faults now, and its sector is erased at the next start,
   instead of in the middle of the next copy.
2. USB OTG_FS on PA11/PA12 enumerates as a mass-storage device. TinyUSB's own VBUS sensing stays off
   (the box is self-powered; B-session override as in `usb_link.cpp`). The board reads VBUS_SENSE on
   PA9 through the R6/R7 divider, debounced 50 ms, holds the D+ pull-up only while the cable carries
   VBUS, and posts the unplug event on a pull, so a pulled cable ends the host session. The USB
   serial number is the 96-bit MCU UID in hex, so two boxes on one Mac are distinct.
3. The drive is `CUBEVOX`, FAT, read-only except for UF2 blocks. It holds two files of fixed size,
   built at every mount. Once a write has begun the image is checked again first, so `Installed`
   follows the flash.

| File | Content |
|---|---|
| `INFO_UF2.TXT` | 512 bytes. `cubevox bootloader vB.BB`, `Board-ID: cubevox-h743-v4`, `Serial: <uid>`, `Installed: v2.00.10` or `none` or `invalid`, TinyUF2 and TinyUSB versions and MIT notices |
| `DIAG.TXT` | 4096 bytes. Section Diagnostics |

   `CURRENT.UF2` and `INDEX.HTM` are not served. Reading back 896 KB of flash to produce
   `CURRENT.UF2` is the one path that could touch an unreadable word Finder would then trip over,
   and the OLED replaces the HTML page. Both are dropped by a board define, patched and sent
   upstream.
4. Writes. Each UF2 block passes the board's filter first (patch 04): this family, inside
   [0x08020000, 0x08100000), whole 32-byte flash words. A refused block is counted and never
   written. A sector is erased before its first block. A block the flash fails to take is counted
   as failed and never toward the end of the transfer (patch 09), so the update cannot complete over
   it. After the last block, `bootFails` is zeroed, `lastReason` cleared, the D-cache cleaned, and
   the box resets with no wait.
5. A second file in one host session restarts the transfer: a block with another block count, or
   one landing on other data in a sector the transfer already erased. A copy cancelled in Finder
   therefore never stops the next one. After a failed write or a refusal, the file's first block
   arriving again restarts the transfer too, so the same file dropped again gets freshly erased
   sectors.
6. A file that is not UF2 has no blocks for the filter to see. The board reads each root directory
   sector the host writes (patch 10) and treats a regular file with the extension BIN, HEX, ELF, DFU,
   ZIP, GZ, TGZ, TAR or 7Z as a wrong file. The wrong-file screen shows, and `DIAG.TXT` names the
   file by its 8.3 name. Finder's own files never carry these extensions.
7. Finder's `.fseventsd`, `.Spotlight-V100`, `._*` files and the "disk not ejected properly" notice
   after the reset are expected and harmless.

## OLED in update mode
The bootloader drives the SH1106 over I2C2 (PB10/PB11) with its own driver, about 150 lines: the
init sequence and a full-frame write. U8g2 is not linked. Screens are drawn at run time by
`screens.c` with the 5x7 font from `cubevox_fonts.h`; the same file compiles on the host and renders
the numbered proof PNGs (`build.sh --proofs`), so the proofs show the shipped pixels.

Owner rules for these screens (2026-10-24): every line is a full sentence with a period, no bare
titles; prose in the 5x7 font sits on alternate rows (0, 2, 4, 6), so a screen holds at most four
lines of 21 characters; the extension `uf2` never appears on screen because the font blurs it.

| State | Rows 0 / 2 / 4 / 6 |
|---|---|
| State | Rows 0 / 2 / 4 / 6 |
|---|---|
| Waiting, from the menu, the app or after a bootloader fault, valid image | `Drop the firmware on` / `the CUBEVOX drive.` / `You have vX.YY.ZZ.` / `Wait 60 s to cancel.` |
| Waiting, no valid image; a failed write or a refusal after accepted blocks | `The update stopped.` / `Drop the firmware on` / `the CUBEVOX drive.` / (blank) |
| Waiting, three failed boots | `Startup failed.` / `Drop the firmware on` / `the CUBEVOX drive.` / (blank) |
| Host connected | row 6 becomes `Computer connected.` on every waiting screen |
| Writing | `Writing the firmware.` / `Do not unplug.` |
| Wrong file: refused blocks or a non-UF2 file | `That is not a cubevox` / `firmware file.` / `Drop the firmware on` / `the CUBEVOX drive.` |
| Cable pulled after a host connected, when the 60 s timeout cannot run | `The cable came out.` / `Plug it back in and` / `drop the firmware on` / `the CUBEVOX drive.` |
| Clock failed | `The clock failed.` / `Send the DIAG.TXT` / `file on the CUBEVOX` / `drive for a repair.` |
| Done | `The update is done.` / `Restarting now.` |

1. The screens get a numbered proof set and the owner approves per number, as for every OLED
   screen. The wording above is the approved source; the code matches it. The cable and clock
   screens (proofs 08 and 09) await approval.
2. The application's confirm screen is `Update the firmware?` / `You have vX.YY.ZZ.` /
   `Click to start.` / `Turn to cancel.` on rows 0, 2, 4, 6. Same proof rule.
3. The application's boot splash shows the version for 2 s so Adam can confirm an update without a
   computer.

## Failure handling
| Event | Box | Adam sees | `DIAG.TXT` records |
|---|---|---|---|
| Power loss mid-copy, no half-programmed word | CRC fails at next boot, update mode | `The update stopped.`, drive | `reason ImageInvalid` |
| Power loss mid-copy, half-programmed word | read fault, sector erased, update mode | same | `reason ImageUnreadable, address` |
| USB pulled mid-copy | host session ends, box waits | `The cable came out.`, drive returns when replugged | nothing; the next copy starts afresh |
| USB pulled before a copy | the 60 s timeout starts again from the pull | the waiting screen, then normal | `reason AppRequest` or `MenuHeld` |
| Wrong UF2 file dragged | every block refused, image intact | `That is not a cubevox firmware file.` | `blocks: 0 written, N refused, 0 failed, last family X` |
| Non-UF2 file dragged (`.bin`, `.zip`) | nothing written, image intact | `That is not a cubevox firmware file.` | `wrong file: NAME.BIN` |
| Flash write fails | not counted, the update cannot complete | `The update stopped.`; dropping the file again retries on erased sectors | `blocks: ... N failed, first failure at X` |
| Second file, or a copy cancelled in Finder | the transfer restarts | `Writing the firmware.` | the block counts |
| Corrupt download | CRC fails, update mode | `The update stopped.` | `reason ImageInvalid` |
| New firmware crashes at start | 3 resets, update mode | `Startup failed.` | `reason BootFailed`, fault record |
| New firmware hangs in `setup()` | the 16 s watchdog resets, 3 of those, update mode | `Startup failed.` | `reason BootFailed` |
| New firmware works but sounds wrong | — | — | Adam drags the previous release file; every release stays downloadable |
| Menu dead, app otherwise fine | hold MENU at power-on | `Drop the firmware on the CUBEVOX drive.` | `reason MenuHeld` |
| Confirm with no computer | 60 s, back to the app | `Drop the firmware on the CUBEVOX drive.`, then normal | `reason AppRequest, timeout` |
| 9 V dips or flickers | BOR resets at 2.7 V; a power-on clears the start count | normal start | nothing |
| Bootloader faults | reset, update mode again for the same reason; a second fault there runs a valid image | the waiting screen | `reason BootFault`, `boot fault: pc cfsr hfsr bfar` |
| Crystal or PLL1 fails | HSI, USB on HSI48 with CRS, update mode | `The clock failed.` | `reason ClockFailed`, `clock: failed in <step>` |
| Bootloader hangs | power cycle | screen frozen, then normal | nothing |
| Bootloader itself damaged | shop only | box dark | BOOT0 inside the enclosure, `firmware/bootloader/flash.sh` |

## Diagnostics
1. `DIAG.TXT` is 4096 bytes, built when the drive mounts and again, at most every 250 ms, after a
   refused block, a failed write or a wrong file. It lists the bootloader version, board and
   serial, the installed image, `lastReason` and `faultAddress`, the bootloader's own fault
   registers, the clock (crystal, or the step that failed), the option bytes (the step's result,
   BOR level, sector 0 protection), the application's `appSummary`, the DTCM fault record if its
   magic is valid (kind, pc, lr, cfsr, hfsr, bfar, detail, formatted as `faultReport` prints them
   over serial), the blocks written, refused and failed in this session with the last refused
   family and the first failing address, a wrong file by name, and the newest 16 entries of the
   diagnostic log in flash.
2. The diagnostic log is bank 2 sector 2 (sector index 2 in `board/flash_bank2`), 256-byte entries
   with their own magic (`CVDG`), append-only, erased when full. `core/entry_log` writes it over
   the flash steps it shares with `record_store` (`core/flash_io`). It follows the layout record's
   non-blocking rules, one flash operation per `loop()` pass, and the sequence keeps counting
   across erases. The two share one flash controller, so each waits while the other erases.
   1. The application queues one entry at boot whenever the reset cause is not power-on alone or a
      fault record is present, and one entry when it requests update mode.
   2. An entry carries the firmware version, the uptime (unknown at boot), the reset flags, the
      fault summary, the boot-fail count, the bootloader's last reason and fault address, the
      option byte step's result, and the SAI and overrun counters of the run. `DIAG.TXT` shows the
      option byte result only when the step failed.
   3. `enterDfu()` queues its entry first and waits up to 3 s for it. An erase the entry needs does
      not count against that wait, because a reset would leave the sector half erased. A timeout
      prints a warning and the update goes ahead.
3. The bootloader reads the log and never writes it. A read fault anywhere in bank 2, where the
   bootloader reads nothing but the log, marks the log `unreadable` in `DIAG.TXT` and leaves
   recovery to the application. At boot, before any scan,
   the application erases the bank 2 sector that its own fault record or `faultAddress` in the
   boot block names. After a successful erase it clears `faultAddress`, so the bootloader reads
   the log again. The boot entry still records the address the bootloader left.
4. Adam's support step is one sentence, told to him: open the `CUBEVOX` drive, send `DIAG.TXT`.

## Application changes
| File | Change |
|---|---|
| `firmware/build.sh` | `--build-property build.flash_offset=0x20000` sets the FLASH origin and `VECT_TAB_OFFSET` in one place (`platform.txt:68,78`). USB ids stay the core's defaults, which equal `boot_block.h`. Size gate 917504. Runs `stamp_image.py`, emits `cubevox.uf2` next to the `.bin`. |
| `firmware/cubevox.ld` | New, the one owned linker script. It replaces `cubevox_noinit.ld` and the platform copy. `.sram1_bss` and `.dtcm_bss` (bound `0x2001FF00`), `.cv_boot_block`, `.cv_noinit`, and `.cv_image_hdr` at app+0x400. Asserts image end `<= 0x08100000`. |
| `board/boot_block.h` | New. The shared contract. C-compatible, no C++ in it. |
| `board/boot_block.cpp` | New. The `.cv_boot_block` variable and its accessor `bootBlock()` (declared in `boot_block_access.h`). |
| `board/image_header.{h,cpp}` | New. The `.cv_image_hdr` object; version string from `CUBEVOX_FW_VERSION` (`-bench` added in the bench build); the CRC check of the running image. |
| `board/dfu.{h,cpp}` | `enterDfu()` keeps its name and both callers. Body: write `appSummary`, queue the diagnostic entry and `diagFlush`, `recordFlush`, `serialFlush`, `muteForReset`, write `request`, `NVIC_SystemReset()`. The ROM jump is gone. |
| `core/boot_health_rule.h` | New. The pure rules: healthy window, failed-start count. The reason, fault-kind and clock-step names live in `board/boot_block.h` as inline functions both programs call (one definition). |
| `board/boot_health.{h,cpp}` | New. `bootHealthPoll(nowMs)` from `loop()`; clears `bootFails` when the 5 s rule holds; exposes the count and the last reason for the boot report, `status` and the diagnostic log. |
| `core/crc32.{h,cpp}` | New. The one CRC-32, moved out of `core/record.cpp`; also checks the image. |
| `board/device_serial.{h,cpp}` | New. The MCU id as 24 hex characters, for `hello`. |
| `core/flash_io.{h,cpp}` | New. `FlashIo` moves here from `record_store.h`, with `kFlashWordBytes`, `kFlashSectorBytes` and the three steps both stores take: blank test, program one word, poll an erase. `core/le_bytes.h` holds the little-endian helpers that `record.cpp` and the log share. |
| `core/entry_log.{h,cpp}` | New. Append log of fixed-size entries in one sector over `FlashIo`. It stamps the sequence and the CRC-32 and knows no other field of `CvDiagEntry`. |
| `board/diag_service.{h,cpp}` | New. Builds `CvDiagEntry` bytes on bank 2 sector 2: `diagInit` (boot entry), `diagPoll`, `diagNoteUpdateRequest`, `diagFlush`, `diagReportBoot` (the `[boot] diag log:` line). |
| `board/flash_bank2.{h,cpp}`, `board/record_service.cpp` | Sector index 2 maps to `CV_DIAG_LOG_ADDR`, and `bank2Flash()` is the one controller both stores use. Each store skips its poll while the other erases. `recoverEcc` also covers sector 2 and the sector a bootloader read fault names, and clears that fault address. |
| `board/block_mem.{h,cpp}` | Moved from `platform/`, trimmed, flash table fixed (section Teensy-H7-Port cut-over). |
| `board/fault.cpp` | Boot report line `[boot] image v2.00.10 crc ok, boot fails 0, last update reason none`. `faultResetFlags`, `faultResetWasPowerOn` and `faultRecordPresent` feed the diagnostic log. |
| `ui/menu.cpp` | Confirm screen per section OLED. |
| `ui/splash.{h,cpp}` | Boot splash with version, 2 s. |
| `core/protocol.cpp` | `enter_dfu` unchanged in name and reply. `status` gains `boot_fails` and `update_reason`. `hello` gains `serial`. |
| `platform_shim.cpp`, `platform.h`, `firmware/platform/` | Deleted. |

## Teensy-H7-Port cut-over
The firmware builds against STM32duino 3.0.0 (`build.sh:97`). Teensy-H7-Port survives only as four
verbatim copies in `firmware/platform/`. The owner asked whether that link is still needed; it is
not, and this spec ends it.

1. `h7_block_mem.{h,cpp}` become `board/block_mem.{h,cpp}`, reduced to the calls `audio.cpp`
   makes, with the 128 KB FLASH table corrected to the H743's 2 MB in 8 × 128 KB sectors per bank,
   and the "copied verbatim, do not edit" banner replaced by a one-line credit.
2. `h7_h743v_sections.ld` becomes `firmware/cubevox_sections.ld`, owned.
3. `h7_dfu_reboot.h` is deleted with `platform_shim.cpp` and `platform.h`. `initVariant` is no
   longer needed. The ROM DFU stays reachable by BOOT0 as a shop tool.
4. `firmware/platform/` is removed. `CLAUDE.md` names Teensy-H7-Port and DrumSynthV3 as references
   to read, never to edit, with nothing copied from them.

## Bootloader tree and build
```
firmware/bootloader/
  README.md                 pin, toolchain, build, flash, drills, licences
  build.sh                  clones adafruit/tinyuf2 at the pin into build/tinyuf2, applies patches/, make BOARD=cubevox; --selftest, --proofs
  flash.sh                  ROM DFU of the bootloader at 0x08000000 (BOOT0 held), clears sector 0 protection first, reads back; shop only
  patches/                  01-11: flash bound and NOINIT origin, optional CURRENT.UF2 and INDEX.HTM, board files, block filter,
                            DFU task hook, weak dfu_complete, double-tap default, write state reset, failed write not counted,
                            root directory hook, 1 ms SysTick; sent upstream
  tools/                    imagecheck, drivetext, proof (host programs for --selftest and --proofs), option_bytes.py (flash.sh)
  boards/cubevox/           board.h, board.mk, board_hooks.c (TinyUF2 hooks, fault entry, option bytes, cable, watchdog),
                            boot_decide.c (pure policy), clock.c, oled.c, screens.c, diag.c, image_check.c, text.h, fonts.cpp
```
1. Pin. adafruit/tinyuf2 `29d9ce231bada513a7200c087f10a91c832aec42` (master, 2026-10-23) and TinyUSB
   at its submodule commit. `build.sh` checks the pin the way the firmware `build.sh` checks the
   Arduino core version.
2. Toolchain. The compiler is the one the Arduino core installs,
   `~/Library/Arduino15/packages/STMicroelectronics/tools/xpack-arm-none-eabi-gcc/14.2.1-1.1`, passed
   as `CROSS_COMPILE`. One toolchain pin for both programs, `XPACK_VERSION` in `firmware/build.sh`,
   which the bootloader build reads through `firmware/build.sh --toolchain`.
3. `boot_decide.c` has no HAL includes. It takes the block, the reset flags, the image status, the
   switch state and the clock step and returns the action and reason; the fault rule, the option
   byte step, the block filter, the transfer restart, the directory scan, the screen choice and the
   cable debounce live beside it. `test/firmware/boot_decide.cpp` compiles it and covers every row
   of the decision table, the power-on rule, the check-word garbage case, the counter wrap, the
   fault paths and the timeout rule.
4. The bootloader has its own version, `CV_BOOT_VERSION` in `board.h`, shown in `INFO_UF2.TXT` and
   `DIAG.TXT`, bumped on every bootloader change. It is independent of `VERSION.txt` because the
   two ship on different paths.
5. Two builds of one commit are byte for byte the same. `board.mk` passes a fixed `COMPILE_DATE` and
   `COMPILE_TIME`, and `build.sh` passes TinyUF2's and TinyUSB's describe strings pinned beside the
   commit, so fetched tags change nothing. `build.sh` prints the SHA-256 the shop can compare.
6. `build.sh --selftest` runs the image check against `stamp_image.py`, the screen wording and rows,
   the `DIAG.TXT` and `INFO_UF2.TXT` texts, `option_bytes.py`'s own test, and upstream's
   `test_ghostfat` on the patched tree.
5. Bootloader self-update in the field is not a supported path. TinyUF2's self-update image exists
   and could be used as a last resort; it rewrites sector 0 with a window of about 2 s in which a
   power loss leaves the box for the shop, and it would first have to clear sector 0's write
   protection. The bootloader is kept small so that it never needs it.

## USB identity
1. The box only ever meets two computers, so no registered ids are sought. The application keeps
   `0483:5740` as today. The bootloader uses the pid.codes test id `1209:0001`, meant for exactly
   this kind of private device. The two differ because macOS and Windows remember the device class
   per id.
2. The serial string in both programs is the MCU UID, so two boxes on one Mac are distinct. The
   bringup log maps box number to UID.

## Shop steps, every unit
1. Board on the bench, enclosure open, 9 V on. Hold BOOT0 down through the whole run and tap NRST.
   `firmware/bootloader/flash.sh` clears sector 0's write protection when a bootloader set it,
   writes the bootloader with dfu-util at 0x08000000, reads it back and starts it; let go of BOOT0
   when it says so. Expected within 3 s: OLED `The update stopped.`, `CUBEVOX` drive mounted,
   `INFO_UF2.TXT` reads `Installed: none`, `DIAG.TXT` reads
   `option bytes: programmed, BOR level 3, sector 0 protected`.
2. Drag the release `.uf2` from Finder. This is Adam's path and doubles as its acceptance test.
   Expected: `Writing the firmware.`, `The update is done.`, boot splash with the version, boot report
   `crc ok, boot fails 0`.
3. `./bringup.py --board N --faults --update` runs the drills below and writes
   `hardware/bringup/board-N.md` with the UID, bootloader version, firmware version and every
   drill result.
4. Close the enclosure. BOOT0 is now unreachable, which is the intended state.

## Drills (`bringup.py --update`)
| # | Drill | Expected |
|---|---|---|
| 1 | Send `enter_dfu` over serial | CDC port gone, `/Volumes/CUBEVOX` mounted within 5 s, OLED `Drop the firmware on the CUBEVOX drive.` |
| 2 | Copy the bench `.uf2` to the volume | Volume unmounts, CDC port back within 10 s, `hello` says `bench` |
| 3 | Operator pulls USB at about 30 % of a copy | OLED `The cable came out.`, box stays in update mode; a full copy then succeeds |
| 4 | Operator cuts 9 V at about 30 % of a copy | Next power-on shows `The update stopped.`; `DIAG.TXT` says `ImageInvalid` or `ImageUnreadable` with an address; a full copy then succeeds |
| 5 | Copy a `.uf2` made with `stamp_image.py uf2 --base 0x08100000` | Every block refused, OLED `That is not a cubevox firmware file.`, layout record intact (`get_layout` after reboot) |
| 6 | Copy a `.uf2` made with `stamp_image.py uf2 --family-id 0x00000000` from the bench image | Every block refused (family), installed image intact |
| 7 | Copy a `.uf2` with one payload byte flipped | Box resets into update mode, `INFO_UF2.TXT` `Installed: invalid` |
| 8 | Bench op `f` (bus fault) three times inside 5 s of boot | Third reset lands in update mode, `DIAG.TXT` carries the fault record |
| 9 | `set_layout`, update, `get_layout` | Same layout |
| 10 | Operator holds MENU, cycles power | OLED `Drop the firmware on the CUBEVOX drive.`, `DIAG.TXT` reason `MenuHeld` |
| 11 | `enter_dfu` with no USB data connection (power only) | Back to the application after 60 s |
| 12 | Operator: MENU → Update firmware → click | Confirm screen shows the installed version, click enters update mode |
| 13 | Release image copy, `hello` | `build == "release"`, version equals the file name, `serial` equals the UID |

## Tooling, CI, release, docs
1. `firmware/flash.sh` becomes the shop tool for the application. With a CDC port present it sends
   `enter_dfu`, waits for `/Volumes/CUBEVOX`, copies the `.uf2`, waits for the port and prints the
   boot report. `--rom` keeps the dfu-util path at `0x08020000:leave` for a board whose
   application is unreachable but whose bootloader is in. `firmware/bootloader/flash.sh` owns the
   `0483:df11` wait.
2. `cubevox_port.py` gains `--wait-volume CUBEVOX`, shared by `flash.sh` and `bringup.py`.
3. `.github/workflows/ci.yml` adds a `bootloader` job using the same xpack toolchain the firmware
   job installs, runs `firmware/bootloader/build.sh`, and checks both `.uf2` files with
   `stamp_image.py check`. The host job runs the `boot_decide` tests with the rest of `ctest`.
4. `release.yml` attaches `cubevox-vX.YY.ZZ.uf2` and `cubevox-rom-0x08020000.elf` to a draft
   release once the host tests, the bootloader selftest and the bench build have passed, and the
   owner publishes the draft. The `.elf` resolves the addresses in a `DIAG.TXT`. The ROM-path image
   `cubevox-rom-0x08020000.bin` stays a CI artifact for the shop, because a `.bin` beside the
   `.uf2` on the release page is the file Adam would most likely drag by mistake. The address in
   its name is the one dfu-util must write it at, so the old `cubevox.bin` name that the parked app
   wrote at 0x08000000 can never hit the bootloader. The bootloader binary is shop-only too; its
   job stays as a build check. The app jobs stay; the app is parked, not removed.
5. `VERSION.txt` is bumped as today; the header string and the release file name derive from it.
6. `LICENSE` at the repository root is MIT. The bootloader README carries the TinyUF2 and TinyUSB
   notices. Nothing else is needed on licensing.
7. The parked app, when it returns, replaces its dfu-util step with a copy of the release `.uf2` to
   `/Volumes/CUBEVOX` and a wait for the CDC port. `app/dfu-util-static.sh` and the `dfu-util`
   release job go at that time, not before.

## Compatibility rules
1. The boot block offsets, the header layout, the family id, the application start address and the
   bootloader's VID:PID are frozen once a box ships. Changes are additive, within reserved space
   or after the header's 32 bytes, and bump the version field they belong to: `blockVersion` for
   the bootloader's fields, `appFieldsVersion` for the application's, the header version for the
   header.
2. Every reader accepts every version from 1 up and reads the fields it knows. A reader that
   refused a later version would turn the first release that follows rule 1 into a box that never
   starts again.
3. The application must keep working with the oldest shipped bootloader. Both programs compile the
   one `boot_block.h`, whose offset asserts fail either build when a field moves, and the host test
   compiles the bootloader's own `boot_decide.c` with the application's tests.
4. `enter_dfu` keeps its name and reply forever; the parked app and `flash.sh` depend on it.

## Budgets
| Item | Value |
|---|---|
| Boot delay added by the bootloader | clock init plus CRC, under 20 ms for today's image, no double-tap wait |
| Application region | 896 KB, asserted by the linker, gated by `build.sh` |
| Bootloader | about 35 KB of the 128 KB sector, OLED driver and screens included |
| Shared boot block | 256 B DTCM at 0x2001FF00 |
| Watchdog from the jump to the end of `setup()` | 16 s, then the application's 2 s |
| Healthy-boot window | 5 s of audio blocks after `setup()` |
| Boot-fail limit | 3 |
| Update-mode timeout, no host | 60 s, menu, app request or bootloader fault only; starts again from a cable pull with nothing written |
| Option byte step | once per power-on, OPT_BUSY wait at most 1 s |
| Update time | about 3 s for a 200 KB image, erase included |

## Roadmap
1. `boot_block.h`, `stamp_image.py` (stamp, check, UF2 write), `boot_decide.c`, host tests for all
   of them. Gate: `ctest`.
2. Bootloader board, patches, build script, OLED driver and screens, `DIAG.TXT`. Proof set for the
   OLED screens. Flash board 1 over BOOT0. Gate: `CUBEVOX` drive on the Mac, `INFO_UF2.TXT` and
   `DIAG.TXT` readable, screens approved.
3. Application relocation, header, `enterDfu` rewrite, boot health, confirm screen, splash,
   Teensy-H7-Port cut-over. Gates: `ctest`, both `build.sh` variants with the warn gate, a hand
   copy of the `.uf2` boots and shows the version.
4. Diagnostic log in bank 2 sector 2, `status` and `hello` fields, serial spec line.
5. `flash.sh`, `cubevox_port.py`, `bringup.py --update`, workflows. Gate: drills 1–13 logged for
   board 1.
6. Shop steps on every shipping unit, one log per unit in `hardware/bringup/`.

## Open items for the owner
1. OLED screen proofs arrive with roadmap step 2 and the confirm screen with step 3, numbered,
   approved per number.
2. The cable screen (proof 08) and the clock screen (proof 09) await approval.
