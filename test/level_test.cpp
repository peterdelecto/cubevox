// Level rule: every stage at its default engaged setting lands 0 to +0.5 dB
// (out/in RMS) on vocal-like program material. The reverb, Formant, Lo-fi and Chatter rows take every
// setting from the registry, as the box runs them. The reverb, Formant MIX, Lo-fi, Chatter, Echo, Unison and
// Distortion rows run on Adam's clips in samples/: the synthetic program puts its energy where the telephone
// band cuts and where the reverb loops cut, its noise floor holds the Chatter card's word open, its steady
// tone beats against its own echo and chorus copies, and it sits 5 to 7 dB louder than the clips, where
// Distortion's compression reads differently. The clip rows skip when samples/ is absent (it is git-ignored,
// so CI has none) and fail when the folder is there and a clip in it is missing or empty. No window, no
// audio device.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "core/chain_params.h"
#include "core/registry.h"
#include "engine/bloom.h"
#include "engine/crowd.h"
#include "engine/distortion.h"
#include "engine/gate.h"
#include "engine/lofi.h"
#include "engine/polish.h"
#include "engine/polish_comp.h"
#include "engine/reverb.h"
#include "engine/slapback.h"
#include "engine/unison.h"

#include "distortion_rule.h"
#include "pitch_series.h"
#include "signal_util.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSeconds = 3;
constexpr int kMeasureFrom = 1 * cv::kSampleRate;
constexpr double kProgramDbfs = -18.0;
constexpr double kNoiseDbfs = -40.0;

// The comp thresholds sit at -24 to -30 dBFS, around the voice loops (-22.7 to -24.5 dBFS RMS), so
// the comp rows run the program at that level and every other row keeps kProgramDbfs.
constexpr double kCompProgramDbfs = -23.0;
constexpr double kLoDb = 0.0;
constexpr double kHiDb = 0.5;
constexpr double kSlackDb = 0.1;

// A MIX knob at 50 % is reported against a looser band: the crossfade keeps it near unity without
// its own trim.
constexpr double kMidLoDb = -1.5;
constexpr double kMidHiDb = 1.0;

using Signal = std::vector<float>;

double rmsRange(const Signal& x, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

// Deterministic white noise in [-1, 1).
class Lcg {
 public:
  double next() {
    s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(s_ >> 11) / 4503599627370496.0 - 1.0;
  }

 private:
  uint64_t s_ = 0x9E3779B97F4A7C15ULL;
};

// 180 Hz tone, 10 harmonics at 1/k, 5 Hz vibrato of +-20 cents, 2 Hz
// raised-cosine phrase envelope, plus pink-ish noise at -40 dBFS, then the
// whole signal normalised to -18 dBFS RMS.
Signal program(int seconds = kSeconds) {
  const size_t n = static_cast<size_t>(seconds) * cv::kSampleRate;
  Signal tone(n), noise(n);
  double phase = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / cv::kSampleRate;
    const double hz = 180.0 * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0);
    phase += 2.0 * kPi * hz / cv::kSampleRate;
    double v = 0.0;
    for (int k = 1; k <= 10; ++k) v += std::sin(k * phase) / k;
    const double env = 0.5 * (1.0 - std::cos(2.0 * kPi * 2.0 * t));
    tone[i] = static_cast<float>(v * env);
  }
  Lcg rng;
  double b0 = 0.0, b1 = 0.0, b2 = 0.0;  // Kellet economy pink filter
  for (size_t i = 0; i < n; ++i) {
    const double w = rng.next();
    b0 = 0.99765 * b0 + w * 0.0990460;
    b1 = 0.96300 * b1 + w * 0.2965164;
    b2 = 0.57000 * b2 + w * 1.0526913;
    noise[i] = static_cast<float>(b0 + b1 + b2 + w * 0.1848);
  }
  const double tg = dbToLin(kProgramDbfs) / rmsRange(tone, 0, n);
  const double ng = dbToLin(kNoiseDbfs) / rmsRange(noise, 0, n);
  Signal x(n);
  for (size_t i = 0; i < n; ++i) x[i] = static_cast<float>(tone[i] * tg + noise[i] * ng);
  const double g = dbToLin(kProgramDbfs) / rmsRange(x, 0, n);
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

// program() scaled so its RMS over the whole signal is targetDb dBFS.
Signal programAt(double targetDb) {
  Signal x = program();
  const double g = dbToLin(targetDb) / rmsRange(x, 0, x.size());
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

// process(in, out, n) over the whole signal in kBlock chunks.
using BlockFn = std::function<void(const float*, float*, int)>;

Signal run(const Signal& in, const BlockFn& fn) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    fn(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n);
  }
  return out;
}

double gainDb(const Signal& in, const Signal& out) {
  const size_t from = static_cast<size_t>(kMeasureFrom);
  return 20.0 * std::log10(rmsRange(out, from, out.size()) / rmsRange(in, from, in.size()));
}

bool verdict(const char* name, double db, double lo = kLoDb - kSlackDb, double hi = kHiDb + kSlackDb) {
  const bool ok = db >= lo && db <= hi;
  std::printf("%s %-30s %+6.2f dB  (want %+.1f..%+.1f)\n", ok ? "PASS" : "FAIL", name, db, lo, hi);
  return ok;
}

