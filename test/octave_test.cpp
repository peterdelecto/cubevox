// Octave checks: shift up/down, fifth, bypass, shared-tracker wiring, no-alloc,
// then option B pitch, formant-vs-pitch, formant envelope and level, bypass.
// No window, no audio device.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/pitch_fx.h"

namespace {

std::atomic<bool> gInProcess{false};

void guardAlloc() {
  if (gInProcess.load()) std::abort();
}

}  // namespace

// Allocation in process() aborts the test binary.
void* operator new(std::size_t n) {
  guardAlloc();
  void* p = std::malloc(n);
  if (!p) std::abort();
  return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
  guardAlloc();
  std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kSettle = cv::kSampleRate / 10;  // 100 ms

// Ten harmonics at 1/k amplitude, peak-normalised to 0.5.
std::vector<float> tone(double hz, int n) {
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(2.0 * kPi * hz * k * i / cv::kSampleRate) / k;
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  std::vector<float> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

const float kThreshold = cv::HarmonyTuning{}.voicedThreshold;

// PitchFx::pitch() read after one block, stamped with the block's end sample.
struct Hop {
  int end;
  cv::PitchResult r;
};

std::vector<float> run(cv::PitchFx& fx, const std::vector<float>& in, const cv::PitchFxParams& p,
                       std::vector<Hop>* hops = nullptr) {
  std::vector<float> out(in.size());
  if (hops) hops->reserve(in.size() / cv::kBlock + 1);
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    fx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
    if (hops) hops->push_back({i + n, fx.pitch()});
  }
  return out;
}

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0, double d = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::PitchFx gFx;

cv::PitchFxParams octaveOnly(int semis) {
  cv::PitchFxParams p;
  p.octave.semitones = semis;
  p.octave.mix = 1.0f;
  return p;
}

// Median f0 of the output over seconds 1..2, measured by a fresh tracker.
double outputHz(double inHz, const cv::PitchFxParams& p) {
  gFx.reset();
  const std::vector<float> out = run(gFx, tone(inHz, 2 * cv::kSampleRate), p);
  cv::PitchTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&out[static_cast<size_t>(i)], n, kThreshold);
    if (i + n > cv::kSampleRate && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

bool checkShift(const char* name, int semis, double expect) {
  const double got = outputHz(220.0, octaveOnly(semis));
  const double err = got > 0.0 ? cents(got, expect) : 9999.0;
  return report(name, std::fabs(err) <= 10.0, "out=%.2f Hz, expect %.2f, err=%.2f cents (+-10)",
                got, expect, err);
}

bool testOctave() {
  bool ok = true;
  ok &= checkShift("octave up", 12, 440.0);
  ok &= checkShift("octave down", -12, 110.0);
  return ok;
}

bool testFifth() { return checkShift("fifth", 7, 329.63); }

bool testBypass() {
  const std::vector<float> in = tone(220.0, cv::kSampleRate);
  float worst = 0.0f;
  for (float mix : {0.0f, 0.5f, 1.0f}) {
    cv::PitchFxParams p = octaveOnly(0);
    p.octave.mix = mix;
    gFx.reset();
    const std::vector<float> out = run(gFx, in, p);
    for (size_t i = kSettle; i < in.size(); ++i) worst = std::max(worst, std::fabs(out[i] - in[i]));
  }
  cv::PitchFxParams off = octaveOnly(12);
  off.octave.mix = 0.5f;
  off.octave.on = false;
  gFx.reset();
  const std::vector<float> offOut = run(gFx, in, off);
  for (size_t i = kSettle; i < in.size(); ++i) worst = std::max(worst, std::fabs(offOut[i] - in[i]));
  return report("bypass", worst < 1e-6f, "mix 0/0.5/1 and on=false max|out-in|=%.3g (<1e-6)", worst);
}

// Octave and Harmony on together; the tracker must still hear only the dry tone.
bool testWiring() {
  cv::PitchFxParams p = octaveOnly(12);
  p.harmony.mix = 1.0f;
  p.harmony.slots[0] = {cv::HarmonyVoice::High, 3};
  gFx.reset();
  std::vector<Hop> hops;
  run(gFx, tone(220.0, 2 * cv::kSampleRate), p, &hops);
  int bad = 0;
  int checked = 0;
  double worst = 0.0;
  for (const Hop& h : hops) {
    if (h.end <= kSettle) continue;
    ++checked;
    if (!h.r.voiced) {
      ++bad;
      continue;
    }
    const double err = std::fabs(cents(h.r.hz, 220.0));
    worst = std::max(worst, err);
    if (err > 5.0) ++bad;
  }
  return report("wiring", bad == 0 && checked > 0,
                "pitch() worst=%.3f cents (+-5), bad hops=%.0f of %.0f (0)", worst, bad, checked);
}

bool testNoAlloc() {
  cv::PitchFxParams p = octaveOnly(-12);
  p.octave.mix = 0.5f;
  p.harmony.mix = 0.5f;
  p.harmony.slots[0] = {cv::HarmonyVoice::High, 3};
  p.harmony.slots[1] = {cv::HarmonyVoice::Lower, 2};
  const std::vector<float> in = tone(220.0, cv::kBlock * 64);
  gFx.reset();
  run(gFx, in, p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// Option B checks.

cv::PitchFxParams octaveB(int semis, float formant) {
  cv::PitchFxParams p = octaveOnly(semis);
  p.octave.engine = 1;
  p.octave.formant = formant;
  p.octave.tuning.trimDbB = 0.0f;  // level checks here are against unity, not the level-rule trim
  return p;
}

bool checkShiftB(const char* name, int semis, float formant, double expect) {
  const double got = outputHz(220.0, octaveB(semis, formant));
  const double err = got > 0.0 ? cents(got, expect) : 9999.0;
  return report(name, std::fabs(err) <= 10.0, "out=%.2f Hz, expect %.2f, err=%.2f cents (+-10)",
                got, expect, err);
}

bool testOctaveB() {
  bool ok = true;
  ok &= checkShiftB("B octave up", 12, 0.0f, 440.0);
  ok &= checkShiftB("B octave down", -12, 0.0f, 110.0);
  return ok;
}

bool testFormantKeepsPitchB() { return checkShiftB("B formant pitch", 7, 12.0f, 329.63); }

// Goertzel magnitude of x at hz.
double goertzel(const float* x, int n, double hz) {
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  const double c = 2.0 * std::cos(w);
  double s1 = 0.0;
  double s2 = 0.0;
  for (int i = 0; i < n; ++i) {
    const double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
}

// Amplitude-weighted mean harmonic number over harmonics 1..10 of 220 Hz,
// measured on second 1..2 (exactly 220 cycles, so no leakage).
double centroidB(float formant) {
  gFx.reset();
  const std::vector<float> out = run(gFx, tone(220.0, 2 * cv::kSampleRate), octaveB(0, formant));
  double num = 0.0;
  double den = 0.0;
  for (int k = 1; k <= 10; ++k) {
    const double a = goertzel(&out[cv::kSampleRate], cv::kSampleRate, 220.0 * k);
    num += k * a;
    den += a;
  }
  return den > 0.0 ? num / den : 0.0;
}

bool testFormantEnvelopeB() {
  const double flat = centroidB(0.0f);
  const double up = centroidB(12.0f);
  const double down = centroidB(-12.0f);
  const double rUp = up / flat;
  const double rDown = down / flat;
  return report("B formant envelope", rUp >= 1.2 && rDown <= 0.8,
                "centroid 0 st=%.3f, +12 x%.3f (>=1.2), -12 x%.3f (<=0.8)", flat, rUp, rDown);
}

// Goertzel magnitude of harmonic k of 220 Hz over second 1..2.
double harmonic(const std::vector<float>& x, int k) {
  return goertzel(&x[cv::kSampleRate], cv::kSampleRate, 220.0 * k);
}

// Formant shift moves the envelope, so the level is checked where it is
// defined: an output harmonic against the input harmonic it maps from.
bool testFormantLevelB() {
  const std::vector<float> in = tone(220.0, 2 * cv::kSampleRate);
  gFx.reset();
  const std::vector<float> up = run(gFx, in, octaveB(0, 12.0f));
  gFx.reset();
  const std::vector<float> down = run(gFx, in, octaveB(0, -12.0f));
  const double upDb = 20.0 * std::log10(harmonic(up, 2) / harmonic(in, 1));
  const double downDb = 20.0 * std::log10(harmonic(down, 1) / harmonic(in, 2));
  return report("B formant level", std::fabs(upDb) <= 0.2 && std::fabs(downDb) <= 0.2,
                "+12 out h2 vs in h1 %+.3f dB, -12 out h1 vs in h2 %+.3f dB (+-0.2)", upDb,
                downDb);
}

bool testBypassB() {
  const std::vector<float> in = tone(220.0, cv::kSampleRate);
  float worst = 0.0f;
  for (float mix : {0.0f, 0.5f, 1.0f}) {
    cv::PitchFxParams p = octaveB(0, 0.0f);
    p.octave.mix = mix;
    gFx.reset();
    const std::vector<float> out = run(gFx, in, p);
    for (size_t i = kSettle; i < in.size(); ++i) worst = std::max(worst, std::fabs(out[i] - in[i]));
  }
  return report("B bypass", worst == 0.0f, "semitones 0 mix 0/0.5/1 max|out-in|=%.3g (0)", worst);
}

// Option C checks.

cv::PitchFxParams octaveC(int semis) {
  cv::PitchFxParams p = octaveOnly(semis);
  p.octave.engine = 2;
  return p;
}

bool testPitchC() {
  bool ok = true;
  for (int semis : {12, -12}) {
    const double expect = semis > 0 ? 440.0 : 110.0;
    const double got = outputHz(220.0, octaveC(semis));
    const double err = got > 0.0 ? cents(got, expect) : 9999.0;
    ok &= report(semis > 0 ? "C octave up" : "C octave down", std::fabs(err) <= 15.0,
                 "out=%.2f Hz, expect %.2f, err=%.2f cents (+-15)", got, expect, err);
  }
  return ok;
}

// Root-sum-square Goertzel magnitude within +-halfHz of hz over second 1..2.
// A granular shifter spreads each harmonic into window-rate sidebands, so a
// single bin at the harmonic would read the comb ripple, not the envelope.
double bandAmp(const std::vector<float>& x, double hz, double halfHz) {
  double e = 0.0;
  const int steps = static_cast<int>(halfHz / 2.0);
  for (int d = -steps; d <= steps; ++d) {
    const double a = goertzel(&x[cv::kSampleRate], cv::kSampleRate, hz + 2.0 * d);
    e += a * a;
  }
  return std::sqrt(e);
}

// Amplitude-weighted mean frequency over the given harmonic numbers of f0.
double centroidHz(const std::vector<float>& x, double f0, int first, int last, int step) {
  double num = 0.0;
  double den = 0.0;
  for (int k = first; k <= last; k += step) {
    const double a = bandAmp(x, f0 * k, 0.45 * f0 * step);
    num += f0 * k * a;
    den += a;
  }
  return den > 0.0 ? num / den : 0.0;
}

// Median f0 of a signal over seconds 1..2, from a fresh tracker.
double medianHz(const std::vector<float>& x) {
  cv::PitchTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&x[static_cast<size_t>(i)], n, kThreshold);
    if (i + n > cv::kSampleRate && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

// C moves the envelope with the pitch: the centroid in harmonics of its own
// f0 matches the input's. A keeps the envelope in place: its output harmonics
// 2, 4, .. 10 sit on the input's 220..1100 Hz grid, so those centroids match.
bool testFormantMovesC() {
  const std::vector<float> in = tone(220.0, 2 * cv::kSampleRate);
  const double inRel = centroidHz(in, 220.0, 1, 10, 1) / 220.0;
  const double inAbs = centroidHz(in, 220.0, 1, 5, 1);

  gFx.reset();
  const std::vector<float> c = run(gFx, in, octaveC(-12));
  const double fC = medianHz(c);
  const double rC = centroidHz(c, fC, 1, 10, 1) / fC / inRel;

  gFx.reset();
  const std::vector<float> a = run(gFx, in, octaveOnly(-12));
  const double fA = medianHz(a);
  const double rA = centroidHz(a, fA, 2, 10, 2) / inAbs;
  return report("C formants move", std::fabs(rC - 1.0) <= 0.10 && std::fabs(rA - 1.0) <= 0.10,
                "C centroid/f0 x%.3f of input (+-10%%), A absolute centroid x%.3f (+-10%%), "
                "C f0=%.1f",
                rC, rA, fC);
}

bool testBypassC() {
  const std::vector<float> in = tone(220.0, cv::kSampleRate);
  float worst = 0.0f;
  for (float mix : {0.0f, 0.5f, 1.0f}) {
    cv::PitchFxParams p = octaveC(0);
    p.octave.mix = mix;
    gFx.reset();
    const std::vector<float> out = run(gFx, in, p);
    for (size_t i = kSettle; i < in.size(); ++i) worst = std::max(worst, std::fabs(out[i] - in[i]));
  }
  return report("C bypass", worst == 0.0f, "semitones 0 mix 0/0.5/1 max|out-in|=%.3g (0)", worst);
}

bool testNoiseC() {
  const int n = 2 * cv::kSampleRate;
  std::vector<float> in(static_cast<size_t>(n));
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  for (float& v : in) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    v = 0.25f * (static_cast<float>(s >> 40) / 8388608.0f - 1.0f);
  }
  const cv::PitchFxParams p = octaveC(-12);
  gFx.reset();
  const std::vector<float> out = run(gFx, in, p);
  double ri = 0.0;
  double ro = 0.0;
  for (int i = cv::kSampleRate; i < n; ++i) {
    ri += static_cast<double>(in[static_cast<size_t>(i)]) * in[static_cast<size_t>(i)];
    ro += static_cast<double>(out[static_cast<size_t>(i)]) * out[static_cast<size_t>(i)];
  }
  const double gainDb = 10.0 * std::log10(ro / ri);
  const double wantDb = p.octave.tuning.levelDb + p.octave.tuning.trimDbC;
  return report("C noise level", std::fabs(gainDb - wantDb) <= 3.0,
                "out/in %+.2f dB, expect %+.2f dB (+-3)", gainDb, wantDb);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testOctave();
  ok &= testFifth();
  ok &= testBypass();
  ok &= testWiring();
  ok &= testNoAlloc();
  ok &= testOctaveB();
  ok &= testFormantKeepsPitchB();
  ok &= testFormantEnvelopeB();
  ok &= testFormantLevelB();
  ok &= testBypassB();
  ok &= testPitchC();
  ok &= testFormantMovesC();
  ok &= testBypassC();
  ok &= testNoiseC();
  return ok ? 0 : 1;
}
