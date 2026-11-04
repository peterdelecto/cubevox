#pragma once

// Engine contract. Every header under engine/ compiles unchanged on the H7:
// float math only, std::array state, no heap and no I/O in process().

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

}  // namespace cv
