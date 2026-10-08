#pragma once

// Engine contract. Every header under engine/ compiles unchanged on the H7:
// float math only, std::array state, no heap and no I/O in process().

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
// higher. DWELL then saturates only near the top of its travel.
inline float softClipHeadroom(float x, float h) { return h * softClip(x / h); }

// Headroom for the CHASM and SPRING B input drive (drive 1.5), set so a 0 dBFS
// voice at DWELL 0.2 enters the curve at about a third of its knee.
constexpr float kChasmClipHeadroom = 4.5f;

}  // namespace cv
