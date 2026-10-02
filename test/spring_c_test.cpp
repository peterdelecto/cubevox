// Spring option C (PARKER) checks, handover §9.3 made executable: echo spacing,
// first-echo polarity, F_c, decay per pulse, tension range, band limit, three-spring
// detune, stability, Reverb passthrough and no-alloc. No window, no audio device.

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/reverb.h"
#include "engine/spring_c.h"

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
int ms(double m) { return static_cast<int>(m * kSr / 1000.0 + 0.5); }
double toMs(int samples) { return samples * 1000.0 / kSr; }

template <typename Engine, typename Params>
Signal run(Engine& s, const Signal& in, const Params& p) {
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

cv::SpringCParams params(float tension, float dwell) {
  cv::SpringCParams p;
  p.tension = tension;
  p.dwell = dwell;
  return p;
}

static cv::SpringC gSpring;

// Wet impulse response, modulation frozen; solo -1 runs tuning.springs.
Signal impulse(const cv::SpringCParams& p, int n, int solo) {
  Signal in(at(n), 0.0f);
  in[0] = 1.0f;
  gSpring.reset();
  gSpring.setModEnabled(false);
  gSpring.setSolo(solo);
  const Signal out = run(gSpring, in, p);
  gSpring.setModEnabled(true);
  gSpring.setSolo(-1);
  return out;
}

double rmsOf(const Signal& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[at(i)]) * x[at(i)];
  return std::sqrt(s / (to - from));
}

double db(double ratio) { return 10.0 * std::log10(ratio < 1e-30 ? 1e-30 : ratio); }

// Normalised autocorrelation of x[0, len) for lags [0, maxLag].
std::vector<double> autocorr(const Signal& x, int len, int maxLag) {
  std::vector<double> r(at(maxLag + 1), 0.0);
  for (int l = 0; l <= maxLag; ++l) {
    double s = 0.0;
    for (int i = 0; i + l < len; ++i) s += static_cast<double>(x[at(i)]) * x[at(i + l)];
    r[at(l)] = s;
  }
  const double r0 = r[0] > 0.0 ? r[0] : 1.0;
  for (double& v : r) v /= r0;
  return r;
}

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
            double c = 0.0, double d = 0.0, double e = 0.0) {
  char detail[240];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// Lag of the largest |ACF| in [lo, hi].
int acfPeak(const std::vector<double>& r, int lo, int hi) {
  int best = lo;
  for (int l = lo; l <= hi; ++l)
    if (std::fabs(r[at(l)]) > std::fabs(r[at(best)])) best = l;
  return best;
}

// 1. Echo spacing: |ACF| maximum away from zero lag. 20 ms clears the zero-lag
// lobe and the L/5 pre-echo. C_hf runs its own echo period, so it is muted here.
bool testEchoSpacing() {
  cv::SpringCParams p = params(0.5f, 0.0f);
  p.tuning.hfMixDb = -60.0f;
  const Signal ir = impulse(p, ms(300), 0);
  const std::vector<double> r = autocorr(ir, ms(300), ms(150));
  const int lag = acfPeak(r, ms(20), ms(150));
  const double m = toMs(lag);
  return report("echo-spacing", std::fabs(m - 56.0) <= 1.0,
                "|ACF| peak over 20-150 ms at %.2f ms (56 +-1), value %.3f", m, r[at(lag)]);
}

// 2. First echo inverted against the direct path, judged by the normalised
// cross-correlation of the 0-40 ms window with the window one T_D later. Both
// windows are low-passed below 400 Hz (2-pole Butterworth) first, because the low
// band is the least dispersed and shows the loop sign whatever the chirp EQ or
// brightness. The unfiltered value and largest-sample signs are printed for reference.
Signal lowpass400(const Signal& x) {
  const double w = std::tan(kPi * 400.0 / kSr), k = std::sqrt(2.0);
  const double n = 1.0 / (1.0 + k * w + w * w);
  const double b0 = w * w * n, b1 = 2.0 * b0;
  const double a1 = 2.0 * (w * w - 1.0) * n, a2 = (1.0 - k * w + w * w) * n;
  Signal y(x.size());
  double z1 = 0.0, z2 = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double o = b0 * x[i] + z1;
    z1 = b1 * x[i] - a1 * o + z2;
    z2 = b0 * x[i] - a2 * o;
    y[i] = static_cast<float>(o);
  }
  return y;
}

double windowCorr(const Signal& ir, int td, int win) {
  double xc = 0.0, e0 = 0.0, e1 = 0.0;
  for (int i = 0; i < win; ++i) {
    xc += static_cast<double>(ir[at(i)]) * ir[at(i + td)];
    e0 += static_cast<double>(ir[at(i)]) * ir[at(i)];
    e1 += static_cast<double>(ir[at(i + td)]) * ir[at(i + td)];
  }
  return xc / std::sqrt(e0 * e1);
}

