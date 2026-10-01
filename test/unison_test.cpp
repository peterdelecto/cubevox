// Unison checks: passthrough, detune, level, no-alloc. No window, no audio device.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/unison.h"

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
constexpr float kFreq = 440.0f;
constexpr float kAmp = 0.5f;

std::vector<float> sine(int n) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = kAmp * sinf(2.0f * kPi * kFreq * i / cv::kSampleRate);
  return x;
}

// Runs the whole signal through process() in kBlock chunks.
std::vector<float> run(cv::Unison& u, const std::vector<float>& in,
                       const cv::UnisonParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    u.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

cv::UnisonParams params(float depth) {
  cv::UnisonParams p;
  p.on = true;
  p.depth = depth;
  return p;
}

float rms(const std::vector<float>& x) {
  double s = 0.0;
  for (float v : x) s += static_cast<double>(v) * v;
  return static_cast<float>(std::sqrt(s / static_cast<double>(x.size())));
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0) {
  char detail[160];
  std::snprintf(detail, sizeof detail, fmt, a, b);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::Unison gUnison;

bool testPassthrough() {
  const int n = cv::kSampleRate;
  const std::vector<float> in = sine(n);
  gUnison.reset();
  const std::vector<float> out = run(gUnison, in, params(0.0f));
  float maxDiff = 0.0f;
  for (int i = cv::kSampleRate / 20; i < n; ++i) {
    const float d = std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
    if (d > maxDiff) maxDiff = d;
  }

  const std::vector<float> silence(static_cast<size_t>(n), 0.0f);
  gUnison.reset();
  const std::vector<float> sOut = run(gUnison, silence, params(1.0f));
  float maxSilent = 0.0f;
  for (float v : sOut) maxSilent = std::fmax(maxSilent, std::fabs(v));

  return report("passthrough", maxDiff < 1e-6f && maxSilent == 0.0f,
                "max|out-in|=%.3g (<1e-6), silence max=%.3g (==0)", maxDiff, maxSilent);
}

// Peak |cents| from upward zero-crossing periods, linearly interpolated.
float peakCents(const std::vector<float>& x, int skip) {
  float peak = 0.0f;
  float prevCross = -1.0f;
  for (size_t i = static_cast<size_t>(skip); i + 1 < x.size(); ++i) {
    if (!(x[i] < 0.0f && x[i + 1] >= 0.0f)) continue;
    const float cross = static_cast<float>(i) + (-x[i]) / (x[i + 1] - x[i]);
    if (prevCross >= 0.0f) {
      const float hz = cv::kSampleRate / (cross - prevCross);
      const float cents = std::fabs(1200.0f * log2f(hz / kFreq));
      if (cents > peak) peak = cents;
    }
    prevCross = cross;
  }
  return peak;
}

bool testDetune() {
  const int n = 4 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  gUnison.reset();
  gUnison.setVoiceEnabled(0, false);
  const std::vector<float> out = run(gUnison, in, params(1.0f));
  gUnison.setVoiceEnabled(0, true);

  std::vector<float> wet(in.size());
  for (size_t i = 0; i < in.size(); ++i) wet[i] = out[i] - in[i];
  const float cents = peakCents(wet, cv::kSampleRate / 2);
  return report("detune", cents >= 15.0f && cents <= 30.0f,
                "peak=%.2f cents (15..30)", cents);
}

// Mean signed cents from upward zero-crossing periods, linearly interpolated.
float meanCents(const std::vector<float>& x, int skip) {
  double sum = 0.0;
  int count = 0;
  float prevCross = -1.0f;
  for (size_t i = static_cast<size_t>(skip); i + 1 < x.size(); ++i) {
    if (!(x[i] < 0.0f && x[i + 1] >= 0.0f)) continue;
    const float cross = static_cast<float>(i) + (-x[i]) / (x[i + 1] - x[i]);
    if (prevCross >= 0.0f) {
      const float hz = cv::kSampleRate / (cross - prevCross);
      sum += 1200.0f * log2f(hz / kFreq);
      ++count;
    }
    prevCross = cross;
  }
  return count ? static_cast<float>(sum / count) : 0.0f;
}

bool testFixedDetune() {
  const int n = 4 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  cv::UnisonParams p = params(1.0f);
  p.tuning.swingMinMs = 0.0f;
  p.tuning.swingMaxMs = 0.0f;
  p.tuning.detuneCents[1] = 10.0f;
  p.tuning.windowMs = 20.0f;
  gUnison.reset();
  gUnison.setVoiceEnabled(0, false);
  const std::vector<float> out = run(gUnison, in, p);
  gUnison.setVoiceEnabled(0, true);

  std::vector<float> wet(in.size());
  for (size_t i = 0; i < in.size(); ++i) wet[i] = out[i] - in[i];
  const float cents = meanCents(wet, cv::kSampleRate);
  return report("fixed detune", cents >= 8.0f && cents <= 12.0f,
                "mean=%.2f cents (10 +-2)", cents);
}

bool testLevel() {
  const int n = 4 * cv::kSampleRate;
  const std::vector<float> in = sine(n);
  gUnison.reset();
  const std::vector<float> out = run(gUnison, in, params(1.0f));
  const float db = 20.0f * log10f(rms(out) / rms(in));
  return report("level", std::fabs(db) <= 3.0f, "out/in=%.2f dB (+-3)", db);
}

bool testNoAlloc() {
  const std::vector<float> in = sine(cv::kBlock * 8);
  gUnison.reset();
  run(gUnison, in, params(0.7f));  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testDetune();
  ok &= testLevel();
  ok &= testNoAlloc();
  ok &= testFixedDetune();
  return ok ? 0 : 1;
}
