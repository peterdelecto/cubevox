// CHASM checks: passthrough, decay, chirp direction, wobble, wobble level,
// stability, no-alloc. No window, no audio device.

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/chasm.h"
#include "engine/reverb.h"

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

using Signal = std::vector<float>;

size_t at(int i) { return static_cast<size_t>(i); }
int ms(double m) { return static_cast<int>(m * kSr / 1000.0); }

template <class Engine, class Params>
Signal run(Engine& e, const Signal& in, const Params& p) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    e.process(&in[at(i)], &out[at(i)], n, p);
    gInProcess = false;
  }
  return out;
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

Signal sine(int n, double hz, float peak) {
  Signal x(at(n));
  for (int i = 0; i < n; ++i) x[at(i)] = peak * static_cast<float>(std::sin(2.0 * kPi * hz * i / kSr));
  return x;
}

cv::ChasmParams params(float decay, float wobble) {
  cv::ChasmParams p;
  p.decay = decay;
  p.wobble = wobble;
  p.tuning.wetDb = 0.0f;
  return p;
}

static cv::Chasm gChasm;
static cv::Reverb gReverb;

Signal wet(const Signal& in, const cv::ChasmParams& p) {
  gChasm.reset();
  return run(gChasm, in, p);
}

Signal impulse(float decay, float wobble, int n) {
  Signal in(at(n), 0.0f);
  in[0] = 1.0f;
  return wet(in, params(decay, wobble));
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
std::vector<double> spectrum(const Signal& x, int from, int to, int size) {
  std::vector<double> p(at(size / 2 + 1), 0.0);
  std::vector<std::complex<double>> buf(at(size));
  for (int s = from; s + size <= to; s += size / 2) {
    for (int i = 0; i < size; ++i) {
      const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / size);
      buf[at(i)] = w * x[at(s + i)];
    }
    fft(buf);
    for (size_t k = 0; k < p.size(); ++k) p[k] += std::norm(buf[k]);
  }
  return p;
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
  float maxDiff = 0.0f;
  for (int engine = 0; engine < 2; ++engine) {
    cv::ReverbParams p;
    p.on = false;
    p.engine = engine;
    gReverb.reset();
    const Signal out = run(gReverb, in, p);
    for (int i = ms(100); i < static_cast<int>(in.size()); ++i) {
      const float d = std::fabs(out[at(i)] - in[at(i)]);
      maxDiff = d > maxDiff ? d : maxDiff;
    }
  }
  cv::ReverbParams p;
  p.engine = cv::kReverbChasm;
  p.chasm = params(1.0f, 1.0f);
  gReverb.reset();
  const Signal out = run(gReverb, Signal(at(kSr / 2), 0.0f), p);
  float maxSilent = 0.0f;
  for (float v : out) maxSilent = std::fabs(v) > maxSilent ? std::fabs(v) : maxSilent;

  // Mix 0 with the block on and chasm running is a bit-exact copy once settled.
  cv::ReverbParams m;
  m.engine = cv::kReverbChasm;
  m.chasm = params(1.0f, 1.0f);
  m.mix = 0.0f;
  gReverb.reset();
  const Signal mixOut = run(gReverb, in, m);
  float maxMix = 0.0f;
  for (int i = ms(100); i < static_cast<int>(in.size()); ++i) {
    const float d = std::fabs(mixOut[at(i)] - in[at(i)]);
    maxMix = d > maxMix ? d : maxMix;
  }
  return report("passthrough", maxDiff == 0.0f && maxSilent == 0.0f && maxMix == 0.0f,
                "off max|out-in| %.3g, chasm on silence max|out| %.3g, mix 0 max|out-in| %.3g (all 0)",
                maxDiff, maxSilent, maxMix);
}

bool testDecay() {
  double tail[2];
  const float decays[2] = {0.0f, 1.0f};
  for (int k = 0; k < 2; ++k) {
    const Signal w = impulse(decays[k], 0.0f, ms(1500));
    tail[k] = db(std::pow(rmsOf(w, ms(1000), ms(1500)) / rmsOf(w, 0, ms(500)), 2.0));
  }
  return report("decay", tail[1] - tail[0] >= 15.0,
                "late/early RMS decay 0 %.1f dB, decay 1 %.1f dB, gap %.1f dB (>=15)", tail[0],
                tail[1], tail[1] - tail[0]);
}

