// Formant card checks: settled off is a plain copy, MIX 0 leaves the input untouched, SHIFT 0 keeps
// the spectrum's centroid, a SHIFT step moves the spectral envelope's first peak by 2^(st / 12), the
// sung pitch holds with Autotune off and on, the formant voice lines up with Autotune's corrected
// voice, a quick off and on keeps the running voice, SHIFT and MIX steps hold steady-state curvature,
// the equal-power MIX law, the trim, any call length, and no allocation. No window, no audio device.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
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
constexpr int kRate = cv::kSampleRate;
constexpr double kSungHz = 150.0;                       // a whole 320 samples a period
constexpr double kTunedHz = 146.8323839587038;          // D3, the C major note nearest 150 Hz
constexpr double kBumpHz = 1100.0;                      // the vowel's spectral peak
constexpr double kBumpWidthHz = 280.0;

using Signal = std::vector<float>;

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0) {
  char detail[240];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

double cents(double ratio) { return 1200.0 * std::log2(ratio); }

// Signals.

// A sawtooth at hz, harmonics at 1/k up to 8 kHz, peak-normalised to 0.5.
Signal sawtooth(double hz, int n) {
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; hz * k < 8000.0; ++k) s += std::sin(2.0 * kPi * hz * k * i / kRate) / k;
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  Signal out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

// Flat harmonics under a Gaussian bump at kBumpHz, so a formant move shifts the bump and the log of
// the harmonic amplitudes is a parabola around its peak. The waveform starts `offset` samples in, so
// the grain grid meets it at a different phase.
Signal vowel(double hz, int n, int offset = 0) {
  std::vector<double> amp;
  for (int k = 1; hz * k < 4000.0; ++k) {
    const double z = (hz * k - kBumpHz) / kBumpWidthHz;
    amp.push_back(std::exp(-0.5 * z * z));
  }
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (size_t k = 0; k < amp.size(); ++k) {
      s += amp[k] * std::sin(2.0 * kPi * hz * static_cast<double>(k + 1) * (i + offset) / kRate);
    }
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  Signal out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

// The vowel under a random piecewise-linear envelope (a new level every 20 ms, 0.3 to 1), so a
// cross-correlation has one peak where a steady tone has one per period.
Signal irregular(double hz, int n) {
  constexpr int kStep = kRate / 50;
  Signal x = vowel(hz, n);
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  double a = 0.6;
  double b = 0.6;
  for (int i = 0; i < n; ++i) {
    if (i % kStep == 0) {
      a = b;
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      b = 0.3 + 0.7 * static_cast<double>(s >> 40) / 16777216.0;
    }
    const double f = static_cast<double>(i % kStep) / kStep;
    x[static_cast<size_t>(i)] *= static_cast<float>(a + (b - a) * f);
  }
  return x;
}

// level_test's program: a 180 Hz tone of ten harmonics at 1/k with 5 Hz vibrato of +-20 cents under a
// 2 Hz raised-cosine phrase envelope, plus pink-ish noise at -40 dBFS, the whole at -18 dBFS RMS.
Signal program(int seconds) {
  const size_t n = static_cast<size_t>(seconds) * kRate;
  std::vector<double> tone(n);
  std::vector<double> noise(n);
  double phase = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double hz = 180.0 * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0);
    phase += 2.0 * kPi * hz / kRate;
    double v = 0.0;
    for (int k = 1; k <= 10; ++k) v += std::sin(k * phase) / k;
    tone[i] = v * 0.5 * (1.0 - std::cos(2.0 * kPi * 2.0 * t));
  }
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  double b0 = 0.0;
  double b1 = 0.0;
  double b2 = 0.0;
  for (size_t i = 0; i < n; ++i) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const double w = static_cast<double>(s >> 11) / 4503599627370496.0 - 1.0;
    b0 = 0.99765 * b0 + w * 0.0990460;
    b1 = 0.96300 * b1 + w * 0.2965164;
    b2 = 0.57000 * b2 + w * 1.0526913;
    noise[i] = b0 + b1 + b2 + w * 0.1848;
  }
  auto rmsOf = [n](const std::vector<double>& x) {
    double e = 0.0;
    for (double v : x) e += v * v;
    return std::sqrt(e / static_cast<double>(n));
  };
  const double target = std::pow(10.0, -18.0 / 20.0);
  const double tg = target / rmsOf(tone);
  const double ng = std::pow(10.0, -40.0 / 20.0) / rmsOf(noise);
  std::vector<double> sum(n);
  for (size_t i = 0; i < n; ++i) sum[i] = tone[i] * tg + noise[i] * ng;
  const double g = target / rmsOf(sum);
  Signal x(n);
  for (size_t i = 0; i < n; ++i) x[i] = static_cast<float>(sum[i] * g);
  return x;
}

