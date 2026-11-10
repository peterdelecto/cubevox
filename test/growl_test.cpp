// Growl card checks: off is a bit-exact copy, the voice drops an octave, the wave's depth falls without
// a pitch, the throat follows each syllable, plucks on a soft note and rests in silence, MIX 100 % is the plain card and MIX 0 is
// the input at the voice's lag, the output stays finite at every knob corner, the level at the default
// (GROWL 75 %, MIX 100 %) on Adam's clips, and the fold's alias level at DRIVE 100 % on a sung note. GROWL 0
// is pinned by hash, the peak reads +10 dB at 2.5 kHz at GROWL 100 %, the lopsided clip lifts the fold's 2nd
// harmonic and leaves no DC, the jitter repeats from reset, the wet
// path's high-pass cuts 55 Hz by 10.7 dB, the throat drop leaves the heard pitch within 0.1 st, and an onset
// leaves the pitch ratio, rasp rate and read lag alone. The card grows what reaches its slot, so an Octave
// at +12 st ahead of it leaves the raw voice's pitch (the octave-up voice dropped by pitchSt), a Formant ahead
// moves the output's spectral centroid with SHIFT as Formant's own output does, and a Unison ahead is in the
// wet. The Octave and Autotune latencies the card reads are measured, a stopped card clears its ring a slice
// a block, and with Autotune ahead the heard pitch and the level stay where the card had them. Each run feeds a
// pitch series first, so the tracker runs, as the chain does. The clip rows skip
// when samples/ is absent. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "engine/growl.h"
#include "engine/unison.h"

#include "clip_util.h"
#include "pitch_series.h"
#include "ringed_tracker.h"
#include "signal_util.h"

namespace {

using testutil::Signal;
constexpr double kPi = testutil::kPi;
constexpr int kRate = cv::kSampleRate;

bool report(const char* name, bool ok, const char* detail) {
  std::printf("%s %-28s %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// The chain's order for a Growl card behind the pitch cards: Autotune, Formant and Octave run in series as
// `stage` says, each on the output of the one before, and the card runs on the series' output with the
// tracker, shift and delay the series hands it. `autotunePlaced` is true in a menu layout, where the Autotune
// card sits ahead with its toggle on or off and its tracker serves the card, and false in a layout with no
// Autotune card, where the Growl card runs a tracker of its own. `heard` keeps the series' output, the
// card's input.
struct Rig {
  explicit Rig(bool autotunePlaced = true) : pitch(std::make_unique<PitchSeries>(autotunePlaced)) {}

  std::unique_ptr<PitchSeries> pitch;
  std::unique_ptr<cv::Growl> growl = std::make_unique<cv::Growl>();
  cv::PitchCardParams stage;
  Signal heard;
  std::vector<float> latency;  // the series' outputLatency() after each block

  // One block through the series and the card.
  void step(const float* in, float* out, int n, const cv::GrowlParams& g) {
    pitch->process(in, out, n, stage, g.on);
    growl->process(out, out, n, g, pitch->upstream());
  }

  // in through the series and the card, with the card's params from `at(sample)` per block.
  template <typename ParamsAt>
  Signal run(const Signal& in, ParamsAt at) {
    Signal out(in.size());
    heard.assign(in.size(), 0.0f);
    latency.clear();
    for (size_t i = 0; i < in.size(); i += cv::kBlock) {
      const int n = static_cast<int>(in.size() - i < cv::kBlock ? in.size() - i : cv::kBlock);
      const cv::GrowlParams g = at(i);
      pitch->process(&in[i], &out[i], n, stage, g.on);
      std::copy(&out[i], &out[i] + n, &heard[i]);
      latency.push_back(pitch->outputLatency());
      growl->process(&out[i], &out[i], n, g, pitch->upstream());
    }
    return out;
  }

  Signal run(const Signal& in, const cv::GrowlParams& g) {
    return run(in, [&g](size_t) { return g; });
  }
};

// The engine's rasp and fold, not the pot. The pot map is tested with the registry.
cv::GrowlParams growlAt(float growl, float drive, bool on = true, float mix = 1.0f) {
  cv::GrowlParams g;
  g.on = on;
  g.growl = growl;
  g.drive = drive;
  g.mix = mix;
  return g;
}

// The engine at a GROWL pot position and a MIX, through the same map the registry applies.
cv::GrowlParams potAt(float pot, float mix) {
  return growlAt(pot * cv::kGrowlRaspTop, pot * cv::kGrowlDriveTop, true, mix);
}

// A sung note: harmonics at 1/k up to 5 kHz, a 5 Hz vibrato of +-20 cents and a phrase envelope.
Signal sungNote(double hz, double seconds, double dbfs) {
  const size_t n = static_cast<size_t>(seconds * kRate);
  Signal x(n);
  double phase = 0.0;
  double sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    phase += 2.0 * kPi * hz * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0) / kRate;
    double v = 0.0;
    for (int k = 1; k * hz < 5000.0; ++k) v += std::sin(k * phase) / k;
    x[i] = static_cast<float>(v);
    sum += v * v;
  }
  const double g = std::pow(10.0, dbfs / 20.0) / std::sqrt(sum / static_cast<double>(n));
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

double rms(const Signal& x, size_t from, size_t to) { return testutil::rmsOf(x, from, to); }

// Adam's three clips, each behind 0.4 s of silence so the card's crossfade settles first. Empty when
// samples/ is absent.
constexpr size_t kPad = kRate * 2 / 5;
std::vector<Signal> loadClips(bool& whole) { return ::clips::load(CUBEVOX_SAMPLES_DIR, whole); }

// Off, the card is the stage's output bit for bit, and so it is again once a switch-off has settled.
bool checkOff() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig rig;
  const Signal off = rig.run(in, growlAt(0.5f, 0.5f, false));
  bool exact = off == in;
  Rig offMixed;
  exact = exact && offMixed.run(in, growlAt(0.5f, 0.5f, false, 0.3f)) == in;

  Rig flip;
  const size_t offAt = in.size() / 2;
  const Signal out = flip.run(in, [offAt](size_t i) { return growlAt(1.0f, 1.0f, i < offAt); });
  const size_t settled = offAt + kRate / 10;  // the 20 ms crossfade with room to spare
  size_t differ = 0;
  for (size_t i = settled; i < in.size(); ++i) differ += out[i] != in[i] ? 1 : 0;
  exact = exact && differ == 0;
  char detail[96];
  std::snprintf(detail, sizeof detail, "never on and 100 ms after a switch-off: %zu samples differ (0)", differ);
  return report("off is a copy", exact, detail);
}

// The voice sits an octave under the note: the tracker reads twice the input's period on the output at
// GROWL 0 and DRIVE 0.
bool checkDrop() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig rig;
  const Signal out = rig.run(in, growlAt(0.0f, 0.0f));
  RingedTracker heard;
  double sum = 0.0;
  int frames = 0;
  for (size_t i = 0; i + cv::kBlock <= out.size(); i += cv::kBlock) {
    heard.push(&out[i], cv::kBlock);
    if (i > static_cast<size_t>(kRate) && heard.result().voiced) {
      sum += heard.result().hz;
      ++frames;
    }
  }
  const double hz = frames > 0 ? sum / frames : 0.0;
  char detail[96];
  std::snprintf(detail, sizeof detail, "220 Hz in reads %.1f Hz out over %d voiced frames (110 +- 3)", hz, frames);
  return report("octave down", frames > 0 && std::fabs(hz - 110.0) < 3.0, detail);
}

// The wave's depth falls to 0 within 20 ms of the tracker losing the pitch, and is 1 on a note.
bool checkDepthFade() {
  Signal in = sungNote(220.0, 1.0, -20.0);
  const Signal noise = testutil::noise(static_cast<size_t>(kRate / 2), 0.05f);
  in.insert(in.end(), noise.begin(), noise.end());
  Rig rig;
  float onNote = 0.0f;
  bool fellInTime = true;
  size_t lostAt = 0;
  Signal out(in.size());
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    const cv::GrowlParams g = growlAt(1.0f, 0.5f);
    rig.step(&in[i], &out[i], cv::kBlock, g);
    if (i == static_cast<size_t>(kRate) - cv::kBlock) onNote = rig.growl->depthFade();
    if (i >= static_cast<size_t>(kRate) && lostAt == 0 && !rig.pitch->pitch().voiced) lostAt = i;
    if (lostAt > 0 && i >= lostAt + static_cast<size_t>(20 * kRate / 1000) + cv::kBlock) {
      fellInTime = fellInTime && rig.growl->depthFade() == 0.0f;
    }
  }
  char detail[96];
  std::snprintf(detail, sizeof detail, "on the note %.2f (1), 0 within 20 ms of the pitch going: %s", onNote,
                lostAt > 0 && fellInTime ? "yes" : "no");
  return report("depth fade", onNote == 1.0f && lostAt > 0 && fellInTime, detail);
}

