#pragma once

#include <array>
#include <cmath>

// Keys and the major scale Autotune corrects to. The KEY knob steps round the circle of fifths
// from C, as on the Zoom V3.

namespace cv {

constexpr int kKeyCount = 12;
constexpr std::array<int, kKeyCount> kKeyRoot = {0, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10, 5};
constexpr std::array<const char*, kKeyCount> kKeyName = {"C / Am",   "G / Em",   "D / Bm",   "A / F#m",
                                                         "E / C#m",  "B / G#m",  "F# / D#m", "Db / Bbm",
                                                         "Ab / Fm",  "Eb / Cm",  "Bb / Gm",  "F / Dm"};

constexpr std::array<int, 7> kMajor = {0, 2, 4, 5, 7, 9, 11};

struct Degree {
  int index;  // 0..6 in kMajor
  float dev;  // sung pitch minus the scale note, semitones
};

// A pitch in semitones on the MIDI numbering, A4 = 69.
inline float midiOf(float hz) { return 69.0f + 12.0f * log2f(hz / 440.0f); }

// Nearest major-scale degree of the pitch midi in key `key` (a kKeyRoot index) by circular
// distance. A pitch halfway between two notes snaps to the lower one.
inline Degree degreeOfMidi(float midi, int key) {
  constexpr float kTieEps = 1e-4f;
  const int k = ((key % kKeyCount) + kKeyCount) % kKeyCount;
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

// The same for a pitch in Hz.
inline Degree degreeOf(float hz, int key) { return degreeOfMidi(midiOf(hz), key); }

// Autotune keeps its last scale note until the sung pitch sits this far past the halfway point to
// the next one, so a pitch near the halfway point holds one note instead of trilling between two.
constexpr float kHoldSemis = 0.15f;

// The scale note, as a MIDI pitch, that the pitch midi pulls to in key, given the last note it pulled
// to in the same key. The last note holds until the pitch sits more than kHoldSemis past the halfway
// point to the nearest note; then the nearest note takes over.
inline float heldNote(float midi, int key, float last) {
  const float nearest = midi - degreeOfMidi(midi, key).dev;
  return fabsf(midi - last) - fabsf(midi - nearest) <= 2.0f * kHoldSemis ? last : nearest;
}

}  // namespace cv