// A pure 220 Hz sine, whose second difference is small and steady, so any cut stands out.
Signal sine(int n) {
  Signal x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 220.0 * i / kRate));
  return x;
}

// Runs.

static cv::PitchFx gFx;

// The card on at unity trim, so the checks read the voice itself; testDefaultLevel reads the shipped trim.
cv::PitchFxParams formantOn(int shiftSt, float mix = 1.0f) {
  cv::PitchFxParams p;
  p.formant.on = true;
  p.formant.shiftSt = shiftSt;
  p.formant.mix = mix;
  p.formant.tuning.trimDb = 0.0f;
  return p;
}

// The card off, with the same unity trim as formantOn, so a toggle changes nothing else.
cv::PitchFxParams formantOff() {
  cv::PitchFxParams p;
  p.formant.tuning.trimDb = 0.0f;
  return p;
}

// Autotune hard-tuned to C major.
void autotuneOn(cv::PitchFxParams& p) {
  p.autotune.on = true;
  p.autotune.key = 0;
  p.autotune.responseMs = 1.0f;
}

// Runs `in` through a fresh PitchFx in calls of `block` samples, with `a` before sample `stepAt` and
// `b` from there on (a block edge).
Signal runSteps(const Signal& in, const cv::PitchFxParams& a, const cv::PitchFxParams& b, int stepAt,
                int block = cv::kBlock) {
  gFx.reset();
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total;) {
    const int n = std::min(block, total - i);
    gInProcess = true;
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, i < stepAt ? a : b);
    gInProcess = false;
    i += n;
  }
  return out;
}

Signal run(const Signal& in, const cv::PitchFxParams& p, int block = cv::kBlock) {
  return runSteps(in, p, p, 0, block);
}

// Measures.

double rms(const Signal& x, int from, int to) {
  double e = 0.0;
  for (int i = from; i < to; ++i) e += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(e / (to - from));
}

double db(double ratio) { return 20.0 * std::log10(ratio); }

constexpr int kWindow = 8192;  // 171 ms: bins 5.9 Hz apart, 25 per harmonic of 150 Hz

// The magnitude of the Hann-windowed DFT of x[from, from + kWindow) at its largest within a quarter
// bin grid either side of hz, so a pitch a few cents off the harmonic still reads its peak.
double peakMag(const Signal& x, int from, double hz) {
  static const std::vector<double> window = [] {
    std::vector<double> w(kWindow);
    for (int i = 0; i < kWindow; ++i) w[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kWindow);
    return w;
  }();
  const double bin = static_cast<double>(kRate) / kWindow;
  double best = 0.0;
  for (int j = -24; j <= 24; ++j) {
    const double f = hz + 0.25 * bin * j;
    const std::complex<double> step = std::polar(1.0, -2.0 * kPi * f / kRate);
    std::complex<double> rot = 1.0;
    std::complex<double> sum = 0.0;
    for (size_t i = 0; i < window.size(); ++i) {
      sum += window[i] * x[static_cast<size_t>(from) + i] * rot;
      rot *= step;
    }
    best = std::max(best, std::abs(sum));
  }
  return best;
}

// Magnitudes of harmonics 1..count of f0.
std::vector<double> harmonics(const Signal& x, int from, double f0, int count) {
  std::vector<double> m;
  for (int k = 1; k <= count; ++k) m.push_back(peakMag(x, from, f0 * k));
  return m;
}

// Amplitude-weighted mean frequency of the harmonics.
double centroidHz(const std::vector<double>& m, double f0) {
  double num = 0.0;
  double den = 0.0;
  for (size_t k = 0; k < m.size(); ++k) {
    num += f0 * static_cast<double>(k + 1) * m[k];
    den += m[k];
  }
  return num / den;
}

