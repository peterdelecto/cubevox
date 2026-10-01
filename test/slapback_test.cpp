// Slapback checks: passthrough, time, lowpass, feedback, no-alloc. No window, no audio device.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/slapback.h"

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

constexpr float kPi = 3.14159265358979323846f;
constexpr float kAmp = 0.5f;

std::vector<float> sine(float hz, int n) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = kAmp * sinf(2.0f * kPi * hz * i / cv::kSampleRate);
  return x;
}

// Runs the whole signal through process() in kBlock chunks.
std::vector<float> run(cv::Slapback& s, const std::vector<float>& in,
                       const cv::SlapbackParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    s.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

cv::SlapbackParams params(float intensity) {
  cv::SlapbackParams p;
  p.intensity = intensity;
  return p;
}

std::vector<float> wetOf(const std::vector<float>& out, const std::vector<float>& in) {
  std::vector<float> wet(in.size());
  for (size_t i = 0; i < in.size(); ++i) wet[i] = out[i] - in[i];
  return wet;
}

float rms(const std::vector<float>& x, size_t from) {
  double s = 0.0;
  for (size_t i = from; i < x.size(); ++i) s += static_cast<double>(x[i]) * x[i];
  return static_cast<float>(std::sqrt(s / static_cast<double>(x.size() - from)));
}

// Index of the largest |x| in [from, to).
int peakIndex(const std::vector<float>& x, int from, int to) {
  int best = from;
  for (int i = from; i < to; ++i)
    if (std::fabs(x[static_cast<size_t>(i)]) > std::fabs(x[static_cast<size_t>(best)])) best = i;
  return best;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0) {
  char detail[160];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::Slapback gSlap;

constexpr int kImpulseAt = cv::kSampleRate / 2;

std::vector<float> impulse(int n) {
  std::vector<float> x(static_cast<size_t>(n), 0.0f);
  x[static_cast<size_t>(kImpulseAt)] = 1.0f;
  return x;
}

bool testPassthrough() {
  const int n = cv::kSampleRate;
  const std::vector<float> in = sine(440.0f, n);
  gSlap.reset();
  const std::vector<float> out = run(gSlap, in, params(0.0f));
  float maxDiff = 0.0f;
  for (int i = cv::kSampleRate / 10; i < n; ++i) {
    const float d = std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
    if (d > maxDiff) maxDiff = d;
  }
  cv::SlapbackParams off = params(1.0f);
  off.on = false;
  gSlap.reset();
  const std::vector<float> offOut = run(gSlap, in, off);
  for (int i = cv::kSampleRate / 10; i < n; ++i) {
    const float d = std::fabs(offOut[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
    if (d > maxDiff) maxDiff = d;
  }
  return report("passthrough", maxDiff < 1e-6f, "intensity 0 and on=false max|out-in|=%.3g (<1e-6)",
                maxDiff);
}

bool testTime() {
  const float times[3] = {30.0f, 80.0f, 120.0f};
  bool ok = true;
  int worst = 0;
  for (float ms : times) {
    const int expect = kImpulseAt + static_cast<int>(ms * cv::kSampleRate / 1000.0f);
    const std::vector<float> in = impulse(cv::kSampleRate);
    cv::SlapbackParams p = params(1.0f);
    p.tuning.timeMs = ms;
    p.tuning.feedback = 0.0f;
    p.tuning.lowpassHz = 12000.0f;
    gSlap.reset();
    const std::vector<float> wet = wetOf(run(gSlap, in, p), in);
    const int at = peakIndex(wet, kImpulseAt + 1, cv::kSampleRate);
    const int err = std::abs(at - expect);
    if (err > worst) worst = err;
    if (err > 2) ok = false;
  }
  return report("time", ok, "worst peak error over 30/80/120 ms=%.0f samples (<=2)", worst);
}

bool testLowpass() {
  const int n = 2 * cv::kSampleRate;
  cv::SlapbackParams p = params(1.0f);
  p.tuning.lowpassHz = 4000.0f;
  p.tuning.feedback = 0.0f;
  const size_t skip = static_cast<size_t>(cv::kSampleRate / 2);

  const std::vector<float> lo = sine(500.0f, n);
  gSlap.reset();
  const float loRms = rms(wetOf(run(gSlap, lo, p), lo), skip);

  const std::vector<float> hi = sine(8000.0f, n);
  gSlap.reset();
  const float hiRms = rms(wetOf(run(gSlap, hi, p), hi), skip);

  const float db = 20.0f * log10f(hiRms / loRms);
  return report("lowpass", db <= -9.0f && db >= -15.0f,
                "8 kHz wet vs 500 Hz wet=%.2f dB (-15..-9)", db);
}

bool testFeedback() {
  const std::vector<float> in = impulse(cv::kSampleRate);
  cv::SlapbackParams p = params(1.0f);
  p.tuning.lowpassHz = 12000.0f;
  p.tuning.timeMs = 80.0f;
  p.tuning.feedback = 0.5f;
  gSlap.reset();
  const std::vector<float> wet = wetOf(run(gSlap, in, p), in);
  const int first = kImpulseAt + 80 * cv::kSampleRate / 1000;
  const int second = kImpulseAt + 160 * cv::kSampleRate / 1000;
  const float p1 = std::fabs(wet[static_cast<size_t>(peakIndex(wet, first - 10, first + 10))]);
  const float p2 = std::fabs(wet[static_cast<size_t>(peakIndex(wet, second - 10, second + 10))]);
  const float ratio = p2 / p1;
  return report("feedback", ratio >= 0.4f && ratio <= 0.6f,
                "repeat 2 / repeat 1=%.3f (0.5 +-0.1), peaks %.3f %.3f", ratio, p1, p2);
}

bool testNoAlloc() {
  const std::vector<float> in = sine(440.0f, cv::kBlock * 8);
  cv::SlapbackParams p = params(0.7f);
  p.tuning.feedback = 0.3f;
  gSlap.reset();
  run(gSlap, in, p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testTime();
  ok &= testLowpass();
  ok &= testFeedback();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
