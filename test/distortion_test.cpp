// Distortion checks: passthrough, top boost, harmonics, clean, level, asymmetry, DC, no-alloc. No window, no audio device.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/distortion.h"

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
constexpr float kAmp = 0.251f;  // -12 dBFS
constexpr float kHz = 220.0f;
constexpr int kSecond = cv::kSampleRate;

std::vector<float> sine(int n) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] =
        kAmp * static_cast<float>(std::sin(2.0 * kPi * kHz * i / cv::kSampleRate));
  return x;
}

// Runs the whole signal through process() in kBlock chunks.
std::vector<float> run(cv::Distortion& d, const std::vector<float>& in,
                       const cv::DistortionParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    d.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

cv::DistortionParams params(float drive) {
  cv::DistortionParams p;
  p.drive = drive;
  return p;
}

// Amplitude of one frequency over [from, to), a whole number of cycles.
double goertzel(const std::vector<float>& x, int from, int to, double hz) {
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  double re = 0.0, im = 0.0;
  for (int i = from; i < to; ++i) {
    const double v = x[static_cast<size_t>(i)];
    re += v * std::cos(w * (i - from));
    im += v * std::sin(w * (i - from));
  }
  return 2.0 * std::sqrt(re * re + im * im) / (to - from);
}

// Harmonic h over the fundamental, in dB, from second 1 to second 2.
double harmonicDb(const std::vector<float>& x, int h) {
  return 20.0 * std::log10(goertzel(x, kSecond, 2 * kSecond, kHz * h) /
                           goertzel(x, kSecond, 2 * kSecond, kHz));
}

double rmsOf(const std::vector<float>& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / (to - from));
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0) {
  char detail[160];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::Distortion gDist;

std::vector<float> render(const cv::DistortionParams& p) {
  gDist.reset();
  return run(gDist, sine(2 * kSecond), p);
}

bool testPassthrough() {
  const int n = kSecond;
  const std::vector<float> in = sine(n);
  float maxDiff = 0.0f;
  cv::DistortionParams zero = params(0.0f);
  cv::DistortionParams off = params(1.0f);
  off.on = false;
  for (const cv::DistortionParams& p : {zero, off}) {
    gDist.reset();
    const std::vector<float> out = run(gDist, in, p);
    for (int i = cv::kSampleRate / 10; i < n; ++i) {
      const float d = std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
      if (d > maxDiff) maxDiff = d;
    }
  }
  return report("passthrough", maxDiff < 1e-6f, "drive 0 and on=false max|out-in|=%.3g (<1e-6)",
                maxDiff);
}

// Stage 1 gain before the top boost: the plain exponential sweep, in the same float operations
// as the engine used before topBoostDb existed.
float todaysGain1(const cv::DistortionTuning& t, float drive) {
  return t.gain1Min * expf(logf(t.gain1Max / t.gain1Min) * drive);
}

// Applies p's tuning to d, so stage1GainAt() reads it.
void prime(cv::Distortion& d, const cv::DistortionParams& p) {
  const std::vector<float> in(static_cast<size_t>(cv::kBlock), 0.0f);
  std::vector<float> out(in.size());
  d.reset();
  d.process(in.data(), out.data(), cv::kBlock, p);
}

// DRIVE 0 to topBoostFrom: the boost term is exactly 0.0f, so the gain law and the whole output
// match the sweep without the boost, sample for sample.
bool testBelowBoost() {
  cv::DistortionParams p = params(0.5f);
  prime(gDist, p);
  const cv::DistortionTuning& t = p.tuning;
  bool law = true;
  for (int k = 0; k <= 60; ++k) {
    const float drive = static_cast<float>(k) / 100.0f;
    if (gDist.stage1GainAt(drive) != todaysGain1(t, drive)) law = false;
  }
  if (gDist.stage1GainAt(t.topBoostFrom) != todaysGain1(t, t.topBoostFrom)) law = false;

  const float drives[4] = {0.1f, 0.3f, 0.5f, 0.6f};
  bool audio = true;
  for (float drive : drives) {
    const std::vector<float> boosted = render(params(drive));
    cv::DistortionParams plain = params(drive);
    plain.tuning.topBoostDb = 0.0f;
    const std::vector<float> flat = render(plain);
    for (size_t i = 0; i < boosted.size(); ++i)
      if (boosted[i] != flat[i]) audio = false;
  }
  char detail[160];
  std::snprintf(detail, sizeof detail,
                "gain at DRIVE 0..0.6 equals today's law (==): %s; output at 0.1/0.3/0.5/0.6 equals boost 0 dB (==): %s",
                law ? "yes" : "NO", audio ? "yes" : "NO");
  std::printf("%s below-boost  %s\n", law && audio ? "PASS" : "FAIL", detail);
  return law && audio;
}

// topBoostDb extra stage 1 gain at DRIVE 1, half of it halfway up the ramp, and none of it
// when topBoostDb is 0.
bool testTopBoost() {
  cv::DistortionParams p = params(0.5f);
  prime(gDist, p);
  const cv::DistortionTuning& t = p.tuning;
  const double want = std::pow(10.0, t.topBoostDb / 20.0);
  const double top = gDist.stage1GainAt(1.0f) / todaysGain1(t, 1.0f);
  const double mid = gDist.stage1GainAt(0.8f) / todaysGain1(t, 0.8f);
  const bool topOk = std::fabs(top / want - 1.0) < 1e-4 && std::fabs(top - 2.0) < 0.01;
  const bool midOk = std::fabs(mid / std::sqrt(want) - 1.0) < 1e-4;

  p.tuning.topBoostDb = 0.0f;
  prime(gDist, p);
  bool offOk = true;
  for (int k = 0; k <= 100; ++k) {
    const float drive = static_cast<float>(k) / 100.0f;
    if (gDist.stage1GainAt(drive) != todaysGain1(p.tuning, drive)) offOk = false;
  }
  const bool ok = topOk && midOk && offOk;
  char detail[160];
  std::snprintf(detail, sizeof detail,
                "stage 1 gain vs today's: DRIVE 1 x%.5f = %.3f dB (x%.5f), DRIVE 0.8 x%.5f; boost 0 dB equals today's: %s",
                top, 20.0 * std::log10(top), want, mid, offOk ? "yes" : "NO");
  std::printf("%s top-boost  %s\n", ok ? "PASS" : "FAIL", detail);
  return ok;
}

// The stage 1 gain rises with DRIVE for the default tuning and for the widest boost.
bool testGainMonotonic() {
  bool ok = true;
  for (int wide = 0; wide < 2; ++wide) {
    cv::DistortionParams p = params(0.5f);
    if (wide) {
      p.tuning.topBoostDb = 12.0f;
      p.tuning.topBoostFrom = 0.0f;
    }
    prime(gDist, p);
    float prev = 0.0f;
    for (int k = 0; k <= 1000; ++k) {
      const float g = gDist.stage1GainAt(static_cast<float>(k) / 1000.0f);
      if (!(g > prev)) ok = false;
      prev = g;
    }
  }
  return report("gain-monotonic", ok,
                "stage 1 gain strictly rising over DRIVE 0..1 in %.0f steps, default and 12 dB from 0",
                1000.0);
}

// DRIVE taper (owner 2026-10-02: dead until 50-60 %, too saturated at 100 %). Each drive has a
// 3rd-harmonic target and a +-3 dB band. Drives 0.7 and 1 carry the top boost (owner 2026-10-30).
bool testHarmonics() {
  constexpr int kPoints = 5;
  const float drives[kPoints] = {0.1f, 0.3f, 0.5f, 0.7f, 1.0f};
  const double target[kPoints] = {-40.0, -28.0, -20.0, -14.0, -11.6};
  double h[kPoints];
  bool ok = true;
  for (int k = 0; k < kPoints; ++k) {
    h[k] = harmonicDb(render(params(drives[k])), 3);
    if (std::fabs(h[k] - target[k]) > 3.0) ok = false;
    if (k > 0 && h[k] <= h[k - 1]) ok = false;
  }
  char detail[160];
  std::snprintf(detail, sizeof detail,
                "3rd at drive 0.1/0.3/0.5/0.7/1 = %.1f / %.1f / %.1f / %.1f / %.1f dB (targets -40/-28/-20/-14/-11.6 +-3, rising)",
                h[0], h[1], h[2], h[3], h[4]);
  std::printf("%s harmonics  %s\n", ok ? "PASS" : "FAIL", detail);
  return ok;
}

// Old band: 3rd <= -40 dB at drive 0.15. New: just breaking up at drive 0.1.
bool testClean() {
  const double h3 = harmonicDb(render(params(0.1f)), 3);
  return report("clean", h3 <= -38.0, "3rd at drive 0.1 = %.1f dB (<=-38)", h3);
}

bool testLevel() {
  const double inRms = rmsOf(sine(kSecond), 0, kSecond);
  const float drives[3] = {0.15f, 0.5f, 1.0f};
  double db[3];
  bool ok = true;
  for (int k = 0; k < 3; ++k) {
    db[k] = 20.0 * std::log10(rmsOf(render(params(drives[k])), kSecond, 2 * kSecond) / inRms);
    if (std::fabs(db[k]) > 4.0) ok = false;
  }
  return report("level", ok, "output vs input RMS at drive 0.15/0.5/1 = %.2f / %.2f / %.2f dB (+-4)",
                db[0], db[1], db[2]);
}

// Drive 0.2 sits just past the clipping onset (about 0.1), where one rail clips first.
bool testAsymmetry() {
  const double asym = harmonicDb(render(params(0.2f)), 2);
  cv::DistortionParams sym = params(0.2f);
  sym.tuning.railAsym = 0.0f;
  const double flat = harmonicDb(render(sym), 2);
  return report("asymmetry", asym >= -46.0 && flat <= -55.0,
                "2nd at drive 0.2: default %.1f dB (>=-46), railAsym 0 %.1f dB (<=-55)", asym, flat);
}

bool testDc() {
  const std::vector<float> out = render(params(1.0f));
  double mean = 0.0;
  for (int i = kSecond; i < 2 * kSecond; ++i) mean += out[static_cast<size_t>(i)];
  mean /= kSecond;
  return report("dc", std::fabs(mean) < 1e-3, "drive 1 mean=%.3g (|mean|<1e-3)", mean);
}

bool testNoAlloc() {
  const std::vector<float> in = sine(cv::kBlock * 8);
  gDist.reset();
  run(gDist, in, params(0.7f));  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

bool testTone() {
  std::vector<float> in(static_cast<size_t>(2 * kSecond));
  for (size_t i = 0; i < in.size(); ++i)
    in[i] = kAmp * static_cast<float>(std::sin(2.0 * kPi * 8000.0 * static_cast<double>(i) / cv::kSampleRate));
  auto wetRms = [&](float tone, bool flatTuning) {
    cv::DistortionParams p = params(0.5f);
    p.tone = tone;
    if (flatTuning) p.tuning.toneMinDb = p.tuning.toneMaxDb = 0.0f;
    gDist.reset();
    return rmsOf(run(gDist, in, p), kSecond, 2 * kSecond);
  };
  const double lo = wetRms(0.0f, false), mid = wetRms(0.5f, false), hi = wetRms(1.0f, false);
  const double base = wetRms(0.5f, true);
  const double spanDb = 20.0 * std::log10(hi / lo);
  const double midDb = 20.0 * std::log10(mid / base);
  return report("tone", spanDb >= 12.0 && std::fabs(midDb) <= 0.5,
                "8 kHz at drive 0.5: tone 1 vs 0 = %.1f dB (>=12), tone 0.5 vs old default = %.2f dB (+-0.5)",
                spanDb, midDb);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testBelowBoost();
  ok &= testTopBoost();
  ok &= testGainMonotonic();
  ok &= testHarmonics();
  ok &= testClean();
  ok &= testLevel();
  ok &= testAsymmetry();
  ok &= testDc();
  ok &= testTone();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