// Every knob corner, on the synthetic note and on Adam's clips, gives finite output.
bool checkCorners(const std::vector<Signal>& voices) {
  std::vector<Signal> inputs = voices;
  inputs.push_back(sungNote(180.0, 2.0, -18.0));
  size_t bad = 0;
  for (const Signal& in : inputs) {
    for (const float pot : {0.0f, 1.0f}) {
      for (const float mix : {0.0f, 0.5f, 1.0f}) {
        Rig rig;
        for (const float v : rig.run(in, potAt(pot, mix))) bad += std::isfinite(v) ? 0 : 1;
      }
    }
    // The engine's own corners, rasp and fold apart, and a MIX that flips every block.
    for (const float growl : {0.0f, 1.0f}) {
      for (const float drive : {0.0f, 1.0f}) {
        Rig rig;
        for (const float v : rig.run(in, growlAt(growl, drive))) bad += std::isfinite(v) ? 0 : 1;
      }
    }
    Rig flip;
    const auto flipMix = [](size_t i) { return potAt(1.0f, (i / cv::kBlock) % 2 == 0 ? 0.0f : 1.0f); };
    for (const float v : flip.run(in, flipMix)) bad += std::isfinite(v) ? 0 : 1;
  }
  char detail[112];
  std::snprintf(detail, sizeof detail, "%zu inputs at the pot and MIX corners and a flipping MIX: %zu samples not finite (0)",
                inputs.size(), bad);
  return report("finite at the corners", bad == 0, detail);
}

// The mean level over Adam's clips of the output against the input, over the clip alone, in dB.
double meanLevelDb(const std::vector<Signal>& voices, float pot, float mix, const char* label) {
  double sum = 0.0;
  for (const Signal& in : voices) {
    Rig rig;
    const Signal out = rig.run(in, potAt(pot, mix));
    const double db = 20.0 * std::log10(rms(out, kPad, out.size()) / rms(in, kPad, in.size()));
    std::printf("NOTE level %-12s clip           %+6.2f dB\n", label, db);
    sum += db;
  }
  return sum / static_cast<double>(voices.size());
}

// The level rule at the default, GROWL 75 % and MIX 100 %: the mean over Adam's clips of the output
// against the input, over the clip alone, lands 0 to +0.5 dB. GROWL 100 % and MIX 50 % are reported
// beside it.
bool checkLevel(const std::vector<Signal>& voices, bool whole) {
  if (!whole) return report("level 75/100", false, "a clip in samples/ is missing or empty");
  if (voices.empty()) return report("level 75/100", true, "SKIP, samples/ is not in this checkout");
  const double mean = meanLevelDb(voices, cv::kGrowlDefaultPot, 1.0f, "75/100");
  const double top = meanLevelDb(voices, 1.0f, 1.0f, "100/100");
  const double half = meanLevelDb(voices, cv::kGrowlDefaultPot, 0.5f, "75/50");
  std::printf("NOTE level GROWL 100 %% MIX 100 %%: mean %+.2f dB\n", top);
  std::printf("NOTE level GROWL 75 %% MIX 50 %%: mean %+.2f dB\n", half);
  char detail[96];
  std::snprintf(detail, sizeof detail, "mean of %zu clips %+.2f dB (0 to +0.5)", voices.size(), mean);
  return report("level 75/100", mean >= 0.0 && mean <= 0.5, detail);
}

// What the card's throat did over one run, a point per block.
struct ThroatTrace {
  std::vector<float> target;  // the throat the sweep asked for, st
  std::vector<float> glided;  // the throat the voice played, st
  std::vector<float> sweep;   // the syllable sweep, 0 to 1
};

ThroatTrace traceThroat(const Signal& in, const cv::GrowlParams& g) {
  Rig rig;
  ThroatTrace t;
  Signal out(in.size());
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) {
    rig.step(&in[i], &out[i], cv::kBlock, g);
    t.target.push_back(rig.growl->throatTargetSt());
    t.glided.push_back(rig.growl->throatSt());
    t.sweep.push_back(rig.growl->sweep());
  }
  return t;
}

// 0.3 s of silence, a loud sung note, then silence.
constexpr double kNoteSec = 1.5;
constexpr double kRestSec = 1.2;

// A soft note's RMS: its peaks sit about 5 dB over it, inside the drive's window and under its 0.5 onset at
// the default SENSITIVITY (the window runs -30 to -18 dBFS).
constexpr double kSoftNoteDbfs = -32.0;

Signal noteThenSilence(double dbfs = -14.0) {
  Signal in(static_cast<size_t>(0.3 * kRate), 0.0f);
  const Signal note = sungNote(220.0, kNoteSec, dbfs);
  in.insert(in.end(), note.begin(), note.end());
  in.insert(in.end(), static_cast<size_t>(kRestSec * kRate), 0.0f);
  return in;
}

// The deepest throat the GROWL pot reaches: the engine's rasp tops at kGrowlRaspTop, so -5 - 7 x 0.8 st.
float throatAtPotTop() {
  const cv::GrowlTuning t;
  return t.formantSt + t.throatDropTop * cv::kGrowlRaspTop;
}

// GROWL 0: the throat stays at -5 st through a note. The GROWL pot at 100 %: an onset drops the throat to
// within 0.05 st of -10.6 st, and it is back at -5 st within DECAY while the note holds. Both are the throat
// the sweep asks for. The voice plays it through the 20 ms glide, which comes within 0.05 st of -5 st again
// 100 ms after the return.
bool checkThroat() {
  const Signal in = noteThenSilence();
  const size_t noteEnd = static_cast<size_t>((0.3 + kNoteSec) * kRate) / cv::kBlock;
  const float rest = cv::GrowlTuning{}.formantSt;
  const float decayBlocks = cv::GrowlTuning{}.decayMs * cv::kSmpPerMs / cv::kBlock;
  const float deepest = throatAtPotTop();

  const ThroatTrace flat = traceThroat(in, growlAt(0.0f, 0.5f));
  float flatWorst = 0.0f;
  for (size_t b = 0; b < noteEnd; ++b) {
    if (flat.sweep[b] > 0.0f || flat.glided[b] != 0.0f) flatWorst = std::fmax(flatWorst, std::fabs(flat.glided[b] - rest));
  }
  const bool flatOk = flatWorst < 1e-4f;

  const ThroatTrace deep = traceThroat(in, potAt(1.0f, 1.0f));
  float low = 0.0f, lowGlided = 0.0f;
  size_t lowAt = 0;
  for (size_t b = 0; b < noteEnd; ++b) {
    if (deep.target[b] < low) {
      low = deep.target[b];
      lowAt = b;
    }
    lowGlided = std::fmin(lowGlided, deep.glided[b]);
  }
  size_t backAt = 0;
  for (size_t b = lowAt; b < noteEnd; ++b) {
    if (deep.target[b] > rest - 0.05f) {
      backAt = b;
      break;
    }
  }
  const float returnBlocks = static_cast<float>(backAt - lowAt);
  const size_t holdFrom = std::min(noteEnd, backAt + static_cast<size_t>(100 * cv::kSmpPerMs / cv::kBlock) + 1);
  float heldWorst = 0.0f;
  for (size_t b = holdFrom; b < noteEnd; ++b) heldWorst = std::fmax(heldWorst, std::fabs(deep.glided[b] - rest));
  char detail[200];
  std::snprintf(detail, sizeof detail,
                "GROWL 0 holds %.4f st off -5; the pot at 100 %% drops to %.3f st (%.2f, glided %.2f), back at -5 after %.0f ms "
                "(<= %.0f), then %.3f st off -5",
                static_cast<double>(flatWorst), static_cast<double>(low), static_cast<double>(deepest),
                static_cast<double>(lowGlided), static_cast<double>(returnBlocks * cv::kBlock / cv::kSmpPerMs),
                static_cast<double>((decayBlocks + 1.0f + 15.0f * cv::kSmpPerMs / cv::kBlock) * cv::kBlock / cv::kSmpPerMs),
                static_cast<double>(heldWorst));
  const bool deepOk = low < deepest + 0.05f && lowGlided > deepest - 0.05f && backAt > lowAt &&
                      returnBlocks <= decayBlocks + 1.0f + 15.0f * cv::kSmpPerMs / cv::kBlock && heldWorst < 0.05f;
  return report("throat follows syllable", flatOk && deepOk, detail);
}

// The depth at a GROWL pot position: the deepest throat the sweep asks for on the note, -5 - 7 x 0.8 x pot st.
bool checkThroatDepth() {
  const Signal in = noteThenSilence();
  const size_t noteEnd = static_cast<size_t>((0.3 + kNoteSec) * kRate) / cv::kBlock;
  char detail[200] = "";
  bool ok = true;
  size_t used = 0;
  for (const float pot : {0.25f, 0.5f, 1.0f}) {
    const ThroatTrace t = traceThroat(in, potAt(pot, 1.0f));
    float low = 0.0f;
    for (size_t b = 0; b < noteEnd; ++b) low = std::fmin(low, t.target[b]);
    const float want = -5.0f - 7.0f * cv::kGrowlRaspTop * pot;
    ok = ok && std::fabs(low - want) < 0.05f;
    used += static_cast<size_t>(std::snprintf(detail + used, sizeof detail - used, "GROWL %.0f %%: %.2f st (%.2f); ",
                                              static_cast<double>(pot * 100.0f), static_cast<double>(low),
                                              static_cast<double>(want)));
  }
  return report("throat depth per GROWL", ok, detail);
}

