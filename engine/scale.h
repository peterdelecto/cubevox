#pragma once

#include <array>
#include <cmath>

// Keys and the major scale Autotune corrects to. The KEY knob steps round the circle of fifths
// from C, as on the Zoom V3.

namespace cv {

constexpr int kKeyCount = 12;
constexpr std::array<int, kKeyCount> kKeyRoot = {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5};
constexpr const char* kKeyName[kKeyCount] = {"C / Am",   "G / Em",   "D / Bm",   "A / F#m",
                                             "E / C#m",  "B / G#m",  "F# / D#m", "Db / Bbm",
                                             "Ab / Fm",  "Eb / Cm",  "Bb / Gm",  "F / Dm"};

constexpr std::array<int, 7> kMajor = {0, 2, 4, 5, 7, 9, 11};

struct Degree {
  int index;  // 0..6 in kMajor
  float dev;  // sung pitch minus the scale note, semitones
};

// Nearest major-scale degree of hz in key `key` (a kKeyRoot index) by circular distance. A pitch
// halfway between two notes snaps to the lower one.
inline Degree degreeOf(float hz, int key) {
  constexpr float kTieEps = 1e-4f;
  const int k = ((key % kKeyCount) + kKeyCount) % kKeyCount;
  const float midi = 69.0f + 12.0f * log2f(hz / 440.0f);
  float rel = fmodf(midi - static_cast<float>(kKeyRoot[static_cast<size_t>(k)]), 12.0f);
  if (rel < 0.0f) rel += 12.0f;
  Degree best{0, 0.0f};
  float bestDist = 99.0f;
  for (int d = 0; d < 7; ++d) {
    float s = rel - static_cast<float>(kMajor[static_cast<size_t>(d)]);
    if (s >= 6.0f) s -= 12.0f;
    if (s < -6.0f) s += 12.0f;
    const float dist = fabsf(s);
    const bool tie = fabsf(dist - bestDist) <= kTieEps;
    if ((dist < bestDist && !tie) || (tie && s > 0.0f)) {
      best = {d, s};
      bestDist = dist;
    }
  }
  return best;
}

}  // namespace cv