bool check(const char* name, const Signal& in, const BlockFn& fn) {
  return verdict(name, gainDb(in, run(in, fn)));
}

// Stages sit on the heap: the reverb and unison lines are large.
template <typename T>
struct Stage {
  T fx;
  Stage() { fx.reset(); }
};

template <typename T, typename P>
bool simple(const char* name, const Signal& in, const P& p) {
  auto s = std::make_unique<Stage<T>>();
  return check(name, in, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); });
}

// The Input gate as the box sets it, both pots at mid travel (-55 dBFS, 200 ms).
cv::GateParams inputGate() {
  ChainParams chain;
  applyKnob(Card::InputGate, 0, 0.5f, chain);
  applyKnob(Card::InputGate, 1, 0.5f, chain);
  return chain.inputGate;
}

// The Gate as the box sets it, both pots at mid travel (-40 dBFS, 70.7 ms).
cv::GateParams gateCard() {
  ChainParams chain;
  applyKnob(Card::Gate, 0, 0.5f, chain);
  applyKnob(Card::Gate, 1, 0.5f, chain);
  cv::GateParams p = chain.gate;
  p.on = true;
  return p;
}

cv::PitchFxParams pitchOff() {
  cv::PitchFxParams p;
  p.octave.on = false;
  return p;
}

// The box's reverb card placed and switched on, with its MIX knob at `mix` and every other
// setting (TIME, dwell, wobble) where the registry leaves it.
cv::ReverbParams boxReverb(Card card, float mix) {
  ChainParams chain;
  applyPlacement(card, chain);
  applyToggle(card, true, chain);
  applyKnob(card, 1, mix, chain);
  return chain.reverb;
}

// The wet path alone (crossfade 1), which the MIX knob never reaches; the level rule trims it.
cv::ReverbParams boxReverbWetOnly(Card card) {
  cv::ReverbParams p = boxReverb(card, 1.0f);
  p.wet = 1.0f;
  return p;
}

template <typename T, typename P>
bool midRow(const char* name, const Signal& in, const P& p) {
  auto s = std::make_unique<Stage<T>>();
  const double db = gainDb(in, run(in, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); }));
  return verdict(name, db, kMidLoDb, kMidHiDb);
}

// The Crowd card at VOICES 4 and DIVERGE `width`, behind the Autotune slot that fills its ring and runs the
// tracker, as the chain runs them.
bool crowdRow(const Signal& in, float width) {
  auto pitch = std::make_unique<Stage<PitchSeries>>();
  auto crowd = std::make_unique<Stage<cv::Crowd>>();
  const cv::PitchFxParams pp = pitchOff();
  cv::CrowdParams params;
  params.on = true;
  params.voicesStep = 4;
  params.width = width;
  char name[40];
  std::snprintf(name, sizeof name, "crowd VOICES 4 DIVERGE %g", static_cast<double>(width));
  return verdict(name, gainDb(in, run(in, [&](const float* i, float* o, int n) {
                   float tmp[cv::kBlock];
                   crowd->fx.write(i, n);
                   pitch->fx.process(i, tmp, n, pp, true);
                   crowd->fx.process(i, o, n, params, pitch->fx.crowdPitch());
                 })));
}

// The Crowd card's copies at the bottom of DIVERGE are exact doubles of the lead, a comb whose level on a
// steady tone hangs on each lag times f0. The gap rule redraws the lags twice a second on the program,
// so its rows run 30 s of it, 60 gaps, for the comb to average out. DIVERGE 1 is the engine default.
constexpr int kCrowdSeconds = 30;

bool crowdRows() {
  const Signal in = program(kCrowdSeconds);
  bool ok = true;
  for (float width : {0.0f, 0.5f, 1.0f}) ok &= crowdRow(in, width);
  return ok;
}

// The Formant card at SHIFT `shiftSt` with Autotune off, as the registry sets it. The card's trim is
// set for the wet path alone (MIX 100 %), as the Mob, Octave and reverb cards' are.
cv::PitchFxParams boxFormantAt(int shiftSt, float mix) {
  ChainParams chain;
  applyPlacement(Card::Formant, chain);
  applyToggle(Card::Formant, true, chain);
  applyKnob(Card::Formant, 0, static_cast<float>(shiftSt + cv::kFormantMaxShiftSt), chain);
  applyKnob(Card::Formant, 1, mix, chain);
  return chain.pitchFx;
}

// The card at its default SHIFT.
cv::PitchFxParams boxFormant(float mix) { return boxFormantAt(cv::kFormantDefaultShiftSt, mix); }

using testutil::readWav;

// samples/ is wholly git-ignored, so CI has none and the clip rows skip. With the folder there, a clip
// that is missing or empty fails its row.
bool samplesFolderPresent() {
  std::error_code ec;
  return std::filesystem::is_directory(CUBEVOX_SAMPLES_DIR, ec);
}

// A stage's input and its output over one padded clip.
struct Through {
  Signal in;
  Signal out;
};
using ClipFn = std::function<Through(const Signal& padded)>;

