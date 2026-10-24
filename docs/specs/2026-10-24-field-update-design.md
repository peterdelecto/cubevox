# Field firmware update — design

Owner decision 2026-10-24: the Mac app is parked and the box ships to Adam with the default chain.
Adam updates the firmware himself over USB-C with no software to install. This spec puts a UF2
bootloader in flash sector 0, moves the application to 0x08020000, and defines the shop steps that
happen before the box leaves. It supersedes the ROM-DFU parts of `2026-10-02-hardware-design.md`
(MENU item, lines 151 and 180) and the Update parts of `2026-10-15-mac-app-design.md` for the box
side; when the app returns it downloads the same `.uf2` and copies it to the drive.

## What Adam does
1. Downloads `cubevox-vX.YY.ZZ.uf2` from the release page. One file.
2. Plugs the box into the Mac with USB-C. Turns MENU to `Update firmware`, clicks, clicks again to
   confirm.
3. A drive named `CUBEVOX` appears on the desktop. He drags the file onto it.
4. The drive goes away by itself, the box reboots, and the face shows the new version bottom left.
   The layout and settings are unchanged.
5. If anything interrupts the copy, the drive comes back on its own at the next power-up and he
   drags again. Nothing he can do from outside the box leaves it unbootable.

## Why a bootloader
1. The ROM DFU needs dfu-util on the host and a BOOT0 press to recover a bad image. BOOT0 is inside
   the sealed enclosure, so neither is a path Adam can take.
2. A bootloader in its own sector never changes in the field. Whatever state the application is in,
   the update path still works.
3. UF2 is the file format that survives a drag onto a Finder drive. Each 512-byte block carries its
   own target address and a family id, so dot files, out-of-order writes and a wrong file are all
   harmless.
4. TinyUF2 (Adafruit, MIT, on TinyUSB) already has an STM32H7 port whose Nucleo-H743 board runs the
   application from internal flash at 0x08020000. We add a cubevox board to that port and keep the
   upstream commit pinned.

## Flash map
| Region | Address | Size | Content |
|---|---|---|---|
| Bank 1 sector 0 | 0x08000000 | 128 KB | TinyUF2 bootloader, cubevox board |
| Bank 1 sectors 1–7 | 0x08020000 | 896 KB | Application image, header at +0x400 |
| Bank 2 sectors 0–1 | 0x08100000 | 256 KB | Layout record (`2026-10-12-layout-record-design.md`), untouched |
| Bank 2 sectors 2–7 | 0x08140000 | 768 KB | Unused |

1. The bootloader writes only inside the application region. `BOARD_FLASH_SIZE` is 1 MB in the
   cubevox board, so a UF2 block addressed into bank 2 is refused. Upstream hard-codes 2048K in
   `ports/stm32h7/linker/memory.ld`; the board build carries a patch that takes the size from the
   board until the upstream change lands.
2. `build.sh` fails when the application `.bin` exceeds 917504 bytes (896 KB). Today's release
   image is about 180 KB.
3. Bootloader size is about 40 KB. The rest of sector 0 is spare; the sector is the erase unit, so
   nothing else can live there.

## Shared boot block (DTCM)
Both programs agree on 64 bytes at the top of DTCM, below the fault record. One header,
`firmware/cubevox/src/board/boot_block.h`, defines it and the bootloader board includes the same
file. Nothing else defines these offsets.

| Address | Word | Meaning |
|---|---|---|
| 0x2001FF80 | `request` | TinyUF2's double-tap register. `0xF01669EF` = stay in the bootloader. The app writes it to ask for an update. |
| 0x2001FF84 | `bootFails` | Count of jumps to the app with no healthy report since the last good run |
| 0x2001FF88 | `bootFailsCheck` | `~bootFails`; the pair disagreeing means power-on garbage, treated as 0 |
| 0x2001FF8C–0x2001FFBF | reserved | Zero on write |
| 0x2001FFC0 | fault record | `.cv_noinit`, 64 B, unchanged (`FAU3`) |

1. The bootloader's NOINIT origin moves from 0x20010000 to 0x2001FF80 (board-level linker override,
   same patch as item 1 above). The application's `.dtcm_bss` assert drops to `<= 0x2001FF80`.
