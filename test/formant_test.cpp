// Formant card checks: settled off is a plain copy, MIX 0 leaves the input untouched, SHIFT 0 keeps
// the spectrum's centroid, a SHIFT step moves the spectral envelope's first peak by 2^(st / 12), the
// sung pitch holds with Autotune off and on, the formant voice lines up with Autotune's corrected
// voice, a quick off and on keeps the running voice, MIX leaves 0 and 100 % without a click or a dip,
// the toggle holds steady-state curvature, the MIX level, the trim, any call length, and no
// allocation. The voice's silence skip returns the plain voice's samples, SHIFT starts at the knob
// when the card comes up, a voice that starts late takes the other's grain grid, the tracker's range
// flags put the card aside, and a NaN does not stay in the level gain. SHIFT and MIX steps are
// knob_step_test's, the default level is level_test's. No window, no audio device.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "engine/pitch_fx.h"

#include "alloc_guard.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = cv::kSampleRate;
constexpr int kShift = cv::kFormantDefaultShiftSt;  // the SHIFT the card starts on
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

// The harmonic amplitudes of a vowel at hz: a Gaussian bump at kBumpHz over flat harmonics.
std::vector<double> bumpAmps(double hz) {
  std::vector<double> amp;
  for (int k = 1; hz * k < 4000.0; ++k) {
    const double z = (hz * k - kBumpHz) / kBumpWidthHz;
    amp.push_back(std::exp(-0.5 * z * z));
  }
  return amp;
}

