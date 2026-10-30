// Spring reverb checks: three echo spacings, sweep direction and growth for both chirp signs,
// level across DECAY, stability, the return low-pass, Reverb passthrough and no-alloc. No window,
// no audio device.

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
#include "engine/spring.h"

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

// Plain loops: no diffusion, no wobble, filters wide open, no drive.
cv::SpringParams plain() {
  cv::SpringParams p;
  p.dwell = 0.0f;
  p.tuning.diffuse = 0.0f;
  p.tuning.wobbleDepth = 0.0f;
  p.tuning.trebleLossHz = 20000.0f;
  p.tuning.bassCutHz = 20.0f;
  p.tuning.returnLpHz = 20000.0f;
  return p;
}

Signal run(const Signal& in, const cv::SpringParams& p) {
  auto sb = std::make_unique<cv::Spring>();
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

std::complex<double> dft(const Signal& x, size_t from, size_t to, double hz) {
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  std::complex<double> acc = 0.0;
  for (size_t i = from; i < to; ++i)
    acc += static_cast<double>(x[i]) * std::polar(1.0, -w * static_cast<double>(i - from));
  return acc;
}

// Group delay in samples of x[from..to) at hz, from the phase slope.
double groupDelay(const Signal& x, size_t from, size_t to, double hz) {
  const double dHz = 2.0;
  const double dw = 2.0 * kPi * dHz / cv::kSampleRate;
  const double dphi = std::arg(dft(x, from, to, hz + dHz) / dft(x, from, to, hz - dHz));
  return static_cast<double>(from) - dphi / (2.0 * dw);
}

// 1. With no diffusion or sweep, the first three echoes land at springMs1, springMs2 and springMs3.
bool echoSpacing() {
  cv::SpringParams p = plain();
  p.tuning.chirpSections = 0;
  const Signal y = run(impulse(4800), p);
  const auto peakIn = [&](double fromMs, double toMs) {
    size_t best = static_cast<size_t>(fromMs * kMs);
    for (size_t i = best; i < static_cast<size_t>(toMs * kMs); ++i)
      if (std::fabs(y[i]) > std::fabs(y[best])) best = i;
    return static_cast<double>(best) / kMs;
  };
  const double s1 = p.tuning.springMs1, s2 = p.tuning.springMs2, s3 = p.tuning.springMs3;
  const double e1 = peakIn(1.0, (s1 + s2) / 2.0);
  const double e2 = peakIn((s1 + s2) / 2.0, (s2 + s3) / 2.0);
  const double e3 = peakIn((s2 + s3) / 2.0, (s3 + 2.0 * s1) / 2.0);
  const bool ok = std::fabs(e1 - s1) < 0.5 && std::fabs(e2 - s2) < 0.5 && std::fabs(e3 - s3) < 0.5;
  return report("echo spacing", ok, "echoes at %.2f / %.2f / %.2f ms (want %.0f / %.0f / %.0f)", e1, e2, e3,
                s1, s2, s3);
}

// 2. Each echo sweeps: with chirpSign +1, 3.8 kHz arrives after 300 Hz (up); with -1, before
// (down). Echo 2 has passed the disperser twice, so its lag is about twice echo 1's.
bool sweep(float sign, const char* name) {
  cv::SpringParams p = plain();
  p.tuning.springMs1 = p.tuning.springMs2 = p.tuning.springMs3 = 80.0f;
  p.tuning.chirpSign = sign;
  p.decay = 1.0f;
  const Signal y = run(impulse(static_cast<int>(240 * kMs)), p);
  const auto lagMs = [&](int k) {
    const size_t from = static_cast<size_t>((80.0 * k - 20.0) * kMs);
    const size_t to = static_cast<size_t>((80.0 * k + 60.0) * kMs);
    return (groupDelay(y, from, to, 3800.0) - groupDelay(y, from, to, 300.0)) / kMs;
  };
  const double lag1 = lagMs(1), lag2 = lagMs(2);
  const double growth = lag2 / lag1;
  const bool ok = static_cast<double>(sign) * lag1 > 5.0 && growth > 1.7 && growth < 2.3;
  return report(name, ok, "3.8 kHz minus 300 Hz arrival %+.1f ms (echo 1), %+.1f ms (echo 2)", lag1, lag2);
}

// 3. The input scales by sqrt((1 - g^2) / g), so the tail's level holds as DECAY moves.
bool levelAcrossDecay(float sign, const char* name) {
  const Signal x = noise(4 * cv::kSampleRate, 0.25f);
  cv::SpringParams lo, hi;
  lo.tuning.chirpSign = hi.tuning.chirpSign = sign;
  lo.decay = 0.0f;
  hi.decay = 1.0f;
  const size_t from = static_cast<size_t>(2 * cv::kSampleRate);
  const double a = rms(run(x, lo), from, x.size()), b = rms(run(x, hi), from, x.size());
  const double db = 20.0 * std::log10(b / a);
  return report(name, std::fabs(db) < 1.0, "DECAY 1 vs 0: %+.2f dB (want within 1)", db);
}

// 4. Full DECAY and dwell on 20 s of loud noise stays finite and dies after the input stops.
bool stable(float sign, const char* name) {
  const int on = 20 * cv::kSampleRate;
  Signal x = noise(on + 20 * cv::kSampleRate, 1.0f);
  for (size_t i = static_cast<size_t>(on); i < x.size(); ++i) x[i] = 0.0f;
  cv::SpringParams p;
  p.tuning.chirpSign = sign;
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
  return report(name, ok, "peak %.2f over 20 s, tail RMS %.2g 20 s after input stops",
                static_cast<double>(peak), tail);
}

// 5. The return low-pass sits on the summed output: the impulse response at returnLpHz 6 kHz over
// the one at the widest corner matches the ratio of the two one-poles at 1 and 12 kHz.
bool returnLowPass() {
  cv::SpringParams wide = plain();
  wide.decay = 0.0f;
  cv::SpringParams lp = wide;
  lp.tuning.returnLpHz = cv::SpringTuning{}.returnLpHz;
  const Signal x = impulse(2 * cv::kSampleRate);
  const Signal yw = run(x, wide), yl = run(x, lp);
  const auto onePoleDb = [](double cornerHz, double hz) {
    const double c = cv::onePoleCoef(static_cast<float>(cornerHz));
    const std::complex<double> h = c / (1.0 - (1.0 - c) * std::polar(1.0, -2.0 * kPi * hz / cv::kSampleRate));
    return 20.0 * std::log10(std::abs(h));
  };
  bool ok = true;
  double got[2] = {0.0, 0.0}, want[2] = {0.0, 0.0};
  const double hz[2] = {1000.0, 12000.0};
  for (int i = 0; i < 2; ++i) {
    got[i] = 20.0 * std::log10(std::abs(dft(yl, 0, x.size(), hz[i])) / std::abs(dft(yw, 0, x.size(), hz[i])));
    want[i] = onePoleDb(lp.tuning.returnLpHz, hz[i]) - onePoleDb(wide.tuning.returnLpHz, hz[i]);
    ok = ok && std::fabs(got[i] - want[i]) < 0.2;
  }
  ok = ok && want[1] < -5.0;
  return report("return low-pass", ok,
                "%.0f Hz corner: %+.2f dB at 1 kHz (want %+.2f), %+.2f dB at 12 kHz (want %+.2f)",
                static_cast<double>(lp.tuning.returnLpHz), got[0], want[0], got[1], want[1]);
}

// 6. Reverb engine Spring: settled off is a bit-exact copy on every sample; process() never
// allocates.
bool reverbBlock() {
  auto rv = std::make_unique<cv::Reverb>();
  cv::ReverbParams p;
  p.engine = cv::kReverbSpring;
  p.wet = 1.0f;
  const Signal x = noise(2 * cv::kSampleRate, 0.5f);
  Signal y(x.size());
  const size_t off = x.size() / 2;
  gInProcess = true;
  for (size_t i = 0; i < off; i += cv::kBlock) rv->process(&x[i], &y[i], cv::kBlock, p);
  p.on = false;
  for (size_t i = off; i + cv::kBlock <= x.size(); i += cv::kBlock) rv->process(&x[i], &y[i], cv::kBlock, p);
  gInProcess = false;
  // The off fade snaps to 0 about 280 ms after the switch; from 500 ms on every sample must match.
  size_t settled = x.size();
  while (settled > off && y[settled - 1] == x[settled - 1]) --settled;
  size_t mismatches = 0;
  for (size_t i = off + static_cast<size_t>(cv::kSampleRate / 2); i < x.size(); ++i)
    mismatches += y[i] != x[i];
  const double settledMs = static_cast<double>(settled - off) / kMs;
  return report("reverb block", mismatches == 0,
                "on=false out == in on every sample from %.1f ms after the switch, no allocation", settledMs);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= echoSpacing();
  ok &= sweep(1.0f, "sweep up, grows");
  ok &= sweep(-1.0f, "sweep down, grows");
  ok &= levelAcrossDecay(1.0f, "level DECAY up");
  ok &= levelAcrossDecay(-1.0f, "level DECAY down");
  ok &= stable(1.0f, "stability up");
  ok &= stable(-1.0f, "stability down");
  ok &= returnLowPass();
  ok &= reverbBlock();
  std::printf("sizeof(cv::Spring) = %zu bytes\n", sizeof(cv::Spring));
  std::printf("%s spring\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
