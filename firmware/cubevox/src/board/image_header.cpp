#include "board/image_header.h"

#include "board/build_kind.h"
#include "core/crc32.h"
#include "core/version.h"

// The bench image says so in its header, so INFO_UF2.TXT shows which kind of image is installed.
#if CUBEVOX_BENCH
#define CV_HEADER_NAME CUBEVOX_FW_VERSION "-bench"
#else
#define CV_HEADER_NAME CUBEVOX_FW_VERSION
#endif

static_assert(sizeof(CV_HEADER_NAME) <= CV_IMAGE_VERSION_BYTES, "the version string must fit the 16-byte header field");

// firmware/cubevox.ld places the section at CV_APP_START + CV_IMAGE_HEADER_OFFSET and keeps it.
__attribute__((section(".cv_image_hdr"), used)) const CvImageHeader kImageHeader = {
    CV_IMAGE_MAGIC, CV_IMAGE_HEADER_VERSION, 0u, 0u, CV_HEADER_NAME};

static_assert(sizeof(kImageHeader) == CV_IMAGE_HEADER_SIZE, "the header must be 32 bytes");

namespace {

constexpr uint32_t kHeaderAddress = CV_APP_START + CV_IMAGE_HEADER_OFFSET;
constexpr uint32_t kCrcOffset = CV_IMAGE_HEADER_OFFSET + CV_IMAGE_OFFSET_CRC;
constexpr uint32_t kMinLength = CV_IMAGE_HEADER_OFFSET + CV_IMAGE_HEADER_SIZE;

enum class Verdict : int8_t { Unknown = -1, Bad = 0, Good = 1 };

Verdict gVerdict = Verdict::Unknown;

// Reads the stamped header through its address. The compiled constant holds zero length and CRC.
bool checkImageInFlash() {
  const volatile CvImageHeader* stamped = reinterpret_cast<const volatile CvImageHeader*>(kHeaderAddress);
  if (stamped->magic != CV_IMAGE_MAGIC || stamped->headerVersion != CV_IMAGE_HEADER_VERSION) return false;
  const uint32_t length = stamped->length;
  if (length < kMinLength || length > CV_APP_END - CV_APP_START) return false;
  const uint8_t* image = reinterpret_cast<const uint8_t*>(CV_APP_START);
  return imageCrc(image, length, kCrcOffset) == stamped->crc32;
}

}  // namespace

const char* imageVersion() { return kImageHeader.version; }

bool imageCrcOk() {
  if (gVerdict == Verdict::Unknown) gVerdict = checkImageInFlash() ? Verdict::Good : Verdict::Bad;
  return gVerdict == Verdict::Good;
}