// A soft note whose drive stays under the 0.5 onset at the default SENSITIVITY: the note's first block with
// a drive above 0 starts the sweep, so the throat dips and is back within 0.05 st of -5 st from DECAY after
// that block while the note holds, the voice within 0.05 st five glide times later.
bool checkThroatSoftNote() {
  const Signal in = noteThenSilence(kSoftNoteDbfs);
  const size_t noteEnd = static_cast<size_t>((0.3 + kNoteSec) * kRate) / cv::kBlock;
  const size_t decayBlocks = static_cast<size_t>(cv::GrowlTuning{}.decayMs * cv::kSmpPerMs / cv::kBlock) + 1;
  const size_t glideBlocks = static_cast<size_t>(100 * cv::kSmpPerMs / cv::kBlock);
  const float rest = cv::GrowlTuning{}.formantSt;
  const ThroatTrace t = traceThroat(in, potAt(1.0f, 1.0f));
  size_t start = 0;
  while (start < noteEnd && !(t.sweep[start] > 0.0f)) ++start;
  float sweepPeak = 0.0f, dip = rest, held = 0.0f, heldGlided = 0.0f;
  for (size_t b = start; b < noteEnd; ++b) {
    sweepPeak = std::fmax(sweepPeak, t.sweep[b]);
    dip = std::fmin(dip, t.target[b]);
    if (b >= start + decayBlocks) held = std::fmax(held, std::fabs(t.target[b] - rest));
    if (b >= start + decayBlocks + glideBlocks) heldGlided = std::fmax(heldGlided, std::fabs(t.glided[b] - rest));
  }
  char detail[200];
  std::snprintf(detail, sizeof detail,
                "sweep peak %.3f (< 0.5), dips to %.2f st, after DECAY %.4f st off -5, the voice %.4f st (<= 0.05)",
                static_cast<double>(sweepPeak), static_cast<double>(dip), static_cast<double>(held),
                static_cast<double>(heldGlided));
  return report("throat plucks on a soft note", sweepPeak > 0.0f && sweepPeak < 0.5f && dip < rest - 0.5f &&
                                                    held < 0.05f && heldGlided < 0.05f,
                detail);
}

// After the note the throat rests at -5 st and the sweep reads 0. Room noise 50 dB under full scale
// fires no onset either, so the sweep stays 0 through it.
bool checkThroatSilence() {
  Signal in = noteThenSilence();
  const size_t quietFrom = static_cast<size_t>((0.3 + kNoteSec + 0.6) * kRate);
  const Signal hiss = testutil::noise(in.size() - quietFrom, 0.003f);
  std::copy(hiss.begin(), hiss.end(), in.begin() + static_cast<std::ptrdiff_t>(quietFrom));
  const ThroatTrace t = traceThroat(in, growlAt(1.0f, 0.5f));
  const float rest = cv::GrowlTuning{}.formantSt;
  float sweepMax = 0.0f, offRest = 0.0f;
  for (size_t b = quietFrom / cv::kBlock; b < t.target.size(); ++b) {
    sweepMax = std::fmax(sweepMax, t.sweep[b]);
    offRest = std::fmax(offRest, std::fabs(t.glided[b] - rest));
  }
  char detail[120];
  std::snprintf(detail, sizeof detail, "after the note and through noise: sweep peak %.4f (0), throat %.4f st off -5 (<= 0.05)",
                static_cast<double>(sweepMax), static_cast<double>(offRest));
  return report("throat rests in silence", sweepMax == 0.0f && offRest < 0.05f, detail);
}

// The sweep's extremes (SENSITIVITY and DECAY at their bounds, the throat drop at both signs) give finite output.
bool checkThroatTuning() {
  const Signal in = noteThenSilence();
  size_t bad = 0;
  int runs = 0;
  for (const float sens : {0.0f, 1.0f}) {
    for (const float decay : {cv::kSweepMinDecayMs, cv::kSweepMaxDecayMs}) {
      for (const float drop : {-7.0f, -12.0f, 7.0f}) {
        cv::GrowlParams g = growlAt(1.0f, 1.0f);
        g.tuning.sensitivity = sens;
        g.tuning.decayMs = decay;
        g.tuning.throatDropTop = drop;
        Rig rig;
        for (const float v : rig.run(in, g)) bad += std::isfinite(v) ? 0 : 1;
        ++runs;
      }
    }
  }
  char detail[96];
  std::snprintf(detail, sizeof detail, "%d tuning corners at GROWL 100 %%, DRIVE 100 %%: %zu samples not finite (0)", runs, bad);
  return report("finite at the sweep corners", bad == 0, detail);
}

// The fold at DRIVE 100 % on a sung note, the A3 Adam sings dropped an octave: harmonics at 1/k up to
// 8 kHz at -20 dBFS RMS. The note repeats exactly every 8192 samples (19 periods of 111.3 Hz), so its
// harmonics sit on every 19th bin of an 8192-point FFT with no leakage, and so does all that the fold
// makes from them in continuous time. A harmonic past 24 kHz folds onto a bin off that grid. The alias
// level is the power off the grid against the power on it.
bool checkAlias() {
  constexpr size_t kN = 8192;
  constexpr int kPeriods = 19;
  const double f0 = static_cast<double>(kPeriods) * kRate / kN;
  const size_t total = kRate + kN;
  Signal x(total);
  for (size_t i = 0; i < total; ++i) {
    double v = 0.0;
    for (int k = 1; k * f0 < 8000.0; ++k) {
      v += std::sin(2.0 * kPi * k * kPeriods * static_cast<double>(i % kN) / kN + 0.3 * k * k) / k;
    }
    x[i] = static_cast<float>(v);
  }
  const double g = std::pow(10.0, -20.0 / 20.0) / rms(x, 0, kN);
  for (float& v : x) v = static_cast<float>(v * g);

  cv::growl_detail::Fold fold;
  fold.setTuning(cv::GrowlTuning{});
  const cv::growl_detail::FoldGains gains = fold.gainsAt(1.0f);
  for (size_t i = 0; i < total; i += cv::kBlock) fold.run(&x[i], cv::kBlock, gains, cv::growl_detail::FoldGains{0, 0, 0, 0, 0});

  std::vector<std::complex<double>> spec(kN);
  for (size_t i = 0; i < kN; ++i) spec[i] = x[total - kN + i];
  testutil::fft(spec);
  double on = 0.0, off = 0.0, peakOff = 0.0, fundamental = 0.0;
  for (size_t k = 1; k <= kN / 2; ++k) {
    const double p = std::norm(spec[k]);
    if (k % kPeriods == 0) {
      on += p;
      if (k == kPeriods) fundamental = p;
    } else {
      off += p;
      peakOff = std::fmax(peakOff, p);
    }
  }
  const double aliasDb = 10.0 * std::log10(off / on);
  const double peakDb = 10.0 * std::log10(peakOff / fundamental);
  std::printf("NOTE fold alias at DRIVE 100 %%: %+.1f dB of the note's power, the strongest alias line %+.1f dB under "
              "the fundamental (%.1f Hz)\n",
              aliasDb, peakDb, f0);
  char detail[96];
  std::snprintf(detail, sizeof detail, "%.1f dB, measured and finite", aliasDb);
  return report("fold alias", std::isfinite(aliasDb) && on > 0.0, detail);
}

// FNV-1a over the samples' bit patterns.
uint64_t hashOf(const Signal& x) {
  uint64_t h = 1469598103934665603ull;
  for (const float v : x) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    for (int s = 0; s < 32; s += 8) h = (h ^ ((bits >> s) & 0xffu)) * 1099511628211ull;
  }
  return h;
}

constexpr uint64_t kFullMixHash = 0x93b01d5392e7a07eull;

// MIX at its default 100 % is the plain card. The hash is the output on a sung note at GROWL 0.80 and
// DRIVE 0.65, and it moves with any change to the voice, the rasp or the fold. It also moves with the
// ring's length, which sets how the grains' read positions round (the output differs by -88 dB of the
// signal between a ring of 3,852 samples and one of 12,756).
bool checkFullMix() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig rig;
  const uint64_t h = hashOf(rig.run(in, growlAt(0.80f, 0.65f)));
  std::printf("NOTE full-mix hash %016llx\n", static_cast<unsigned long long>(h));
  char detail[96];
  std::snprintf(detail, sizeof detail, "output hash %016llx", static_cast<unsigned long long>(h));
  return report("MIX 100 % is the plain card", h == kFullMixHash, detail);
}

// With no Autotune card ahead the card runs a tracker of its own, and its output is the one it gives behind
// an Autotune card whose toggle is off and whose tracker serves it: both trackers hear the same input.
bool checkOwnTracker() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig alone(false);
  Rig behind(true);
  const Signal own = alone.run(in, growlAt(0.80f, 0.65f));
  const Signal shared = behind.run(in, growlAt(0.80f, 0.65f));
  char detail[96];
  std::snprintf(detail, sizeof detail, "own tracker %016llx, Autotune's tracker %016llx",
                static_cast<unsigned long long>(hashOf(own)), static_cast<unsigned long long>(hashOf(shared)));
  return report("own tracker", own == shared && hashOf(own) == kFullMixHash, detail);
}

