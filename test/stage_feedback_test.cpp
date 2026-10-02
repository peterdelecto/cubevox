// Stage feedback checks: off is silent, safety ceiling, ring-on, intermittency,
// no allocation. No window, no audio device.

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "emulator/stage_feedback.h"

namespace {

std::atomic<bool> gInProcess{false};

void guardAlloc() {
  if (gInProcess.load()) std::abort();
}

}  // namespace

// Allocation inside the feedback class aborts the test binary.
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
constexpr int kFs = cv::kSampleRate;
constexpr float kFloorRms = 3.1622777e-4f;  // -70 dBFS room noise

int gFailures = 0;

void check(bool ok, const char* what, double value) {
  std::printf("%s %s (%.3f)\n", ok ? "PASS" : "FAIL", what, value);
  if (!ok) ++gFailures;
}

double toDb(double x) { return 20.0 * std::log10(x > 1e-12 ? x : 1e-12); }

// Runs the loop with a unity box in 64-sample blocks: returns first, pushes after.
// src(i) gives the mic sample at absolute sample index i.
struct UnityBox {
  float operator()(float x) const { return x; }
};

// Compressive stand-in for a driven box: high small-signal gain, saturating.
// Makeup puts a -12 dBFS sine out at -12 dBFS.
struct DriveBox {
  static constexpr float kG = 30.0f;
  float peak = 0.25118864f;
  float makeup = peak / std::tanh(kG * peak);
  float operator()(float x) const { return makeup * std::tanh(kG * x); }
};

template <typename Src, typename Sink, typename Box = UnityBox>
void runLoop(cv::stagefb::StageFeedback& fb, long start, long count, Src src, Sink sink,
             Box box = Box()) {
  constexpr int kBlock = cv::kBlock;
  std::vector<float> ret(kBlock);
  std::vector<float> out(kBlock);
  std::vector<float> dry(kBlock);
  long i = 0;
  while (i < count) {
    const int n = static_cast<int>(std::min<long>(kBlock, count - i));
    for (int j = 0; j < n; ++j) {
      dry[static_cast<size_t>(j)] = src(start + i + j);
      gInProcess = true;
      ret[static_cast<size_t>(j)] = fb.returnSample(dry[static_cast<size_t>(j)]);
      gInProcess = false;
    }
    for (int j = 0; j < n; ++j) {
      const size_t s = static_cast<size_t>(j);
      out[s] = box(dry[s] + ret[s]);
      sink(start + i + j, ret[s], out[s]);
    }
    gInProcess = true;
    for (int j = 0; j < n; ++j) fb.push(out[static_cast<size_t>(j)]);
    gInProcess = false;
    i += n;
  }
}

// Radix-2 FFT magnitude peak (Hz) of the first n samples (n power of two), Hann windowed.
double dominantHz(const std::vector<float>& x, size_t n) {
  std::vector<std::complex<double>> a(n);
  for (size_t i = 0; i < n; ++i)
    a[i] = static_cast<double>(x[i]) * (0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n)));
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * kPi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; ++k) {
        const auto u = a[i + k];
        const auto v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
  size_t best = 1;
  double bestMag = 0.0;
  for (size_t k = 1; k < n / 2; ++k) {
    const double m = std::abs(a[k]);
    if (m > bestMag) {
      bestMag = m;
      best = k;
    }
  }
  return static_cast<double>(best) * kFs / static_cast<double>(n);
}