// The frequency of the envelope's peak. The envelope is one bump, so the harmonic magnitudes are fit
// with a Gaussian: a least-squares parabola through their logs in kHz, each point weighted by its
// power, and the peak is the parabola's vertex. Three harmonics around the largest would sample a
// bump 150 Hz apart and read the comb's ripple, not the bump.
double envelopePeakHz(const std::vector<double>& m, double f0) {
  double s[5] = {0.0, 0.0, 0.0, 0.0, 0.0};  // sums of w f^0 .. w f^4
  double t[3] = {0.0, 0.0, 0.0};            // sums of w y f^0 .. w y f^2
  for (size_t k = 0; k < m.size(); ++k) {
    if (m[k] < 1e-3) continue;
    const double f = f0 * static_cast<double>(k + 1) * 0.001;
    const double y = std::log(m[k]);
    const double w = m[k] * m[k];
    double fp = 1.0;
    for (int j = 0; j < 5; ++j) {
      s[j] += w * fp;
      if (j < 3) t[j] += w * y * fp;
      fp *= f;
    }
  }
  const double det = s[0] * (s[2] * s[4] - s[3] * s[3]) - s[1] * (s[1] * s[4] - s[3] * s[2]) + s[2] * (s[1] * s[3] - s[2] * s[2]);
  const double c1 = (s[0] * (t[1] * s[4] - s[3] * t[2]) - t[0] * (s[1] * s[4] - s[3] * s[2]) + s[2] * (s[1] * t[2] - t[1] * s[2])) / det;
  const double c2 = (s[0] * (s[2] * t[2] - t[1] * s[3]) - s[1] * (s[1] * t[2] - t[1] * s[2]) + t[0] * (s[1] * s[3] - s[2] * s[2])) / det;
  return -c1 / (2.0 * c2) * 1000.0;
}

// The period of a steady periodic signal near `nominal` samples: the lag of the best autocorrelation
// peak near ten periods out, refined by a parabola and divided by ten.
double periodOf(const Signal& x, int from, double nominal) {
  constexpr int kPeriods = 10;
  constexpr int kLen = 8000;
  const int centre = static_cast<int>(std::lround(nominal * kPeriods));
  const int reach = static_cast<int>(nominal * kPeriods * 0.01);
  std::vector<double> r;
  for (int lag = centre - reach - 1; lag <= centre + reach + 1; ++lag) {
    double s = 0.0;
    for (int i = 0; i < kLen; ++i) s += static_cast<double>(x[static_cast<size_t>(from + i)]) * x[static_cast<size_t>(from + i + lag)];
    r.push_back(s);
  }
  size_t top = 1;
  for (size_t k = 1; k + 1 < r.size(); ++k) {
    if (r[k] > r[top]) top = k;
  }
  const double d = r[top - 1] - 2.0 * r[top] + r[top + 1];
  const double off = d < 0.0 ? 0.5 * (r[top - 1] - r[top + 1]) / d : 0.0;
  return (static_cast<double>(centre - reach - 1) + static_cast<double>(top) + off) / kPeriods;
}

// The lag in [-reach, reach] at which b best matches a over [from, from + len): b[i + lag] ~ a[i].
int bestLag(const Signal& a, const Signal& b, int from, int len, int reach) {
  int best = 0;
  double bestSum = -1e300;
  for (int lag = -reach; lag <= reach; ++lag) {
    double s = 0.0;
    for (int i = from; i < from + len; ++i) s += static_cast<double>(a[static_cast<size_t>(i)]) * b[static_cast<size_t>(i + lag)];
    if (s > bestSum) {
      bestSum = s;
      best = lag;
    }
  }
  return best;
}

// Largest |second difference| of x over [from, to).
double peakCurvature(const Signal& x, int from, int to) {
  double worst = 0.0;
  for (int i = std::max(from, 2); i < to; ++i) {
    const size_t k = static_cast<size_t>(i);
    worst = std::max(worst, std::fabs(static_cast<double>(x[k]) - 2.0 * x[k - 1] + x[k - 2]));
  }
  return worst;
}

// Checks.