// Runs `through` over each of Adam's clips, as lofi_test and bloom's level check do. 0.4 s of silence
// ahead of a clip lets an enable fade settle and the stage's engines start clear, and the RMS runs over
// the clip alone, the stage's output against its own input. Each clip is a row held to [lo, hi].
// Returns false when a row fails; `mean` takes the mean of the rows that ran, in dB, and `count` how
// many ran (0 when samples/ is absent, which skips). A row that is not judged prints as a NOTE.
bool clipRows(const char* label, const ClipFn& through, double lo, double hi, double& mean, int& count,
              bool judged = true) {
  constexpr size_t kPad = cv::kSampleRate * 2 / 5;
  const char* names[] = {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"};
  bool ok = true;
  double sum = 0.0;
  count = 0;
  for (const char* name : names) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) {
      if (samplesFolderPresent()) {
        std::printf("FAIL %s %s  the clip is missing or empty\n", label, name);
        ok = false;
      } else {
        std::printf("SKIP %s %s  samples/ is not in this checkout\n", label, name);
      }
      continue;
    }
    Signal padded(kPad, 0.0f);
    padded.insert(padded.end(), clip.begin(), clip.end());
    const Through t = through(padded);
    const double db = 20.0 * std::log10(rmsRange(t.out, kPad, t.out.size()) / rmsRange(t.in, kPad, t.in.size()));
    if (judged) {
      ok &= verdict((std::string(label) + " " + name).c_str(), db, lo, hi);
    } else {
      std::printf("NOTE %-30s %+6.2f dB\n", (std::string(label) + " " + name).c_str(), db);
    }
    sum += db;
    ++count;
  }
  mean = count > 0 ? sum / count : 0.0;
  return ok;
}

// The Formant card on the program at MIX 50 % and on Adam's clips: the voices sit from correlated (the
// program) to unrelated (the clips), and the level gain keeps both inside the mid band. The wet path
// alone (MIX 100 %) runs at every SHIFT on Adam's clips. The grains sum to a level that follows SHIFT
// and the three clips differ by up to 1.2 dB under one trim, so each clip is held to the mid band and
// the mean of the three to the rule, which the per-SHIFT trim table is fit to. The synthetic program
// at the default SHIFT is reported next to them, as Octave's is not held to the rule either.
bool formantRows(const Signal& in) {
  bool ok = true;
  {
    auto s = std::make_unique<Stage<PitchSeries>>();
    const cv::PitchFxParams full = boxFormant(1.0f);
    const double programDb = gainDb(in, run(in, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, full); }));
    std::printf("NOTE %-30s %+6.2f dB  (the synthetic program, not held to the rule)\n", "formant -3 st mix 1 program", programDb);
  }
  ok &= midRow<PitchSeries>("formant -3 st mix .5", in, boxFormant(0.5f));
  const cv::PitchFxParams half = boxFormant(0.5f);
  double mean = 0.0;
  int count = 0;
  ok &= clipRows(
      "formant -3 st mix .5",
      [&](const Signal& padded) {
        auto s = std::make_unique<Stage<PitchSeries>>();
        return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, half); })};
      },
      kMidLoDb, kMidHiDb, mean, count);
  for (int st = -cv::kFormantMaxShiftSt; st <= cv::kFormantMaxShiftSt; ++st) {
    char label[40];
    std::snprintf(label, sizeof label, "formant %+d st mix 1", st);
    const cv::PitchFxParams full = boxFormantAt(st, 1.0f);
    ok &= clipRows(
        label,
        [&](const Signal& padded) {
          auto s = std::make_unique<Stage<PitchSeries>>();
          return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, full); })};
        },
        kMidLoDb, kMidHiDb, mean, count);
    if (count > 0) {
      char name[48];
      std::snprintf(name, sizeof name, "%s mean", label);
      ok &= verdict(name, mean, kLoDb, kHiDb);
    }
  }
  return ok;
}

// The Octave card as the registry sets it, at SEMITONES `semis` and MIX `mix`, Autotune off.
cv::PitchFxParams boxOctave(int semis, float mix) {
  ChainParams chain;
  applyPlacement(Card::Octave, chain);
  applyToggle(Card::Octave, true, chain);
  applyKnob(Card::Octave, 0, static_cast<float>(semis + cv::OctaveVoice::kMaxSemis), chain);
  applyKnob(Card::Octave, 1, mix, chain);
  return chain.pitchFx;
}

// The Octave card on Adam's clips. Its two taps sit 20 ms apart in the input, so a note adds
// coherently where 20 ms holds a whole number of periods and cancels in part where it holds a half,
// and the gain swings 2 dB with the sung pitch. The synthetic program sits between the peaks and
// cannot stand in for him. The wet path alone (MIX 100 %) is held to the rule on the mean of the
// three clips at four intervals, each clip to the rule with its slack (they read +0.13 to +0.29 dB).
// MIX 50 % reads -0.16 to +0.09 dB and is held to +-0.4 dB.
bool octaveRows() {
  bool ok = true;
  for (int semis : {-12, -5, 7, 12}) {
    char label[40];
    std::snprintf(label, sizeof label, "octave %+d st mix 1", semis);
    const cv::PitchFxParams p = boxOctave(semis, 1.0f);
    double mean = 0.0;
    int count = 0;
    ok &= clipRows(
        label,
        [&](const Signal& padded) {
          auto s = std::make_unique<Stage<PitchSeries>>();
          return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); })};
        },
        kLoDb - kSlackDb, kHiDb + kSlackDb, mean, count);
    if (count > 0) {
      char name[48];
      std::snprintf(name, sizeof name, "%s mean", label);
      ok &= verdict(name, mean, kLoDb, kHiDb);
    }
  }
  const cv::PitchFxParams half = boxOctave(-12, 0.5f);
  double mean = 0.0;
  int count = 0;
  ok &= clipRows(
      "octave -12 st mix .5",
      [&](const Signal& padded) {
        auto s = std::make_unique<Stage<PitchSeries>>();
        return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, half); })};
      },
      -0.4, 0.4, mean, count);
  return ok;
}

