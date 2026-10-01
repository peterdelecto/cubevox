// Spring checks: passthrough, chirp direction, echo train, decay, band limit,
// splash, stability, no-alloc. No window, no audio device.

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

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
constexpr int kSr = cv::kSampleRate;
constexpr int kFft = 2048;

using Signal = std::vector<float>;

size_t at(int i) { return static_cast<size_t>(i); }
int ms(double m) { return static_cast<int>(m * kSr / 1000.0); }

Signal run(cv::Spring& s, const Signal& in, const cv::SpringParams& p) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    s.process(&in[at(i)], &out[at(i)], n, p);
    gInProcess = false;
  }
  return out;
}

Signal minus(const Signal& a, const Signal& b) {
  Signal d(a.size());
  for (size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
  return d;
}

// Uniform white noise, fixed seed.
Signal noise(int n, float peak) {
  Signal x(at(n));
  uint32_t r = 0x2545F491u;
  for (float& v : x) {
    r = r * 1664525u + 1013904223u;
    v = peak * (static_cast<float>(r >> 8) / 8388608.0f - 1.0f);
  }
  return x;
}

cv::SpringParams params(float tension, float dwell) {
  cv::SpringParams p;
  p.tension = tension;
  p.dwell = dwell;
  p.tuning.wetDb = 0.0f;
  return p;
}

static cv::Spring gSpring;

// Wet impulse response of spring A alone, wander frozen.
Signal impulse(float tension, int n) {
  Signal in(at(n), 0.0f);
  in[0] = 1.0f;
  gSpring.reset();
  gSpring.setModEnabled(false);
  gSpring.setSolo(0);
  const Signal out = run(gSpring, in, params(tension, 0.0f));
  gSpring.setModEnabled(true);
  gSpring.setSolo(-1);
  return minus(out, in);
}

Signal wetNoise(const cv::SpringParams& p, int n) {
  const Signal in = noise(n, 0.25f);
  gSpring.reset();
  return minus(run(gSpring, in, p), in);
}

// RBJ band-pass, 0 dB peak.
Signal bandPass(const Signal& x, double hz, double q) {
  const double w0 = 2.0 * kPi * hz / kSr;
  const double alpha = std::sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double b0 = alpha / a0, b2 = -alpha / a0;
  const double a1 = -2.0 * std::cos(w0) / a0, a2 = (1.0 - alpha) / a0;
  Signal y(x.size());
  double z1 = 0.0, z2 = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double v = b0 * x[i] + z1;
    z1 = -a1 * v + z2;
    z2 = b2 * x[i] - a2 * v;
    y[i] = static_cast<float>(v);
  }
  return y;
}

// Zero-phase RMS envelope: a one-pole on the square, run forward then backward.
Signal envelope(const Signal& x, double sec) {
  const double a = 1.0 - std::exp(-1.0 / (sec * kSr));
  std::vector<double> p(x.size());
  double s = 0.0;
  for (size_t i = 0; i < x.size(); ++i) p[i] = s += a * (static_cast<double>(x[i]) * x[i] - s);
  s = 0.0;
  for (size_t i = x.size(); i-- > 0;) p[i] = s += a * (p[i] - s);
  Signal e(x.size());
  for (size_t i = 0; i < x.size(); ++i) e[i] = static_cast<float>(std::sqrt(p[i]));
  return e;
}

// First local maximum in [0, to) within 3 dB of the window's largest value.
int firstPeak(const Signal& e, int to) {
  float top = 0.0f;
  for (int i = 0; i < to; ++i) top = e[at(i)] > top ? e[at(i)] : top;
  for (int i = 1; i + 1 < to; ++i)
    if (e[at(i)] >= 0.708f * top && e[at(i)] >= e[at(i - 1)] && e[at(i)] > e[at(i + 1)]) return i;
  return -1;
}

double rmsOf(const Signal& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[at(i)]) * x[at(i)];
  return std::sqrt(s / (to - from));
}

double db(double ratio) { return 10.0 * std::log10(ratio < 1e-30 ? 1e-30 : ratio); }

void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const std::complex<double> w = std::polar(1.0, -2.0 * kPi / static_cast<double>(len));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> wk = 1.0;
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * wk;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        wk *= w;
      }
    }
  }
}

// Welch power spectrum over [from, to), Hann window, half overlap.
std::vector<double> spectrum(const Signal& x, int from, int to) {
  std::vector<double> p(kFft / 2 + 1, 0.0);
  std::vector<std::complex<double>> buf(kFft);
  for (int s = from; s + kFft <= to; s += kFft / 2) {
    for (int i = 0; i < kFft; ++i) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kFft);
      buf[at(i)] = w * x[at(s + i)];
    }
    fft(buf);
    for (size_t k = 0; k < p.size(); ++k) p[k] += std::norm(buf[k]);
  }
  return p;
}