// Switched off from the start, and switched off after a spell on and settled, the stage copies its
// input bit for bit. MIX 0 does the same while the card is on.
bool testOffExact() {
  const Signal in = vowel(kSungHz, 2 * kRate);
  const Signal fresh = run(in, cv::PitchFxParams{});
  size_t differs = 0;
  for (size_t i = 0; i < in.size(); ++i) differs += fresh[i] != in[i] ? 1u : 0u;
  bool ok = report("off exact, fresh", differs == 0, "%.0f of %.0f samples differ (0)", static_cast<double>(differs),
                   static_cast<double>(in.size()));

  const Signal long3 = vowel(kSungHz, 3 * kRate);
  const Signal after = runSteps(long3, formantOn(-3), formantOff(), kRate);
  const bool settled = gFx.formantFade() == 0.0f;
  differs = 0;
  for (int i = 2 * kRate; i < 3 * kRate; ++i) differs += after[static_cast<size_t>(i)] != long3[static_cast<size_t>(i)] ? 1u : 0u;
  ok &= report("off exact, after on", settled && differs == 0,
               "crossfade settled at 0: %.0f (1); %.0f of the last second's samples differ (0)", settled ? 1.0 : 0.0,
               static_cast<double>(differs));

  const Signal mixZero = run(in, formantOn(-7, 0.0f));
  differs = 0;
  for (size_t i = 0; i < in.size(); ++i) differs += mixZero[i] != in[i] ? 1u : 0u;
  ok &= report("mix 0 exact", differs == 0, "%.0f of %.0f samples differ (0)", static_cast<double>(differs),
               static_cast<double>(in.size()));
  return ok;
}

// SHIFT 0 at MIX 100 % hands back the input's spectrum.
bool testShiftZero() {
  const Signal in = sawtooth(kSungHz, 3 * kRate);
  const Signal out = run(in, formantOn(0));
  const int from = 2 * kRate;
  const double cIn = centroidHz(harmonics(in, from, kSungHz, 26), kSungHz);
  const double cOut = centroidHz(harmonics(out, from, kSungHz, 26), kSungHz);
  const double err = cOut / cIn - 1.0;
  return report("shift 0 centroid", std::fabs(err) <= 0.02, "centroid %.1f Hz against the input's %.1f Hz, off by %.3f %% (+-2)",
                cOut, cIn, 100.0 * err);
}

// A SHIFT of st moves the envelope's first peak by 2^(st / 12). The grain grid sits on the period
// grid at no fixed place against the waveform, and two input harmonics that land on one output
// harmonic add with a phase that follows that place. The harmonic powers are therefore averaged over
// eight placements, the waveform started 40 samples apart over its 320 sample period.
bool testEnvelope() {
  constexpr int kPlacements = 8;
  constexpr int kHarmonics = 26;
  const int from = 2 * kRate;
  const Signal inRef = vowel(kSungHz, 3 * kRate);
  const double inPeak = envelopePeakHz(harmonics(inRef, from, kSungHz, kHarmonics), kSungHz);
  bool ok = true;
  for (int st : {-7, -3, 3, 7}) {
    std::vector<double> power(kHarmonics, 0.0);
    for (int p = 0; p < kPlacements; ++p) {
      const Signal out = run(vowel(kSungHz, 3 * kRate, p * 40), formantOn(st));
      const std::vector<double> m = harmonics(out, from, kSungHz, kHarmonics);
      for (size_t k = 0; k < m.size(); ++k) power[k] += m[k] * m[k] / kPlacements;
    }
    for (double& v : power) v = std::sqrt(v);
    const double peak = envelopePeakHz(power, kSungHz);
    const double want = std::exp2(st / 12.0);
    const double got = peak / inPeak;
    char name[40];
    std::snprintf(name, sizeof name, "envelope peak, %+d st", st);
    ok &= report(name, std::fabs(got / want - 1.0) <= 0.05,
                 "peak %.0f Hz from %.0f Hz is x%.4f against x%.4f (+-5 %%)", peak, inPeak, got, want);
  }
  return ok;
}

// The output pitch holds at every SHIFT: on the input's with Autotune off, on the tuned note with it on.
bool testPitch() {
  const Signal in = vowel(kSungHz, 3 * kRate);
  const int from = kRate + kRate / 2;
  bool ok = true;
  for (int pass = 0; pass < 2; ++pass) {
    const double wantHz = pass == 0 ? kSungHz : kTunedHz;
    const double wantPeriod = kRate / wantHz;
    double worst = 0.0;
    int worstSt = 0;
    for (int st = -7; st <= 7; ++st) {
      cv::PitchFxParams p = formantOn(st);
      if (pass == 1) autotuneOn(p);
      const Signal out = run(in, p);
      const double err = std::fabs(cents(wantPeriod / periodOf(out, from, wantPeriod)));
      if (err > worst) {
        worst = err;
        worstSt = st;
      }
    }
    ok &= report(pass == 0 ? "pitch holds, autotune off" : "pitch holds, autotune on", worst <= 3.0,
                 "worst %.2f cents (+-3) at SHIFT %+.0f st, against %.2f Hz", worst, static_cast<double>(worstSt), wantHz);
  }
  return ok;
}