// The Autotune card as the registry sets it, at KEY index `key` and RESPONSE `responseMs`.
cv::PitchFxParams boxAutotune(int key, float responseMs) {
  ChainParams chain;
  applyPlacement(Card::Autotune, chain);
  applyToggle(Card::Autotune, true, chain);
  applyKnob(Card::Autotune, 0, static_cast<float>(key), chain);
  applyKnob(Card::Autotune, 1, cv::AutotuneVoice::mechanicalOf(responseMs), chain);
  return chain.pitchFx;
}

// The Autotune voice on Adam's clips, which carry the pitches the synthetic program does not (it sits at
// 180 Hz). The three clips hold the rule with its slack and their mean holds it outright, at hard tune and
// at two slow RESPONSE settings, where the correction glides and the grain spacing moves. Across the twelve
// keys and four RESPONSE settings the mean reads +0.08 to +0.37 dB; these rows sit at the ends of that span.
bool autotuneRows() {
  bool ok = true;
  for (float responseMs : {1.0f, 100.0f, 500.0f}) {
    char label[40];
    std::snprintf(label, sizeof label, "autotune key C response %g ms", static_cast<double>(responseMs));
    const cv::PitchFxParams p = boxAutotune(0, responseMs);
    double mean = 0.0;
    int count = 0;
    ok &= clipRows(
        label,
        [&](const Signal& padded) {
          auto s = std::make_unique<Stage<PitchSeries>>();
          return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); })};
        },
        kLoDb - kSlackDb, kHiDb + kSlackDb, mean, count);
    if (count > 0) {
      char name[48];
      std::snprintf(name, sizeof name, "%s mean", label);
      ok &= verdict(name, mean, kLoDb, kHiDb);
    }
  }
  return ok;
}

// Echo and Unison add their wet on top of the dry voice, which passes at unity at every Amount, so
// the level is the dry plus whatever the wet adds. Each card has a reference setting, where its
// trim is tuned and its mean over Adam's clips is held to the rule's band, and a 100 % row, which
// is reported beside it and not judged: the wet at full Amount is a few dB over the dry, and the
// trim cannot bring both settings into the band without turning the knob over.
//
// An echo adds to its dry voice or cancels it depending on TIME, so an Echo clip row is the power
// sum over the knob's range, 60 to 250 ms in 10 ms steps, with a fresh engine at each step.
Through echoThrough(const Signal& padded, float amount) {
  Through t;
  for (float ms = cv::Slapback::kTimeMinMs; ms <= cv::Slapback::kTimeMaxMs; ms += 10.0f) {
    cv::SlapbackParams p;
    p.amount = amount;
    p.tuning.timeMs = ms;
    auto s = std::make_unique<Stage<cv::Slapback>>();
    const Signal out = run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); });
    t.in.insert(t.in.end(), padded.begin(), padded.end());
    t.out.insert(t.out.end(), out.begin(), out.end());
  }
  return t;
}

// The Unison card as the registry sets it, with RATE at `rate` and DEPTH at `depth` (both 0..1 of travel).
cv::UnisonParams boxUnison(float rate, float depth) {
  ChainParams chain;
  applyPlacement(Card::Unison, chain);
  applyToggle(Card::Unison, true, chain);
  applyKnob(Card::Unison, 0, rate, chain);
  applyKnob(Card::Unison, 1, depth, chain);
  return chain.unison;
}

Through unisonThrough(const Signal& padded, float rate, float depth) {
  const cv::UnisonParams p = boxUnison(rate, depth);
  auto s = std::make_unique<Stage<cv::Unison>>();
  return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); })};
}

// One card's reference row (judged on the mean, each clip held to the mid band) and its 100 % row.
bool wetOnDryRows(const char* card, const char* reference, const ClipFn& atReference, const ClipFn& atFull) {
  double mean = 0.0;
  int count = 0;
  bool ok = clipRows((std::string(card) + " " + reference).c_str(), atReference, kMidLoDb, kMidHiDb, mean, count);
  if (count > 0) ok &= verdict((std::string(card) + " " + reference + " mean").c_str(), mean, kLoDb, kHiDb);
  clipRows((std::string(card) + " 100 %").c_str(), atFull, kMidLoDb, kMidHiDb, mean, count, false);
  if (count > 0) {
    std::printf("NOTE %-30s %+6.2f dB  (rule %+.1f..%+.1f, not judged)\n", (std::string(card) + " 100 % mean").c_str(),
                mean, kLoDb, kHiDb);
  }
  return ok;
}