// MIX 0 is the card's input delayed by the voice's lag. A mirror of the card's ring is read at the
// delay the card reports, over every block where that delay held still, and the delay itself must be the
// one the voice plays at (with nothing ahead of the card, the grain delay plus a period, clamped as the
// voice clamps it).
bool checkMixZero() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig rig;
  std::vector<float> mirror(static_cast<size_t>(cv::growl_detail::kRingLen), 0.0f);
  Signal out(in.size());
  double worst = 0.0;
  size_t compared = 0, blocks = 0;
  float period = 0.0f;
  float lag = 0.0f;
  bool lagHeld = false;
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) {
    const cv::GrowlParams g = potAt(1.0f, 0.0f);
    const float lagBefore = rig.growl->dryLagSamples();
    rig.step(&in[i], &out[i], cv::kBlock, g);
    for (size_t j = 0; j < static_cast<size_t>(cv::kBlock); ++j) mirror[(i + j) % mirror.size()] = in[i + j];
    if (rig.pitch->pitch().period > 0.0f) period = rig.pitch->pitch().period;
    lag = rig.growl->dryLagSamples();
    lagHeld = lag == lagBefore;
    ++blocks;
    if (i < static_cast<size_t>(kRate) || !lagHeld || rig.growl->wet() != 1.0f) continue;
    for (size_t j = 0; j < static_cast<size_t>(cv::kBlock); ++j) {
      const float want = cv::readRing<cv::growl_detail::kRingLen>(mirror.data(), static_cast<int>((i + j) % mirror.size()), lag);
      worst = std::fmax(worst, std::fabs(static_cast<double>(out[i + j] - want)));
      ++compared;
    }
  }
  const float voiceLag = cv::formant_detail::clampLag<cv::growl_detail::kRingLen>(
      static_cast<float>(cv::PsolaVoice::kGrainDelay) + cv::PitchTracker::clampPeriod(period), period);
  char detail[128];
  std::snprintf(detail, sizeof detail, "%zu samples in %zu blocks differ by %.2e (1e-6); lag %.2f vs the voice's %.2f", compared,
                blocks, worst, static_cast<double>(lag), static_cast<double>(voiceLag));
  return report("MIX 0 is the delayed input", compared > 20000 && worst < 1e-6 && std::fabs(lag - voiceLag) < 0.01f,
                detail);
}

// At GROWL 0 the peak is flat and the jitter is 0, and the output is pinned by hash on a sung note at
// MIX 100 %, 50 % and 0 (the hashes move with the high-pass, the voice and the level trim). The throat
// sits at rest there, so pulse anchoring shifts nothing.
bool checkGrowlZero() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  constexpr uint64_t kWant[] = {0xf5892faad19f3526ull, 0x3c9227a8ea073bd7ull, 0x2c0b07b7e38985e3ull};
  const float mixes[] = {1.0f, 0.5f, 0.0f};
  bool same = true;
  float jitter = 1.0f;
  for (size_t k = 0; k < 3; ++k) {
    Rig rig;
    const uint64_t h = hashOf(rig.run(in, potAt(0.0f, mixes[k])));
    std::printf("NOTE GROWL 0 hash MIX %.1f %016llx\n", static_cast<double>(mixes[k]), static_cast<unsigned long long>(h));
    same = same && h == kWant[k];
    jitter = rig.growl->jitter();
  }
  cv::growl_detail::Fold fold;
  fold.setTuning(cv::GrowlTuning{});
  const float peak = fold.gainsAt(0.0f).peak;
  char detail[112];
  std::snprintf(detail, sizeof detail, "3 MIX settings match their hashes: %s; peak gain less 1 %.1f (0), jitter %.1f (0)",
                same ? "yes" : "no", static_cast<double>(peak), static_cast<double>(jitter));
  return report("GROWL 0 pinned", same && peak == 0.0f && jitter == 0.0f, detail);
}

