// The mid-song mark in the boot block's application area (core/mid_song_mark).

#pragma once

// Call first in setup(). On a box-caused reset with a whole mark that names the running image, zeroes
// the bootloader's failed-start counter. It reads the boot block and four words of flash and touches
// no pin, so nothing in it can fault or block.
void midSongMarkClearFailedStarts();

// Writes the mark for the running image. Call once the box has run audio for 5 s (board/boot_health).
void midSongMarkWrite();
