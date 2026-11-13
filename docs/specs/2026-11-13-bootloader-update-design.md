# cubevox — updating the bootloader from the CUBEVOX drive

Owner 2026-11-13: the bootloader should update the way the firmware does, by a file dragged onto
the `CUBEVOX` drive, with no buttons and the enclosure closed. Today it goes in through the chip's
ROM DFU, which needs BOOT0 held for the whole run and a tap on NRST, and the hold is hard to keep.
Built 2026-11-13 in `firmware/bootloader/updater/`, `build.sh` and `flash.sh`; not yet run on a box.

## What the person does
1. Opens the menu, picks `Update firmware`, confirms. The `CUBEVOX` drive appears on the Mac.
   (`firmware/bootloader/flash.sh` does steps 1 to 5 by itself over the serial port.)
2. Drags `cubevox-bootloader-v0.09.uf2` onto the drive. The screen reads `Writing the firmware.`,
   as for any file, and the box restarts.
3. Sees `Updating the bootloader.` and `Keep the power on.` for about 3 s. Does nothing.
4. Sees `Bootloader updated.` / `Drop the firmware on the CUBEVOX drive.`, and the drive is back
   on the Mac. The updater leaves a mark in the boot block only after sector 0 compares; the new
   bootloader shows this screen while the mark is set and no firmware is installed. `INFO_UF2.TXT` names the new bootloader version and `Installed: none`.
5. Drags the firmware `.uf2`. The box restarts into the firmware.

What can go wrong for them.
1. The power is cut during step 3. The box has no bootloader and shows nothing. It is recovered
   with BOOT0 and `flash.sh --rom`, as today. This is the one risk the drive path adds, and it
   lasts about 2 s.
2. The file is damaged. The updater finds that before it touches the bootloader, shows
   `The bootloader file is damaged.`, and step 4 follows with the old bootloader still in place.
3. They stop after step 4. The box stays in update mode until a firmware file arrives.
4. The power check or the chip check of the updater fails. The old bootloader stays, the box
   shows `The update stopped.` / `Drop the firmware on the CUBEVOX drive.`, and DIAG.TXT's
   `app summary:` line names the reason (`bootloader kept: 5VA 4100 mV`).

## How it works
1. The file is an image in the application's format: based at 0x08020000, family `cubevox`,
   stamped with the header and CRC the bootloader checks, so every bootloader since v0.04 accepts
   it. Its header version reads `boot-0.09`. It holds a small program, the updater, and the new
   bootloader's bytes with their length and CRC.
2. The updater runs from the application region. In order it:
   1. starts the OLED and shows the screen of step 3;
   2. checks the bootloader bytes it carries against their length and CRC, and that they fit
      bank 1 sector 0;
   3. checks the power sense has a reading and no trip (BOR level 3 is already set);
   4. clears the write protection of sector 0 by the option byte sequence the bootloader itself
      uses (`board_hooks.c`, RM0433);
   5. erases sector 0, writes the new bytes, reads them back and compares;
   6. on a mismatch does step 5 again, for as long as it takes; it never resets on a sector 0
      that holds neither bootloader whole. Three passes in a row that leave sector 0 equal to the
      old bootloader (the chip refused the erase) end the run with the old bootloader kept;
   7. erases the first sector of the application region, so no application is installed;
   8. resets.
3. The new bootloader starts, protects sector 0 again as it does on every start, finds no
   application and offers the drive.
4. The firmware never holds code that can erase sector 0, and sector 0 stays write protected
   whenever the firmware runs. The bootloader is rewritten only by this separate file. That is why
   the bootloader does not ride inside the firmware file, which would save the second drag.
5. BOOT0 and `flash.sh --rom` stay as the way into a box that cannot start.

## Build notes
1. TinyUF2 ships `apps/self_update`; use it if it fits this port, otherwise a small program from
   the bootloader's own flash and SH1106 code. The builder decides and says why.
2. `firmware/bootloader/build.sh` writes `cubevox-bootloader-vB.BB.uf2` beside the `.bin`.
3. `firmware/bootloader/flash.sh` takes the drive path by default (enter update mode over the
   serial port, copy the bootloader file, wait for the drive to return with the new version in
   `INFO_UF2.TXT`, then copy the firmware with `firmware/flash.sh`). `--rom` keeps today's path.
4. Screens: 5x7 prose on rows 0, 2, 4, 6, 21 columns, proofs by number.

## Tests
1. Host selftest: the updater's checks refuse a wrong length, a wrong CRC and an image larger than
   the sector, and accept the built bootloader.
2. On the box, from the Mac with no hands: v0.08 to v0.09 by the drive, `INFO_UF2.TXT` reads
   v0.09 and `sector 0 protected`, the firmware goes back on and the boot report reads `crc ok`.
   Then the same file again (v0.09 over v0.09) to show a second run is safe.
3. A bootloader file with one byte flipped inside the carried bootloader: the damaged-file screen,
   the old bootloader still answers.