2. DTCM survives a system reset and is random after power-on. Every word in the block is validated
   before use; the fault record already does this with a magic.

## Boot sequence
1. Reset vector in sector 0 runs TinyUF2. Clocks come up from the 25 MHz crystal (`kHseHz`) with
   PLL1 giving a 48 MHz USB clock, the same arrangement as `board/sysclock.cpp`. HSI48 stays off.
2. `check_dfu_mode` decides, in this order. `request` = `0xF01669EF` → bootloader. Image invalid
   (section Image header) → bootloader. `bootFails >= 3` → bootloader. Otherwise `bootFails += 1`,
   write `bootFailsCheck`, jump.
3. The double-tap wait is off. The box has no reset button Adam can reach, so the 500 ms window
   would cost boot time and nothing else. `TINYUF2_DBL_TAP_DFU` is 0 and the register check stays.
4. Jump: `HAL_RCC_DeInit`, caches off, `SCB->VTOR = 0x08020000`, MSP from the image, branch to the
   reset vector. The STM32duino core then sets VTOR itself from `VECT_TAB_OFFSET` (`system_stm32h7xx.c:291`),
   so both sides agree.
5. The application reports healthy once `setup()` has finished, the watchdog is running, and the
   audio ISR has advanced blocks for 5 s. It then writes `bootFails = 0` with its check word. A
   fault, a watchdog reset or a hang inside those 5 s leaves the count standing.
6. Bootloader mode: USB OTG_FS on PA11/PA12 enumerates as a mass-storage drive with volume label
   `CUBEVOX`. `INFO_UF2.TXT` reads `Model: cubevox`, `Board-ID: cubevox-h743-v4`, the bootloader
   version, and the installed application version read from the image header (`Current: v2.00.10`
   or `Current: none`). The LED pin is the OLED-less status the port supports; the OLED is not driven
   by the bootloader.
7. After the last UF2 block lands, TinyUF2 writes `bootFails = 0` and calls `NVIC_SystemReset()`.
   The new image boots with no wait.
8. The mute relay K102 is open while the bootloader runs (MUTE_N idle low), so the update is silent.

## Image header
A 32-byte header sits at application offset 0x400, right after the 0x298-byte vector table.
`firmware/cubevox_image.ld` places section `.cv_image_hdr` at that absolute address with `INSERT
AFTER .isr_vector`, the same mechanism `cubevox_noinit.ld` uses today.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `CVFW` |
| 4 | 4 | header version, 1 |
| 8 | 4 | image length in bytes (whole `.bin`) |
| 12 | 4 | CRC-32 (zlib polynomial) over the whole image with this field as zero |
| 16 | 16 | version string, `v2.00.10`, NUL padded, from `VERSION.txt` |

1. The compiler emits the header with length and CRC zero. `tools/stamp_image.py` fills both after
   the link and `build.sh` runs it on every image, then converts to UF2. The same script verifies
   the CRC of any `.bin` or `.uf2` handed to it (`--check`).
2. The bootloader's `board_app_valid2` hook (upstream weak symbol) checks magic, header version,
   length inside the region, and the CRC. TinyUF2's own check is only a stack-pointer and
   reset-vector sanity test; a half-written image passes it, which is why the header exists.
3. CRC over the largest allowed image is under 10 ms in software at 400 MHz. The hardware CRC unit
   is not used, so the Python side and the C side share one unambiguous definition.
4. The bench image carries the same header. Version string gets a `-bench` suffix.

