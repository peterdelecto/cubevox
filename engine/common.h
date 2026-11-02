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

constexpr float kClipMax = 3.0f;

// Padé tanh, clamped where it meets the rail.
inline float softClip(float x) {
  x = x < -kClipMax ? -kClipMax : (x > kClipMax ? kClipMax : x);
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// The same curve with headroom h: unity gain below the knee, which sits h times
// higher, so a reverb's input drive saturates only near the top of its range. With a constant h the
// compiler folds 1 / h, so the input scale costs a multiply.
inline float softClipHeadroom(float x, float h) { return h * softClip(x * (1.0f / h)); }

// Headroom for both reverbs' input drive, set so a 0 dBFS voice at each engine's default dwell of
// 0.2 enters the curve at about a third of its knee. Chasm's drive is 8^0.2 = 1.52, which is 0.34
// of the knee. Spring's is 4^0.2 = 1.32, which is 0.29.
constexpr float kReverbClipHeadroom = 4.5f;

}  // namespace cv
