// Level rule: every stage at its default engaged setting lands 0 to +0.5 dB
// (out/in RMS) on vocal-like program material. No window, no audio device.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "engine/distortion.h"
#include "engine/gate.h"
#include "engine/pitch_fx.h"
#include "engine/polish.h"
#include "engine/reverb.h"
#include "engine/slapback.h"
#include "engine/unison.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSeconds = 3;
constexpr int kMeasureFrom = 1 * cv::kSampleRate;
constexpr double kProgramDbfs = -18.0;
constexpr double kNoiseDbfs = -40.0;
constexpr double kLoDb = 0.0;
constexpr double kHiDb = 0.5;
constexpr double kSlackDb = 0.1;

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

bool check(const char* name, const Signal& in, const BlockFn& fn) {
  const double db = gainDb(in, run(in, fn));
  const bool ok = db >= kLoDb - kSlackDb && db <= kHiDb + kSlackDb;
  std::printf("%s %-26s %+6.2f dB  (want %+.1f..%+.1f)\n", ok ? "PASS" : "FAIL", name, db,
              kLoDb, kHiDb);
  return ok;
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

cv::GateParams inputGate() {
  cv::GateParams g;
  g.thresholdDb = -40.0f;
  g.tuning.rangeDb = -12.0f;  // the emulator's live-stage input gate
  g.tuning.holdMs = 100.0f;
  g.tuning.releaseMs = 25.0f;
  return g;
}

cv::PitchFxParams pitchOff() {
  cv::PitchFxParams p;
  p.harmony.on = false;
  p.octave.on = false;
  return p;
}

cv::ReverbParams reverbParams(int engine) {
  cv::ReverbParams p;
  p.on = true;
  p.engine = engine;
  p.mix = 0.5f;
  p.spring.tension = p.spring.dwell = 0.5f;
  p.parker.tension = p.parker.dwell = 0.5f;
  p.chasm.decay = 0.5f;
  p.chasm.wobble = 0.3f;
  return p;
}

}  // namespace

int main() {
  const Signal in = program();
  bool ok = true;

  ok &= simple<cv::Gate>("input gate -40", in, inputGate());
  cv::GateParams panelGate;
  panelGate.thresholdDb = -40.0f;
  ok &= simple<cv::Gate>("panel gate -40", in, panelGate);

  cv::PitchFxParams harmony = pitchOff();
  harmony.harmony.on = true;
  harmony.harmony.slots[0] = {cv::HarmonyVoice::High, 2};
  ok &= simple<cv::PitchFx>("harmony A High L2 mix .5", in, harmony);

  cv::PitchFxParams harmonyB = harmony;
  harmonyB.harmony.engine = 1;
  ok &= simple<cv::PitchFx>("harmony B High L2 mix .5", in, harmonyB);

  cv::PitchFxParams harmonyC = harmony;
  harmonyC.harmony.engine = 2;
  ok &= simple<cv::PitchFx>("harmony C High L2 mix .5", in, harmonyC);

  cv::PitchFxParams octA = pitchOff();
  octA.octave.on = true;
  octA.octave.engine = 0;
  octA.octave.semitones = -12;
  ok &= simple<cv::PitchFx>("octave A -12 mix .5", in, octA);

  cv::PitchFxParams octB = pitchOff();
  octB.octave.on = true;
  octB.octave.engine = 1;
  octB.octave.semitones = 12;
  ok &= simple<cv::PitchFx>("octave B +12 mix .5", in, octB);

  cv::PitchFxParams octC = pitchOff();
  octC.octave.on = true;
  octC.octave.engine = 2;
  octC.octave.semitones = -12;
  ok &= simple<cv::PitchFx>("octave C -12 mix .5", in, octC);

  cv::UnisonParams unison;
  unison.on = true;
  unison.depth = 0.8f;
  ok &= simple<cv::Unison>("unison depth .8", in, unison);

  cv::SlapbackParams slap;
  slap.on = true;
  slap.intensity = 0.5f;
  ok &= simple<cv::Slapback>("slapback .5", in, slap);

  cv::DistortionParams dist;
  dist.on = true;
  dist.drive = 0.3f;
  dist.tone = 0.5f;
  ok &= simple<cv::Distortion>("distortion drive .3", in, dist);

  ok &= simple<cv::Reverb>("reverb SPRING mix .5", in, reverbParams(cv::kReverbSpring));
  ok &= simple<cv::Reverb>("reverb CHASM mix .5", in, reverbParams(cv::kReverbChasm));
  ok &= simple<cv::Reverb>("reverb PARKER mix .5", in, reverbParams(cv::kReverbParker));

  cv::PolishParams eq;
  eq.on = true;
  ok &= simple<cv::Polish>("polish EQ defaults", in, eq);

  std::printf("%s level\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