// The vowel's harmonics under that bump, so a formant move shifts the bump and the log of the
// harmonic amplitudes is a parabola around its peak. The waveform starts `offset` samples in, so
// the grain grid meets it at a different phase.
Signal vowel(double hz, int n, int offset = 0) {
  const std::vector<double> amp = bumpAmps(hz);
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

// x under a random piecewise-linear envelope (a new level every 20 ms, 0.3 to 1), so a
// cross-correlation has one peak where a steady tone has one per period.
Signal withEnvelope(Signal x) {
  constexpr int kStep = kRate / 50;
  const int n = static_cast<int>(x.size());
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

// The vowel under that envelope.
Signal irregular(double hz, int n) { return withEnvelope(vowel(hz, n)); }

// The vowel with 5 Hz vibrato of +-cents about hz, under that envelope. The harmonic weights stay those
// of the vowel at hz and the phase runs from the sung frequency.
Signal vibratoVowel(double hz, int n, double cents) {
  const std::vector<double> amp = bumpAmps(hz);
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double peak = 0.0;
  double phase = 0.0;
  for (int i = 0; i < n; ++i) {
    phase += 2.0 * kPi * hz * std::exp2(cents * std::sin(2.0 * kPi * 5.0 * i / kRate) / 1200.0) / kRate;
    double s = 0.0;
    for (size_t k = 0; k < amp.size(); ++k) s += amp[k] * std::sin(static_cast<double>(k + 1) * phase);
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  Signal out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return withEnvelope(out);
}

// level_test's pink-ish noise: white noise through a Kellet economy filter.
std::vector<double> pinkNoise(size_t n) {
  std::vector<double> noise(n);
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
  return noise;
}

// level_test's program: a 180 Hz tone of ten harmonics at 1/k with 5 Hz vibrato of +-20 cents under a
// 2 Hz raised-cosine phrase envelope, plus pink-ish noise at -40 dBFS, the whole at -18 dBFS RMS.
Signal program(int seconds) {
  const size_t n = static_cast<size_t>(seconds) * kRate;
  std::vector<double> tone(n);
  const std::vector<double> noise = pinkNoise(n);
  double phase = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double hz = 180.0 * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0);
    phase += 2.0 * kPi * hz / kRate;
    double v = 0.0;
    for (int k = 1; k <= 10; ++k) v += std::sin(k * phase) / k;
    tone[i] = v * 0.5 * (1.0 - std::cos(2.0 * kPi * 2.0 * t));
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

// A pure sine at hz, amplitude 0.25.
Signal tone(double hz, int n) {
  Signal x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<float>(0.25 * std::sin(2.0 * kPi * hz * i / kRate));
  return x;
}

// A pure 220 Hz sine, whose second difference is small and steady, so any cut stands out.
Signal sine(int n) { return tone(220.0, n); }

// Runs.

static cv::PitchFx gFx;

// The card on at unity trim, so the checks read the voice itself. level_test reads the shipped trim.
cv::PitchFxParams formantOn(int shiftSt, float mix = 1.0f) {
  cv::PitchFxParams p;
  p.formant.on = true;
  p.formant.shiftSt = shiftSt;
  p.formant.mix = mix;
  p.formant.tuning.trimDb.fill(0.0f);
  return p;
}

// The card off, with the same unity trim as formantOn, so a toggle changes nothing else.
cv::PitchFxParams formantOff() {
  cv::PitchFxParams p;
  p.formant.tuning.trimDb.fill(0.0f);
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

// bestLag refined by a parabola through the correlation at the best lag and its two neighbours.
double bestLagFine(const Signal& a, const Signal& b, int from, int len, int reach) {
  const int best = bestLag(a, b, from, len, reach);
  double r[3];
  for (int k = -1; k <= 1; ++k) {
    r[k + 1] = 0.0;
    for (int i = from; i < from + len; ++i) r[k + 1] += static_cast<double>(a[static_cast<size_t>(i)]) * b[static_cast<size_t>(i + best + k)];
  }
  const double d = r[0] - 2.0 * r[1] + r[2];
  return best + (d < 0.0 ? 0.5 * (r[0] - r[2]) / d : 0.0);
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
  const Signal after = runSteps(long3, formantOn(kShift), formantOff(), kRate);
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
  return report("shift 0 centroid", std::fabs(err) <= 0.001, "centroid %.1f Hz against the input's %.1f Hz, off by %.3f %% (+-0.1)",
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
    ok &= report(pass == 0 ? "pitch holds, autotune off" : "pitch holds, autotune on", worst <= 0.05,
                 "worst %.3f cents (+-0.05) at SHIFT %+.0f st, against %.2f Hz", worst, static_cast<double>(worstSt), wantHz);
  }
  return ok;
}

// The lag, within 8 samples, at which the Formant voice (SHIFT 0, MIX 100 %, Autotune on) best matches
// Autotune's corrected voice alone over the second after 2 s.
double lagToAutotune(const Signal& in) {
  cv::PitchFxParams tuned;
  autotuneOn(tuned);
  cv::PitchFxParams both = formantOn(0);
  autotuneOn(both);
  return bestLagFine(run(in, tuned), run(in, both), 2 * kRate, kRate, 8);
}

// With Autotune on and SHIFT 0 the two voices are the same sample. With Autotune off the formant
// voice lags the input by Autotune's voice delay, the grain delay plus one period.
bool testAlignment() {
  const Signal in = irregular(kSungHz, 4 * kRate);
  const int from = 2 * kRate;
  const double lag = lagToAutotune(in);
  bool ok = report("aligned with autotune", std::fabs(lag) <= 0.8, "cross-correlation peaks at lag %.2f samples (0, +-0.8)", lag);

  const Signal plain = run(in, formantOn(0));
  const double want = static_cast<double>(cv::PsolaVoice::kGrainDelay) + kRate / kSungHz;
  const int reach = static_cast<int>(want) + 8;
  const int dry = bestLag(in, plain, from, kRate, reach);
  ok &= report("lag behind the input", std::fabs(dry - want) <= 1.0,
               "peaks at lag %.0f samples against grain delay + period %.1f (+-1)", static_cast<double>(dry), want);
  return ok;
}

// A quick off and on keeps the running voice: once the crossfade is back at 1 the output equals the
// run that never stopped, bit for bit.
bool testQuickToggleKeepsVoice() {
  const Signal in = irregular(kSungHz, 4 * kRate);
  const cv::PitchFxParams on = formantOn(kShift);
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

// MIX 0 settles the stage off, so it sits out like a card that is switched off. Leaving it by a touch
// or a jump starts the voice clean: no click on a steady tone.
bool testMixLeavesZero() {
  const Signal in = sine(4 * kRate);
  double worst = 0.0;
  for (float m : {0.015f, 0.5f, 1.0f}) worst = std::max(worst, stepRatio(in, formantOn(kShift, 0.0f), formantOn(kShift, m)));
  return report("mix leaves 0", worst <= 1.01, "peak curvature after MIX 0 to 1.5, 50, 100 %%: x%.4f of steady state (<= 1.01)", worst);
}

// The windows of kPeriodSamples after the step, against the same length of steady output before it.
constexpr int kPeriodSamples = 320;  // one period of kSungHz

// With Autotune on and MIX at 100 % the corrected voice is multiplied by 0, so it sits out. MIX
// leaving 100 % brings it back clean. The level over the first 20 ms, in windows of one period,
// stays within a dB of the level before the step, at the default SHIFT and at 0, where the two
// voices are one waveform.
bool testMixLeavesFull() {
  const Signal in = vowel(kSungHz, 4 * kRate);
  const int stepAt = 2 * kRate;
  double worst = 0.0;
  for (int shift : {kShift, 0}) {
    for (float m : {0.9f, 0.5f}) {
      cv::PitchFxParams a = formantOn(shift, 1.0f);
      cv::PitchFxParams b = formantOn(shift, m);
      autotuneOn(a);
      autotuneOn(b);
      const Signal out = runSteps(in, a, b, stepAt);
      const double ref = rms(out, stepAt - 10 * kPeriodSamples, stepAt);
      for (int w = 0; w < 3; ++w) {
        const int from = stepAt + w * kPeriodSamples;
        worst = std::max(worst, std::fabs(db(rms(out, from, from + kPeriodSamples) / ref)));
      }
    }
  }
  return report("mix leaves 100 %", worst <= 1.0, "level of the first 20 ms after MIX 100 to 90 and 50 %% is within %.2f dB of before (1)", worst);
}

// The toggle's crossfade, in both directions, on the 220 Hz sine. The dry sine and the shifted voice
// are different signals, so the crossfade may pass above both settled peaks; the number is reported
// next to the knob steps.
bool testToggleSteps() {
  const Signal in = sine(4 * kRate);
  const double up = stepRatio(in, formantOff(), formantOn(kShift));
  const double down = stepRatio(in, formantOn(kShift), formantOff());
  return report("toggle steps", up <= 1.01 && down <= 1.01,
                "peak curvature after the step: off to on x%.4f, on to off x%.4f of steady state (<= 1.01)", up, down);
}

// MIX holds the input's level whether the two voices are one waveform or unrelated. At SHIFT 0 with
// Autotune off the shifted voice is the input delayed by the grain delay plus one period.
// 1. A 208 sample period (230.8 Hz) makes that delay 1248 samples, six whole periods, so the voices are
//    one waveform and the linear gains give (1 - m) + m = 1 of the input's level.
// 2. A 150 Hz sine makes the delay 1360 samples, 4.25 cycles, so the voices sit a quarter cycle apart and
//    add in power. Linear gains alone read 1/sqrt(2) at MIX 50 %; the level gain restores 1.
bool testMixLaw() {
  constexpr double kHz = static_cast<double>(kRate) / 208.0;
  bool ok = true;
  for (int pass = 0; pass < 2; ++pass) {
    const Signal in = pass == 0 ? vowel(kHz, 3 * kRate) : tone(kSungHz, 3 * kRate);
    const double inRms = rms(in, kRate, 3 * kRate);
    double worst = 0.0;
    for (float m : {0.25f, 0.5f, 0.75f}) {
      worst = std::max(worst, std::fabs(db(rms(run(in, formantOn(0, m)), kRate, 3 * kRate) / inRms)));
    }
    ok &= report(pass == 0 ? "mix level, one waveform" : "mix level, quarter cycle apart", worst <= 0.02,
                 "level over the input at MIX 25, 50, 75 %% is within %.3f dB of 0 dB (0.02)", worst);
  }
  return ok;
}

// On the program at SHIFT -3 with the shipped trim, MIX 25, 50 and 75 % read within 1 dB of MIX 100 %,
// Autotune off and on. The voices sit anywhere from correlated to unrelated here, so a fixed linear or
// equal-power law leaves the middle of the knob 1 to 3 dB off.
bool testMixHoldsLevel() {
  const Signal in = program(3);
  bool ok = true;
  for (int pass = 0; pass < 2; ++pass) {
    auto level = [&](float m) {
      cv::PitchFxParams p;
      p.formant.on = true;
      p.formant.mix = m;
      if (pass == 1) autotuneOn(p);
      return rms(run(in, p), kRate, 3 * kRate);
    };
    const double top = level(1.0f);
    double worst = 0.0;
    for (float m : {0.25f, 0.5f, 0.75f}) worst = std::max(worst, std::fabs(db(level(m) / top)));
    ok &= report(pass == 0 ? "mix holds level, autotune off" : "mix holds level, autotune on", worst <= 1.0,
                 "MIX 25, 50, 75 %% read within %.2f dB of MIX 100 %% on the program at SHIFT -3 (1)", worst);
  }
  return ok;
}

// The tuning's trim scales the shifted voice by its dB.
bool testTrim() {
  const Signal in = irregular(kSungHz, 3 * kRate);
  cv::PitchFxParams loud = formantOn(kShift);
  loud.formant.tuning.trimDb.fill(6.0f);
  const double a = rms(run(in, formantOn(kShift)), kRate, 3 * kRate);
  const double b = rms(run(in, loud), kRate, 3 * kRate);
  return report("trim", std::fabs(db(b / a) - 6.0) <= 0.01, "trim +6 dB reads %.3f dB (+-0.01)", db(b / a));
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
    for (float x : run(in, formantOn(kShift), block)) sane &= std::isfinite(x) && std::fabs(x) < 2.0f;
  }
  return report("any call length", sane && worst <= 0.06,
                "SHIFT 0 level within %.3f dB of the input's (0.06), SHIFT -3 finite and under 2: %.0f (1)", worst, sane ? 1.0 : 0.0);
}

// The Formant voice told how many exact zeros sit ahead of each chunk returns, bit for bit, what the
// voice that plays every tap returns. `heard` is what the ring receives and `told` is what the zero
// count sees; they are one signal unless a check makes the caller lie. Returns how many output
// samples differ.
size_t skipDiffers(const Signal& heard, const Signal& told, float ratio, float rate) {
  cv::VoiceRing ring{};
  cv::formant_detail::FormantPsolaVoice skip;
  cv::formant_detail::FormantPsolaVoice plain;
  skip.reset();
  plain.reset();
  const float period = static_cast<float>(kRate / kSungHz);
  const float delay = static_cast<float>(cv::PsolaVoice::kGrainDelay) + period;
  int head = 0;
  int zeros = cv::kVoiceRingLen;
  size_t differs = 0;
  for (size_t i = 0; i + cv::kBlock <= heard.size(); i += cv::kBlock) {
    int at = head;
    for (int j = 0; j < cv::kBlock; ++j) {
      ring[static_cast<size_t>(at)] = heard[i + static_cast<size_t>(j)];
      at = at + 1 == cv::kVoiceRingLen ? 0 : at + 1;
    }
    skip.prepare(period, ratio, rate, delay, cv::kBlock);
    plain.prepare(period, ratio, rate, delay, cv::kBlock);
    std::array<float, cv::kBlock> a{};
    std::array<float, cv::kBlock> b{};
    skip.render(ring, head, cv::kBlock, a.data(), zeros);
    plain.render(ring, head, cv::kBlock, b.data());
    for (size_t j = 0; j < a.size(); ++j) differs += a[j] != b[j] ? 1u : 0u;
    int run = 0;
    while (run < cv::kBlock && told[i + static_cast<size_t>(cv::kBlock - 1 - run)] == 0.0f) ++run;
    zeros = run < cv::kBlock ? run : std::min(zeros + cv::kBlock, cv::kVoiceRingLen);
    head = at;
  }
  return differs;
}

// Skipping the taps of grains that read only zeros changes nothing: across runs of exact zeros of
// several lengths and the edge where the vowel returns, with grains in flight on the zeros, the
// skipping voice returns the plain voice's samples bit for bit. The edge sits inside a chunk. The
// second half of the check tells the voice the stretch is zeros while the ring holds a constant, and
// the outputs must then differ, so the first half is not passing without ever skipping.
bool testSilenceSkip() {
  struct Setting {
    float ratio;
    float rate;
  };
  size_t worst = 0;
  size_t lied = 0;
  for (int gap : {300, 1000, 2500, 5000}) {
    Signal heard = vowel(kSungHz, 6400);
    heard.insert(heard.end(), static_cast<size_t>(gap), 0.0f);
    const Signal tail = vowel(kSungHz, 6400, 123);
    heard.insert(heard.end(), tail.begin(), tail.end());
    Signal liar = heard;
    for (size_t i = 6400; i < 6400 + static_cast<size_t>(gap); ++i) liar[i] = 0.25f;
    for (const Setting s : {Setting{1.0f, 0.8409f}, Setting{0.9789f, 1.1892f}}) {
      worst = std::max(worst, skipDiffers(heard, heard, s.ratio, s.rate));
      lied += skipDiffers(liar, heard, s.ratio, s.rate);
    }
  }
  return report("silence skip", worst == 0 && lied > 0,
                "%.0f samples differ from the plain voice across the zeros and the edge (0); with a false zero count %.0f differ (> 0)",
                static_cast<double>(worst), static_cast<double>(lied));
}

// The parameters of second `sec` in testShiftOnSwitchOn's script: on at +5, off with the knob moved to
// -5, on at -5, a step to +3, on at MIX 0 with the knob moved to +6, then MIX 100 % at -6.
cv::PitchFxParams shiftScript(int sec) {
  switch (sec) {
    case 0: return formantOn(5);
    case 1: {
      cv::PitchFxParams p = formantOff();
      p.formant.shiftSt = -5;
      return p;
    }
    case 2: return formantOn(-5);
    case 3: return formantOn(3);
    case 4: return formantOn(6, 0.0f);
    default: return formantOn(-6);
  }
}

// SHIFT set while the card is off, or on at MIX 0, does not glide in from an older value: the card
// comes on at the knob. A SHIFT change while it is on glides.
bool testShiftOnSwitchOn() {
  const Signal in = vowel(kSungHz, 6 * kRate);
  Signal out(in.size());
  gFx.reset();
  float fadeBefore = 1.0f;
  float onFirst = 0.0f;
  float mixFirst = 0.0f;
  float stepFirst = 0.0f;
  float stepLast = 0.0f;
  for (int i = 0; i < 6 * kRate; i += cv::kBlock) {
    const int sec = i / kRate;
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, shiftScript(sec));
    const bool first = i % kRate == 0;
    const bool last = (i + cv::kBlock) % kRate == 0;
    if (sec == 1 && last) fadeBefore = gFx.formantFade();
    if (sec == 3 && first) stepFirst = gFx.shiftSemis();
    if (sec == 3 && last) stepLast = gFx.shiftSemis();
    // The card comes up once the tracker has a period, a few calls after the edge.
    const bool up = gFx.formantFade() > 0.0f;
    if (sec == 2 && up && onFirst == 0.0f) onFirst = gFx.shiftSemis();
    if (sec == 5 && up && mixFirst == 0.0f) mixFirst = gFx.shiftSemis();
  }
  const bool ok = fadeBefore == 0.0f && onFirst == -5.0f && mixFirst == -6.0f && stepFirst > -5.0f && stepFirst < 3.0f &&
                  std::fabs(stepLast - 3.0f) < 0.001f;
  return report("shift on switch-on", ok,
                "when the card comes up: SHIFT %.2f (-5) off to on, %.2f (-6) MIX 0 to 100 %%; a step from -5 to +3 reads %.2f (between) then %.3f (3)",
                static_cast<double>(onFirst), static_cast<double>(mixFirst), static_cast<double>(stepFirst),
                static_cast<double>(stepLast));
}

// A voice that starts beside a running one takes its grain grid, so the two sum as one waveform. At
// SHIFT 0 and MIX 50 % the level is one voice's when they line up and falls where they miss by a
// fraction of a period. Formant starting beside a running Autotune, and Autotune beside a running
// Formant, read the level of the two starting together.
bool testLateStart() {
  const Signal in = vowel(kSungHz, 5 * kRate);
  const int stepAt = 2 * kRate;
  cv::PitchFxParams both = formantOn(0, 0.5f);
  autotuneOn(both);
  cv::PitchFxParams autotuneOnly = formantOff();
  autotuneOn(autotuneOnly);
  const cv::PitchFxParams formantOnly = formantOn(0, 0.5f);
  const double ref = rms(run(in, both), 3 * kRate, 4 * kRate);
  double worst[2] = {0.0, 0.0};
  const cv::PitchFxParams* first[2] = {&autotuneOnly, &formantOnly};
  for (int k = 0; k < 2; ++k) worst[k] = std::fabs(db(rms(runSteps(in, *first[k], both, stepAt), 3 * kRate, 4 * kRate) / ref));
  return report("late start lines up", worst[0] <= 0.05 && worst[1] <= 0.05,
                "level within %.3f dB (Formant beside Autotune) and %.3f dB (Autotune beside Formant) of both starting together (0.05)",
                worst[0], worst[1]);
}

// A voiced frame on the tracker's edge or past it reads the edge, not the note, so the card steps
// aside for the dry voice, and stays aside through the unvoiced frames after it, until a frame
// inside the range returns. 70 Hz and 1000 Hz sit on the edges, 1300 Hz past the top.
bool testRangeFlags() {
  bool ok = true;
  for (double hz : {70.0, 1000.0, 1300.0}) {
    Signal in = sawtooth(hz, kRate);
    in.insert(in.end(), static_cast<size_t>(kRate * 3 / 10), 0.0f);
    const Signal back = vowel(kSungHz, kRate);
    in.insert(in.end(), back.begin(), back.end());
    Signal out(in.size());
    gFx.reset();
    float edgeFade = 0.0f;
    size_t edgeDiffers = 0;
    for (int i = 0; i + cv::kBlock <= static_cast<int>(in.size()); i += cv::kBlock) {
      gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, formantOn(kShift));
      if (i >= kRate / 2 && i < kRate * 13 / 10) edgeFade = std::max(edgeFade, gFx.formantFade());
      if (i >= kRate / 2 && i < kRate) {
        for (int j = 0; j < cv::kBlock; ++j) edgeDiffers += out[static_cast<size_t>(i + j)] != in[static_cast<size_t>(i + j)] ? 1u : 0u;
      }
    }
    const bool back_on = gFx.formantFade() > 0.99f;
    char name[48];
    std::snprintf(name, sizeof name, "range flags, %.0f Hz", hz);
    ok &= report(name, edgeFade == 0.0f && edgeDiffers == 0 && back_on,
                 "card aside through the note and the silence after it: fade peak %.3f (0), %.0f samples differ from the input (0); back on for the vowel: %.0f (1)",
                 static_cast<double>(edgeFade), static_cast<double>(edgeDiffers), back_on ? 1.0 : 0.0);
  }
  return ok;
}

// A NaN in the input does not stay behind in the level gain's running powers: once the ring has
// flushed it, the output is finite and its level is the clean run's.
bool testNanGuard() {
  const Signal in = vowel(kSungHz, 5 * kRate);
  const cv::PitchFxParams p = formantOn(kShift, 0.5f);
  const Signal clean = run(in, p);
  gFx.reset();
  Signal out(in.size());
  const int poisonAt = 2 * kRate;
  for (int i = 0; i < 5 * kRate; i += cv::kBlock) {
    Signal block(in.begin() + i, in.begin() + i + cv::kBlock);
    if (i == poisonAt) block[5] = std::numeric_limits<float>::quiet_NaN();
    gFx.process(block.data(), &out[static_cast<size_t>(i)], cv::kBlock, p);
  }
  bool finite = true;
  for (size_t i = static_cast<size_t>(3 * kRate); i < out.size(); ++i) finite = finite && std::isfinite(out[i]);
  const double off = std::fabs(db(rms(out, 4 * kRate, 5 * kRate) / rms(clean, 4 * kRate, 5 * kRate)));
  return report("nan guard", finite && off <= 0.05, "output finite from 1 s after the NaN: %.0f (1); level %.3f dB from the clean run (0.05)",
                finite ? 1.0 : 0.0, off);
}

// Autotune's correction moves through a vibrato, and the Formant voice holds one ratio for each call
// of 64 samples. Read at the middle of the call, the ratio keeps the voices on the same sample.
bool testAlignmentVibrato() {
  const double lag = lagToAutotune(vibratoVowel(kSungHz, 4 * kRate, 40.0));
  return report("aligned through vibrato", std::fabs(lag) <= 0.8,
                "cross-correlation peaks at lag %.2f samples under 5 Hz vibrato of +-40 cents (0, +-0.8)", lag);
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
  ok &= testMixLeavesZero();
  ok &= testMixLeavesFull();
  ok &= testToggleSteps();
  ok &= testMixLaw();
  ok &= testMixHoldsLevel();
  ok &= testTrim();
  ok &= testAnyLength();
  ok &= testSilenceSkip();
  ok &= testShiftOnSwitchOn();
  ok &= testLateStart();
  ok &= testRangeFlags();
  ok &= testNanGuard();
  ok &= testAlignmentVibrato();
  std::printf("%s formant\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