// With Autotune on and SHIFT 0 the two voices are the same sample. With Autotune off the formant
// voice lags the input by Autotune's voice delay, the grain delay plus one period.
bool testAlignment() {
  const Signal in = irregular(kSungHz, 4 * kRate);
  const int from = 2 * kRate;
  cv::PitchFxParams tuned;
  autotuneOn(tuned);
  cv::PitchFxParams both = formantOn(0);
  autotuneOn(both);
  const Signal a = run(in, tuned);
  const Signal f = run(in, both);
  const int lag = bestLag(a, f, from, kRate, 8);
  bool ok = report("aligned with autotune", std::abs(lag) <= 1, "cross-correlation peaks at lag %.0f samples (0, +-1)",
                   static_cast<double>(lag));

  const Signal plain = run(in, formantOn(0));
  const double want = static_cast<double>(cv::PsolaVoice::kGrainDelay) + kRate / kSungHz;
  const int reach = static_cast<int>(want) + 8;
  const int dry = bestLag(in, plain, from, kRate, reach);
  ok &= report("lag behind the input", std::fabs(dry - want) <= 1.5,
               "peaks at lag %.0f samples against grain delay + period %.1f (+-1.5)", static_cast<double>(dry), want);
  return ok;
}

// A quick off and on keeps the running voice: once the crossfade is back at 1 the output equals the
// run that never stopped, bit for bit.
bool testQuickToggleKeepsVoice() {
  const Signal in = irregular(kSungHz, 4 * kRate);
  const cv::PitchFxParams on = formantOn(-3);
  const Signal steady = run(in, on);
  gFx.reset();
  Signal out(in.size());
  const int offAt = kRate;
  const int onAt = kRate + kRate / 33;  // 30 ms off: the crossfade falls to a fifth, not to 0
  for (int i = 0; i < static_cast<int>(in.size()); i += cv::kBlock) {
    const size_t k = static_cast<size_t>(i);
    gFx.process(&in[k], &out[k], cv::kBlock, i >= offAt && i < onAt ? formantOff() : on);
  }
  size_t differs = 0;
  for (int i = 3 * kRate; i < 4 * kRate; ++i) differs += out[static_cast<size_t>(i)] != steady[static_cast<size_t>(i)] ? 1u : 0u;
  return report("quick toggle keeps the voice", differs == 0,
                "%.0f of the last second's samples differ from the run that never stopped (0)", static_cast<double>(differs));
}

// Steady-state curvature across a step: the peak second difference in 100 ms after the step against the
// larger peak of the two settled settings, as knob_step_test measures it.
double stepRatio(const Signal& in, const cv::PitchFxParams& a, const cv::PitchFxParams& b) {
  const int stepAt = 2 * kRate;
  const int total = 4 * kRate;
  const Signal out = runSteps(in, a, b, stepAt);
  const double steady = std::max(peakCurvature(out, stepAt - kRate / 2, stepAt), peakCurvature(out, total - kRate / 2, total));
  const double step = peakCurvature(out, stepAt, stepAt + kRate / 10);
  return steady > 0.0 ? step / steady : (step > 0.0 ? 1e9 : 0.0);
}

bool testShiftSteps() {
  const Signal in = sine(4 * kRate);
  constexpr double kMax = 1.05;
  double worst = 0.0;
  int worstSt = 0;
  for (int st = -7; st < 7; ++st) {
    const double r = stepRatio(in, formantOn(st), formantOn(st + 1));
    if (r >= worst) {
      worst = r;
      worstSt = st;
    }
  }
  bool ok = report("shift steps", worst <= kMax, "worst step %+.0f to %+.0f st: peak curvature x%.4f of steady state (<= 1.05)",
                   static_cast<double>(worstSt), static_cast<double>(worstSt + 1), worst);
  double worstMix = 0.0;
  for (float m : {0.2f, 0.5f, 0.8f}) {
    worstMix = std::max(worstMix, stepRatio(in, formantOn(-3, m), formantOn(-3, m + 0.015f)));
  }
  ok &= report("mix steps", worstMix <= kMax, "worst 1.5 %% step: peak curvature x%.4f of steady state (<= 1.05)", worstMix);
  return ok;
}

