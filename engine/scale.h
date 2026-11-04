#pragma once

#include <array>
#include <cmath>

#include "engine/fastmath.h"

// Keys and the major scale Autotune corrects to. The KEY knob steps round the circle of fifths
// from C, as on the Zoom V3.

namespace cv {

constexpr int kKeyCount = 12;
constexpr std::array<int, kKeyCount> kKeyRoot = {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5};
constexpr std::array<const char*, kKeyCount> kKeyName = {"C / Am",   "G / Em",   "D / Bm",   "A / F#m",
                                                         "E / C#m",  "B / G#m",  "F# / D#m", "Db / Bbm",
                                                         "Ab / Fm",  "Eb / Cm",  "Bb / Gm",  "F / Dm"};

constexpr std::array<int, 7> kMajor = {0, 2, 4, 5, 7, 9, 11};

// A pitch in semitones on the MIDI numbering, A4 = 69. log2Fast alone is within 0.04 cents, which is
// enough to move a pitch that sits on the edge of the hold band to the other side of it, so one Newton
// step brings the logarithm to float rounding (the second-order error is under 1e-9 octave).
inline float midiOf(float hz) {
  const float x = hz / 440.0f;
  const float y = log2Fast(x);
  return 69.0f + 12.0f * (y + (x * exp2Fast(-y) - 1.0f) * kLog2E);
}

// How far the pitch midi sits above the nearest major-scale note of key `key` (a kKeyRoot index), in
// semitones, by circular distance. A pitch halfway between two notes snaps to the lower one.
inline float scaleDeviation(float midi, int key) {
  constexpr float kTieEps = 1e-4f;
  const int k = ((key % kKeyCount) + kKeyCount) % kKeyCount;
  const float above = midi - static_cast<float>(kKeyRoot[static_cast<size_t>(k)]);
  const float rel = above - 12.0f * floorf(above / 12.0f);  // [0, 12)
  float best = 0.0f;
  float bestDist = 99.0f;
  for (size_t d = 0; d < kMajor.size(); ++d) {
    float s = rel - static_cast<float>(kMajor[d]);
    if (s >= 6.0f) s -= 12.0f;
    if (s < -6.0f) s += 12.0f;
    const float dist = fabsf(s);
    const bool tie = fabsf(dist - bestDist) <= kTieEps;
    if ((dist < bestDist && !tie) || (tie && s > 0.0f)) {
      best = s;
      bestDist = dist;
    }
  }
  return best;
}

// Autotune keeps its last scale note until the sung pitch sits this far past the halfway point to
// the next one, so a pitch near the halfway point holds one note instead of trilling between two.
constexpr float kHoldSemis = 0.15f;

// The scale note, as a MIDI pitch, that the pitch midi pulls to in key, given the last note it pulled
// to in the same key. The last note holds until the pitch sits more than kHoldSemis past the halfway
// point to the nearest note; then the nearest note takes over.
inline float heldNote(float midi, int key, float last) {
  const float nearest = midi - scaleDeviation(midi, key);
  return fabsf(midi - last) - fabsf(midi - nearest) <= 2.0f * kHoldSemis ? last : nearest;
}

}  // namespace cv