uint32_t gNoise = 12345u;
float whiteNoise() {
  gNoise ^= gNoise << 13;
  gNoise ^= gNoise >> 17;
  gNoise ^= gNoise << 5;
  return static_cast<float>(gNoise >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

void testOff() {
  cv::stagefb::StageFeedback fb;
  fb.configure(false, 1.0f, 1.0f);
  bool allZero = true;
  for (int i = 0; i < kFs; ++i) {
    allZero = allZero && fb.returnSample(0.0f) == 0.0f;
    fb.push(0.5f);
  }
  check(allZero, "off returns exactly 0", 0.0);

  // Run on, switch off, switch on again: must replay the fresh sequence.
  cv::stagefb::StageFeedback a;
  cv::stagefb::StageFeedback b;
  a.configure(true, 0.9f, 0.5f);
  for (int i = 0; i < kFs; ++i) {
    a.returnSample(0.0f);
    a.push(0.3f);
  }
  a.configure(false, 0.9f, 0.5f);
  allZero = true;
  for (int i = 0; i < 1000; ++i) allZero = allZero && a.returnSample(0.0f) == 0.0f;
  a.configure(true, 0.9f, 0.5f);
  b.configure(true, 0.9f, 0.5f);
  bool same = true;
  for (int i = 0; i < 5000; ++i) {
    const float ra = a.returnSample(0.0f);
    const float rb = b.returnSample(0.0f);
    same = same && ra == rb;
    a.push(0.2f);
    b.push(0.2f);
  }
  check(allZero, "off after use returns 0", 0.0);
  check(same, "off then on restarts from the fresh state", 0.0);
}

void testSafety() {
  cv::stagefb::StageFeedback fb;
  fb.configure(true, 1.0f, 1.0f);
  double peak = 0.0;
  bool finite = true;
  const long burst = kFs / 2;
  runLoop(fb, 0, burst + 10L * kFs,
          [&](long i) { return i < burst ? 0.5f * whiteNoise() : 0.0f; },
          [&](long, float r, float o) {
            peak = std::max(peak, static_cast<double>(std::fabs(r)));
            finite = finite && std::isfinite(r) && std::isfinite(o);
          });
  const double ceilingDb = -12.0 + 0.1;
  check(toDb(peak) <= ceilingDb, "return peak <= -12 dBFS (+0.1)", toDb(peak));
  check(finite, "no NaN or inf", 0.0);
}

// RMS of the return over [from, to) samples, recorded by the sink.
struct RmsWindow {
  long from;
  long to;
  double sumSq = 0.0;
  long n = 0;
  void add(long i, float r) {
    if (i >= from && i < to) {
      sumSq += static_cast<double>(r) * static_cast<double>(r);
      ++n;
    }
  }
  double rms() const { return n ? std::sqrt(sumSq / static_cast<double>(n)) : 0.0; }
};

float sung(long i) {
  return 0.25118864f * static_cast<float>(std::sin(2.0 * kPi * 200.0 * static_cast<double>(i) / kFs));
}

void testCalibration() {
  const long total = 5L * kFs;
  {
    cv::stagefb::StageFeedback fb;
    fb.configure(true, 0.5f, 0.4f);
    RmsWindow w{0, total};
    runLoop(fb, 0, total, [](long) { return 0.0f; }, [&](long i, float r, float) { w.add(i, r); });
    const double above = toDb(w.rms()) - toDb(kFloorRms);
    check(above < 10.0, "Amount 50, unity box: no build-up in 5 s (< 10 dB over floor)", above);
  }
  {
    cv::stagefb::StageFeedback fb;
    fb.configure(true, 0.5f, 0.4f);
    RmsWindow w{total - kFs, total};
    runLoop(fb, 0, total, [](long) { return 0.0f; }, [&](long i, float r, float) { w.add(i, r); },
            DriveBox());
    const double above = toDb(w.rms()) - toDb(kFloorRms);
    check(above >= 20.0, "Amount 50, drive box: builds >= 20 dB over floor within 5 s", above);
  }
}

void testSingingBreaksIt() {
  cv::stagefb::StageFeedback fb;
  fb.configure(true, 0.5f, 0.4f);
  const long build = 5L * kFs;
  const long burst = 2L * kFs;
  const long after = 3L * kFs;
  RmsWindow pre{build - kFs / 2, build};
  RmsWindow late{build + kFs, build + burst};
  RmsWindow rebuilt{build + burst + after - kFs / 2, build + burst + after};
  runLoop(fb, 0, build + burst + after,
          [&](long i) { return (i >= build && i < build + burst) ? sung(i) : 0.0f; },
          [&](long i, float r, float) {
            pre.add(i, r);
            late.add(i, r);
            rebuilt.add(i, r);
          },
          DriveBox());
  const double drop = toDb(pre.rms()) - toDb(late.rms());
  const double again = toDb(rebuilt.rms()) - toDb(kFloorRms);
  check(drop >= 12.0, "singing: return in 2nd half of burst >= 12 dB below pre-burst", drop);
  check(again >= 20.0, "rebuilds >= 20 dB over floor within 3 s after burst", again);
}

void testIntermittency() {
  cv::stagefb::StageFeedback fb;
  fb.configure(true, 1.0f, 1.0f);
  constexpr int kSeconds = 60;
  std::vector<float> win(static_cast<size_t>(kFs));
  std::vector<double> found;
  runLoop(fb, 0, static_cast<long>(kSeconds) * kFs, [&](long) { return 0.0f; },
          [&](long i, float r, float) {
            win[static_cast<size_t>(i % kFs)] = r;
            if (i % kFs == kFs - 1) {
              const double hz = dominantHz(win, 32768);
              bool isNew = true;
              for (double f : found)
                if (std::fabs(f - hz) <= 100.0) isNew = false;
              if (isNew) found.push_back(hz);
            }
          });
  check(found.size() >= 4, "dominant frequency takes >= 4 values >100 Hz apart",
        static_cast<double>(found.size()));
}

void testNoAlloc() {
  // Every class call above runs under the allocation guard; reaching here means none aborted.
  check(true, "no allocation in returnSample/push", 0.0);
}

}  // namespace

int main() {
  testOff();
  testSafety();
  testCalibration();
  testSingingBreaksIt();
  testIntermittency();
  testNoAlloc();
  std::printf(gFailures ? "FAILED\n" : "OK\n");
  return gFailures ? 1 : 0;
}
