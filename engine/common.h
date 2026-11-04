#pragma once

// Engine contract. Every header under engine/ compiles unchanged on the H7:
// float math only, std::array state, no heap and no I/O in process().

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Forced inlining and no-alias pointers for hot loops.
#if defined(__GNUC__) || defined(__clang__)
#define CV_INLINE inline __attribute__((always_inline))
#define CV_RESTRICT __restrict
#else
#define CV_INLINE inline
#define CV_RESTRICT
#endif

namespace cv {

constexpr int kSampleRate = 48000;
constexpr int kBlock = 64;
constexpr float kSmpPerMs = kSampleRate / 1000.0f;

// True when two tunings, structs of whole floats with no padding, hold the same words. A NaN field
// reads as unchanged. The words are compared inline because the board's memcmp is a byte loop.
template <typename T>
CV_INLINE bool sameWords(const T& a, const T& b) {
  static_assert(std::is_trivially_copyable<T>::value && sizeof(T) % sizeof(uint32_t) == 0,
                "sameWords compares a struct of whole 32-bit words");
  uint32_t diff = 0;
  for (size_t k = 0; k < sizeof(T) / sizeof(uint32_t); ++k) {
    uint32_t x, y;
    std::memcpy(&x, reinterpret_cast<const char*>(&a) + k * sizeof x, sizeof x);
    std::memcpy(&y, reinterpret_cast<const char*>(&b) + k * sizeof y, sizeof y);
    diff |= x ^ y;
  }
  return diff == 0;
}

}  // namespace cv
