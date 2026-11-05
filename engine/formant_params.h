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

// The trim of the corrected voice's share when the card cuts from Autotune's input, in dB, on top of the table
// trim and Autotune's own. The table fits the voice cut from the card's own input, and Autotune's trim lifts the
// corrected voice, so together they read about 0.3 dB over the level rule once SHIFT leaves 0 (SHIFT 0 reads on
// target). The trim ramps in over the first semitone of SHIFT.
constexpr float kFormantGroupTrimDb = -0.3f;

// How far the base and the shifted voice correlate at a SHIFT of 0 to 12 st (the sign does not matter),
// measured on Adam's three clips. The level gain starts from it when the base has sat out at MIX 100 %
// and comes back.
//
// Method (2026-11-05, with the grain group in place): each clip runs through the engine at MIX 50 %, so
// the base is heard, at every whole SHIFT from -12 to +12 st, once on its own ring and tracker and once
// directly behind a running Autotune (hard tune, key C). At the end of every block after the first 0.1 s
// of the clip, the engine's running powers are summed, the base against the voice before its trim:
// corr = sum(baseVoice) / sqrt(sum(baseSq) * sum(voiceSq)), pooled over the three clips. The table is the
// mean of the four runs at each |SHIFT| (alone and behind, both signs). The two placements agree within
// 0.03, except at SHIFT 0, where the card alone reads 0.97 and behind Autotune 1.00 (the same waveform).
// Past 3 st the correlation turns negative, to -0.18 at 6 st, and comes back to 0 near 10 st.
constexpr std::array<float, kFormantVoiceMaxShiftSt + 1> kFormantCoherence = {
    0.985f, 0.355f, 0.085f, -0.016f, -0.106f, -0.149f, -0.176f, -0.165f, -0.099f, -0.059f, -0.009f, 0.017f, 0.033f};

struct FormantTuning {
  // The shifted voice's trim in dB at each SHIFT from -7 to +7 st, fit on Adam's three clips (the cleaned set) so that
  // the mean of the voice alone at MIX 100 % lands on +0.25 dB, the middle of 0 to +0.5. The grains sum
  // to a level that follows SHIFT, and one trim cannot hold it.
  std::array<float, kFormantShiftSteps> trimDb = {4.42f, 3.93f, 3.23f, 2.52f, 2.21f, 2.02f, 1.11f, 0.26f,
                                                  1.04f, 1.56f, 1.46f, 1.68f, 1.87f, 1.66f, 1.70f};
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
