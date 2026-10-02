// Autotune checks: snap, scale, response time, Harmony follows the corrected
// note, bypass, no-alloc. No window, no audio device.

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
constexpr double kA4 = 440.0;
constexpr int kWindow = cv::kSampleRate / 50;  // 20 ms
constexpr int kWindowHop = cv::kSampleRate / 200;  // 5 ms

// Ten harmonics at 1/k amplitude, peak 0.5. hzAt gives the pitch per sample,
// so a step keeps the phase continuous.
template <typename F>
std::vector<float> tone(int n, F hzAt) {
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double phase = 0.0;
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(k * phase) / k;
    phase += 2.0 * kPi * hzAt(i) / cv::kSampleRate;
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  std::vector<float> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

std::vector<float> tone(double hz, int n) {
  return tone(n, [hz](int) { return hz; });
}

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

const float kThreshold = cv::HarmonyTuning{}.voicedThreshold;

// Median f0 over seconds 1..2 from a fresh tracker.
double medianHz(const std::vector<float>& x) {
  cv::PitchTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&x[static_cast<size_t>(i)], n, kThreshold);
    if (i + n > cv::kSampleRate && i + n <= 2 * cv::kSampleRate && t.result().voiced)
      hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

// Hann-windowed DTFT magnitude at hz over x[from, from + n).
double magnitude(const std::vector<float>& x, int from, int n, double hz) {
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  double re = 0.0, im = 0.0;
  for (int j = 0; j < n; ++j) {
    const double win = 0.5 * (1.0 - std::cos(2.0 * kPi * (j + 0.5) / n));
    const double v = win * x[static_cast<size_t>(from + j)];
    re += v * std::cos(w * j);
    im -= v * std::sin(w * j);
  }
  return std::sqrt(re * re + im * im);
}

// Fundamental of one window: the strongest 1 Hz bin in lo..hi, refined by a
// parabola through the log magnitudes.
double windowHz(const std::vector<float>& x, int from, double lo, double hi) {
  double best = lo;
  double bestM = -1.0;
  for (double f = lo; f <= hi; f += 1.0) {
    const double m = magnitude(x, from, kWindow, f);
    if (m > bestM) {
      bestM = m;
      best = f;
    }
  }
  const double a = std::log(magnitude(x, from, kWindow, best - 1.0) + 1e-12);
  const double b = std::log(bestM + 1e-12);
  const double c = std::log(magnitude(x, from, kWindow, best + 1.0) + 1e-12);
  const double denom = a - 2.0 * b + c;
  return denom < 0.0 ? best + 0.5 * (a - c) / denom : best;
}

std::vector<float> run(cv::PitchFx& fx, const std::vector<float>& in,
                       const cv::PitchFxParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    fx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0, double d = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::PitchFx gFx;

// Autotune alone in key C; Harmony and Octave off.
cv::PitchFxParams autotuneOnly(float responseMs) {
  cv::PitchFxParams p;
  p.autotune.on = true;
  p.autotune.responseMs = responseMs;
  p.harmony.on = false;
  p.harmony.key = 0;
  p.octave.on = false;
  return p;
}

double outputHz(double inHz, const cv::PitchFxParams& p) {
  gFx.reset();
  return medianHz(run(gFx, tone(inHz, 2 * cv::kSampleRate), p));
}

bool testSnap() {
  const double in = kA4 * std::pow(2.0, 35.0 / 1200.0);
  const double got = outputHz(in, autotuneOnly(5.0f));
  const double err = got > 0.0 ? cents(got, kA4) : 9999.0;
  return report("snap", std::fabs(err) <= 5.0, "in=%.2f Hz, out=%.2f Hz, err=%.2f cents (+-5)",
                in, got, err);
}

bool testScale() {
  const double in = 466.16;
  const double got = outputHz(in, autotuneOnly(5.0f));
  const double err = got > 0.0 ? cents(got, kA4) : 9999.0;
  bool ok = report("scale Bb4 in C", std::fabs(err) <= 5.0,
                   "out=%.2f Hz, err vs A4=%.2f cents (+-5)", got, err);
  cv::PitchFxParams chrom = autotuneOnly(5.0f);
  chrom.autotune.chromatic = true;
  const double gotC = outputHz(in, chrom);
  const double errC = gotC > 0.0 ? cents(gotC, in) : 9999.0;
  ok &= report("scale chromatic", std::fabs(errC) <= 5.0,
               "out=%.2f Hz, err vs in=%.2f cents (+-5)", gotC, errC);
  return ok;
}

// Step from A4 to A4 + 40 cents at 1 s. The output's correction is its cents
// minus the input's, read per 20 ms window; the input reaches the output one
// shifter delay late. Time is from the step to the first window at 63 % of
// the full -40 cents, interpolated between windows.
bool testResponse() {
  constexpr double kStepCents = 40.0;
  constexpr int kStep = cv::kSampleRate;
  constexpr int kDelay = cv::PsolaVoice::kGrainDelay;
  const double sharp = kA4 * std::pow(2.0, kStepCents / 1200.0);
  const std::vector<float> in =
      tone(2 * cv::kSampleRate, [&](int i) { return i < kStep ? kA4 : sharp; });
  gFx.reset();
  const std::vector<float> out = run(gFx, in, autotuneOnly(200.0f));

  const double goal = -0.63 * kStepCents;
  double prevT = 0.0, prevC = 0.0;
  double hitMs = -1.0;
  const int last = static_cast<int>(out.size()) - kWindow;
  for (int from = kStep + kDelay; from <= last; from += kWindowHop) {
    const double t = (from + 0.5 * kWindow - kStep) * 1000.0 / cv::kSampleRate;
    const double c = cents(windowHz(out, from, 380.0, 520.0), sharp);
    if (from > kStep + kDelay && c <= goal) {
      hitMs = prevT + (t - prevT) * (goal - prevC) / (c - prevC);
      break;
    }
    prevT = t;
    prevC = c;
  }
  return report("response 200 ms", hitMs >= 160.0 && hitMs <= 240.0,
                "63%% of -40 cents at %.1f ms after the step (200 +-40)", hitMs);
}

bool testHarmonyFollows() {
  cv::PitchFxParams p = autotuneOnly(5.0f);
  p.harmony.on = true;
  p.harmony.mix = 1.0f;
  p.harmony.slots[0] = {cv::HarmonyVoice::High, 3};
  const double expect = 523.25;
  const double got = outputHz(kA4 * std::pow(2.0, 35.0 / 1200.0), p);
  const double err = got > 0.0 ? cents(got, expect) : 9999.0;
  return report("harmony follows", std::fabs(err) <= 10.0,
                "out=%.2f Hz, err vs C5=%.2f cents (+-10)", got, err);
}

int countDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from) {
  int n = 0;
  for (size_t i = from; i < a.size(); ++i)
    if (a[i] != b[i]) ++n;
  return n;
}

bool testBypass() {
  const std::vector<float> in = tone(kA4 * std::pow(2.0, 35.0 / 1200.0), cv::kSampleRate);
  cv::PitchFxParams off = autotuneOnly(40.0f);
  off.autotune.on = false;
  gFx.reset();
  const int diffOff = countDiff(in, run(gFx, in, off), 0);
  bool ok = report("bypass off", diffOff == 0, "samples differing from input=%.0f (0)", diffOff);

  // On for 250 ms, then off: bit-exact once the 20 ms fade snaps, which takes
  // ln(1e6) time constants, about 280 ms.
  const cv::PitchFxParams on = autotuneOnly(40.0f);
  gFx.reset();
  std::vector<float> out(in.size());
  const int offAt = cv::kSampleRate / 4;
  for (int i = 0; i < static_cast<int>(in.size()); i += cv::kBlock) {
    gInProcess = true;
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock,
                i < offAt ? on : off);
    gInProcess = false;
  }
  const int diffOn = countDiff(in, out, static_cast<size_t>(cv::kSampleRate / 10));
  const int diffToggle = countDiff(in, out, static_cast<size_t>(offAt + 2 * cv::kSampleRate / 5));
  ok &= report("bypass after toggle", diffOn > 0 && diffToggle == 0,
               "samples differing 400 ms after off=%.0f (0), while on=%.0f (>0)", diffToggle,
               diffOn);

  cv::PitchFxParams zero = autotuneOnly(5.0f);
  zero.autotune.tuning.maxCorrectionSemis = 0.0f;
  const std::vector<float> in2 = tone(kA4 * std::pow(2.0, 35.0 / 1200.0), 2 * cv::kSampleRate);
  const double inHz = medianHz(in2);
  gFx.reset();
  const double got = medianHz(run(gFx, in2, zero));
  const double err = got > 0.0 ? cents(got, inHz) : 9999.0;
  ok &= report("max correction 0", std::fabs(err) <= 1.0,
               "in=%.2f Hz, out=%.2f Hz, err=%.2f cents (+-1)", inHz, got, err);
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testSnap();
  ok &= testScale();
  ok &= testResponse();
  ok &= testHarmonyFollows();
  ok &= testBypass();
  // Every process() call above ran under the allocation guard.
  ok &= report("no allocation", true, "process() ran under the new/delete guard", 0.0);
  std::printf("%s autotune\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
