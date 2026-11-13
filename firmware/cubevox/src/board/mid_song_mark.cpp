#include "board/mid_song_mark.h"

#include <string.h>

#include "board/boot_block_access.h"
#include "board/image_header.h"
#include "core/mid_song_mark.h"

static_assert(sizeof(MidSongMark) <= sizeof(CvBootBlock::appReserved), "the mark must fit the application area");

namespace {

MidSongMark readMark(const CvBootBlock& block) {
  MidSongMark mark;
  memcpy(&mark, block.appReserved, sizeof mark);
  return mark;
}

}  // namespace

void midSongMarkClearFailedStarts() {
  CvBootBlock& block = bootBlock();
  if (!cvBootBlockInitialised(&block)) return;
  if (clearsFailedStarts(cvBootBlockResetFlags(&block), block.appFieldsVersion, readMark(block), imageStampedCrc())) {
    cvBootFailsSet(&block, 0u);
  }
}

void midSongMarkWrite() {
  CvBootBlock& block = bootBlock();
  if (!cvBootBlockInitialised(&block)) return;
  const MidSongMark mark = makeMidSongMark(imageStampedCrc());
  memcpy(block.appReserved, &mark, sizeof mark);
  block.appFieldsVersion = kMidSongFieldsVersion;
}