bool testChirp() {
  const Signal w = impulse(0.5f, 0.0f, ms(60));
  const int lo = firstPeak(envelope(bandPass(w, 500.0, 4.0), 0.010), ms(60));
  const int hi = firstPeak(envelope(bandPass(w, 4000.0, 4.0), 0.010), ms(60));
  const double loMs = lo * 1000.0 / kSr, hiMs = hi * 1000.0 / kSr;
  return report("chirp-down", lo >= 0 && hi >= 0 && hiMs < loMs,
                "first peak 4 kHz %.2f ms, 500 Hz %.2f ms, lead %.2f ms (>0)", hiMs, loMs,
                loMs - hiMs);
}

// Energy in 900-1100 Hz, excluding 1 kHz +- 5 Hz.
double sideband(const std::vector<double>& p, int size) {
  double e = 0.0;
  for (size_t k = 0; k < p.size(); ++k) {
    const double hz = static_cast<double>(k) * kSr / size;
    if (hz >= 900.0 && hz < 1100.0 && std::fabs(hz - 1000.0) > 5.0) e += p[k];
  }
  return e;
}

bool testWobble() {
  constexpr int kSize = 32768;
  const Signal in = sine(2 * kSr, 1000.0, 0.25f);
  double side[2];
  const float wobbles[2] = {0.0f, 1.0f};
  for (int k = 0; k < 2; ++k) {
    const Signal w = wet(in, params(0.5f, wobbles[k]));
    side[k] = sideband(spectrum(w, kSr / 2, 2 * kSr, kSize), kSize);
  }
  return report("wobble", db(side[1] / side[0]) >= 10.0,
                "900-1100 Hz minus 1 kHz +-5 Hz: wobble 0 %.1f dB, wobble 1 %.1f dB, rise %.1f (>=10)",
                db(side[0]), db(side[1]), db(side[1] / side[0]));
}

bool testWobbleLevel() {
  const Signal in = noise(3 * kSr, 0.25f);
  const float wobbles[4] = {0.0f, 0.25f, 0.5f, 1.0f};
  double d[4];
  double ref = 0.0;
  bool ok = true;
  for (int k = 0; k < 4; ++k) {
    const double r = rmsOf(wet(in, params(0.5f, wobbles[k])), kSr / 2, 3 * kSr);
    if (k == 0) ref = r;
    d[k] = db(std::pow(r / ref, 2.0));
    ok = ok && std::fabs(d[k]) <= 1.5;
  }
  return report("wobble-level", ok,
                "noise wet RMS re wobble 0: wobble .25 %.2f dB, .5 %.2f dB, 1 %.2f dB (+-1.5)", d[1],
                d[2], d[3]);
}

bool testStable() {
  Signal in = noise(3 * kSr, 0.5f);
  in.resize(at(6 * kSr), 0.0f);
  const Signal w = wet(in, params(1.0f, 1.0f));
  bool finite = true;
  float peak = 0.0f;
  for (float v : w) {
    finite = finite && std::isfinite(v);
    peak = std::fabs(v) > peak ? std::fabs(v) : peak;
  }
  const double first = rmsOf(w, 3 * kSr, 3 * kSr + kSr / 2);
  const double last = rmsOf(w, 6 * kSr - kSr / 2, 6 * kSr);
  return report("stable", finite && peak < 4.0f && last < first,
                "peak |wet| %.3f (<4), silence RMS first 0.5 s %.3g, last 0.5 s %.3g", peak, first,
                last);
}

bool testNoAlloc() {
  const Signal in = noise(cv::kBlock * 8, 0.25f);
  cv::ReverbParams p;
  p.chasm = params(0.7f, 0.7f);
  gReverb.reset();
  run(gReverb, in, p);  // aborts on any new/delete inside process()
  p.engine = cv::kReverbChasm;
  run(gReverb, in, p);
  p.on = false;
  run(gReverb, Signal(at(cv::kBlock * 64), 0.0f), p);
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testDecay();
  ok &= testChirp();
  ok &= testWobble();
  ok &= testWobbleLevel();
  ok &= testStable();
  ok &= testNoAlloc();
  std::printf("sizeof(cv::Chasm) = %zu bytes, sizeof(cv::Reverb) = %zu bytes\n", sizeof(cv::Chasm),
              sizeof(cv::Reverb));
  return ok ? 0 : 1;
}