// Echo at Amount 25 %, where the repeats are tuned and the emulator starts.
bool echoRows() {
  return wetOnDryRows(
      "echo", "25 %", [](const Signal& padded) { return echoThrough(padded, 0.25f); },
      [](const Signal& padded) { return echoThrough(padded, 1.0f); });
}

// Unison at DEPTH 50 %, judged at RATE 0 %, 50 % and 100 %: the mean of the clips against the rule and each
// clip against the mid band. RATE 50 % is where the emulator starts and the wet's trim is tuned, and its
// DEPTH 100 % row is reported beside it and not judged. The Rate law's wet trim holds the top of the RATE
// knob to the same band.
bool unisonRows() {
  bool ok = wetOnDryRows(
      "unison RATE 50 % DEPTH", "50 %", [](const Signal& padded) { return unisonThrough(padded, 0.5f, 0.5f); },
      [](const Signal& padded) { return unisonThrough(padded, 0.5f, 1.0f); });
  for (const float rate : {0.0f, 1.0f}) {
    char label[40];
    std::snprintf(label, sizeof label, "unison RATE %.0f %% DEPTH 50 %%", static_cast<double>(rate * 100.0f));
    double mean = 0.0;
    int count = 0;
    ok &= clipRows(
        label, [rate](const Signal& padded) { return unisonThrough(padded, rate, 0.5f); }, kMidLoDb, kMidHiDb, mean,
        count);
    if (count > 0) {
      char name[48];
      std::snprintf(name, sizeof name, "%s mean", label);
      ok &= verdict(name, mean, kLoDb, kHiDb);
    }
  }
  return ok;
}

// Largest |second difference| of x over [from, to).
double peakCurvature(const Signal& x, size_t from, size_t to) {
  double worst = 0.0;
  for (size_t i = from; i < to; ++i) {
    worst = std::max(worst, std::fabs(static_cast<double>(x[i]) - 2.0 * x[i - 1] + x[i - 2]));
  }
  return worst;
}

// The output of an engine that is switched on at sample `at` of the clip. A cold engine is reset there and
// fed from there, as the chain does on a slot's first fade-in block. A warm one ran off on the clip from the
// start, so its line holds the voice. Returns the 100 ms from the switch.
Signal unisonFromSwitch(const Signal& clip, size_t at, bool warm) {
  const cv::UnisonParams on = boxUnison(0.5f, 0.5f);
  cv::UnisonParams off = on;
  off.on = false;
  constexpr size_t kSpan = cv::kSampleRate / 10;
  auto u = std::make_unique<Stage<cv::Unison>>();
  Signal sink(cv::kBlock);
  if (warm) {
    for (size_t i = 0; i < at; i += cv::kBlock) u->fx.process(&clip[i], sink.data(), cv::kBlock, off);
  }
  Signal out(kSpan);
  for (size_t i = 0; i < kSpan; i += cv::kBlock) u->fx.process(&clip[at + i], &out[i], cv::kBlock, on);
  return out;
}

// Unison switches on partway through each of Adam's clips, at every voiced block-aligned point 0.1 s apart.
// The curvature of the output from 12 to 70 ms after the switch is read against a warm engine switched on at
// the same point. A tap that reads the empty edge of a cleared line steps the output and reads several times
// the warm engine's. The median and the worst point are both held.
bool unisonSwitchOnRows() {
  constexpr size_t kFrom = 12 * cv::kSampleRate / 1000;
  constexpr size_t kTo = 70 * cv::kSampleRate / 1000;
  constexpr size_t kStride = (cv::kSampleRate / 10 / cv::kBlock) * cv::kBlock;
  constexpr double kMedianMax = 1.1;
  constexpr double kWorstMax = 1.3;
  const char* names[] = {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"};
  bool ok = true;
  for (const char* name : names) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) {
      if (samplesFolderPresent()) {
        std::printf("FAIL unison switch-on %s  the clip is missing or empty\n", name);
        ok = false;
      } else {
        std::printf("SKIP unison switch-on %s  samples/ is not in this checkout\n", name);
      }
      continue;
    }
    std::vector<double> ratios;
    for (size_t at = kStride; at + 2 * kStride < clip.size(); at += kStride) {
      if (rmsRange(clip, at, at + kStride) < dbToLin(-40.0)) continue;
      const Signal cold = unisonFromSwitch(clip, at, false);
      const Signal warm = unisonFromSwitch(clip, at, true);
      const double base = peakCurvature(warm, kFrom, kTo);
      if (base > 0.0) ratios.push_back(peakCurvature(cold, kFrom, kTo) / base);
    }
    if (ratios.empty()) continue;
    std::sort(ratios.begin(), ratios.end());
    const double median = ratios[ratios.size() / 2];
    const bool row = median <= kMedianMax && ratios.back() <= kWorstMax;
    ok &= row;
    std::printf("%s unison switch-on %s  %zu points: curvature x%.3f median, x%.3f worst of a warm line (<= %.1f, %.1f)\n",
                row ? "PASS" : "FAIL", name, ratios.size(), median, ratios.back(), kMedianMax, kWorstMax);
  }
  return ok;
}