## Application changes
| File | Change |
|---|---|
| `firmware/build.sh` | `--build-property build.flash_offset=0x20000` (sets the FLASH origin and `VECT_TAB_OFFSET` in one place, `platform.txt:68,78`). Size gate 917504. Stamp, then `uf2conv.py -c -b 0x08020000 -f 0x6db66082`. Emits `cubevox.uf2` next to the `.bin`. |
| `firmware/cubevox_image.ld` | New. `.cv_image_hdr` at app+0x400. |
| `firmware/cubevox_noinit.ld` | `.dtcm_bss` assert `<= 0x2001FF80`; boot block symbols. |
| `board/boot_block.h` | New. Addresses, magic, `BootBlock` struct, `bootBlockValidFails()`. Shared with the bootloader. |
| `board/dfu.{h,cpp}` | `enterDfu()` keeps its name and callers. Body: `recordFlush`, `serialFlush`, `muteForReset`, write `request`, `NVIC_SystemReset()`. No ROM jump. |
| `board/boot_health.{h,cpp}` | New. `bootHealthPoll(nowMs)` from `loop()`; clears `bootFails` once the 5 s rule holds. |
| `board/image_header.cpp` | New. The `.cv_image_hdr` object, version string from `CUBEVOX_VERSION`. |
| `board/fault.cpp` | Boot report prints `[boot] image v2.00.10 crc ok, boot fails 0` from the header and block. |
| `ui/menu.cpp` | Confirm screen text names the drive instead of DFU mode. Proposed rows: `UPDATE FIRMWARE` / `Click: box reboots` / `as CUBEVOX drive.` / `Turn to cancel.` Owner proof before it lands, per the OLED rule. |
| `core/protocol.cpp` | `enter_dfu` unchanged in name and reply. The serial spec gains one line saying the box reboots into the UF2 drive. |
| `platform_shim.cpp`, `platform.h` | Deleted with `h7_dfu_reboot.h`. `initVariant` is no longer needed. |

## Teensy-H7-Port cut-over
The firmware builds against STM32duino 3.0.0 (`build.sh:97`). Teensy-H7-Port only survives as four
verbatim copies in `firmware/platform/`. This spec ends that arrangement; the owner asked and the
answer is that nothing still needs it.

1. `h7_block_mem.{h,cpp}` move to `board/block_mem.{h,cpp}`, trimmed to what `audio.cpp` calls,
   with the 128 KB FLASH table corrected (H743: 2 MB, 8 × 128 KB per bank) and the "copied verbatim"
   header replaced by a one-line credit.
2. `h7_h743v_sections.ld` becomes `firmware/cubevox_sections.ld`, owned, with the new DTCM bound.
3. `h7_dfu_reboot.h` is deleted. The ROM DFU remains reachable by BOOT0 and is a shop tool only.
4. `firmware/platform/` is removed. `CLAUDE.md` says Teensy-H7-Port and DrumSynthV3 are references
   to read, not sources to copy.

## Bootloader tree
```
firmware/bootloader/
  README.md            pinned commit, build, flash, test
  build.sh             clones adafruit/tinyuf2 at the pin into build/tinyuf2, applies patches, make BOARD=cubevox
  flash.sh             ROM DFU of the bootloader at 0x08000000 (BOOT0), then optional app copy
  patches/             board-overridable flash size and NOINIT origin (sent upstream)
  boards/cubevox/      board.h, board.mk, board.cmake, clock_init for 25 MHz HSE, app_valid2, boot block
  tools/uf2conv.py     vendored from microsoft/uf2 (MIT), with uf2families.json
```
1. Pin: adafruit/tinyuf2 `29d9ce231bada513a7200c087f10a91c832aec42` (master, 2026-10-23), TinyUSB
   at its submodule commit. The pin and the toolchain (`arm-none-eabi-gcc` version) live in the
   README and are checked by `build.sh`, the way `build.sh` checks the Arduino core today.
2. USB identity. The bootloader needs its own VID:PID; borrowing an ST or Adafruit pair is not
   done. Decision: request a pid.codes PID for cubevox (free, one pull request) and build the
   bootloader with it. The application keeps the core's `0483:5740` CDC identity because the port
   glob and the parked app depend on it. Until the PID is granted the bootloader builds with the
   pid.codes test PID `1209:0001`, which is allowed for devices that never leave the bench.
3. Volume label `CUBEVOX`, product `cubevox`, board id `cubevox-h743-v4`.
4. No bootloader self-update in the field. A bootloader change is a shop operation over BOOT0.

## Shop steps before shipping
1. Board on the bench, enclosure open. Hold BOOT0, apply power, release. `firmware/bootloader/flash.sh`
   writes the bootloader with dfu-util at 0x08000000. Expected: a `CUBEVOX` drive appears within
   3 s of the next reset, `INFO_UF2.TXT` says `Current: none`.
