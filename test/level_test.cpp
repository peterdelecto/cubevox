// Level rule: every stage at its default engaged setting lands 0 to +0.5 dB
// (out/in RMS) on vocal-like program material. The reverb rows take every
// setting from the registry, as the box runs them. No window, no audio device.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "core/chain_params.h"
#include "core/registry.h"
#include "engine/conditioning.h"
#include "engine/distortion.h"
#include "engine/gate.h"
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

// A MIX knob at 50 % is reported against a looser band: the equal-power crossfade keeps it near
// unity without its own trim.
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
Signal program() {
  const size_t n = static_cast<size_t>(kSeconds) * cv::kSampleRate;
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
  applyKnob(card, 0, mix, chain);
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

// The input conditioning at its defaults, HPF 120 Hz and LPF 12 kHz, alone and ahead of the Input
// gate as slot 1 hears it. A filter has no trim and the spec fixes its response, so these rows read
// what the filters take off this program, whose 180 Hz fundamental sits near the high-pass corner.
// Their band stops at the slack above 0 dB, because a filter adds no level, and at kHeadLoDb below.
constexpr double kHeadLoDb = -1.0;

bool headRows(const Signal& in) {
  bool ok = true;
  const cv::ConditioningParams defaults;
  {
    auto s = std::make_unique<Stage<cv::InputConditioning>>();
    ok &= verdict("conditioning HPF 120 + LPF 12k", gainDb(in, run(in, [&](const float* i, float* o, int n) {
                                                             s->fx.process(i, o, n, defaults);
                                                           })),
                  kHeadLoDb, kSlackDb);
  }
  {
    auto s = std::make_unique<Stage<cv::InputConditioning>>();
    cv::ConditioningParams off;
    off.set(0, 0);
    ok &= verdict("conditioning both OFF", gainDb(in, run(in, [&](const float* i, float* o, int n) {
                                                    s->fx.process(i, o, n, off);
                                                  })),
                  0.0, 0.0);
  }
  {
    auto head = std::make_unique<Stage<cv::InputConditioning>>();
    auto gate = std::make_unique<Stage<cv::Gate>>();
    const cv::GateParams g = inputGate();
    ok &= verdict("conditioning + input gate -55", gainDb(in, run(in, [&](const float* i, float* o, int n) {
                                                      float tmp[cv::kBlock];
                                                      head->fx.process(i, tmp, n, defaults);
                                                      gate->fx.process(tmp, o, n, g);
                                                    })),
                  kHeadLoDb, kSlackDb);
  }
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
  ok &= simple<cv::Reverb>("reverb spring wet only", in, boxReverbWetOnly(Card::ReverbSpring));
  ok &= midRow<cv::Reverb>("reverb chasm MIX 50 %", in, boxReverb(Card::ReverbChasm, 0.5f));
  ok &= midRow<cv::Reverb>("reverb spring MIX 50 %", in, boxReverb(Card::ReverbSpring, 0.5f));
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

  cv::PolishParams eq;
  eq.on = true;
  ok &= simple<cv::Polish>("polish EQ defaults", in, eq);
  return ok;
}

}  // namespace

int main() {
  const Signal in = program();
  bool ok = headRows(in);
  ok &= slotRows(in);
  ok &= tailRows(in);
  std::printf("%s level\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
