// Harmony checks: tracker, shift, diatonic intervals, bypass, no-alloc, then
// engines B and C, formant, chromatic intervals, and C on noise.
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

// Uniform white noise at -20 dBFS RMS, fixed seed.
std::vector<float> noise(int n) {
  const float amp = 0.1f * std::sqrt(3.0f);
  uint32_t state = 12345u;
  std::vector<float> x(static_cast<size_t>(n));
  for (float& v : x) {
    state = state * 1664525u + 1013904223u;
    v = amp * (2.0f * static_cast<float>(state >> 8) / 16777216.0f - 1.0f);
  }
  return x;
}

// One tracker result per block of kBlock samples, timestamped at block end.
struct Frame {
  int end;
  cv::PitchResult r;
};

std::vector<Frame> track(const std::vector<float>& x, float threshold) {
  cv::PitchTracker t;
  std::vector<Frame> frames;
  frames.reserve(x.size() / cv::kBlock + 1);
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&x[static_cast<size_t>(i)], n, threshold);
    frames.push_back({i + n, t.result()});
  }
  return frames;
}

// Harmony through the shared front end with Octave off.
std::vector<float> run(cv::PitchFx& h, const std::vector<float>& in, const cv::HarmonyParams& hp) {
  cv::PitchFxParams p;
  p.harmony = hp;
  p.octave.semitones = 0;
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    h.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
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

const float kThreshold = cv::HarmonyTuning{}.voicedThreshold;

bool testTracker() {
  bool ok = true;
  for (double hz : {110.0, 220.0, 440.0}) {
    const std::vector<Frame> f = track(tone(hz, cv::kSampleRate), kThreshold);
    double worst = 0.0;
    int unvoiced = 0;
    for (const Frame& fr : f) {
      if (fr.end <= kSettle) continue;
      if (!fr.r.voiced) {
        ++unvoiced;
        continue;
      }
      worst = std::max(worst, std::fabs(cents(fr.r.hz, hz)));
    }
    char name[32];
    std::snprintf(name, sizeof name, "tracker %.0f Hz", hz);
    ok &= report(name, unvoiced == 0 && worst <= 5.0,
                 "worst=%.3f cents (+-5), unvoiced frames=%.0f (0)", worst, unvoiced);
  }
  const std::vector<Frame> f = track(noise(cv::kSampleRate), kThreshold);
  int voiced = 0;
  for (const Frame& fr : f)
    if (fr.end > kSettle && fr.r.voiced) ++voiced;
  ok &= report("tracker noise", voiced == 0, "voiced frames=%.0f (0)", voiced);
  return ok;
}

static cv::PitchFx gFx;

cv::HarmonyParams highVoice() {
  cv::HarmonyParams p;
  p.key = 0;
  p.mix = 1.0f;
  p.slots[0] = {cv::HarmonyVoice::High, 3};
  return p;
}

// Median f0 of a signal over seconds 1..2, measured by a fresh tracker.
double medianHz(const std::vector<float>& x) {
  const std::vector<Frame> f = track(x, kThreshold);
  std::vector<double> hz;
  for (const Frame& fr : f)
    if (fr.end > cv::kSampleRate && fr.r.voiced) hz.push_back(fr.r.hz);
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

double outputHz(double inHz, const cv::HarmonyParams& p) {
  gFx.reset();
  return medianHz(run(gFx, tone(inHz, 2 * cv::kSampleRate), p));
}

double outputHz(double inHz) { return outputHz(inHz, highVoice()); }

bool testShift() {
  const double expect = 261.63;
  const double got = outputHz(220.0);
  const double err = got > 0.0 ? cents(got, expect) : 9999.0;
  return report("shift", std::fabs(err) <= 10.0, "out=%.2f Hz, err=%.2f cents (+-10)", got, err);
}

bool testDiatonic() {
  const double in[3] = {261.63, 293.66, 329.63};
  const double semis[3] = {4.0, 3.0, 3.0};
  bool ok = true;
  for (int i = 0; i < 3; ++i) {
    const double got = outputHz(in[i]);
    const double interval = got > 0.0 ? cents(got, in[i]) : 9999.0;
    const double err = interval - 100.0 * semis[i];
    char name[32];
    std::snprintf(name, sizeof name, "diatonic %.2f Hz", in[i]);
    ok &= report(name, std::fabs(err) <= 10.0,
                 "interval=%.2f cents (expect %.0f +-10), err=%.2f", interval, 100.0 * semis[i],
                 err);
  }
  return ok;
}

float maxDiffAfterSettle(const std::vector<float>& in, const std::vector<float>& out) {
  float m = 0.0f;
  for (size_t i = kSettle; i < in.size(); ++i) m = std::max(m, std::fabs(out[i] - in[i]));
  return m;
}

bool testBypass() {
  const std::vector<float> in = tone(220.0, cv::kSampleRate);

  cv::HarmonyParams dryMix = highVoice();
  dryMix.mix = 0.0f;
  dryMix.slots[1] = {cv::HarmonyVoice::Low, 3};
  gFx.reset();
  const float a = maxDiffAfterSettle(in, run(gFx, in, dryMix));

  cv::HarmonyParams noVoices;
  noVoices.mix = 0.7f;
  gFx.reset();
  const float b = maxDiffAfterSettle(in, run(gFx, in, noVoices));

  cv::HarmonyParams off = highVoice();
  off.mix = 0.5f;
  off.on = false;
  gFx.reset();
  const float c = maxDiffAfterSettle(in, run(gFx, in, off));

  return report("bypass", a < 1e-6f && b < 1e-6f && c < 1e-6f,
                "mix 0 max|out-in|=%.3g, no voices %.3g, on=false %.3g (<1e-6)", a, b, c);
}

bool testNoAlloc() {
  cv::HarmonyParams p = highVoice();
  p.slots[1] = {cv::HarmonyVoice::Lower, 2};
  p.mix = 0.5f;
  const std::vector<float> in = tone(220.0, cv::kBlock * 64);
  gFx.reset();
  run(gFx, in, p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}


// Engines B and C, formant, chromatic.

cv::HarmonyParams highOn(int engine) {
  cv::HarmonyParams p = highVoice();
  p.engine = engine;
  return p;
}

bool checkHz(const char* name, const cv::HarmonyParams& p, double inHz, double expect,
             double tol) {
  const double got = outputHz(inHz, p);
  const double err = got > 0.0 ? cents(got, expect) : 9999.0;
  return report(name, std::fabs(err) <= tol,
                "out=%.2f Hz, expect %.2f, err=%.2f cents (+-%.0f)", got, expect, err, tol);
}

bool testPitchBC() {
  bool ok = true;
  ok &= checkHz("B shift", highOn(1), 220.0, 261.63, 10.0);
  ok &= checkHz("C shift", highOn(2), 220.0, 261.63, 15.0);
  return ok;
}

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

// Root-sum-square Goertzel magnitude within +-halfHz of hz over second 1..2,
// as in octave_test.
double bandAmp(const std::vector<float>& x, double hz, double halfHz) {
  double e = 0.0;
  const int steps = static_cast<int>(halfHz / 2.0);
  for (int d = -steps; d <= steps; ++d) {
    const double a = goertzel(&x[cv::kSampleRate], cv::kSampleRate, hz + 2.0 * d);
    e += a * a;
  }
  return std::sqrt(e);
}

// Amplitude-weighted mean harmonic number over harmonics 1..10 of f0.
double centroidRel(const std::vector<float>& x, double f0) {
  double num = 0.0;
  double den = 0.0;
  for (int k = 1; k <= 10; ++k) {
    const double a = bandAmp(x, f0 * k, 0.45 * f0);
    num += k * a;
    den += a;
  }
  return den > 0.0 ? num / den : 0.0;
}

bool testFormantB() {
  const std::vector<float> in = tone(220.0, 2 * cv::kSampleRate);
  double hz[2] = {0.0, 0.0};
  double rel[2] = {0.0, 0.0};
  const float formant[2] = {0.0f, 12.0f};
  for (int i = 0; i < 2; ++i) {
    cv::HarmonyParams p = highOn(1);
    p.slots[0].formant = formant[i];
    gFx.reset();
    const std::vector<float> out = run(gFx, in, p);
    hz[i] = medianHz(out);
    rel[i] = hz[i] > 0.0 ? centroidRel(out, hz[i]) : 0.0;
  }
  const double err = hz[1] > 0.0 ? cents(hz[1], 261.63) : 9999.0;
  const double ratio = rel[0] > 0.0 ? rel[1] / rel[0] : 0.0;
  return report("B formant", std::fabs(err) <= 10.0 && ratio >= 1.2,
                "+12: out=%.2f Hz err=%.2f cents (+-10), centroid/f0 x%.3f of formant 0 "
                "(>=1.2, base %.3f)",
                hz[1], err, ratio, rel[0]);
}

bool testChromatic() {
  cv::HarmonyParams high = highVoice();
  high.chromatic = true;
  cv::HarmonyParams lower = high;
  lower.slots[0].voice = cv::HarmonyVoice::Lower;
  bool ok = true;
  ok &= checkHz("chromatic High +4", high, 220.0, 220.0 * std::pow(2.0, 4.0 / 12.0), 10.0);
  ok &= checkHz("chromatic Lower -7", lower, 220.0, 220.0 * std::pow(2.0, -7.0 / 12.0), 10.0);
  return ok;
}

// White noise leaves the tracker unvoiced; chromatic keeps the interval at
// +4 so C actually shifts.
bool testNoiseC() {
  const std::vector<float> in = noise(2 * cv::kSampleRate);
  cv::HarmonyParams p = highOn(2);
  p.chromatic = true;
  gFx.reset();
  const std::vector<float> out = run(gFx, in, p);
  double ri = 0.0;
  double ro = 0.0;
  for (size_t i = cv::kSampleRate; i < in.size(); ++i) {
    ri += static_cast<double>(in[i]) * in[i];
    ro += static_cast<double>(out[i]) * out[i];
  }
  const double gainDb = 10.0 * std::log10(ro / ri);
  const double wantDb = p.tuning.levelDb[2] + p.tuning.trimDb[2];
  return report("C noise level", std::fabs(gainDb - wantDb) <= 3.0,
                "out/in %+.2f dB, expect %+.2f dB (+-3)", gainDb, wantDb);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testTracker();
  ok &= testShift();
  ok &= testDiatonic();
  ok &= testBypass();
  ok &= testNoAlloc();
  ok &= testPitchBC();
  ok &= testFormantB();
  ok &= testChromatic();
  ok &= testNoiseC();
  return ok ? 0 : 1;
}