bool testPolarity() {
  const Signal ir = impulse(params(0.5f, 0.0f), ms(100), 0);
  const int td = ms(56), win = ms(40);
  const double corr = windowCorr(lowpass400(ir), td, win);
  const double raw = windowCorr(ir, td, win);
  int direct = 0, echo = ms(40);
  for (int i = 0; i < ms(10); ++i)
    if (std::fabs(ir[at(i)]) > std::fabs(ir[at(direct)])) direct = i;
  for (int i = ms(40); i < ms(70); ++i)
    if (std::fabs(ir[at(i)]) > std::fabs(ir[at(echo)])) echo = i;
  return report("first-echo-polarity", corr < -0.5,
                "corr(direct, +T_D) below 400 Hz %.3f (< -0.5); unfiltered %.3f; largest samples %.4f (direct), %.4f (echo)",
                corr, raw, ir[at(direct)], ir[at(echo)]);
}

// 3. F_c: frequency of the latest-arriving energy in the chain IR (2048-point
// STFT ridge, hop 32). The ridge maximum is the centroid of the bins within one
// hop of the latest arrival, searched below 8 kHz.
bool testFc() {
  constexpr int kLen = 8192, kHop = 32;
  Signal h(at(kLen + kFft), 0.0f);
  {
    cv::SpringCParams p = params(0.5f, 0.0f);
    Signal in(at(cv::kBlock), 0.0f), out(at(cv::kBlock));
    gSpring.reset();
    gSpring.setSolo(0);
    gSpring.process(in.data(), out.data(), cv::kBlock, p);  // load geometry
    gSpring.chainImpulse(0, &h[at(kFft / 2)], kLen);
    gSpring.setSolo(-1);
  }
  const int bins = static_cast<int>(8000.0 * kFft / kSr);
  std::vector<double> bestE(at(bins), 0.0);
  std::vector<int> arrival(at(bins), 0);
  std::vector<std::complex<double>> buf(kFft);
  for (int s = 0; s + kFft <= static_cast<int>(h.size()); s += kHop) {
    for (int i = 0; i < kFft; ++i)
      buf[at(i)] = (0.5 - 0.5 * std::cos(2.0 * kPi * i / kFft)) * h[at(s + i)];
    fft(buf);
    for (int k = 1; k < bins; ++k) {
      const double e = std::norm(buf[at(k)]);
      if (e > bestE[at(k)]) {
        bestE[at(k)] = e;
        arrival[at(k)] = s;  // frame start; the chain IR starts at kFft/2
      }
    }
  }
  int latest = 0;
  for (int k = 1; k < bins; ++k) latest = arrival[at(k)] > latest ? arrival[at(k)] : latest;
  double sum = 0.0;
  int count = 0;
  for (int k = 1; k < bins; ++k)
    if (arrival[at(k)] >= latest - kHop) {
      sum += k;
      ++count;
    }
  const double hz = sum / count * kSr / kFft;
  return report("fc", std::fabs(hz - 4275.0) <= 150.0,
                "latest arrival %.2f ms at %.0f Hz (4275 +-150), %.0f bins in the ridge top",
                toMs(latest), hz, count);
}

// 4. Schroeder EDC at k·T_D, k = 1..6, least-squares slope per pulse, at the
// tension where |g_lf|·gComp = 0.537 (10^(-5.4/20)).
bool testDecayPerPulse() {
  const cv::SpringCTuning t;
  const float x = (0.537f / t.gComp - t.gLo) / (t.gHi - t.gLo);
  const Signal ir = impulse(params(x, 0.0f), 3 * kSr, 0);
  std::vector<double> edc(ir.size() + 1, 0.0);
  for (size_t i = ir.size(); i-- > 0;) edc[i] = edc[i + 1] + static_cast<double>(ir[i]) * ir[i];
  double sk = 0.0, sy = 0.0, skk = 0.0, sky = 0.0;
  for (int k = 1; k <= 6; ++k) {
    const double y = db(edc[at(ms(56.0 * k))] / edc[0]);
    sk += k;
    sy += y;
    skk += k * k;
    sky += k * y;
  }
  const double slope = (6.0 * sky - sk * sy) / (6.0 * skk - sk * sk);
  return report("decay-per-pulse", std::fabs(slope + 5.4) <= 1.0,
                "tension %.3f (|g|*comp 0.537): EDC slope %.2f dB/pulse (-5.4 +-1.0)", x, slope);
}

