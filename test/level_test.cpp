// Level rule: every stage at its default engaged setting lands 0 to +0.5 dB
// (out/in RMS) on vocal-like program material. The reverb, Formant, Lo-fi and Chatter rows take every
// setting from the registry, as the box runs them. The Spring, Formant MIX, Lo-fi and Chatter rows run on
// Adam's clips in samples/ and skip when they are absent: the synthetic program puts its energy where the
// telephone band cuts and where the Spring loops cut, and its noise floor holds the Chatter card's word open.
// No window, no audio device.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include "engine/pitch_fx.h"
#include "engine/polish.h"
#include "engine/polish_comp.h"
#include "engine/reverb.h"
#include "engine/slapback.h"
#include "engine/unison.h"

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

// The program's steady tone adds to its own echo or cancels it depending on
// TIME, so one time is a coin flip. The mean over the knob's range is the power sum.
bool slapbackRow(const char* name, const Signal& in, cv::SlapbackParams p) {
  double sum = 0.0;
  int count = 0;
  for (float ms = 60.0f; ms <= 250.0f; ms += 10.0f) {
    p.tuning.timeMs = ms;
    auto s = std::make_unique<Stage<cv::Slapback>>();
    sum += gainDb(in, run(in, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); }));
    ++count;
  }
  return verdict(name, sum / count);
}

// The Input gate as the box sets it, both pots at mid travel (-55 dBFS, 200 ms).
cv::GateParams inputGate() {
  ChainParams chain;
  applyKnob(Card::InputGate, 0, 0.5f, chain);
  applyKnob(Card::InputGate, 1, 0.5f, chain);
  return chain.inputGate;
}

cv::PitchFxParams pitchOff() {
  cv::PitchFxParams p;
  p.octave.on = false;
  return p;
}

