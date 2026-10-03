// SPRING B checks: echo spacing, upward sweep, sweep growth per lap, level
// across DECAY, stability, Reverb passthrough and no-alloc. No window, no audio device.

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "engine/reverb.h"
#include "engine/spring_b.h"

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
constexpr double kMs = cv::kSampleRate / 1000.0;
using Signal = std::vector<float>;

bool report(const char* name, bool pass, const char* fmt, ...) {
  std::printf("%s %-18s ", pass ? "PASS" : "FAIL", name);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stdout, fmt, args);
  va_end(args);
  std::printf("\n");
  return pass;
}

// Plain loop: no diffusion, no wobble, filters wide open, no drive.
cv::SpringBParams plain() {
  cv::SpringBParams p;
  p.dwell = 0.0f;
  p.tuning.diffuse = 0.0f;
  p.tuning.wobbleDepth = 0.0f;
  p.tuning.trebleLossHz = 20000.0f;
  p.tuning.bassCutHz = 20.0f;
  return p;
}

Signal run(const Signal& in, const cv::SpringBParams& p) {
  auto sb = std::make_unique<cv::SpringB>();
  Signal out(in.size());
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock)
    sb->process(&in[i], &out[i], cv::kBlock, p);
  return out;
}

Signal impulse(int n) {
  Signal x(static_cast<size_t>(n), 0.0f);
  x[0] = 1.0f;
  return x;
}

Signal noise(int n, float amp) {
  Signal x(static_cast<size_t>(n));
  uint32_t s = 0x12345678u;
  for (float& v : x) {
    s = s * 1664525u + 1013904223u;
    v = amp * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f);
  }
  return x;
}

double rms(const Signal& x, size_t from, size_t to) {
  double e = 0.0;
  for (size_t i = from; i < to; ++i) e += static_cast<double>(x[i]) * x[i];
  return std::sqrt(e / static_cast<double>(to - from));
}

// Group delay in samples of x[from..to) at hz, from the phase slope.
double groupDelay(const Signal& x, size_t from, size_t to, double hz) {
  const auto dft = [&](double w) {
    std::complex<double> acc = 0.0;
    for (size_t i = from; i < to; ++i)
      acc += static_cast<double>(x[i]) * std::polar(1.0, -w * static_cast<double>(i - from));
    return acc;
  };
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  const double dw = 2.0 * kPi * 2.0 / cv::kSampleRate;
  const double dphi = std::arg(dft(w + dw) / dft(w - dw));
  return static_cast<double>(from) - dphi / (2.0 * dw);
}

// 1. With no diffusion or sweep, the first two echoes land at halfMs1 and halfMs2.
bool echoSpacing() {
  cv::SpringBParams p = plain();
  p.tuning.chirpSections = 0;
  const Signal y = run(impulse(4800), p);
  const auto peakIn = [&](double fromMs, double toMs) {
    size_t best = static_cast<size_t>(fromMs * kMs);
    for (size_t i = best; i < static_cast<size_t>(toMs * kMs); ++i)
      if (std::fabs(y[i]) > std::fabs(y[best])) best = i;
    return static_cast<double>(best) / kMs;
  };
  const double e1 = peakIn(1.0, 37.0), e2 = peakIn(37.0, 45.0);
  const bool ok = std::fabs(e1 - p.tuning.halfMs1) < 0.5 && std::fabs(e2 - p.tuning.halfMs2) < 0.5;
  return report("echo spacing", ok, "echoes at %.2f / %.2f ms (want %.0f / %.0f)", e1, e2,
                static_cast<double>(p.tuning.halfMs1), static_cast<double>(p.tuning.halfMs2));
}

