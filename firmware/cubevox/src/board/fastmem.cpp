// Word-wise memset, memcpy and memmove for the board.
//
// newlib-nano ships all three as byte loops, 4 to 5 instructions and about 5 cycles a byte on the
// M7, so one 51 kB clear took over 250k cycles. These versions replace them at link time (the
// sketch's objects come before libc, so the archive members never load) and move a word a store.
// Every std::fill, std::copy, array fill and struct copy in the firmware that GCC turns into a call
// gets the speed-up.
//
// Each routine aligns the destination with a byte head, moves 16 bytes a pass, finishes whole
// words, then bytes. A source that is not word aligned is read with unaligned loads, which the
// M7 allows on Normal memory (RAM, flash, and RAM_D2 as the MPU sets it up in block_mem.cpp).
//
// GCC would turn the byte and word loops below back into calls to memset and memcpy, which would
// recurse, so loop-to-call conversion is off for this file.

#include <stddef.h>
#include <stdint.h>

#pragma GCC optimize("no-tree-loop-distribute-patterns")

namespace {

using AlignedWord = uint32_t __attribute__((may_alias));

inline bool isWordAligned(const void* p) { return (reinterpret_cast<uintptr_t>(p) & 3u) == 0; }

// One source word at word index i from p. The unaligned form goes through a 4-byte builtin
// memcpy, which GCC compiles to a single unaligned LDR. A typedef carrying aligned(1) cannot do
// this job because a template argument drops its attributes.
template <bool kAligned>
inline uint32_t loadWord(const uint8_t* p, ptrdiff_t i) {
  if constexpr (kAligned) {
    return reinterpret_cast<const AlignedWord*>(p)[i];
  } else {
    uint32_t v;
    __builtin_memcpy(&v, p + 4 * i, 4);
    return v;
  }
}

// Copies n bytes from the low end up. Safe when dst <= src, even if the ranges overlap, because
// each pass loads all 16 bytes before it stores any of them. The caller has made d word aligned.
template <bool kSrcAligned>
void copyUp(uint8_t* d, const uint8_t* s, size_t n) {
  auto* dw = reinterpret_cast<AlignedWord*>(d);

  for (; n >= 16; n -= 16, dw += 4, s += 16) {
    const uint32_t a = loadWord<kSrcAligned>(s, 0);
    const uint32_t b = loadWord<kSrcAligned>(s, 1);
    const uint32_t c = loadWord<kSrcAligned>(s, 2);
    const uint32_t e = loadWord<kSrcAligned>(s, 3);
    dw[0] = a;
    dw[1] = b;
    dw[2] = c;
    dw[3] = e;
  }
  for (; n >= 4; n -= 4, s += 4) *dw++ = loadWord<kSrcAligned>(s, 0);

  d = reinterpret_cast<uint8_t*>(dw);
  while (n--) *d++ = *s++;
}

// Copies n bytes from the high end down, where d and s point one past the last byte. Safe when
// dst > src. The caller has made d word aligned.
template <bool kSrcAligned>
void copyDown(uint8_t* d, const uint8_t* s, size_t n) {
  auto* dw = reinterpret_cast<AlignedWord*>(d);

  for (; n >= 16; n -= 16, dw -= 4, s -= 16) {
    const uint32_t a = loadWord<kSrcAligned>(s, -1);
    const uint32_t b = loadWord<kSrcAligned>(s, -2);
    const uint32_t c = loadWord<kSrcAligned>(s, -3);
    const uint32_t e = loadWord<kSrcAligned>(s, -4);
    dw[-1] = a;
    dw[-2] = b;
    dw[-3] = c;
    dw[-4] = e;
  }
  for (; n >= 4; n -= 4) {
    s -= 4;
    *--dw = loadWord<kSrcAligned>(s, 0);
  }

  d = reinterpret_cast<uint8_t*>(dw);
  while (n--) *--d = *--s;
}

}  // namespace

extern "C" {

void* memset(void* dst, int c, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  const auto b = static_cast<uint8_t>(c);

  if (n >= 8) {
    while (!isWordAligned(d)) {
      *d++ = b;
      --n;
    }

    const uint32_t w = b * 0x01010101u;
    auto* dw = reinterpret_cast<AlignedWord*>(d);

    for (; n >= 16; n -= 16, dw += 4) {
      dw[0] = w;
      dw[1] = w;
      dw[2] = w;
      dw[3] = w;
    }
    for (; n >= 4; n -= 4) *dw++ = w;

    d = reinterpret_cast<uint8_t*>(dw);
  }
  while (n--) *d++ = b;
  return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  auto* s = static_cast<const uint8_t*>(src);

  if (n >= 8) {
    while (!isWordAligned(d)) {
      *d++ = *s++;
      --n;
    }
    if (isWordAligned(s)) {
      copyUp<true>(d, s, n);
    } else {
      copyUp<false>(d, s, n);
    }
    return dst;
  }
  while (n--) *d++ = *s++;
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  auto* d = static_cast<uint8_t*>(dst);
  auto* s = static_cast<const uint8_t*>(src);

  // A copy from the low end up is safe unless dst lies inside the source range.
  if (d <= s || d >= s + n) return memcpy(dst, src, n);

  d += n;
  s += n;
  if (n >= 8) {
    while (!isWordAligned(d)) {
      *--d = *--s;
      --n;
    }
    if (isWordAligned(s)) {
      copyDown<true>(d, s, n);
    } else {
      copyDown<false>(d, s, n);
    }
    return dst;
  }
  while (n--) *--d = *--s;
  return dst;
}

}  // extern "C"