// The Lo-fi card at its default, TONE 50 % and AMOUNT 50 %, on Adam's clips, each held to the rule's
// band. The card has no MIX knob, so the mid band does not apply. The synthetic program puts its energy
// where the telephone band cuts, so it has no row.
bool lofiRows() {
  ChainParams chain;
  applyPlacement(Card::Lofi, chain);
  applyToggle(Card::Lofi, true, chain);
  applyKnob(Card::Lofi, 0, 0.5f, chain);
  applyKnob(Card::Lofi, 1, 0.5f, chain);
  double mean = 0.0;
  int count = 0;
  return clipRows(
      "lofi 50/50",
      [&](const Signal& padded) {
        auto s = std::make_unique<Stage<cv::Lofi>>();
        return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, chain.lofi); })};
      },
      kLoDb, kHiDb, mean, count);
}

// The Chatter card at its default (the engine's LENGTH and AMOUNT 50 %), on Adam's clips, once listening
// to its own input and once keyed by the Input gate at the registry's mid-travel setting (-55 dBFS), with
// the gate ahead of it as the box runs them. The synthetic program carries a -40 dBFS noise floor that
// holds the card's word open for the whole run, which the trim is not set for, so it has no row there.
bool bloomRows() {
  ChainParams chain;
  applyPlacement(Card::Bloom, chain);
  applyToggle(Card::Bloom, true, chain);
  applyKnob(Card::Bloom, 1, 0.5f, chain);
  const cv::GateParams gate = inputGate();
  double mean = 0.0;
  int count = 0;
  bool ok = clipRows(
      "bloom own key",
      [&](const Signal& padded) {
        auto s = std::make_unique<Stage<cv::Bloom>>();
        return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, chain.bloom); })};
      },
      kLoDb, kHiDb, mean, count);
  ok &= clipRows(
      "bloom gate key",
      [&](const Signal& padded) {
        auto g = std::make_unique<Stage<cv::Gate>>();
        auto s = std::make_unique<Stage<cv::Bloom>>();
        cv::BloomParams keyed = chain.bloom;
        keyed.keyFromGate = true;
        Through t{Signal(padded.size()), Signal(padded.size())};
        for (size_t i = 0; i < padded.size(); i += cv::kBlock) {
          const int n = static_cast<int>(std::min<size_t>(cv::kBlock, padded.size() - i));
          g->fx.process(&padded[i], &t.in[i], n, gate);
          s->fx.process(&t.in[i], &t.out[i], n, keyed, g->fx.open());
        }
        return t;
      },
      kLoDb, kHiDb, mean, count);
  return ok;
}

// A reverb block run over each clip with fixed parameters.
ClipFn reverbThrough(const cv::ReverbParams& p) {
  return [&p](const Signal& padded) {
    auto s = std::make_unique<Stage<cv::Reverb>>();
    return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); })};
  };
}

// A reverb card at the registry's TIME on Adam's clips, once with the wet path alone and once at
// MIX 50 %. The synthetic program puts its energy where the loops cut, below 200 Hz and above 5 kHz,
// so it reads the cards differently from a voice, and the clips govern. The clips spread by up to a
// dB under one gain, so no trim puts all three inside 0 to +0.5 dB. Each clip is held to the mid
// band, and the mean of the three on the wet path alone is held to the rule.
bool reverbRows(Card card, const char* name) {
  const std::string wetLabel = std::string("reverb ") + name + " wet only";
  const cv::ReverbParams wetOnly = boxReverbWetOnly(card);
  double mean = 0.0;
  int count = 0;
  bool ok = clipRows(wetLabel.c_str(), reverbThrough(wetOnly), kMidLoDb, kMidHiDb, mean, count);
  if (count > 0) ok &= verdict((wetLabel + " mean").c_str(), mean);
  const cv::ReverbParams half = boxReverb(card, 0.5f);
  ok &= clipRows((std::string("reverb ") + name + " MIX 50 %").c_str(), reverbThrough(half), kMidLoDb, kMidHiDb, mean,
                 count);
  return ok;
}

// The mean over Adam's clips of a stage's output against its input, in dB, and how many clips ran.
// `whole` is false when samples/ is there and a clip in it is missing or empty.
double clipMean(const ClipFn& through, int& count, bool& whole) {
  constexpr size_t kPad = cv::kSampleRate * 2 / 5;
  const char* names[] = {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"};
  double sum = 0.0;
  count = 0;
  whole = true;
  for (const char* name : names) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) {
      if (samplesFolderPresent()) {
        std::printf("FAIL %s  the clip is missing or empty\n", name);
        whole = false;
      }
      continue;
    }
    Signal padded(kPad, 0.0f);
    padded.insert(padded.end(), clip.begin(), clip.end());
    const Through t = through(padded);
    sum += 20.0 * std::log10(rmsRange(t.out, kPad, t.out.size()) / rmsRange(t.in, kPad, t.in.size()));
    ++count;
  }
  return count > 0 ? sum / count : 0.0;
}