// A bin of x at hz by direct DFT over [from, to), as an amplitude.
double toneAmp(const Signal& x, double hz, size_t from, size_t to) {
  double re = 0.0, im = 0.0;
  for (size_t i = from; i < to; ++i) {
    const double w = 2.0 * kPi * hz * static_cast<double>(i) / kRate;
    re += x[i] * std::cos(w);
    im += x[i] * std::sin(w);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

// The wet path's high-pass: a 110 Hz note comes out with its 55 Hz fundamental. At hpHz 100 that
// fundamental sits 10.7 dB under what it is at hpHz 20, within 1.5 dB, and a 220 Hz note's 110 Hz output
// moves less than 3 dB.
bool checkHighPass() {
  const auto fundamentalDb = [](double noteHz, float hpHz) {
    const Signal in = sungNote(noteHz, 3.0, -20.0);
    cv::GrowlParams g = potAt(0.0f, 1.0f);
    g.tuning.hpHz = hpHz;
    Rig rig;
    const Signal out = rig.run(in, g);
    double best = 0.0;
    for (double hz = 0.5 * noteHz - 2.0; hz <= 0.5 * noteHz + 2.0; hz += 0.05) {
      best = std::fmax(best, toneAmp(out, hz, static_cast<size_t>(kRate), out.size()));
    }
    return 20.0 * std::log10(best);
  };
  const double low = fundamentalDb(110.0, 100.0) - fundamentalDb(110.0, 20.0);
  const double mid = fundamentalDb(220.0, 100.0) - fundamentalDb(220.0, 20.0);
  char detail[128];
  std::snprintf(detail, sizeof detail, "55 Hz fundamental %+.1f dB with hpHz 100 against 20 (-10.7 +- 1.5), 110 Hz %+.1f dB (> -3)", low,
                mid);
  return report("wet high-pass", std::fabs(low + 10.7) < 1.5 && mid > -3.0, detail);
}

// The heard pitch of x around sample c, in Hz: the normalised autocorrelation of a 30 ms window peaks at
// two output periods (the rasp makes alternate periods differ), found within 15 % of twice `guessHz`'s period
// and refined by a parabola. 0 when the window is too short or silent.
double heardHz(const Signal& x, long c, double guessHz) {
  constexpr long kLen = 1440;
  const long a = c - kLen / 2;
  const double guess = 2.0 * kRate / guessHz;
  const long lo = static_cast<long>(guess * 0.85), hi = static_cast<long>(guess * 1.15);
  if (a < 0 || a + kLen + hi + 2 >= static_cast<long>(x.size())) return 0.0;
  double e0 = 0.0;
  for (long i = 0; i < kLen; ++i) e0 += static_cast<double>(x[a + i]) * x[a + i];
  if (e0 < 1e-6) return 0.0;
  std::vector<double> r(static_cast<size_t>(hi + 2), 0.0);
  long best = lo;
  for (long l = lo - 1; l <= hi + 1; ++l) {
    double s = 0.0, e1 = 0.0;
    for (long i = 0; i < kLen; ++i) {
      s += static_cast<double>(x[a + i]) * x[a + i + l];
      e1 += static_cast<double>(x[a + i + l]) * x[a + i + l];
    }
    r[static_cast<size_t>(l)] = s / std::sqrt(e0 * e1 + 1e-12);
    if (l >= lo && l <= hi && r[static_cast<size_t>(l)] > r[static_cast<size_t>(best)]) best = l;
  }
  const double y0 = r[static_cast<size_t>(best - 1)], y1 = r[static_cast<size_t>(best)], y2 = r[static_cast<size_t>(best + 1)];
  const double den = y0 - 2.0 * y1 + y2;
  const double lag = static_cast<double>(best) + (den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0);
  return 2.0 * kRate / lag;
}

// The onset waver. A 220 Hz note, 150 ms of silence, the same note again, GROWL at 30 % with the throat
// drop widened to -24 st (so the throat falls from -5 to -11 st) and the rasp's wave and the fold otherwise
// flat. The voice sings 110.0 Hz. From 10 ms after the onset to 150 ms, the heard pitch stays within 0.1 st
// of that, the pulse anchoring holding it. With the anchoring off it left by 0.25 st and, before the
// voice had settled, 0.5 st. The same run without the drop reads 110.0 Hz within 0.02 st, which bounds the
// estimate's own error.
bool checkThroatPitch() {
  const auto worstSt = [](float dropTop) {
    Signal in(static_cast<size_t>(0.3 * kRate), 0.0f);
    const auto note = [](Signal& to, double sec) {
      const size_t n = static_cast<size_t>(sec * kRate);
      std::vector<double> v(n);
      double sum = 0.0;
      for (size_t i = 0; i < n; ++i) {
        double s = 0.0;
        for (int k = 1; k * 220.0 < 5000.0; ++k) s += std::sin(2.0 * kPi * k * 220.0 * static_cast<double>(i) / kRate) / k;
        v[i] = s;
        sum += s * s;
      }
      const double gain = std::pow(10.0, -14.0 / 20.0) / std::sqrt(sum / static_cast<double>(n));
      for (const double s : v) to.push_back(static_cast<float>(s * gain));
    };
    note(in, 0.5);
    in.insert(in.end(), static_cast<size_t>(0.15 * kRate), 0.0f);
    const size_t onset = in.size();
    note(in, 0.6);
    in.insert(in.end(), static_cast<size_t>(0.3 * kRate), 0.0f);
    cv::GrowlParams g = growlAt(0.3f, 0.0f);
    g.tuning.throatDropTop = dropTop;
    g.tuning.hardTop = 0.0f;
    g.tuning.thirdTop = 0.0f;
    g.tuning.jitterTop = 0.0f;
    Rig rig;
    const Signal out = rig.run(in, g);
    double worst = 0.0;
    for (double ms = 10.0; ms <= 150.0; ms += 5.0) {
      const long c = static_cast<long>(onset) + static_cast<long>(ms * kRate / 1000.0) + 1260;  // the voice trails by its lag
      const double hz = heardHz(out, c, 110.0);
      worst = hz > 0.0 ? std::fmax(worst, std::fabs(12.0 * std::log2(hz / 110.0))) : 99.0;
    }
    return worst;
  };
  const double drop = worstSt(-24.0f);
  const double flat = worstSt(0.0f);
  char detail[128];
  std::snprintf(detail, sizeof detail, "110.0 Hz off by %.3f st with the drop (0.1), %.3f st without (0.02)", drop, flat);
  return report("throat drop holds pitch", drop < 0.1 && flat < 0.02, detail);
}

// The bite peak through the fold with the knee, shelf and makeup set flat: a 2.5 kHz sine comes out +6 dB
// at DRIVE kGrowlDriveTop (GROWL 100 %), +3 dB at half that and 0 dB at 0, within 0.5 dB, and a sine two
// octaves off the peak moves less than 1 dB.
double foldGainDb(double hz, float drive) {
  cv::GrowlTuning t;
  t.lowShelfDb = 0.0f;
  t.driveDb = 0.0f;
  t.snarlTrimDb = 0.0f;
  cv::growl_detail::Fold fold;
  fold.setTuning(t);
  const cv::growl_detail::FoldGains gains = fold.gainsAt(drive);
  const size_t total = 4 * kRate / 10;
  Signal x(total);
  for (size_t i = 0; i < total; ++i) x[i] = static_cast<float>(0.05 * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
  const Signal ref = x;
  for (size_t i = 0; i < total; i += cv::kBlock) fold.run(&x[i], cv::kBlock, gains, cv::growl_detail::FoldGains{0, 0, 0, 0, 0});
  const size_t from = total / 2;
  return 20.0 * std::log10(rms(x, from, total) / rms(ref, from, total));
}

bool checkPeak() {
  const double full = foldGainDb(2500.0, cv::kGrowlDriveTop);
  const double half = foldGainDb(2500.0, 0.5f * cv::kGrowlDriveTop);
  const double none = foldGainDb(2500.0, 0.0f);
  const double low = foldGainDb(625.0, cv::kGrowlDriveTop);
  const double high = foldGainDb(10000.0, cv::kGrowlDriveTop);
  char detail[160];
  std::snprintf(detail, sizeof detail, "2.5 kHz: %+.2f dB at GROWL 100 %% (+10), %+.2f at 50 %% (+5), %+.2f at 0 (0); 625 Hz %+.2f, 10 kHz %+.2f (< 1)",
                full, half, none, low, high);
  const bool ok = std::fabs(full - 10.0) < 0.5 && std::fabs(half - 5.0) < 0.5 && std::fabs(none) < 0.01 &&
                  std::fabs(low) < 1.0 && std::fabs(high) < 1.0;
  return report("bite peak gain", ok, detail);
}

// The lopsided clip: a 220 Hz sine at -14 dBFS peak through the fold at GROWL 100 % puts its 2nd harmonic at
// least 10 dB over the same run with biasTop 0, and the fold's output carries no DC (mean under 1e-4). The
// bias is 0 at GROWL 0, a quarter of the top at GROWL 50 % and the top at 100 %, and the whole card at
// GROWL 100 % leaves no DC either.
struct BiasRun {
  double h2;
  double mean;
};

BiasRun biasRun(float biasTop, double amp) {
  cv::GrowlTuning t;
  t.biasTop = biasTop;
  cv::growl_detail::Fold fold;
  fold.setTuning(t);
  const cv::growl_detail::FoldGains gains = fold.gainsAt(cv::kGrowlDriveTop);
  const size_t total = 3 * static_cast<size_t>(kRate);
  Signal x(total);
  for (size_t i = 0; i < total; ++i) x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * 220.0 * static_cast<double>(i) / kRate));
  for (size_t i = 0; i < total; i += cv::kBlock) fold.run(&x[i], cv::kBlock, gains, cv::growl_detail::FoldGains{0, 0, 0, 0, 0});
  const size_t from = total - static_cast<size_t>(kRate);
  double sum = 0.0;
  for (size_t i = from; i < total; ++i) sum += x[i];
  return {toneAmp(x, 440.0, from, total), sum / kRate};
}

bool checkBias() {
  constexpr double kAmp = 0.2;  // -14 dBFS peak, which the fold's input gain drives well into the rail
  const BiasRun on = biasRun(0.25f, kAmp);
  const BiasRun off = biasRun(0.0f, kAmp);
  const double rise = 20.0 * std::log10(on.h2 / off.h2);
  cv::growl_detail::Fold fold;
  fold.setTuning(cv::GrowlTuning{});
  const float b0 = fold.gainsAt(0.0f).bias;
  const float bHalf = fold.gainsAt(0.5f * cv::kGrowlDriveTop).bias;
  const float bTop = fold.gainsAt(cv::kGrowlDriveTop).bias;

  const Signal in = sungNote(220.0, 3.0, -20.0);
  Rig rig;
  const Signal out = rig.run(in, potAt(1.0f, 1.0f));
  double cardMean = 0.0;
  for (size_t i = out.size() - kRate; i < out.size(); ++i) cardMean += out[i];
  cardMean /= kRate;

  char detail[200];
  std::snprintf(detail, sizeof detail, "2nd harmonic +%.1f dB over no bias (>= 10), mean %.1e fold and %.1e card (< 1e-4), bias %.4f / %.4f / %.4f at GROWL 0 / 50 / 100 %% (0 / 0.0625 / 0.25)",
                rise, on.mean, cardMean, static_cast<double>(b0), static_cast<double>(bHalf), static_cast<double>(bTop));
  const bool ok = rise >= 10.0 && std::fabs(on.mean) < 1e-4 && std::fabs(cardMean) < 1e-4 && b0 == 0.0f &&
                  std::fabs(bHalf - 0.0625f) < 1e-6f && std::fabs(bTop - 0.25f) < 1e-6f;
  return report("lopsided clip", ok, detail);
}

// Two runs from reset are bit for bit the same, so the jitter repeats. The jitter is on: the output differs
// from a run with jitterTop 0. It reads 0.55 at GROWL 100 % and 0.275 at 50 %, and the period depth stays
// in 1 - jitter .. 1 and takes more than a few values.
bool checkJitter() {
  const Signal in = sungNote(220.0, 2.0, -20.0);
  Rig a;
  Rig b;
  const Signal outA = a.run(in, potAt(1.0f, 1.0f));
  const Signal outB = b.run(in, potAt(1.0f, 1.0f));
  const bool repeats = outA == outB;
  cv::GrowlParams flat = potAt(1.0f, 1.0f);
  flat.tuning.jitterTop = 0.0f;
  Rig c;
  const bool active = c.run(in, flat) != outA;
  const float jitterTop = a.growl->jitter();
  Rig half;
  half.run(in, potAt(0.5f, 1.0f));
  const float jitterHalf = half.growl->jitter();

  Rig watch;
  float lo = 1.0f, hi = 0.0f;
  size_t distinct = 0;
  float last = -1.0f;
  Signal out(in.size());
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) {
    watch.step(&in[i], &out[i], cv::kBlock, potAt(1.0f, 1.0f));
    if (i < static_cast<size_t>(kRate)) continue;
    const float depth = watch.growl->periodDepth();
    lo = std::fmin(lo, depth);
    hi = std::fmax(hi, depth);
    distinct += depth != last ? 1 : 0;
    last = depth;
  }
  char detail[176];
  std::snprintf(detail, sizeof detail, "repeat bit-exact %s, differs from no jitter %s, jitter %.3f (0.55) and %.3f (0.275), depth %.3f..%.3f in (0.45..1), %zu changes",
                repeats ? "yes" : "no", active ? "yes" : "no", static_cast<double>(jitterTop), static_cast<double>(jitterHalf),
                static_cast<double>(lo), static_cast<double>(hi), distinct);
  const bool ok = repeats && active && std::fabs(jitterTop - 0.55f) < 1e-5f && std::fabs(jitterHalf - 0.275f) < 1e-5f &&
                  lo >= 0.45f - 1e-5f && hi <= 1.0f && lo < 0.7f && hi > 0.9f && distinct > 20;
  return report("rasp jitter", ok, detail);
}