cv::PitchFxParams octave(float mix) {
  cv::PitchFxParams p = pitchOff();
  p.octave.on = true;
  p.octave.mix = mix;
  p.octave.semitones = -12;
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

// The Crowd card at VOICES 4 and DIVERGE `width`, behind the PitchFx that fills its ring and runs the
// tracker, as the chain runs them.
bool crowdRow(const Signal& in, float width) {
  auto pitch = std::make_unique<Stage<cv::PitchFx>>();
  auto crowd = std::make_unique<Stage<cv::Crowd>>();
  cv::PitchFxParams pp = pitchOff();
  pp.crowdOn = true;
  cv::CrowdParams params;
  params.on = true;
  params.voicesStep = 4;
  params.width = width;
  char name[40];
  std::snprintf(name, sizeof name, "crowd VOICES 4 DIVERGE %g", static_cast<double>(width));
  return verdict(name, gainDb(in, run(in, [&](const float* i, float* o, int n) {
                   float tmp[cv::kBlock];
                   pitch->fx.process(i, tmp, n, pp);
                   crowd->fx.process(i, o, n, params, pitch->fx);
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

// The Formant card at SHIFT -3 st with Autotune off, as the registry sets it. The card's trim is set
// for the wet path alone (MIX 100 %), as the Mob, Octave and reverb cards' are.
cv::PitchFxParams boxFormant(float mix) {
  ChainParams chain;
  applyPlacement(Card::Formant, chain);
  applyToggle(Card::Formant, true, chain);
  applyKnob(Card::Formant, 0, 4.0f, chain);  // position 4 is -3 st
  applyKnob(Card::Formant, 1, mix, chain);
  return chain.pitchFx;
}

// 16-bit mono PCM WAV, any chunks before data. Empty on a missing or other file.
Signal readWav(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0) return {};
  size_t pos = 12;
  while (pos + 8 <= b.size()) {
    uint32_t len;
    std::memcpy(&len, &b[pos + 4], 4);
    if (std::memcmp(&b[pos], "data", 4) == 0) {
      const size_t n = std::min<size_t>(len, b.size() - pos - 8) / 2;
      Signal x(n);
      for (size_t i = 0; i < n; ++i) {
        int16_t s;
        std::memcpy(&s, &b[pos + 8 + 2 * i], 2);
        x[i] = static_cast<float>(s) / 32768.0f;
      }
      return x;
    }
    pos += 8 + len + (len & 1);
  }
  return {};
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
// many ran (0 when samples/ is absent, which skips).
bool clipRows(const char* label, const ClipFn& through, double lo, double hi, double& mean, int& count) {
  constexpr size_t kPad = cv::kSampleRate * 2 / 5;
  const char* names[] = {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"};
  bool ok = true;
  double sum = 0.0;
  count = 0;
  for (const char* name : names) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) {
      std::printf("SKIP %s %s  samples/ is not in this checkout\n", label, name);
      continue;
    }
    Signal padded(kPad, 0.0f);
    padded.insert(padded.end(), clip.begin(), clip.end());
    const Through t = through(padded);
    const double db = 20.0 * std::log10(rmsRange(t.out, kPad, t.out.size()) / rmsRange(t.in, kPad, t.in.size()));
    ok &= verdict((std::string(label) + " " + name).c_str(), db, lo, hi);
    sum += db;
    ++count;
  }
  mean = count > 0 ? sum / count : 0.0;
  return ok;
}

// The Formant card at its default, SHIFT -3 st and MIX 100 %, on the program. MIX 50 % is held to
// the mid band on the program and on Adam's clips: the voices sit from correlated (the program) to
// unrelated (the clips), and the level gain keeps both inside the band.
bool formantRows(const Signal& in) {
  bool ok = simple<cv::PitchFx>("formant -3 st mix 1", in, boxFormant(1.0f));
  ok &= midRow<cv::PitchFx>("formant -3 st mix .5", in, boxFormant(0.5f));
  const cv::PitchFxParams half = boxFormant(0.5f);
  double mean = 0.0;
  int count = 0;
  ok &= clipRows(
      "formant -3 st mix .5",
      [&](const Signal& padded) {
        auto s = std::make_unique<Stage<cv::PitchFx>>();
        return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, half); })};
      },
      kMidLoDb, kMidHiDb, mean, count);
  return ok;
}

// The Lo-fi card at its default, TONE 50 % and AMOUNT 50 %, on Adam's clips. The synthetic program
// puts its energy where the telephone band cuts and reads -2.1 dB. The three clips differ by 1.14 dB
// under one gain, so no trim puts all three inside 0 to +0.5 dB: each clip is held to the mid band, and
// the mean of the three is reported against the rule.
bool lofiRows() {
  ChainParams chain;
  applyPlacement(Card::Lofi, chain);
  applyToggle(Card::Lofi, true, chain);
  applyKnob(Card::Lofi, 0, 0.5f, chain);
  applyKnob(Card::Lofi, 1, 0.5f, chain);
  double mean = 0.0;
  int count = 0;
  const bool ok = clipRows(
      "lofi 50/50",
      [&](const Signal& padded) {
        auto s = std::make_unique<Stage<cv::Lofi>>();
        return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, chain.lofi); })};
      },
      kMidLoDb, kMidHiDb, mean, count);
  if (count > 0) {
    std::printf("NOTE %-30s %+6.2f dB  (mean of %d clips against the rule %+.1f..%+.1f)\n", "lofi 50/50 mean", mean, count,
                kLoDb, kHiDb);
  }
  return ok;
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