// 2. Echo 1 sweeps up: 3.8 kHz arrives after 300 Hz. Echo 2 has passed the
// disperser twice, so its lag is about twice echo 1's.
bool sweepGrows() {
  cv::SpringBParams p = plain();
  p.tuning.halfMs1 = p.tuning.halfMs2 = 80.0f;
  p.decay = 1.0f;
  const Signal y = run(impulse(static_cast<int>(240 * kMs)), p);
  const auto lagMs = [&](int k) {
    const size_t from = static_cast<size_t>((80.0 * k - 20.0) * kMs);
    const size_t to = static_cast<size_t>((80.0 * k + 60.0) * kMs);
    return (groupDelay(y, from, to, 3800.0) - groupDelay(y, from, to, 300.0)) / kMs;
  };
  const double lag1 = lagMs(1), lag2 = lagMs(2);
  const bool ok = lag1 > 5.0 && lag2 > 1.7 * lag1 && lag2 < 2.3 * lag1;
  return report("sweep up, grows", ok, "3.8 kHz lags 300 Hz by %.1f ms (echo 1), %.1f ms (echo 2)", lag1, lag2);
}

// 3. The input scales by sqrt(1 - g^2), so the tail's level holds as DECAY moves.
bool levelAcrossDecay() {
  const Signal x = noise(4 * cv::kSampleRate, 0.25f);
  cv::SpringBParams lo, hi;
  lo.decay = 0.0f;
  hi.decay = 1.0f;
  const size_t from = static_cast<size_t>(2 * cv::kSampleRate);
  const double a = rms(run(x, lo), from, x.size()), b = rms(run(x, hi), from, x.size());
  const double db = 20.0 * std::log10(b / a);
  return report("level vs DECAY", std::fabs(db) < 3.0, "DECAY 1 vs 0: %+.2f dB (want within 3)", db);
}

// 4. Full DECAY and DWELL on loud noise stays finite and dies after the input stops.
bool stable() {
  const int on = 10 * cv::kSampleRate;
  Signal x = noise(on + 20 * cv::kSampleRate, 1.0f);
  for (size_t i = static_cast<size_t>(on); i < x.size(); ++i) x[i] = 0.0f;
  cv::SpringBParams p;
  p.decay = 1.0f;
  p.dwell = 1.0f;
  const Signal y = run(x, p);
  float peak = 0.0f;
  bool finite = true;
  for (float v : y) {
    finite = finite && std::isfinite(v);
    peak = std::fmax(peak, std::fabs(v));
  }
  const double tail = rms(y, y.size() - static_cast<size_t>(cv::kSampleRate / 10), y.size());
  const bool ok = finite && peak < 16.0f && tail < 1e-5;
  return report("stability", ok, "peak %.2f, tail RMS %.2g 20 s after input stops", static_cast<double>(peak), tail);
}

// 5. Reverb engine SPRING B: on=false is a bit-exact copy; process() never allocates.
bool reverbBlock() {
  auto rv = std::make_unique<cv::Reverb>();
  cv::ReverbParams p;
  p.engine = cv::kReverbSpringB;
  p.mix = 1.0f;
  const Signal x = noise(cv::kSampleRate, 0.5f);
  Signal y(x.size());
  gInProcess = true;
  for (size_t i = 0; i < x.size() / 2; i += cv::kBlock) rv->process(&x[i], &y[i], cv::kBlock, p);
  p.on = false;
  for (size_t i = x.size() / 2; i + cv::kBlock <= x.size(); i += cv::kBlock)
    rv->process(&x[i], &y[i], cv::kBlock, p);
  gInProcess = false;
  double worst = 0.0;
  // The off fade snaps to 0 about 280 ms after the switch; compare the last 100 ms.
  for (size_t i = x.size() - static_cast<size_t>(cv::kSampleRate / 10); i < x.size(); ++i)
    worst = std::fmax(worst, std::fabs(static_cast<double>(y[i]) - x[i]));
  return report("reverb block", worst == 0.0, "on=false max|out-in| %.3g, no allocation", worst);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= echoSpacing();
  ok &= sweepGrows();
  ok &= levelAcrossDecay();
  ok &= stable();
  ok &= reverbBlock();
  std::printf("sizeof(cv::SpringB) = %zu bytes\n", sizeof(cv::SpringB));
  std::printf("%s spring_b\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