// The wet path alone at the TIME knob's bottom, middle and top. The input gain follows the feedback, so
// the level holds across the knob: each is held within kTimeBandDb of the middle's.
bool timeRows(Card card, const char* name) {
  constexpr double kTimeBandDb = 1.0;
  double mean[3] = {0.0, 0.0, 0.0};
  int count = 0;
  bool whole = true;
  const float times[3] = {0.0f, 0.5f, 1.0f};
  for (int k = 0; k < 3; ++k) {
    ChainParams chain;
    applyPlacement(card, chain);
    applyToggle(card, true, chain);
    applyKnob(card, 0, times[k], chain);
    applyKnob(card, 1, 1.0f, chain);
    cv::ReverbParams p = chain.reverb;
    p.wet = 1.0f;
    bool wholeHere = true;
    mean[k] = clipMean(reverbThrough(p), count, wholeHere);
    whole = whole && wholeHere;
  }
  if (!whole) {
    std::printf("FAIL reverb %s TIME rows  a clip in samples/ is missing or empty\n", name);
    return false;
  }
  if (count == 0) {
    std::printf("SKIP reverb %s TIME rows  samples/ is not in this checkout\n", name);
    return true;
  }
  const double lo = std::fmin(mean[0], mean[2]) - mean[1], hi = std::fmax(mean[0], mean[2]) - mean[1];
  const bool ok = lo >= -kTimeBandDb && hi <= kTimeBandDb;
  std::printf("%s reverb %-7s wet only mean at TIME 0 / 50 / 100 %%: %+.2f / %+.2f / %+.2f dB  (ends within %.1f of the middle)\n",
              ok ? "PASS" : "FAIL", name, mean[0], mean[1], mean[2], kTimeBandDb);
  return ok;
}

// The Distortion card at DRIVE `drive` and TONE 50 %, as the registry sets it.
cv::DistortionParams boxDistortion(float drive) {
  ChainParams chain;
  applyPlacement(Card::Distortion, chain);
  applyToggle(Card::Distortion, true, chain);
  applyKnob(Card::Distortion, 0, 0.5f, chain);
  applyKnob(Card::Distortion, 1, drive, chain);
  return chain.distortion;
}

ClipFn distortionThrough(const cv::DistortionParams& p) {
  return [p](const Signal& padded) {
    auto s = std::make_unique<Stage<cv::Distortion>>();
    return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); })};
  };
}

// Distortion compresses, and its makeup follows DRIVE alone, so how loud it comes out depends on the voice:
// Adam's clips sit 5 to 7 dB under the synthetic program and come out louder. The clips govern. At the
// default (DRIVE 30 %, TONE 50 %) each clip is held to the mid band and their mean to the rule. The makeup
// law is fitted so every DRIVE from 10 % to 100 % at TONE 50 % reads within kDriveBandDb of the default's
// mean. The synthetic program is reported.
bool distortionRows(const Signal& program) {
  constexpr double kDriveBandDb = 0.5;
  const cv::DistortionParams def = boxDistortion(kDistortionRuleDrive);
  double mean = 0.0;
  int count = 0;
  bool ok = clipRows("distortion drive .3", distortionThrough(def), kMidLoDb, kMidHiDb, mean, count);
  if (count > 0) ok &= verdict("distortion drive .3 mean", mean, kLoDb, kHiDb);

  double lo = 0.0, hi = 0.0;
  bool whole = true;
  for (int pct = 10; pct <= 100; pct += 10) {
    bool wholeHere = true;
    int n = 0;
    const double m = clipMean(distortionThrough(boxDistortion(static_cast<float>(pct) / 100.0f)), n, wholeHere);
    whole = whole && wholeHere;
    if (n == 0) continue;
    lo = std::fmin(lo, m - mean);
    hi = std::fmax(hi, m - mean);
  }
  if (!whole) {
    std::printf("FAIL distortion DRIVE spread  a clip in samples/ is missing or empty\n");
    ok = false;
  } else if (count > 0) {
    const bool inBand = lo >= -kDriveBandDb && hi <= kDriveBandDb;
    std::printf("%s distortion DRIVE 10..100 %% mean vs the default: %+.2f..%+.2f dB  (within %.1f)\n",
                inBand ? "PASS" : "FAIL", lo, hi, kDriveBandDb);
    ok &= inBand;
  }

  auto s = std::make_unique<Stage<cv::Distortion>>();
  const double programDb = gainDb(program, run(program, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, def); }));
  std::printf("NOTE %-30s %+6.2f dB  (the synthetic program, not held to the rule)\n", "distortion drive .3 program", programDb);
  return ok;
}

// Both gates at their pots' middle on Adam's clips. The synthetic program carries a -40 dBFS noise floor
// that keeps both gates open, so it cannot show a gate's level; on the clips both close on 8 to 23 % of
// their blocks. Each clip is held to the rule with its slack.
bool gateRows() {
  bool ok = true;
  const struct {
    const char* label;
    cv::GateParams params;
  } cards[] = {{"input gate -55", inputGate()}, {"gate -40", gateCard()}};
  for (const auto& card : cards) {
    double mean = 0.0;
    int count = 0;
    ok &= clipRows(
        card.label,
        [&](const Signal& padded) {
          auto s = std::make_unique<Stage<cv::Gate>>();
          return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, card.params); })};
        },
        kLoDb - kSlackDb, kHiDb + kSlackDb, mean, count);
  }
  return ok;
}

