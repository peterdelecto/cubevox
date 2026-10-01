// Polish EQ checks: passthrough, default response, flat, no-alloc. No window, no audio device.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/polish.h"

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
constexpr int kSecond = cv::kSampleRate;

std::vector<float> sine(double hz, int n) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] = kAmp * static_cast<float>(std::sin(2.0 * kPi * hz * i / cv::kSampleRate));
  return x;
}

// Runs the whole signal through process() in kBlock chunks.
std::vector<float> run(cv::Polish& e, const std::vector<float>& in, const cv::PolishParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    e.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

double rmsOf(const std::vector<float>& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / (to - from));
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0,
            double d = 0.0, double e = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::Polish gEq;

// Gain in dB of a sine through the EQ, RMS over seconds 1-2.
double gainDb(double hz, const cv::PolishParams& p) {
  const std::vector<float> in = sine(hz, 2 * kSecond);
  gEq.reset();
  const std::vector<float> out = run(gEq, in, p);
  return 20.0 * std::log10(rmsOf(out, kSecond, 2 * kSecond) / rmsOf(in, kSecond, 2 * kSecond));
}

bool testPassthrough() {
  const std::vector<float> in = sine(1000.0, kSecond);
  cv::PolishParams off;
  off.on = false;
  gEq.reset();
  const std::vector<float> out = run(gEq, in, off);
  float maxDiff = 0.0f;
  for (int i = cv::kSampleRate / 10; i < kSecond; ++i) {
    const float d = std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
    if (d > maxDiff) maxDiff = d;
  }
  return report("passthrough", maxDiff == 0.0f, "on=false max|out-in|=%.3g after 100 ms (0)", maxDiff);
}

bool testResponse() {
  const cv::PolishParams p;
  const double g50 = gainDb(50.0, p), g300 = gainDb(300.0, p), g1k = gainDb(1000.0, p);
  const double g35 = gainDb(3500.0, p), g12 = gainDb(12000.0, p);
  const bool ok = g50 <= -8.0 && std::fabs(g300 + 2.5) <= 0.4 && std::fabs(g1k) <= 0.4 &&
                  std::fabs(g35 - 1.5) <= 0.4 && std::fabs(g12 - 1.5) <= 0.5;
  return report("response", ok,
                "50/300/1k/3.5k/12k Hz = %.2f / %.2f / %.2f / %.2f / %.2f dB (<=-8, -2.5, 0, +1.5, +1.5)",
                g50, g300, g1k, g35, g12);
}

bool testFlat() {
  cv::PolishParams p;
  p.hpHz = 40.0f;
  p.dipDb = 0.0f;
  p.presenceDb = 0.0f;
  p.airDb = 0.0f;
  const double g = gainDb(1000.0, p);
  return report("flat", std::fabs(g) <= 0.1, "all gains 0, hp 40 Hz: 1 kHz = %.3f dB (+-0.1)", g);
}

bool testNoAlloc() {
  const std::vector<float> in = sine(500.0, cv::kBlock * 8);
  gEq.reset();
  run(gEq, in, cv::PolishParams{});  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testResponse();
  ok &= testFlat();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
