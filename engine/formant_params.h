#pragma once

#include <array>

// The Formant card's panel range and its tuning. Apraxia drives the same engine with its MIX pinned at 1.

namespace cv {

// The panel's SHIFT range, whole semitones, and the SHIFT and MIX the card starts on.
constexpr int kFormantMaxShiftSt = 7;
constexpr int kFormantShiftSteps = 2 * kFormantMaxShiftSt + 1;
constexpr int kFormantDefaultShiftSt = -3;
constexpr float kFormantDefaultMix = 1.0f;

// The voice's limit, whole semitones either way: the engine clamps the SHIFT it is handed to this, and
// the grains then play at 0.5 to 2 times, the voice's rate range. Apraxia's RANGE reaches -12 st.
constexpr int kFormantVoiceMaxShiftSt = 12;

// The averaging time of the running powers behind the Formant mix's level gain, and that gain's
// ceiling (+6 dB).
constexpr float kFormantPowerSec = 0.04f;
constexpr float kFormantLevelGainMax = 2.0f;
constexpr float kFormantPowerFloor = 1e-20f;  // below this a power is silence

// The shifted voice's trim, which the engine clamps to this many dB either way.
constexpr float kFormantMaxTrimDb = 24.0f;

// How far the base and the shifted voice correlate at a SHIFT of 0, 1, 2 and 3 st or more, measured on
// Adam's three clips. The level gain starts from it when the base has sat out at MIX 100 % and comes
// back.
constexpr std::array<float, 4> kFormantCoherence = {0.95f, 0.4f, 0.1f, 0.0f};

struct FormantTuning {
  // The shifted voice's trim in dB at each SHIFT from -7 to +7 st, fit on Adam's three clips so that
  // the mean of the voice alone at MIX 100 % lands on +0.25 dB, the middle of 0 to +0.5. The grains sum
  // to a level that follows SHIFT, and one trim cannot hold it.
  std::array<float, kFormantShiftSteps> trimDb = {4.29f, 3.87f, 3.12f, 2.36f, 1.98f, 1.72f, 1.01f, 0.20f,
                                                  0.94f, 1.44f, 1.51f, 1.77f, 1.87f, 1.60f, 1.64f};
  float offsetDb = 0.0f;  // added to every entry, for the render tool

  bool operator==(const FormantTuning& o) const { return trimDb == o.trimDb && offsetDb == o.offsetDb; }
};

struct FormantParams {
  bool on = false;
  int shiftSt = kFormantDefaultShiftSt;  // knob A (SHIFT) -7..+7 st, Apraxia -12..+6 st; the grains play at 2^(shiftSt / 12)
  float mix = kFormantDefaultMix;        // knob B (MIX), 0..1
  FormantTuning tuning;
};

}  // namespace cv