2. Copy the release `.uf2` onto the drive from Finder. This is Adam's path and doubles as its test.
   Expected: drive vanishes, OLED shows the version, boot report shows `crc ok, boot fails 0`.
3. `./bringup.py --board N --faults --update` runs the drills in the next section and logs to
   `hardware/bringup/`.
4. Close the enclosure. BOOT0 is now unreachable, which is the intended state.

## Drills (`bringup.py --update`)
| # | Drill | Expected |
|---|---|---|
| 1 | Send `enter_dfu` over serial | CDC port gone, `/Volumes/CUBEVOX` mounted within 5 s |
| 2 | Copy the bench `.uf2` to the volume | Volume unmounts, CDC port back within 10 s, `hello` says `bench` |
| 3 | Pull USB after 30 % of a copy (operator) | Box stays as the drive; a full copy then succeeds |
| 4 | Copy a `.uf2` built with `-b 0x08100000` | Copy fails, record sectors unchanged (`get_layout` after reboot) |
| 5 | Copy a `.uf2` with a corrupted CRC byte | Box reboots into the drive, `INFO_UF2.TXT` shows `Current: invalid` |
| 6 | Bench op `f` (bus fault) three times inside 5 s of boot | Third reset lands in the drive |
| 7 | `set_layout`, update, `get_layout` | Same layout |
| 8 | Operator: MENU → Update firmware → click | Drive appears with no host command |
| 9 | Release image copy, `hello` | `build == "release"`, version matches the file name |

## Tooling and release
1. `firmware/flash.sh` becomes the shop tool. With a CDC port present it asks for `enter_dfu`, waits
   for `/Volumes/CUBEVOX`, copies the `.uf2`, waits for the port and prints the boot report. With
   `--rom` it does the old dfu-util path at `0x08020000:leave` for a board whose bootloader is in
   but whose app is unreachable. The `0483:df11` checks move to `firmware/bootloader/flash.sh`.
2. `cubevox_port.py` gains `--wait-volume CUBEVOX` so every script shares the mount wait.
3. `.github/workflows/ci.yml` adds a `bootloader` job (arm-none-eabi-gcc, `firmware/bootloader/build.sh`)
   and checks both `.uf2` files exist and pass `stamp_image.py --check`.
4. `release.yml` attaches `cubevox-vX.YY.ZZ.uf2`, `cubevox.bin` (shop, ROM path), and
   `cubevox-bootloader-vX.YY.ZZ.bin`. The app jobs stay as they are; the app is parked, not removed.
5. `docs/update-guide.md` is the one page Adam gets. Steps 1–5 of "What Adam does", a screenshot of
   the drive, and the Finder "disk not ejected properly" note (expected, harmless).
6. `VERSION.txt` is bumped as today. The header string and the file name both derive from it.

## Budgets
| Item | Value |
|---|---|
| Boot delay added by the bootloader | clock init plus CRC of the image, under 20 ms for today's image; no double-tap wait |
| Application region | 896 KB; gate in `build.sh` |
| Bootloader | about 40 KB of the 128 KB sector |
| Shared boot block | 64 B DTCM at 0x2001FF80 |
| Healthy-boot window | 5 s of audio blocks after `setup()` |
| Boot-fail limit | 3 |

## Roadmap
1. `boot_block.h` and the image header contract, `stamp_image.py`, vendored `uf2conv.py`. Host
   test for the stamp and check round trip.
2. Bootloader board, patches, build script. Flash board 1 over BOOT0. Gate: `CUBEVOX` drive on the
   Mac, `INFO_UF2.TXT` readable.
3. Application relocation and the file changes above, Teensy-H7-Port cut-over included. Gates:
   `ctest`, both `build.sh` variants with the warn gate, `.uf2` copied by hand boots and shows the
   version.
4. `board_app_valid2`, boot-fail counter, region bound. Gates: drills 3–6.
5. `flash.sh`, `bringup.py --update`, workflows, update guide. Gate: drills 1–9 logged for board 1.
6. Shop steps on every shipping board, log per board in `hardware/bringup/`.

## Open items for the owner
1. OLED confirm-screen text (numbered proof to follow with step 3).
2. pid.codes PID request goes out under the owner's GitHub account, or the bootloader ships with
   the Nucleo PID for this first box.