// The toggle's crossfade, in both directions, on the 220 Hz sine. The dry sine and the shifted voice
// are different signals, so the crossfade may pass above both settled peaks; the number is reported
// next to the knob steps.
bool testToggleSteps() {
  const Signal in = sine(4 * kRate);
  const double up = stepRatio(in, formantOff(), formantOn(-3));
  const double down = stepRatio(in, formantOn(-3), formantOff());
  return report("toggle steps", up <= 1.05 && down <= 1.05,
                "peak curvature after the step: off to on x%.4f, on to off x%.4f of steady state (<= 1.05)", up, down);
}

// MIX is equal-power: the dry and shifted gains are cos and sin of the quarter turn. At SHIFT 0 the
// shifted voice is the input delayed by the grain delay plus one period. A 208 sample period (230.8 Hz)
// makes that delay 1248 samples, six whole periods, so the two voices are one waveform and the output
// level over the input's is cos + sin.
bool testMixLaw() {
  constexpr double kHz = static_cast<double>(kRate) / 208.0;
  const Signal in = vowel(kHz, 3 * kRate);
  const double inRms = rms(in, kRate, 3 * kRate);
  double worst = 0.0;
  for (float m : {0.25f, 0.5f, 0.75f}) {
    const double want = std::cos(m * kPi / 2.0) + std::sin(m * kPi / 2.0);
    worst = std::max(worst, std::fabs(db(rms(run(in, formantOn(0, m)), kRate, 3 * kRate) / inRms / want)));
  }
  return report("mix law", worst <= 0.1, "level over the input at MIX 25, 50, 75 %% is cos + sin to within %.3f dB (0.1)", worst);
}

// The shipped trim: SHIFT -3 at MIX 100 % lands 0 to +0.5 dB over the input on level_test's program,
// within the +-0.1 dB slack that test allows.
bool testDefaultLevel() {
  const Signal in = program(3);
  cv::PitchFxParams p;
  p.formant.on = true;
  const double got = db(rms(run(in, p), kRate, 3 * kRate) / rms(in, kRate, 3 * kRate));
  return report("default level", got >= -0.1 && got <= 0.6, "SHIFT -3, MIX 100 %% reads %+.2f dB over the input (-0.1..+0.6)", got);
}

// The tuning's trim scales the shifted voice by its dB.
bool testTrim() {
  const Signal in = irregular(kSungHz, 3 * kRate);
  cv::PitchFxParams loud = formantOn(-3);
  loud.formant.tuning.trimDb = 6.0f;
  const double a = rms(run(in, formantOn(-3)), kRate, 3 * kRate);
  const double b = rms(run(in, loud), kRate, 3 * kRate);
  return report("trim", std::fabs(db(b / a) - 6.0) <= 0.05, "trim +6 dB reads %.3f dB (+-0.05)", db(b / a));
}

// process() takes any n. At SHIFT 0 the voice is the input delayed, whatever the grain grid does, so
// calls of 1 to 4096 samples return the input's level; at SHIFT -3 they stay finite and in range.
bool testAnyLength() {
  const Signal in = irregular(kSungHz, 3 * kRate);
  const double inRms = rms(in, kRate, 3 * kRate);
  double worst = 0.0;
  bool sane = true;
  for (int block : {1, 7, 37, 64, 200, 1000, 4096}) {
    worst = std::max(worst, std::fabs(db(rms(run(in, formantOn(0), block), kRate, 3 * kRate) / inRms)));
    for (float x : run(in, formantOn(-3), block)) sane &= std::isfinite(x) && std::fabs(x) < 2.0f;
  }
  return report("any call length", sane && worst <= 0.1,
                "SHIFT 0 level within %.3f dB of the input's (0.1), SHIFT -3 finite and under 2: %.0f (1)", worst, sane ? 1.0 : 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testOffExact();
  ok &= testShiftZero();
  ok &= testEnvelope();
  ok &= testPitch();
  ok &= testAlignment();
  ok &= testQuickToggleKeepsVoice();
  ok &= testShiftSteps();
  ok &= testToggleSteps();
  ok &= testMixLaw();
  ok &= testDefaultLevel();
  ok &= testTrim();
  ok &= testAnyLength();
  std::printf("%s formant\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
