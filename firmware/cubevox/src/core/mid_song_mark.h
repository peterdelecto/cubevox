// The mid-song mark (stage-survival spec R.8), pure. Once the box has run audio for 5 s the
// application writes the mark into the boot block's application area: a magic word, the CRC of the
// running image and a check word. After a box-caused reset a whole mark that names the running image
// tells setup() to zero the bootloader's failed-start counter, so a restart in the middle of a song
// never counts toward update mode. A new image has another CRC, so a mark an earlier image wrote does
// not count and a bad update still ends in "Startup failed.". board/mid_song_mark supplies the block
// and the CRC.

#pragma once

#include <stdint.h>

#include "core/restart_rule.h"

// Value of CvBootBlock::appFieldsVersion once the application's area holds this mark.
constexpr uint32_t kMidSongFieldsVersion = 1;

constexpr uint32_t kMidSongMagic = 0x4D494453u;  // "MIDS"

struct MidSongMark {
  uint32_t magic = 0;
  uint32_t imageCrc = 0;
  uint32_t check = 0;
};

constexpr uint32_t midSongCheck(uint32_t imageCrc) { return ~(kMidSongMagic ^ imageCrc); }

// The mark for the image with `imageCrc`.
constexpr MidSongMark makeMidSongMark(uint32_t imageCrc) {
  return MidSongMark{kMidSongMagic, imageCrc, midSongCheck(imageCrc)};
}

// True when the mark is whole and names the running image. A running CRC of 0 means the header gave
// none, and no mark names it.
constexpr bool midSongMarkNames(uint32_t fieldsVersion, const MidSongMark& mark, uint32_t runningCrc) {
  return fieldsVersion == kMidSongFieldsVersion && runningCrc != 0u && mark.magic == kMidSongMagic &&
         mark.check == midSongCheck(mark.imageCrc) && mark.imageCrc == runningCrc;
}

// True when setup() zeroes the failed-start counter: a box-caused reset and a mark that names the
// running image.
constexpr bool clearsFailedStarts(uint32_t resetFlags, uint32_t fieldsVersion, const MidSongMark& mark,
                                  uint32_t runningCrc) {
  return isBoxCausedReset(resetFlags) && midSongMarkNames(fieldsVersion, mark, runningCrc);
}
