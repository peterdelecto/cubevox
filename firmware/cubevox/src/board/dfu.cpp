#include "board/dfu.h"

#include "board/flash_bank2.h"
#include "board/mute.h"
#include "board/platform.h"
#include "board/record_service.h"
#include "board/serial_link.h"

namespace {

// A stuck erase ends this wait when the flash driver's erase timeout does.
constexpr uint32_t kSaveWaitMs = kEraseTimeoutMs;

}  // namespace

void enterDfu() {
  recordFlush(kSaveWaitMs);
  serialFlush(kFlushBeforeResetMs);
  muteForReset();
  platformEnterDfu();
}