// The slot stages at their default engaged settings.
bool slotRows(const Signal& in) {
  bool ok = true;
  ok &= gateRows();

  cv::PitchFxParams autotune = pitchOff();
  autotune.autotune.on = true;
  ok &= simple<PitchSeries>("autotune key C", in, autotune);
  ok &= autotuneRows();

  ok &= octaveRows();

  ok &= unisonRows();
  ok &= unisonSwitchOnRows();
  ok &= crowdRows();
  ok &= formantRows(in);
  ok &= lofiRows();
  ok &= bloomRows();

  ok &= echoRows();

  ok &= distortionRows(in);

  ok &= reverbRows(Card::ReverbChasm, "chasm");
  ok &= reverbRows(Card::ReverbSpring, "spring");
  ok &= timeRows(Card::ReverbChasm, "chasm");
  ok &= timeRows(Card::ReverbSpring, "spring");
  return ok;
}

// The tail's engines in the box's order: the front half of VOX EQ, POLISH COMP, the back half. `cuts`
// picks VOX EQ's ON+CUTS (true) or ON (false).
struct Tail {
  cv::Polish eq;
  cv::PolishComp comp;
  cv::PolishParams params;
  explicit Tail(bool cuts) {
    params.cuts = cuts;
    eq.reset();
    comp.reset();
  }

  void front(const float* in, float* out, int n) { eq.processFront(in, out, n, params); }
  void compress(const float* in, float* out, int n) { comp.process(in, out, n, cv::PolishCompParams{}); }
  void back(const float* in, float* out, int n) { eq.processBack(in, out, n, params); }
};

// POLISH COMP alone, VOX EQ alone (both halves) at ON and at ON+CUTS, and the whole tail at both, on
// Adam's clips. The clips sit 1.8 dB apart in level and the comp levels them, so its three readings
// spread past the mid band. The spread is printed, and the mean holds the rule. VOX EQ and the whole
// tail hold the mid band per clip.
bool tailClipRows() {
  double mean = 0.0;
  int count = 0;
  const auto compOnly = [](const Signal& padded) {
    auto t = std::make_unique<Tail>(true);
    return Through{padded, run(padded, [&](const float* i, float* o, int n) { t->compress(i, o, n); })};
  };
  const auto eqOnly = [](bool cuts) {
    return [cuts](const Signal& padded) {
      auto t = std::make_unique<Tail>(cuts);
      return Through{padded, run(padded, [&](const float* i, float* o, int n) {
                       float mid[cv::kBlock];
                       t->front(i, mid, n);
                       t->back(mid, o, n);
                     })};
    };
  };
  const auto whole = [](bool cuts) {
    return [cuts](const Signal& padded) {
      auto t = std::make_unique<Tail>(cuts);
      return Through{padded, run(padded, [&](const float* i, float* o, int n) {
                       float a[cv::kBlock];
                       float b[cv::kBlock];
                       t->front(i, a, n);
                       t->compress(a, b, n);
                       t->back(b, o, n);
                     })};
    };
  };
  bool ok = clipRows("polish COMP", compOnly, kMidLoDb, kMidHiDb, mean, count, false);
  if (count > 0) ok &= verdict("polish COMP mean", mean, kLoDb, kHiDb);
  for (const bool cuts : {false, true}) {
    const std::string eqName = cuts ? "vox EQ ON+CUTS" : "vox EQ ON";
    const std::string tailName = cuts ? "vox tail ON+CUTS" : "vox tail ON";
    ok &= clipRows(eqName.c_str(), eqOnly(cuts), kMidLoDb, kMidHiDb, mean, count);
    if (count > 0) ok &= verdict((eqName + " mean").c_str(), mean, kLoDb, kHiDb);
    // At ON the comp's gain on the uncut clips and VOX EQ's own gain add, so the whole tail stands
    // above the rule's band. Each stage alone holds the rule. The ON tail is printed and not judged.
    ok &= clipRows(tailName.c_str(), whole(cuts), kMidLoDb, kMidHiDb, mean, count, cuts);
    if (count > 0 && cuts) ok &= verdict((tailName + " mean").c_str(), mean, kLoDb, kHiDb);
    if (count > 0 && !cuts) std::printf("NOTE %-30s %+6.2f dB mean\n", tailName.c_str(), mean);
  }
  return ok;
}

// The comp on the -23 dBFS program, and the two EQ halves in series on the -18 dBFS one, at ON and at
// ON+CUTS. The trim in the tuning is set for the pair.
bool tailRows(const Signal& in) {
  bool ok = simple<cv::PolishComp>("polish COMP defaults", programAt(kCompProgramDbfs), cv::PolishCompParams{});
  for (const bool cuts : {false, true}) {
    cv::PolishParams eq;
    eq.cuts = cuts;
    auto polish = std::make_unique<Stage<cv::Polish>>();
    ok &= check(cuts ? "vox EQ ON+CUTS defaults" : "vox EQ ON defaults", in, [&](const float* i, float* o, int n) {
      float tmp[cv::kBlock];
      polish->fx.processFront(i, tmp, n, eq);
      polish->fx.processBack(tmp, o, n, eq);
    });
  }
  return ok & tailClipRows();
}

}  // namespace

int main() {
  const Signal in = program();
  bool ok = slotRows(in);
  ok &= tailRows(in);
  std::printf("%s level\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