// A steady 220 Hz note with a hard onset out of silence, GROWL 100 %. For the first 150 ms after the voice
// comes in, the card's own pitch ratio stays at 0.5, the rasp wave stays at half the tracker's pitch and the
// voice's read lag stays put, each within the bound printed below. The throat is the one thing that moves
// at an onset, by its law, and stays between -12 and -5 st.
bool checkOnsetHolds() {
  Signal in(static_cast<size_t>(0.3 * kRate), 0.0f);
  constexpr double kHz = 220.0;
  for (size_t i = 0; i < static_cast<size_t>(kRate); ++i) {
    double v = 0.0;
    for (int k = 1; k * kHz < 5000.0; ++k) v += std::sin(2.0 * kPi * k * kHz * static_cast<double>(i) / kRate) / k;
    in.push_back(static_cast<float>(0.1 * v));
  }
  Rig rig;
  const cv::GrowlParams g = potAt(1.0f, 1.0f);
  Signal out(in.size());
  size_t counted = 0;
  const size_t want = static_cast<size_t>(150 * cv::kSmpPerMs) / cv::kBlock;
  float ratioWorst = 0.0f, rateWorst = 0.0f, lagLo = 1e9f, lagHi = 0.0f, throatLo = 0.0f, throatHi = -99.0f;
  for (size_t i = 0; i + cv::kBlock <= in.size() && counted < want; i += cv::kBlock) {
    rig.step(&in[i], &out[i], cv::kBlock, g);
    if (rig.growl->wet() == 0.0f) continue;
    ++counted;
    const float period = rig.pitch->pitch().period;
    ratioWorst = std::fmax(ratioWorst, std::fabs(rig.growl->pitchRatio() - 0.5f));
    rateWorst = std::fmax(rateWorst, std::fabs(rig.growl->raspRateHz() - 0.5f * static_cast<float>(kRate) / period));
    const float lag = static_cast<float>(cv::PsolaVoice::kGrainDelay) + cv::PitchTracker::clampPeriod(period);
    lagLo = std::fmin(lagLo, lag);
    lagHi = std::fmax(lagHi, lag);
    throatLo = std::fmin(throatLo, rig.growl->throatSt());
    throatHi = std::fmax(throatHi, rig.growl->throatSt());
  }
  char detail[176];
  std::snprintf(detail, sizeof detail, "%zu blocks: pitch ratio off 0.5 by %.1e (1e-4), rasp rate off half the pitch by %.3f Hz (0.5), lag spread %.2f samples (0.5), throat %.2f..%.2f st (-12..-5)",
                counted, static_cast<double>(ratioWorst), static_cast<double>(rateWorst), static_cast<double>(lagHi - lagLo),
                static_cast<double>(throatLo), static_cast<double>(throatHi));
  const bool ok = counted == want && ratioWorst < 1e-4f && rateWorst < 0.5f && lagHi - lagLo < 0.5f && throatLo >= -12.0f &&
                  throatHi <= -5.0f + 1e-3f;
  return report("onset holds pitch and lag", ok, detail);
}

// The tracker's mean pitch over the voiced frames of x from sample `from`, in Hz; 0 with none.
double trackedHz(const Signal& x, size_t from) {
  RingedTracker t;
  double sum = 0.0;
  int frames = 0;
  for (size_t i = 0; i + cv::kBlock <= x.size(); i += cv::kBlock) {
    t.push(&x[i], cv::kBlock);
    if (i >= from && t.result().voiced && !t.result().clamped) {
      sum += t.result().hz;
      ++frames;
    }
  }
  return frames > 0 ? sum / frames : 0.0;
}

// Octave at +12 st and MIX 100 % ahead of the card. The card's input is the octave-up voice and the card
// drops it by pitchSt, so the output sings the raw voice's pitch. A card that cut its grains from the raw
// voice would read 110 Hz here, the raw voice dropped.
bool checkOctaveAhead() {
  const Signal in = sungNote(220.0, 2.5, -20.0);
  Rig rig;
  rig.stage.octave.on = true;
  rig.stage.octave.semitones = 12;
  rig.stage.octave.mix = 1.0f;
  const Signal out = rig.run(in, growlAt(0.0f, 0.0f));
  const double stageHz = trackedHz(rig.heard, static_cast<size_t>(kRate));
  const double outHz = trackedHz(out, static_cast<size_t>(kRate));
  const double stageCents = 1200.0 * std::log2(stageHz / 440.0);
  const double outCents = 1200.0 * std::log2(outHz / 220.0);
  char detail[160];
  std::snprintf(detail, sizeof detail, "stage %.1f Hz (%+.0f cents from 440), output %.1f Hz (%+.0f cents from the raw 220; the raw voice dropped is 110)",
                stageHz, stageCents, outHz, outCents);
  return report("octave ahead", stageHz > 0.0 && outHz > 0.0 && std::fabs(stageCents) < 35.0 && std::fabs(outCents) < 35.0, detail);
}

// A held vowel at `hz`: harmonics shaped by three resonances at 700, 1200 and 2600 Hz, so a formant shift
// moves the spectral centroid.
Signal vowel(double hz, double seconds, double dbfs) {
  const size_t n = static_cast<size_t>(seconds * kRate);
  const double centre[3] = {700.0, 1200.0, 2600.0};
  const double width[3] = {120.0, 160.0, 300.0};
  Signal x(n, 0.0f);
  double sum = 0.0;
  for (int k = 1; k * hz < 6000.0; ++k) {
    double a = 0.02;
    for (int f = 0; f < 3; ++f) {
      const double d = (k * hz - centre[f]) / width[f];
      a += 1.0 / (1.0 + d * d);
    }
    for (size_t i = 0; i < n; ++i) x[i] += static_cast<float>(a * std::sin(2.0 * kPi * k * hz * static_cast<double>(i) / kRate));
  }
  for (const float v : x) sum += static_cast<double>(v) * v;
  const double g = std::pow(10.0, dbfs / 20.0) / std::sqrt(sum / static_cast<double>(n));
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

// The power-weighted mean frequency of x over [from, to) up to 8 kHz.
double centroidHz(const Signal& x, size_t from, size_t to) {
  constexpr size_t kSize = 4096;
  const std::vector<double> p = testutil::spectrum(x, from, to, kSize);
  double num = 0.0, den = 0.0;
  for (size_t k = 1; k < p.size(); ++k) {
    const double hz = static_cast<double>(k) * kRate / kSize;
    if (hz > 8000.0) break;
    num += hz * p[k];
    den += p[k];
  }
  return den > 0.0 ? num / den : 0.0;
}

// Formant ahead of the card at SHIFT -5 and +5 st, MIX 100 %. Formant's own output moves its centroid by a
// ratio; the card's output follows with at least half that move in log terms. A card that cut its grains
// from the raw voice would hold the ratio at 1 (the throat is the same at both).
bool checkFormantAhead() {
  const Signal in = vowel(165.0, 2.5, -20.0);
  struct Centroids {
    double stage;
    double out;
  };
  const auto at = [&in](int shift) {
    Rig rig;
    rig.stage.formant.on = true;
    rig.stage.formant.shiftSt = shift;
    rig.stage.formant.mix = 1.0f;
    const Signal out = rig.run(in, growlAt(0.0f, 0.0f));
    const size_t from = static_cast<size_t>(kRate), to = out.size();
    return Centroids{centroidHz(rig.heard, from, to), centroidHz(out, from, to)};
  };
  const Centroids lo = at(-5);
  const Centroids hi = at(5);
  const double stageMove = std::log2(hi.stage / lo.stage);
  const double outMove = std::log2(hi.out / lo.out);
  char detail[176];
  std::snprintf(detail, sizeof detail, "centroid %.0f -> %.0f Hz in Formant's output (%+.2f oct), %.0f -> %.0f Hz in the card's (%+.2f oct, >= half)",
                lo.stage, hi.stage, stageMove, lo.out, hi.out, outMove);
  return report("formant ahead", stageMove > 0.1 && outMove >= 0.5 * stageMove, detail);
}

// Unison between the pitch cards and the card, DEPTH 100 %. The copies are in the card's input, so the
// output differs from the same run without Unison by more than the floor. A card that cut its grains from
// the audio ahead of Unison would give the same output either way.
Signal runUnisonAhead(const Signal& in, bool unisonOn) {
  auto pitch = std::make_unique<PitchSeries>();
  auto unison = std::make_unique<cv::Unison>();
  auto growl = std::make_unique<cv::Growl>();
  cv::UnisonParams up;
  up.on = unisonOn;
  up.depth = 1.0f;
  const cv::GrowlParams g = growlAt(0.0f, 0.0f);
  Signal out(in.size());
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) {
    pitch->process(&in[i], &out[i], cv::kBlock, cv::PitchCardParams{}, true);
    if (unisonOn) unison->process(&out[i], &out[i], cv::kBlock, up);
    growl->process(&out[i], &out[i], cv::kBlock, g, pitch->upstream());
  }
  return out;
}