// 5. Tension range: late/early RMS gap between tension 0 and 1.
bool testTension() {
  double tail[2];
  const float tensions[2] = {0.0f, 1.0f};
  for (int k = 0; k < 2; ++k) {
    const Signal wet = impulse(params(tensions[k], 0.0f), ms(1500), 0);
    tail[k] = db(std::pow(rmsOf(wet, ms(1000), ms(1500)) / rmsOf(wet, 0, ms(500)), 2.0));
  }
  return report("tension", tail[1] - tail[0] >= 20.0,
                "late/early RMS tension 0 %.1f dB, tension 1 %.1f dB, gap %.1f dB (>=20)", tail[0],
                tail[1], tail[1] - tail[0]);
}

// 6. Band limit: white noise, dwell 0, C_hf muted.
bool testBandLimit() {
  cv::SpringCParams p = params(0.5f, 0.0f);
  p.tuning.hfMixDb = -120.0f;
  gSpring.reset();
  const Signal wet = run(gSpring, noise(2 * kSr, 0.25f), p);
  const std::vector<double> s = spectrum(wet, kSr / 2, 2 * kSr);
  const double gap = db(bandEnergy(s, 300.0, 4000.0) / bandEnergy(s, 7000.0, kSr / 2.0));
  return report("band-limit", gap >= 45.0,
                "300 Hz-4 kHz over >7 kHz = %.1f dB (>=45)", gap);
}

// 7. Three springs: distinct |ACF| local maxima near each T_D in 45-65 ms.
bool testDetune() {
  cv::SpringCParams p = params(0.5f, 0.0f);
  p.tuning.springs = 3;
  const Signal ir = impulse(p, ms(300), -1);
  const std::vector<double> r = autocorr(ir, ms(300), ms(70));
  std::vector<int> peaks;
  double top = 0.0;
  for (int l = ms(45); l <= ms(65); ++l) {
    const double v = std::fabs(r[at(l)]);
    if (v >= std::fabs(r[at(l - 1)]) && v > std::fabs(r[at(l + 1)])) {
      peaks.push_back(l);
      top = v > top ? v : top;
    }
  }
  double found[3];
  bool ok = true;
  for (int j = 0; j < 3; ++j) {
    const double target = p.tuning.tdMs * p.tuning.tdFactor[at(j)];
    int best = -1;
    for (int l : peaks)
      if (std::fabs(toMs(l) - target) <= 1.0 &&
          (best < 0 || std::fabs(r[at(l)]) > std::fabs(r[at(best)])))
        best = l;
    // Distinct: a local maximum within 12 dB of the window's largest.
    const bool hit = best >= 0 && std::fabs(r[at(best)]) >= 0.25 * top;
    found[j] = hit ? toMs(best) : -1.0;
    ok = ok && hit;
  }
  return report("three-springs", ok && found[0] != found[1] && found[1] != found[2],
                "|ACF| peaks at %.2f / %.2f / %.2f ms (tdMs * tdFactor[2] / [0] / [1], +-1), %.0f maxima",
                found[2], found[0], found[1], static_cast<double>(peaks.size()));
}

// 8. Stable at the extremes, decaying after the input stops.
bool testStable() {
  Signal in = noise(3 * kSr, 0.5f);
  in.resize(at(6 * kSr), 0.0f);
  cv::SpringCParams p = params(1.0f, 1.0f);
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

static cv::Reverb gReverb;

// 9. Reverb engine PARKER: on=false is a bit-exact copy; process() never allocates.
bool testReverb() {
  const Signal in = noise(kSr / 2, 0.25f);
  cv::ReverbParams p;
  p.engine = cv::kReverbParker;
  p.on = true;
  p.parker.tuning.springs = 3;
  gReverb.reset();
  run(gReverb, in, p);  // aborts on any new/delete inside process()
  p.on = false;
  gReverb.reset();
  const Signal out = run(gReverb, in, p);
  float maxDiff = 0.0f;
  for (int i = ms(100); i < static_cast<int>(in.size()); ++i) {
    const float d = std::fabs(out[at(i)] - in[at(i)]);
    maxDiff = d > maxDiff ? d : maxDiff;
  }
  return report("reverb-passthrough", maxDiff == 0.0f,
                "engine PARKER on=false max|out-in| %.3g after 100 ms (==0), no allocation",
                maxDiff);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testEchoSpacing();
  ok &= testPolarity();
  ok &= testFc();
  ok &= testDecayPerPulse();
  ok &= testTension();
  ok &= testBandLimit();
  ok &= testDetune();
  ok &= testStable();
  ok &= testReverb();
  std::printf("sizeof(cv::SpringC) = %zu bytes\n", sizeof(cv::SpringC));
  return ok ? 0 : 1;
}
