// The board's word-wise memset, memcpy and memmove (firmware/cubevox/src/board/fastmem.cpp), built on
// the host under other names and checked against byte loops: every destination and source alignment
// 0..7, every length 0..80 plus a few large ones, guard bytes either side of the destination, and
// overlapping moves in both directions. The cycle cost is a board property and is read from the
// disassembly, not from here.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#define memset cvMemset
#define memcpy cvMemcpy
#define memmove cvMemmove
#include "board/fastmem.cpp"
#undef memset
#undef memcpy
#undef memmove

namespace {

constexpr size_t kPad = 16;
constexpr size_t kMaxLen = 80;
constexpr uint8_t kGuard = 0xA5;

uint32_t gSeed = 12345u;

uint8_t nextByte() {
  gSeed = gSeed * 1664525u + 1013904223u;
  return static_cast<uint8_t>(gSeed >> 24);
}

void fillRandom(std::vector<uint8_t>& v) {
  for (uint8_t& b : v) b = nextByte();
}

// The lengths to try: 0..kMaxLen, then sizes that run the 16-byte loop many times.
std::vector<size_t> lengths() {
  std::vector<size_t> out;
  for (size_t n = 0; n <= kMaxLen; ++n) out.push_back(n);
  for (size_t n : {127u, 128u, 129u, 1000u, 4093u}) out.push_back(n);
  return out;
}

// 1. memset writes exactly n bytes of the value and leaves the guards alone.
bool testMemset() {
  int bad = 0;
  for (size_t off = 0; off < 8; ++off) {
    for (size_t n : lengths()) {
      for (int value : {0, 0x5A, 0xFF, 0x1FF}) {
        std::vector<uint8_t> buf(n + 2 * kPad + 8, kGuard);
        void* r = cvMemset(buf.data() + kPad + off, value, n);
        bool ok = r == buf.data() + kPad + off;
        for (size_t i = 0; i < buf.size(); ++i) {
          const bool inside = i >= kPad + off && i < kPad + off + n;
          const uint8_t want = inside ? static_cast<uint8_t>(value) : kGuard;
          if (buf[i] != want) ok = false;
        }
        if (!ok) ++bad;
      }
    }
  }
  std::printf("%s memset  %d failing cases of every offset, length and value\n", bad == 0 ? "PASS" : "FAIL", bad);
  return bad == 0;
}

// 2. memcpy copies n bytes from every source alignment to every destination alignment.
bool testMemcpy() {
  int bad = 0;
  for (size_t dOff = 0; dOff < 8; ++dOff) {
    for (size_t sOff = 0; sOff < 8; ++sOff) {
      for (size_t n : lengths()) {
        std::vector<uint8_t> src(n + 16);
        fillRandom(src);
        std::vector<uint8_t> dst(n + 2 * kPad + 8, kGuard);
        void* r = cvMemcpy(dst.data() + kPad + dOff, src.data() + sOff, n);
        bool ok = r == dst.data() + kPad + dOff;
        for (size_t i = 0; i < dst.size(); ++i) {
          const bool inside = i >= kPad + dOff && i < kPad + dOff + n;
          const uint8_t want = inside ? src[sOff + i - kPad - dOff] : kGuard;
          if (dst[i] != want) ok = false;
        }
        if (!ok) ++bad;
      }
    }
  }
  std::printf("%s memcpy  %d failing cases of every destination and source alignment and length\n",
              bad == 0 ? "PASS" : "FAIL", bad);
  return bad == 0;
}

// 3. memmove matches a copy through a scratch buffer when the ranges overlap, shifted by -9..9.
bool testMemmove() {
  int bad = 0;
  for (int shift = -9; shift <= 9; ++shift) {
    for (size_t base = 0; base < 4; ++base) {
      for (size_t n : lengths()) {
        std::vector<uint8_t> buf(n + 2 * kPad + 32);
        fillRandom(buf);
        std::vector<uint8_t> want = buf;

        const size_t srcAt = kPad + 8 + base;
        const size_t dstAt = static_cast<size_t>(static_cast<int>(srcAt) + shift);
        std::vector<uint8_t> scratch(buf.begin() + static_cast<std::ptrdiff_t>(srcAt),
                                     buf.begin() + static_cast<std::ptrdiff_t>(srcAt + n));
        for (size_t i = 0; i < n; ++i) want[dstAt + i] = scratch[i];

        void* r = cvMemmove(buf.data() + dstAt, buf.data() + srcAt, n);
        if (r != buf.data() + dstAt || buf != want) ++bad;
      }
    }
  }
  std::printf("%s memmove %d failing cases of every overlap shift, alignment and length\n",
              bad == 0 ? "PASS" : "FAIL", bad);
  return bad == 0;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testMemset();
  ok &= testMemcpy();
  ok &= testMemmove();
  std::printf("%s fastmem\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
