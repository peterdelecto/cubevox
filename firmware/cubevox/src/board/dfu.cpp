#include "board/dfu.h"

#include "board/mute.h"
#include "board/platform.h"
#include "board/record_service.h"
#include "board/serial_link.h"

namespace {

// Matches the flash driver's erase timeout, so a stuck erase ends this wait too.
constexpr uint32_t kSaveWaitMs = 5000;

}  // namespace

void enterDfu() {
  recordFlush(kSaveWaitMs);
  serialFlush(kFlushBeforeResetMs);
  muteForReset();
  platformEnterDfu();
}