bool checkUnisonAhead() {
  constexpr double kFloor = 0.2;  // relative RMS of the difference
  const Signal in = sungNote(220.0, 2.5, -20.0);
  const Signal with = runUnisonAhead(in, true);
  const Signal without = runUnisonAhead(in, false);
  double d = 0.0, e = 0.0;
  for (size_t i = static_cast<size_t>(kRate); i < in.size(); ++i) {
    const double x = static_cast<double>(with[i]) - without[i];
    d += x * x;
    e += static_cast<double>(without[i]) * without[i];
  }
  const double rel = std::sqrt(d / e);
  char detail[96];
  std::snprintf(detail, sizeof detail, "output differs from the run without Unison by %.2f of its RMS (> %.2f)", rel, kFloor);
  return report("unison ahead", rel > kFloor, detail);
}

// A glottal-like pulse train at `hz`: a short damped burst per period, each at its own random level
// within 1 +- spread, so a cross-correlation of the train with its delayed copy has one peak.
Signal pulseTrain(double hz, double seconds, double dbfs, double spread = 0.4) {
  const size_t n = static_cast<size_t>(seconds * kRate);
  const double period = kRate / hz;
  Signal x(n, 0.0f);
  uint32_t r = 0x1234567u;
  double sum = 0.0;
  for (double at = 100.0; at + 80.0 < static_cast<double>(n); at += period) {
    r = r * 1664525u + 1013904223u;
    const double level = 1.0 - spread + 2.0 * spread * static_cast<double>(r >> 8) / 16777216.0;
    const size_t start = static_cast<size_t>(at);
    for (size_t t = 0; t < 120; ++t) {
      const double tt = static_cast<double>(t) - (at - static_cast<double>(start));
      if (tt < 0.0) continue;
      const double v = level * std::exp(-tt / 25.0) * std::sin(2.0 * kPi * tt / 60.0);
      x[start + t] += static_cast<float>(v);
    }
  }
  for (const float v : x) sum += static_cast<double>(v) * v;
  const double g = std::pow(10.0, dbfs / 20.0) / std::sqrt(sum / static_cast<double>(n));
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

// The delay of b behind a in samples, by the peak of their cross-correlation over lags [lo, hi] on the
// samples [from, to).
long delayOf(const Signal& a, const Signal& b, size_t from, size_t to, long lo, long hi) {
  long best = lo;
  double top = -1e300;
  for (long lag = lo; lag <= hi; ++lag) {
    double s = 0.0;
    for (size_t i = from; i < to; ++i) s += static_cast<double>(a[i]) * b[i + static_cast<size_t>(lag)];
    if (s > top) {
      top = s;
      best = lag;
    }
  }
  return best;
}

// The delays the card reads off the pitch stage, against the stage measured. Autotune: a pulse train
// at 220 Hz (in tune) comes out of the stage `outputLatency()` samples late, within the half period its
// grain grid moves it by. Octave at +12 st and MIX 100 %: isolated impulses come out with their energy
// centred `outputLatency()` samples late, on average over the phases of Octave's sawtooth.
bool checkStageLatency() {
  const Signal train = pulseTrain(220.0, 2.5, -20.0);
  Rig tuned;
  tuned.stage.autotune.on = true;
  tuned.run(train, growlAt(0.0f, 0.0f));
  const long autotuneLag = delayOf(train, tuned.heard, static_cast<size_t>(kRate), static_cast<size_t>(kRate) * 3 / 2, 800, 2200);
  std::vector<float> window(tuned.latency.begin() + kRate / cv::kBlock, tuned.latency.begin() + kRate * 3 / 2 / cv::kBlock);
  std::sort(window.begin(), window.end());
  const float autotuneWant = window[window.size() / 2];

  Signal clicks(static_cast<size_t>(6 * kRate), 0.0f);
  std::vector<size_t> at;
  for (size_t t = 4000; t + 4000 < clicks.size(); t += 8123) {
    clicks[t] = 0.5f;
    at.push_back(t);
  }
  Rig oct;
  oct.stage.octave.on = true;
  oct.stage.octave.semitones = 12;
  oct.stage.octave.mix = 1.0f;
  oct.run(clicks, growlAt(0.0f, 0.0f));
  double centroid = 0.0;
  for (const size_t t : at) {
    double num = 0.0, den = 0.0;
    for (size_t i = t; i < t + 2100; ++i) {
      const double e = static_cast<double>(oct.heard[i]) * oct.heard[i];
      num += e * static_cast<double>(i - t);
      den += e;
    }
    centroid += den > 0.0 ? num / den : 0.0;
  }
  centroid /= static_cast<double>(at.size());
  const float octaveWant = oct.pitch->outputLatency();

  char detail[200];
  std::snprintf(detail, sizeof detail, "Autotune: measured %ld samples, stage says %.1f; Octave +12: measured %.0f, stage says %.1f", autotuneLag,
                static_cast<double>(autotuneWant), centroid, static_cast<double>(octaveWant));
  // Autotune's grains read a half period either side of the mean, depending on where its grid sits.
  const double autotuneSlack = 0.5 * (static_cast<double>(autotuneWant) - cv::PsolaVoice::kGrainDelay) + 20.0;
  return report("stage latency", std::fabs(static_cast<double>(autotuneLag) - autotuneWant) < autotuneSlack &&
                                     std::fabs(centroid - octaveWant) < 150.0,
                detail);
}

// The output stays finite and bounded with the pitch stage at its corners ahead of the card: Octave down and up
// at MIX 100 %, at the 50 % where the period it carries changes, and just under it, then all three stages
// on, and an Octave MIX that crosses 50 % every five blocks. Calls of any length, from one sample to more than
// the ring holds, give the same.
bool checkStagesFinite(const std::vector<Signal>& voices) {
  std::vector<Signal> inputs = voices;
  inputs.push_back(sungNote(180.0, 2.0, -18.0));
  struct Config {
    bool octave;
    int semis;
    float octaveMix;
    bool tuned;
    bool formant;
  };
  const Config configs[] = {{true, -12, 1.0f, false, false}, {true, 12, 0.5f, false, false}, {true, 7, 0.49f, false, false},
                            {true, 12, 1.0f, true, true}};
  size_t bad = 0;
  float peak = 0.0f;
  for (const Signal& in : inputs) {
    for (const Config& c : configs) {
      for (const float pot : {0.0f, 1.0f}) {
        for (const float mix : {0.0f, 0.5f, 1.0f}) {
          Rig rig;
          rig.stage.octave.on = c.octave;
          rig.stage.octave.semitones = c.semis;
          rig.stage.octave.mix = c.octaveMix;
          rig.stage.autotune.on = c.tuned;
          rig.stage.formant.on = c.formant;
          rig.stage.formant.shiftSt = -4;
          for (const float v : rig.run(in, potAt(pot, mix))) {
            bad += std::isfinite(v) ? 0 : 1;
            peak = std::fmax(peak, std::fabs(v));
          }
        }
      }
    }
    Rig flip;
    flip.stage.octave.on = true;
    flip.stage.octave.semitones = 12;
    Signal out(in.size());
    for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) {
      flip.stage.octave.mix = (i / cv::kBlock / 5) % 2 == 0 ? 0.4f : 0.6f;
      flip.step(&in[i], &out[i], cv::kBlock, potAt(1.0f, 0.5f));
    }
    for (const float v : out) {
      bad += std::isfinite(v) ? 0 : 1;
      peak = std::fmax(peak, std::fabs(v));
    }
  }
  const Signal note = sungNote(180.0, 1.5, -18.0);
  for (const size_t n : {size_t{1}, size_t{17}, size_t{200}, size_t{4000}}) {
    auto pitch = std::make_unique<PitchSeries>();
    auto growl = std::make_unique<cv::Growl>();
    cv::PitchCardParams pp;
    pp.octave.on = true;
    pp.octave.semitones = 12;
    pp.octave.mix = 1.0f;
    Signal out(note.size());
    for (size_t i = 0; i < note.size(); i += n) {
      const int m = static_cast<int>(std::min(n, note.size() - i));
      pitch->process(&note[i], &out[i], m, pp, true);
      growl->process(&out[i], &out[i], m, potAt(1.0f, 0.5f), pitch->upstream());
    }
    for (const float v : out) {
      bad += std::isfinite(v) ? 0 : 1;
      peak = std::fmax(peak, std::fabs(v));
    }
  }
  char detail[128];
  std::snprintf(detail, sizeof detail, "%zu inputs, 4 stage settings and a flipping Octave MIX, 4 call lengths: %zu samples not finite (0), peak %.2f (< 8)",
                inputs.size(), bad, static_cast<double>(peak));
  return report("finite behind the stages", bad == 0 && peak < 8.0f, detail);
}

// True when every float of the card's ring is zero.
bool ringIsZero(const cv::Growl& growl) {
  for (const float v : growl.ringForTest().data()) {
    if (v != 0.0f) return false;
  }
  return true;
}