double bandEnergy(const std::vector<double>& p, double lo, double hi) {
  double e = 0.0;
  for (size_t k = 0; k < p.size(); ++k) {
    const double hz = static_cast<double>(k) * kSr / kFft;
    if (hz >= lo && hz < hi) e += p[k];
  }
  return e;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

bool testPassthrough() {
  const Signal in = noise(kSr / 2, 0.25f);
  cv::SpringParams p = params(1.0f, 1.0f);
  p.on = false;
  gSpring.reset();
  const Signal out = run(gSpring, in, p);
  float maxDiff = 0.0f;
  for (int i = ms(100); i < static_cast<int>(in.size()); ++i) {
    const float d = std::fabs(out[at(i)] - in[at(i)]);
    maxDiff = d > maxDiff ? d : maxDiff;
  }
  return report("passthrough", maxDiff < 1e-6f, "on=false max|out-in|=%.3g after 100 ms (<1e-6)",
                maxDiff);
}

bool testChirp() {
  const Signal wet = impulse(0.5f, ms(60));
  const int lo = firstPeak(envelope(bandPass(wet, 500.0, 4.0), 0.010), ms(60));
  const int hi = firstPeak(envelope(bandPass(wet, 4000.0, 4.0), 0.010), ms(60));
  const double loMs = lo * 1000.0 / kSr, hiMs = hi * 1000.0 / kSr;
  return report("chirp-up", lo >= 0 && hi >= 0 && hiMs - loMs >= 10.0,
                "first peak 500 Hz %.2f ms, 4 kHz %.2f ms, lag %.2f ms (>=10)", loMs, hiMs,
                hiMs - loMs);
}

bool testEchoTrain() {
  const Signal e = envelope(impulse(0.5f, ms(500)), 0.010);
  std::vector<float> peaks;
  for (int i = 1; i + 1 < ms(500); ++i)
    if (e[at(i)] >= e[at(i - 1)] && e[at(i)] > e[at(i + 1)]) peaks.push_back(e[at(i)]);
  int count = 0;
  const float floor = peaks.empty() ? 0.0f : peaks[0] * 0.01f;
  for (float v : peaks) count += v > floor ? 1 : 0;
  return report("echo-train", count >= 4 && count <= 40,
                "%.0f envelope maxima above -40 dB re the first in 0-500 ms (4..40)", count);
}

bool testDecay() {
  double tail[2];
  const float tensions[2] = {0.0f, 1.0f};
  for (int k = 0; k < 2; ++k) {
    const Signal wet = impulse(tensions[k], ms(1500));
    tail[k] = db(std::pow(rmsOf(wet, ms(1000), ms(1500)) / rmsOf(wet, 0, ms(500)), 2.0));
  }
  return report("decay", tail[1] - tail[0] >= 20.0,
                "late/early RMS tension 0 %.1f dB, tension 1 %.1f dB, gap %.1f dB (>=20)", tail[0],
                tail[1], tail[1] - tail[0]);
}

bool testBandLimit() {
  // Chirp path only; the high band is the splash and check 6 covers it.
  cv::SpringParams p = params(0.5f, 0.0f);
  p.tuning.hfMixDbLo = p.tuning.hfMixDbHi = -120.0f;
  const std::vector<double> s = spectrum(wetNoise(p, 2 * kSr), kSr / 2, 2 * kSr);
  const double gap = db(bandEnergy(s, 300.0, 4000.0) / bandEnergy(s, 7200.0, kSr / 2.0));
  return report("band-limit", gap >= 40.0,
                "dwell 0, high band muted: 300 Hz-4 kHz over >7.2 kHz = %.1f dB (>=40)", gap);
}

bool testSplash() {
  double ratio[2];
  const float dwells[2] = {0.0f, 1.0f};
  for (int k = 0; k < 2; ++k) {
    const Signal wet = wetNoise(params(0.5f, dwells[k]), 2 * kSr);
    const std::vector<double> p = spectrum(wet, kSr / 2, 2 * kSr);
    ratio[k] = db(bandEnergy(p, 5000.0, kSr / 2.0) / bandEnergy(p, 20.0, 4000.0));
  }
  return report("splash", ratio[1] - ratio[0] >= 6.0,
                ">5 kHz / <4 kHz dwell 0 %.1f dB, dwell 1 %.1f dB, rise %.1f dB (>=6)", ratio[0],
                ratio[1], ratio[1] - ratio[0]);
}

bool testStable() {
  Signal in = noise(3 * kSr, 0.5f);
  in.resize(at(6 * kSr), 0.0f);
  cv::SpringParams p = params(1.0f, 1.0f);
  p.tuning.wetDb = -6.0f;
  p.tuning.springs = 3;
  gSpring.reset();
  const Signal out = run(gSpring, in, p);
  bool finite = true;
  float peak = 0.0f;
  for (float v : out) {
    finite = finite && std::isfinite(v);
    peak = std::fabs(v) > peak ? std::fabs(v) : peak;
  }
  const double first = rmsOf(out, 3 * kSr, 3 * kSr + kSr / 2);
  const double last = rmsOf(out, 6 * kSr - kSr / 2, 6 * kSr);
  return report("stable", finite && peak < 4.0f && last < first,
                "peak |out| %.3f (<4), silence RMS first 0.5 s %.3g, last 0.5 s %.3g", peak, first,
                last);
}

bool testNoAlloc() {
  const Signal in = noise(cv::kBlock * 8, 0.25f);
  cv::SpringParams p = params(0.5f, 0.5f);
  p.tuning.springs = 3;
  p.tuning.hfSections = 200;
  p.tuning.boingDb = 6.0f;
  gSpring.reset();
  run(gSpring, in, p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testChirp();
  ok &= testEchoTrain();
  ok &= testDecay();
  ok &= testBandLimit();
  ok &= testSplash();
  ok &= testStable();
  ok &= testNoAlloc();
  std::printf("sizeof(cv::Spring) = %zu bytes\n", sizeof(cv::Spring));
  return ok ? 0 : 1;
}