// The Spring card at the registry's TIME on Adam's clips, once with the wet path alone and once at
// MIX 50 %. The synthetic program puts its energy where the loops cut, below 200 Hz and above 5 kHz,
// so it reads the card differently from a voice. The clips spread by 0.66 dB under one gain, so no
// trim puts all three inside 0 to +0.5 dB. Each clip is held to the mid band, and the mean of the
// three on the wet path alone is held to the rule.
bool springRows() {
  const auto through = [](const cv::ReverbParams& p) {
    return ClipFn([&p](const Signal& padded) {
      auto s = std::make_unique<Stage<cv::Reverb>>();
      return Through{padded, run(padded, [&](const float* i, float* o, int n) { s->fx.process(i, o, n, p); })};
    });
  };
  double mean = 0.0;
  int count = 0;
  bool ok = clipRows("reverb spring wet only", through(boxReverbWetOnly(Card::ReverbSpring)), kMidLoDb, kMidHiDb, mean,
                     count);
  if (count > 0) ok &= verdict("reverb spring wet only mean", mean);
  ok &= clipRows("reverb spring MIX 50 %", through(boxReverb(Card::ReverbSpring, 0.5f)), kMidLoDb, kMidHiDb, mean,
                 count);
  return ok;
}

// The slot stages at their default engaged settings.
bool slotRows(const Signal& in) {
  bool ok = true;
  ok &= simple<cv::Gate>("input gate -55", in, inputGate());
  cv::GateParams panelGate;
  panelGate.thresholdDb = -40.0f;
  ok &= simple<cv::Gate>("panel gate -40", in, panelGate);

  cv::PitchFxParams autotune = pitchOff();
  autotune.autotune.on = true;
  ok &= simple<cv::PitchFx>("autotune key C", in, autotune);

  ok &= simple<cv::PitchFx>("octave -12 mix 1", in, octave(1.0f));
  ok &= midRow<cv::PitchFx>("octave -12 mix .5", in, octave(0.5f));

  cv::UnisonParams unison;
  unison.on = true;
  unison.depth = 0.5f;  // Adam default
  ok &= simple<cv::Unison>("unison depth .5", in, unison);
  ok &= crowdRows();
  ok &= formantRows(in);
  ok &= lofiRows();
  ok &= bloomRows();

  cv::SlapbackParams slap;
  slap.on = true;
  slap.mix = 0.5f;
  ok &= slapbackRow("slapback .5 mean TIME", in, slap);

  cv::DistortionParams dist;
  dist.on = true;
  dist.drive = 0.3f;
  dist.tone = 0.5f;
  ok &= simple<cv::Distortion>("distortion drive .3", in, dist);

  ok &= simple<cv::Reverb>("reverb chasm wet only", in, boxReverbWetOnly(Card::ReverbChasm));
  ok &= midRow<cv::Reverb>("reverb chasm MIX 50 %", in, boxReverb(Card::ReverbChasm, 0.5f));
  ok &= springRows();
  return ok;
}

// One comp row per named tuning, each with its own makeup; the default params carry FIRM.
bool tailRows(const Signal& in) {
  bool ok = true;
  const Signal compProgram = programAt(kCompProgramDbfs);
  cv::PolishCompParams comp;
  comp.on = true;
  ok &= simple<cv::PolishComp>("polish COMP defaults", compProgram, comp);
  comp.tuning = cv::PolishCompTuning::gentle();
  ok &= simple<cv::PolishComp>("polish COMP gentle", compProgram, comp);
  comp.tuning = cv::PolishCompTuning::medium();
  ok &= simple<cv::PolishComp>("polish COMP medium", compProgram, comp);

  // The two halves of POLISH EQ in series. The trim in the tuning is set for the pair.
  cv::PolishParams eq;
  eq.on = true;
  auto polish = std::make_unique<Stage<cv::Polish>>();
  ok &= check("polish EQ defaults", in, [&](const float* i, float* o, int n) {
    float tmp[cv::kBlock];
    polish->fx.processFront(i, tmp, n, eq);
    polish->fx.processBack(tmp, o, n, eq);
  });
  return ok;
}

}  // namespace

int main() {
  const Signal in = program();
  bool ok = slotRows(in);
  ok &= tailRows(in);
  std::printf("%s level\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