// A stopped card clears its ring a slice a call (2 KB at most), and the reset at its switch-on then finds
// the ring clear and clears nothing. A reset with no rest before it clears the whole ring.
bool checkRingRest() {
  using Ring = cv::InputRing<cv::growl_detail::kRingLen>;
  const Signal in = sungNote(220.0, 1.0, -20.0);
  Rig rig;
  rig.run(in, growlAt(0.5f, 0.5f));
  const bool dirty = !ringIsZero(*rig.growl);
  const uint32_t start = rig.growl->ringClearedForTest();
  int calls = 0;
  uint32_t worstStep = 0;
  while (rig.growl->ringClearedForTest() - start < static_cast<uint32_t>(cv::growl_detail::kRingLen) && calls < 100) {
    const uint32_t before = rig.growl->ringClearedForTest();
    rig.growl->rest();
    worstStep = std::max(worstStep, rig.growl->ringClearedForTest() - before);
    ++calls;
  }
  const bool restClear = ringIsZero(*rig.growl);
  const uint32_t beforeReset = rig.growl->ringClearedForTest();
  rig.growl->reset();
  const bool resetClear = ringIsZero(*rig.growl) && rig.growl->ringClearedForTest() == beforeReset;

  Rig quick;
  quick.run(in, growlAt(0.5f, 0.5f));
  quick.growl->rest();
  quick.growl->reset();
  const bool quickClear = ringIsZero(*quick.growl);
  Rig bare;
  bare.run(in, growlAt(0.5f, 0.5f));
  const uint32_t bareBefore = bare.growl->ringClearedForTest();
  bare.growl->reset();
  const bool bareClear = ringIsZero(*bare.growl) &&
                         bare.growl->ringClearedForTest() - bareBefore == static_cast<uint32_t>(cv::growl_detail::kRingLen);
  const int want = (cv::growl_detail::kRingLen + Ring::kRestClearFloats - 1) / Ring::kRestClearFloats;
  char detail[160];
  std::snprintf(detail, sizeof detail, "ring %d floats (%d B): clear after %d calls (%d), at most %u B a call (2048), reset after a short rest clears it: %s",
                cv::growl_detail::kRingLen, cv::growl_detail::kRingLen * 4, calls, want, static_cast<unsigned>(worstStep * 4),
                quickClear ? "yes" : "no");
  return report("ring rest", dirty && restClear && resetClear && quickClear && bareClear && calls == want && worstStep * 4 <= 2048,
                detail);
}

// The default chain: Autotune ahead of the card. On a 190 Hz note, a quarter tone flat of the nearest scale
// note, the output sings half the corrected pitch, and on Adam's clips at GROWL 75 % and MIX 100 % the mean
// level against the card's input lands 0 to +0.5 dB.
bool checkAutotuneAhead(const std::vector<Signal>& voices, bool whole) {
  const Signal in = sungNote(190.0, 2.5, -20.0);
  Rig rig;
  rig.stage.autotune.on = true;
  const Signal out = rig.run(in, growlAt(0.0f, 0.0f));
  const double stageHz = trackedHz(rig.heard, static_cast<size_t>(kRate));
  const double outHz = trackedHz(out, static_cast<size_t>(kRate));
  const double cents = 1200.0 * std::log2(outHz / (0.5 * stageHz));
  double sum = 0.0;
  for (const Signal& clip : voices) {
    Rig r;
    r.stage.autotune.on = true;
    const Signal o = r.run(clip, potAt(cv::kGrowlDefaultPot, 1.0f));
    sum += 20.0 * std::log10(rms(o, kPad, o.size()) / rms(r.heard, kPad, r.heard.size()));
  }
  const double level = voices.empty() ? 0.0 : sum / static_cast<double>(voices.size());
  char detail[160];
  std::snprintf(detail, sizeof detail, "corrected %.1f Hz, output %.1f Hz (%+.0f cents from half, 35); level %+.2f dB (0 to +0.5)%s",
                stageHz, outHz, cents, level, voices.empty() ? " SKIP" : "");
  const bool levelOk = voices.empty() || (whole && level >= 0.0 && level <= 0.5);
  return report("autotune ahead", stageHz > 0.0 && outHz > 0.0 && std::fabs(cents) < 35.0 && levelOk, detail);
}

// The loudest samples of x over [from, to): local maxima of |x| above `thresh` of the region's peak, the
// larger of any two closer than minDist kept.
std::vector<long> pulsesOf(const Signal& x, size_t from, size_t to, long minDist, float thresh) {
  float top = 0.0f;
  for (size_t i = from; i < to; ++i) top = std::fmax(top, std::fabs(x[i]));
  std::vector<long> found;
  for (size_t i = from + 1; i + 1 < to; ++i) {
    const float a = std::fabs(x[i]);
    if (a < thresh * top || a < std::fabs(x[i - 1]) || a <= std::fabs(x[i + 1])) continue;
    const long at = static_cast<long>(i);
    if (!found.empty() && at - found.back() < minDist) {
      if (a > std::fabs(x[static_cast<size_t>(found.back())])) found.back() = at;
      continue;
    }
    found.push_back(at);
  }
  return found;
}

// Octave at +12 st and MIX 100 % ahead, the card at GROWL 0 and DRIVE 0 on a pulse train. Each output pulse is
// traced back to the card's input: it sits the card's read lag plus a stretch of up to two input periods
// behind the input pulse its grain was cut from (a grain reads from a period past the lag, and the throat
// stretches it). The stretch, its mean and spread over the second half of the note, is logged. The grains
// land on the input's pulses when it holds still; a grid at the wrong period walks it round the period.
bool checkOctavePulses() {
  // The pulses wait in silence until Octave's ring holds its taps' reach, so its voice starts with them.
  const size_t lead = static_cast<size_t>(cv::OctaveVoice::deepestRead(cv::OctaveTuning{}));
  Signal in(lead, 0.0f);
  const Signal train = pulseTrain(220.0, 3.0, -20.0, 0.0);
  in.insert(in.end(), train.begin(), train.end());
  Rig rig;
  rig.stage.octave.on = true;
  rig.stage.octave.semitones = 12;
  rig.stage.octave.mix = 1.0f;
  Signal out = rig.run(in, growlAt(0.0f, 0.0f));
  out.erase(out.begin(), out.begin() + static_cast<long>(lead));
  rig.heard.erase(rig.heard.begin(), rig.heard.begin() + static_cast<long>(lead));
  in.erase(in.begin(), in.begin() + static_cast<long>(lead));
  const float lag = rig.growl->dryLagSamples();
  const size_t from = static_cast<size_t>(kRate) * 3 / 2, to = in.size() - 3000;
  const std::vector<long> src = pulsesOf(rig.heard, from - 4000, to, 70, 0.3f);
  const std::vector<long> got = pulsesOf(out, from, to, 160, 0.3f);
  std::vector<double> off;
  for (const long o : got) {
    const double want = static_cast<double>(o) - static_cast<double>(lag);
    long near = -1;
    for (const long s : src) {
      if (static_cast<double>(s) <= want) near = s;
    }
    if (near >= 0) off.push_back(want - static_cast<double>(near));
  }
  double mean = 0.0, spread = 0.0;
  for (const double v : off) mean += v;
  mean = off.empty() ? 0.0 : mean / static_cast<double>(off.size());
  for (const double v : off) spread += (v - mean) * (v - mean);
  spread = off.empty() ? 0.0 : std::sqrt(spread / static_cast<double>(off.size()));
  const double period = kRate / 440.0;
  char detail[200];
  std::snprintf(detail, sizeof detail,
                "%zu output pulses on %zu input pulses, read lag %.1f: offset %.1f samples (0 to %.0f), spread %.1f (< %.0f)",
                got.size(), src.size(), static_cast<double>(lag), mean, 2.0 * period, spread, 0.3 * period);
  return report("octave pulses", off.size() > 50 && mean >= 0.0 && mean <= 2.0 * period && spread < 0.3 * period, detail);
}

}  // namespace

int main() {
  bool whole = true;
  const std::vector<Signal> voices = loadClips(whole);
  bool ok = true;
  ok &= checkOff();
  ok &= checkDrop();
  ok &= checkDepthFade();
  ok &= checkThroat();
  ok &= checkThroatDepth();
  ok &= checkThroatSoftNote();
  ok &= checkThroatSilence();
  ok &= checkThroatTuning();
  ok &= checkCorners(voices);
  ok &= checkLevel(voices, whole);
  ok &= checkAlias();
  ok &= checkFullMix();
  ok &= checkOwnTracker();
  ok &= checkMixZero();
  ok &= checkGrowlZero();
  ok &= checkHighPass();
  ok &= checkThroatPitch();
  ok &= checkPeak();
  ok &= checkBias();
  ok &= checkJitter();
  ok &= checkOnsetHolds();
  ok &= checkOctaveAhead();
  ok &= checkFormantAhead();
  ok &= checkUnisonAhead();
  ok &= checkStageLatency();
  ok &= checkOctavePulses();
  ok &= checkAutotuneAhead(voices, whole);
  ok &= checkStagesFinite(voices);
  ok &= checkRingRest();
  std::printf("%s growl\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
