// Crowd card checks: bit-exact off, no allocation, pitch moved by pitchSt, formant moved, two copies
// of different colour, the WIDTH law, the level law across VOICES, the lead leaving, the voice
// count and fade order, the solo, the lag following its scale across a gap and holding inside a
// burst, timing scatter, pitch drift, the tracker running without Autotune, the two enhancements
// (every control at its default is the pinned engine bit for bit, level scatter, contour and the
// human-voice ranges), the bottom tenth of WIDTH staying in tune (pitch, drift and contour), the
// shared tone shelf, the correctness pass (the switch with the tracker, the gap rule over a noise
// floor, WIDTH holding the level, notes outside the tracker's range, the read ceiling, the tuning
// ranges), and the raised copies (their pitch over the tuned copies, their 75 ms lag, the unvoiced
// run each copy waits before it plays plain). Every run feeds a PitchFx through its pass-through
// path first, so the ring is written and the tracker runs, as the chain does. No window, no audio
// device.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "engine/crowd.h"

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
constexpr int kSettle = kRate / 10;  // 100 ms

using Signal = std::vector<float>;

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0) {
  char detail[240];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

// Signals.

// Ten harmonics at 1/k amplitude, peak-normalised to 0.5.
Signal tone(double hz, int n) {
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(2.0 * kPi * hz * k * i / kRate) / k;
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  Signal out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

// Harmonics under a Gaussian bump at 1100 Hz. A formant move shifts the bump, so the centroid
// follows it. A 1/k tone has no bump, and its centroid would not move.
Signal vowel(double hz, int n) {
  constexpr double kCentreHz = 1100.0;
  constexpr double kWidthHz = 280.0;
  constexpr double kTopHz = 4000.0;
  std::vector<double> amp;
  for (int k = 1; hz * k < kTopHz; ++k) {
    const double z = (hz * k - kCentreHz) / kWidthHz;
    amp.push_back(std::exp(-0.5 * z * z));
  }
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (size_t k = 0; k < amp.size(); ++k) s += amp[k] * std::sin(2.0 * kPi * hz * static_cast<double>(k + 1) * i / kRate);
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  Signal out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

// The tone under a random piecewise-linear envelope (a new level every 20 ms, 0.3 to 1), so a
// cross-correlation against a delayed copy has one peak where a pure tone has one per period.
Signal irregular(double hz, int n) {
  constexpr int kStep = kRate / 50;
  Signal x = tone(hz, n);
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

constexpr int msToSamples(double ms) { return static_cast<int>(ms * kRate / 1000.0 + 0.5); }

// A burst train. The irregular tone sounds for burstMs, then silence for gapMs, count times, after
// leadMs of silence. Burst k starts at leadMs + k * (burstMs + gapMs).
struct Train {
  int leadMs;
  int burstMs;
  int gapMs;
  int count;

  int start(int k) const { return msToSamples(leadMs + k * (burstMs + gapMs)); }
  int size() const { return start(count); }
};

Signal bursts(double hz, const Train& tr) {
  Signal x = irregular(hz, tr.size());
  for (int k = 0; k < tr.count; ++k) {
    const int from = k == 0 ? 0 : tr.start(k - 1) + msToSamples(tr.burstMs);
    for (int i = from; i < tr.start(k); ++i) x[static_cast<size_t>(i)] = 0.0f;
  }
  for (int i = tr.start(tr.count - 1) + msToSamples(tr.burstMs); i < tr.size(); ++i) x[static_cast<size_t>(i)] = 0.0f;
  return x;
}

double rms(const Signal& x, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

// The number of samples at which a and b differ.
size_t differing(const Signal& a, const Signal& b) {
  size_t n = 0;
  for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i] ? 1 : 0;
  return n;
}

// 180 Hz, ten harmonics at 1/k, 5 Hz vibrato of +-20 cents, a 2 Hz raised-cosine phrase envelope
// and pink-ish noise at -40 dBFS, normalised to -18 dBFS RMS (level_test's program).
Signal program(int seconds) {
  const size_t n = static_cast<size_t>(seconds) * kRate;
  Signal tonePart(n);
  Signal noise(n);
  double phase = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double hz = 180.0 * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0);
    phase += 2.0 * kPi * hz / kRate;
    double v = 0.0;
    for (int k = 1; k <= 10; ++k) v += std::sin(k * phase) / k;
    tonePart[i] = static_cast<float>(v * 0.5 * (1.0 - std::cos(2.0 * kPi * 2.0 * t)));
  }
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  double b0 = 0.0, b1 = 0.0, b2 = 0.0;
  for (size_t i = 0; i < n; ++i) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const double w = static_cast<double>(s >> 11) / 4503599627370496.0 - 1.0;
    b0 = 0.99765 * b0 + w * 0.0990460;
    b1 = 0.96300 * b1 + w * 0.2965164;
    b2 = 0.57000 * b2 + w * 1.0526913;
    noise[i] = static_cast<float>(b0 + b1 + b2 + w * 0.1848);
  }
  const double tg = std::pow(10.0, -18.0 / 20.0) / rms(tonePart, 0, n);
  const double ng = std::pow(10.0, -40.0 / 20.0) / rms(noise, 0, n);
  Signal x(n);
  for (size_t i = 0; i < n; ++i) x[i] = static_cast<float>(tonePart[i] * tg + noise[i] * ng);
  const double g = std::pow(10.0, -18.0 / 20.0) / rms(x, 0, n);
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

// Running a PitchFx and then the Crowd card on the same blocks, in the chain's order.

cv::PitchFx gFx;
cv::Crowd gCrowd;

struct Probe {
  float lead = -1.0f;
  int active = -1;
  float drift = 0.0f;  // largest |drift| in cents any copy showed at the end of any block
};

// The PitchFx params every run uses unless it says otherwise: pass-through with the tracker on.
cv::PitchFxParams passThrough() {
  cv::PitchFxParams pp;
  pp.octave.on = false;
  pp.crowdOn = true;
  return pp;
}

// Crowd card and PitchFx params both switch from their first to their second value at sample
// `switchAt` (a block boundary).
Signal runBoth(const Signal& in, const cv::CrowdParams& first, const cv::CrowdParams& second,
               const cv::PitchFxParams& pitchFirst, const cv::PitchFxParams& pitchSecond, int switchAt,
               Probe* probe = nullptr) {
  gFx.reset();
  gCrowd.reset();
  Signal out(in.size());
  float scratch[cv::kBlock];
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    const bool after = i >= switchAt;
    gInProcess = true;
    gFx.process(&in[static_cast<size_t>(i)], scratch, n, after ? pitchSecond : pitchFirst);
    gCrowd.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, after ? second : first, gFx);
    gInProcess = false;
    if (probe) {
      probe->lead = gCrowd.leadGain();
      probe->active = gCrowd.activeVoices();
      for (size_t v = 0; v < 8; ++v) probe->drift = std::max(probe->drift, std::fabs(gCrowd.driftCents(v)));
    }
  }
  return out;
}

Signal runSwitch(const Signal& in, const cv::CrowdParams& first, const cv::CrowdParams& second,
                 int switchAt, Probe* probe = nullptr) {
  return runBoth(in, first, second, passThrough(), passThrough(), switchAt, probe);
}

Signal run(const Signal& in, const cv::CrowdParams& p, Probe* probe = nullptr) {
  return runSwitch(in, p, p, static_cast<int>(in.size()), probe);
}

// The shipped defaults are the owner's working set (contour, level scatter, timing scatter and drift
// on). A test starts from this neutral tuning and turns on only what it measures.
void neutralize(cv::CrowdTuning& t) {
  t.scatterMs = 0.0f;
  t.driftCents = 0.0f;
  t.levelScatterDb = 0.0f;
  t.contour = {1.0f, 1.0f, 1.0f, 1.0f};
}

cv::CrowdParams knobs(float voices, float width) {
  cv::CrowdParams p;
  p.on = true;
  p.voices = voices;
  p.width = width;
  neutralize(p.tuning);
  return p;
}

// Voice 0 alone with nothing around it. Soloed at the top of VOICES it stands alone at unity, so
// the lead is 0 and its raised copy is silent, and the low cut sits at the bottom of the filter
// band. The voice sits on the lead's pitch with a flat tone, so only its formant move is left.
// WIDTH sits at 1, so a pitchSt is the offset heard.
cv::CrowdParams isolated(float formantSt) {
  cv::CrowdParams p = knobs(1.0f, 1.0f);
  p.solo = 0b0001;
  cv::CrowdTuning& t = p.tuning;
  t.voiceDb = 0.0f;
  t.formantSt[0] = formantSt;
  t.pitchSt[0] = 0.0f;
  t.shelfDb[0] = 0.0f;
  t.hpHz = cv::kFilterMinHz;
  return p;
}

// Copy v alone at the lead's pitch and throat, flat, so its output is the lead moved only by its lag.
cv::CrowdParams lagProbe(size_t v, float lagScale, float width = 1.0f) {
  cv::CrowdParams p = isolated(0.0f);
  p.width = width;
  p.solo = static_cast<uint8_t>(1u << v);
  p.tuning.lagScale = lagScale;
  p.tuning.pitchSt[v] = 0.0f;
  p.tuning.formantSt[v] = 0.0f;
  p.tuning.shelfDb[v] = 0.0f;
  return p;
}

// Voice v of the default tuning, moved into slot 0 and played alone.
cv::CrowdParams alone(size_t v) {
  cv::CrowdParams p = isolated(0.0f);
  const cv::CrowdTuning d;
  p.tuning.pitchSt[0] = d.pitchSt[v];
  p.tuning.formantSt[0] = d.formantSt[v];
  p.tuning.shelfDb[0] = d.shelfDb[v];
  return p;
}

// Measuring.

// Goertzel magnitude of x at hz.
double goertzel(const float* x, int n, double hz) {
  const double w = 2.0 * kPi * hz / kRate;
  const double c = 2.0 * std::cos(w);
  double s1 = 0.0;
  double s2 = 0.0;
  for (int i = 0; i < n; ++i) {
    const double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
}

// The same under a Hann window. Its side lobes fall fast, so over a second a line 8 Hz from a loud
// one reads near nothing of it.
double hannGoertzel(const float* x, int n, double hz) {
  std::vector<float> w(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    w[static_cast<size_t>(i)] = static_cast<float>(x[i] * 0.5 * (1.0 - std::cos(2.0 * kPi * i / n)));
  }
  return goertzel(w.data(), n, hz);
}

// Root-sum-square Goertzel magnitude within +-halfHz of hz over second 1..2.
double bandAmp(const Signal& x, double hz, double halfHz) {
  double e = 0.0;
  const int steps = static_cast<int>(halfHz / 2.0);
  for (int d = -steps; d <= steps; ++d) {
    const double a = goertzel(&x[kRate], kRate, hz + 2.0 * d);
    e += a * a;
  }
  return std::sqrt(e);
}

// Amplitude-weighted mean frequency over harmonics first..last of f0.
double centroidHz(const Signal& x, double f0, int first, int last) {
  double num = 0.0;
  double den = 0.0;
  for (int k = first; k <= last; ++k) {
    const double a = bandAmp(x, f0 * k, 0.45 * f0);
    num += f0 * k * a;
    den += a;
  }
  return den > 0.0 ? num / den : 0.0;
}

// Median f0 of x over [from, to), from a fresh tracker fed from `warm` on.
double medianHzIn(const Signal& x, int warm, int from, int to) {
  cv::PitchTracker t;
  std::vector<double> hz;
  for (int i = warm; i < to; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, to - i);
    t.push(&x[static_cast<size_t>(i)], n);
    if (i + n > from && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

// Median f0 of a signal over seconds 1..2, from a fresh tracker.
double medianHz(const Signal& x) { return medianHzIn(x, 0, kRate, static_cast<int>(x.size())); }

// Frequency of the strongest component within +-2 % of `near` over [from, from + n), found on a
// 0.1 Hz grid, so a drift of a few cents shows.
double peakHz(const Signal& x, int from, int n, double near) {
  double best = near;
  double bestMag = -1.0;
  for (double hz = near * 0.98; hz <= near * 1.02; hz += 0.1) {
    const double m = goertzel(&x[static_cast<size_t>(from)], n, hz);
    if (m > bestMag) {
      bestMag = m;
      best = hz;
    }
  }
  return best;
}

// Lag in samples within [minLag, maxLag] where out[t] best matches in[t - lag], over the output
// window [from, to).
int peakLag(const Signal& in, const Signal& out, int from, int to, int minLag, int maxLag) {
  int best = minLag;
  double bestScore = -1.0;
  for (int lag = minLag; lag <= maxLag; ++lag) {
    double num = 0.0;
    double den = 0.0;
    for (int t = from; t < to; ++t) {
      const double a = in[static_cast<size_t>(t - lag)];
      num += static_cast<double>(out[static_cast<size_t>(t)]) * a;
      den += a * a;
    }
    const double score = num / std::sqrt(den + 1e-12);
    if (score > bestScore) {
      bestScore = score;
      best = lag;
    }
  }
  return best;
}

// Tests.

bool testOffExact() {
  const Signal in = tone(220.0, kRate);
  cv::CrowdParams off;
  off.on = false;
  const Signal a = run(in, off);
  float worst = 0.0f;
  for (size_t i = kSettle; i < in.size(); ++i) worst = std::max(worst, std::fabs(a[i] - in[i]));

  // On, then off: the stage ends bit-exact once its ramp is out.
  const int half = kRate / 2 / cv::kBlock * cv::kBlock;
  const Signal b = runSwitch(in, knobs(0.7f, 0.5f), off, half);
  float worstAfter = 0.0f;
  for (size_t i = static_cast<size_t>(half + kSettle); i < in.size(); ++i)
    worstAfter = std::max(worstAfter, std::fabs(b[i] - in[i]));
  bool ok = report("off exact", worst == 0.0f, "off from reset max|out-in|=%.3g (0)", worst);
  ok &= report("off exact", worstAfter == 0.0f, "on then off, 100 ms after the switch max|out-in|=%.3g (0)", worstAfter);
  return ok;
}

bool testReset() {
  const Signal in = program(1);
  const cv::CrowdParams p = knobs(0.7f, 0.5f);
  const Signal a = run(in, p);
  const Signal b = run(in, p);
  size_t differ = 0;
  for (size_t i = 0; i < a.size(); ++i) differ += a[i] != b[i] ? 1 : 0;
  return report("reset", differ == 0, "a second run after reset() differs in %.0f samples (0)", static_cast<double>(differ));
}

bool testNoAlloc() {
  const Signal in = program(1);
  run(in, knobs(0.9f, 0.5f));  // aborts on any new/delete inside process()
  cv::CrowdParams moving = knobs(0.3f, 0.9f);
  moving.tuning.scatterMs = 12.0f;
  moving.tuning.driftCents = 30.0f;
  run(in, moving);
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// The top of the WIDTH factor on pitch offsets, drift and contour (owner 2026-10-31).
constexpr double kPitchTop = 2.0 / 3.0;

// One voice alone lands on the lead's pitch moved by two thirds of its pitchSt at WIDTH 1, with the
// voice's own formant and tone shelf in place.
bool testPitchMoves() {
  const Signal in = tone(220.0, 2 * kRate);
  bool ok = true;
  for (size_t v = 0; v < 4; ++v) {
    const cv::CrowdParams p = alone(v);
    const double want = 220.0 * std::pow(2.0, static_cast<double>(p.tuning.pitchSt[0]) * kPitchTop / 12.0);
    const double hz = medianHz(run(in, p));
    const double err = hz > 0.0 ? cents(hz, want) : 9999.0;
    char name[40];
    std::snprintf(name, sizeof name, "pitch moves, voice %zu", v);
    ok &= report(name, std::fabs(err) <= 10.0, "f0=%.2f Hz, want %.2f Hz, err=%.2f cents (+-10)", hz, want, err);
  }
  return ok;
}

// The WIDTH law of the owner, written out here so the test does not lean on the engine's code. The
// factor on the formant offsets is linear through (0, 0), (0.1, 0.012) and (1, 1).
double widthFactor(double w) { return w <= 0.1 ? 0.012 * (w / 0.1) : 0.012 + 0.988 * ((w - 0.1) / 0.9); }

// The factor on the pitch offsets, the drift and the contour deviation is the same law with a top of
// 2/3 (owner 2026-10-31), so the knee value is 0.008 and WIDTH 1 reaches two thirds.
double pitchFactor(double w) { return widthFactor(w) * kPitchTop; }

// Copy v's lag in ms on the default delays at WIDTH w. The multiplier on delayMs times lagScale is
// 0.5 at WIDTH 0 and 1 + 20 ms / (the longest base lag) at WIDTH 1, linear between, and maxLagMs
// caps the product.
double lagMsAt(size_t v, double w, double lagScale) {
  const cv::CrowdTuning d;
  double longest = 0.0;
  for (float ms : d.delayMs) longest = std::max(longest, static_cast<double>(ms) * lagScale);
  const double mul = 0.5 * (1.0 - w) + (1.0 + 20.0 / longest) * w;
  return std::min(static_cast<double>(d.delayMs[v]) * lagScale * mul, static_cast<double>(d.maxLagMs));
}

// f0 of a copy of the 220 Hz tone, read at its second harmonic over second 1..2 and halved, near
// the f0 wanted.
double copyHz(const Signal& out, double want) { return peakHz(out, kRate, kRate, 2.0 * want) / 2.0; }

// The -2.5 st copy (index 2, formant -1 st) alone at WIDTH 0, 0.1, 0.5 and 1. Its pitch offset is -2.5 st
// times the pitch factor, exactly 0 at 0, 2.0 cents at 0.1 (the knee), 0.301 of the full offset at
// 0.5 and 0.667 of it (-1.667 st) at 1. Its formant move is the log of its grain playback rate over
// the same log at WIDTH 1, read from the engine, and follows the full width factor: none at 0, 1.2 %
// at 0.1 and 0.451 at 0.5. (A spectral centroid does not read it,
// because the grain windows make the centroid's slope depend on the pitch ratio; at WIDTH 0 the
// output itself shows it, which equals the output of a copy with no pitch or formant offset.) The
// tone shelf stays where it is, and the lag follows the lag multiplier of the WIDTH law.
constexpr size_t kWidthCopy = 2;

struct WidthRow {
  float width;
  double tolCents;
};

constexpr std::array<WidthRow, 4> kWidthRows = {{{0.0f, 1.0}, {0.1f, 1.5}, {0.5f, 10.0}, {1.0f, 10.0}}};

// What the copy showed at a row's WIDTH: its lag in samples, its grain playback rate, its f0 and the
// f0 the law wants.
struct WidthRun {
  float lag = 0.0f;
  double rate = 0.0;
  double hz = 0.0;
  double want = 0.0;
};

using WidthRuns = std::array<WidthRun, 4>;

WidthRuns runWidthRows() {
  const Signal tn = tone(220.0, 2 * kRate);
  const double full = static_cast<double>(cv::CrowdTuning{}.pitchSt[kWidthCopy]);
  WidthRuns runs;
  for (size_t i = 0; i < kWidthRows.size(); ++i) {
    cv::CrowdParams p = alone(kWidthCopy);
    p.width = kWidthRows[i].width;
    const Signal out = run(tn, p);
    runs[i].lag = gCrowd.lagSamples(0);
    runs[i].rate = static_cast<double>(gCrowd.grainRate(0));
    runs[i].want = 220.0 * std::pow(2.0, full * pitchFactor(static_cast<double>(p.width)) / 12.0);
    runs[i].hz = copyHz(out, runs[i].want);
  }
  return runs;
}

bool reportWidthPitch(const WidthRuns& runs) {
  bool ok = true;
  for (size_t i = 0; i < runs.size(); ++i) {
    const double err = cents(runs[i].hz, runs[i].want);
    char name[40];
    std::snprintf(name, sizeof name, "width law, WIDTH %.0f %%", static_cast<double>(kWidthRows[i].width) * 100.0);
    ok &= report(name, std::fabs(err) <= kWidthRows[i].tolCents, "f0=%.3f Hz, want %.3f Hz, err=%.2f cents (+-%.0f)",
                 runs[i].hz, runs[i].want, err, kWidthRows[i].tolCents);
  }
  return ok;
}

bool reportWidthFormant(const WidthRuns& runs) {
  constexpr double kMoveTol = 0.005;
  bool ok = true;
  for (size_t k = 0; k < runs.size(); ++k) {
    const double w = static_cast<double>(kWidthRows[k].width);
    const double move = std::log(runs[k].rate) / std::log(runs[3].rate);
    char name[48];
    std::snprintf(name, sizeof name, "width law formant, WIDTH %.0f %%", w * 100.0);
    ok &= report(name, std::fabs(move - widthFactor(w)) <= kMoveTol, "formant move %.3f of full, want %.3f (+-%.3f)", move,
                 widthFactor(w), kMoveTol);
    std::printf("info width %.2f copy 3 f0 %.3f Hz, %+.2f cents from the 220 Hz lead, formant move %.1f %% of full\n", w,
                runs[k].hz, cents(runs[k].hz, 220.0), 100.0 * move);
  }
  return ok;
}

// At WIDTH 0 the vowel through the copy is the vowel through a copy with no pitch or formant offset.
bool reportWidthNoOffsets() {
  const Signal vow = vowel(220.0, 2 * kRate);
  cv::CrowdParams withOffsets = alone(kWidthCopy);
  withOffsets.width = 0.0f;
  cv::CrowdParams noOffsets = withOffsets;
  noOffsets.tuning.pitchSt[0] = 0.0f;
  noOffsets.tuning.formantSt[0] = 0.0f;
  const size_t differ = differing(run(vow, withOffsets), run(vow, noOffsets));
  return report("width law no offsets at 0", differ == 0,
                "WIDTH 0 with a -2.5 st pitch and a -1 st formant differs from one with none in %.0f samples (0)",
                static_cast<double>(differ));
}

// The slot-0 copy's lag follows the lag multiplier: 0.5 at WIDTH 0, 1.1667 at WIDTH 1.
bool reportWidthLagStretch(const WidthRuns& runs) {
  const double lagScale = static_cast<double>(cv::CrowdTuning{}.lagScale);
  double worstLag = 0.0;
  for (size_t k = 0; k < runs.size(); ++k) {
    const double wantLag = lagMsAt(0, static_cast<double>(kWidthRows[k].width), lagScale) * cv::crowd_detail::kSmpPerMs;
    worstLag = std::max(worstLag, std::fabs(static_cast<double>(runs[k].lag) - wantLag));
  }
  return report("width stretches the lag", worstLag < 1.0,
                "lag at WIDTH 0 / 0.5 / 1 = %.1f / %.1f / %.1f samples, worst error over the four WIDTHs %.2f samples (< 1)",
                static_cast<double>(runs[0].lag), static_cast<double>(runs[2].lag), static_cast<double>(runs[3].lag),
                worstLag);
}

bool testWidthLaw() {
  const WidthRuns runs = runWidthRows();
  bool ok = reportWidthPitch(runs);
  ok &= reportWidthFormant(runs);
  ok &= reportWidthNoOffsets();
  ok &= reportWidthLagStretch(runs);
  return ok;
}

// VOICES 20, 40, 60 and 80 % bring in the tuned copies in index order, the -1.75 st copy first, then
// the +2.75 st, the -2.5 st and the +1 st, each with its raised copy. At each step the output carries
// a line at the f0 of every tuned copy that has come in and none at the f0 of the copies still to
// come. It runs at WIDTH 1 and at WIDTH 0.8, where the offsets are the pitch factor 0.520 of their
// full values. (At WIDTH 0.5 the -1.75 st and -2.5 st copies sit 23 cents apart and their lines
// overlap.) The lines are read under a Hann window, because the lead's line and the first raised
// copy's, both at 220 Hz, leak into the +1 st copy's line 8.6 Hz away under a plain one.
bool testFadeOrder() {
  const Signal in = tone(220.0, 2 * kRate);
  const cv::CrowdTuning d;
  bool ok = true;
  for (float width : {1.0f, 0.8f}) {
    for (int n = 1; n <= 4; ++n) {
      const Signal out = run(in, knobs(0.2f * static_cast<float>(n), width));
      double amp[4];
      double top = 0.0;
      for (size_t v = 0; v < 4; ++v) {
        const double st = static_cast<double>(d.pitchSt[v]) * pitchFactor(static_cast<double>(width));
        amp[v] = hannGoertzel(&out[kRate], kRate, 220.0 * std::pow(2.0, st / 12.0));
        top = std::max(top, amp[v]);
      }
      bool order = true;
      double rel[4];
      for (size_t v = 0; v < 4; ++v) {
        rel[v] = amp[v] / top;
        order &= static_cast<int>(v) < n ? rel[v] >= 0.4 : rel[v] <= 0.1;
      }
      char name[48];
      std::snprintf(name, sizeof name, "fade order, WIDTH %.0f %%, VOICES %d %%", static_cast<double>(width * 100.0f), 20 * n);
      ok &= report(name, order,
                   "line at each copy's f0 re the loudest %.2f %.2f %.2f %.2f (sounding >= 0.4, silent <= 0.1)", rel[0],
                   rel[1], rel[2], rel[3]);
    }
  }
  return ok;
}

// isolated() with the voice's lag set to 90 ms. The centroid of a steady vowel at a shifted
// formant depends on how the grain reads line up with the period grid, so this probe holds the
// lag fixed whatever the default delays are.
cv::CrowdParams formantProbe(float formantSt) {
  cv::CrowdParams p = isolated(formantSt);
  p.tuning.delayMs[0] = 90.0f;
  return p;
}

// The formant moves of a throat shorter than the lead's (+1 st, no default copy has one) and of
// voice 0 (the longest, -1.75 st) against a flat voice, pitch on the lead.
bool testFormantMoves() {
  const Signal in = vowel(220.0, 2 * kRate);
  const cv::CrowdTuning d;
  const double flat = centroidHz(run(in, formantProbe(0.0f)), 220.0, 1, 10);
  const double up = centroidHz(run(in, formantProbe(1.0f)), 220.0, 1, 10);
  const double down = centroidHz(run(in, formantProbe(d.formantSt[0])), 220.0, 1, 10);
  return report("formant moves", up >= 1.03 * flat && down <= 0.94 * flat,
                "centroid 0 st=%.1f Hz, +1 st formant x%.3f (>=1.03), voice 0 formant x%.3f (<=0.94)", flat,
                up / flat, down / flat);
}

// A raised copy shares its tuned copy's throat and tone shelf and is told apart by its pitch: on the
// 220 Hz tone at WIDTH 1 it sits two thirds of 1.75 st, 116.7 cents, over the tuned copy.
bool testRaisedDiffers() {
  const Signal in = tone(220.0, 2 * kRate);
  double gap[4];
  double worst = 0.0;
  for (size_t v = 0; v < 4; ++v) {
    cv::CrowdParams p = alone(v);
    const double tunedHz = medianHz(run(in, p));
    p.solo = 0b10000;  // the raised copy of the tuned copy in slot 0
    const double raisedHz = medianHz(run(in, p));
    gap[v] = tunedHz > 0.0 && raisedHz > 0.0 ? cents(raisedHz, tunedHz) : 9999.0;
    worst = std::max(worst, std::fabs(gap[v] - 175.0 * kPitchTop));
  }
  return report("voices differ, raised copies", worst <= 10.0,
                "raised over tuned copy 1..4 %.1f %.1f %.1f %.1f cents (116.7 +- 10)", gap[0], gap[1], gap[2], gap[3]);
}

// Voice 0 (formant -1.75 st, tone -3 dB) and voice 2 (formant -1 st, tone +3 dB) are two different
// colours. Both sit on the lead's pitch, so only the throat and the tone shelf tell them apart, and
// the spectral centroid of each, over the harmonics of the lead's f0 up to the 20th, differs by at
// least 5 %. The centroid of a steady tone depends on how the grain reads line up with the period
// grid (see formantProbe), so the WIDTH multiplier on the lag is held at 1 and both voices sit at
// 48 ms.
bool testVoicesDiffer() {
  const Signal in = tone(220.0, 2 * kRate);
  const size_t pick[2] = {0, 2};
  double c[2];
  for (size_t i = 0; i < 2; ++i) {
    cv::CrowdParams p = alone(pick[i]);
    p.tuning.pitchSt[0] = 0.0f;
    p.tuning.widthLagBottom = 1.0f;
    p.tuning.widthLagMs = 0.0f;
    c[i] = centroidHz(run(in, p), 220.0, 1, 20);
  }
  const double diff = std::fabs(c[1] - c[0]) / std::min(c[0], c[1]);
  bool ok = report("voices differ", diff >= 0.05, "centroid voice 0 %.1f Hz, voice 2 %.1f Hz, differ %.1f %% (>=5)", c[0],
                   c[1], 100.0 * diff);
  return ok & testRaisedDiffers();
}

double levelDb(const Signal& in, const Signal& out) {
  const size_t from = kRate;
  return 20.0 * std::log10(rms(out, from, out.size()) / rms(in, from, in.size()));
}

bool testLevelLaw() {
  const Signal in = program(3);
  bool ok = true;
  for (float k : {0.2f, 0.4f, 0.6f, 0.8f, 1.0f}) {
    const double db = levelDb(in, run(in, knobs(k, 0.5f)));
    char name[40];
    std::snprintf(name, sizeof name, "level VOICES %.0f %%", static_cast<double>(k * 100.0f));
    ok &= report(name, db >= -1.5 && db <= 1.0, "%+.2f dB re input (-1.5..+1.0)", db);
  }
  for (float w : {0.0f, 1.0f}) {
    const double db = levelDb(in, run(in, knobs(1.0f, w)));
    std::printf("info level VOICES 100 %% WIDTH %.0f %%  %+.2f dB re input\n", static_cast<double>(w * 100.0f), db);
  }
  return ok;
}

bool testLeadLeaves() {
  const Signal in = program(1);
  Probe top;
  run(in, knobs(1.0f, 0.5f), &top);
  Probe four;
  run(in, knobs(0.8f, 0.5f), &four);
  Probe one;
  run(in, knobs(0.2f, 0.5f), &one);
  const cv::CrowdTuning t;
  const double g2 = std::pow(10.0, t.voiceDb / 10.0);
  const double wantFour = std::sqrt(1.0 - 4.0 * g2);
  const double wantOne = std::sqrt(1.0 - g2);
  bool ok = report("lead leaves", top.lead == 0.0f, "lead gain at VOICES 100 %% = %.3g (0)", static_cast<double>(top.lead));
  ok &= report("lead law", std::fabs(four.lead - wantFour) < 0.01 && std::fabs(one.lead - wantOne) < 0.01,
               "lead at 80 %% %.3f (want %.3f), at 20 %% %.3f (want %.3f)", static_cast<double>(four.lead), wantFour,
               static_cast<double>(one.lead), wantOne);
  return ok;
}

// Each fifth of VOICES brings in one tuned copy and its raised copy.
bool testVoiceCount() {
  const Signal in = program(1);
  bool ok = true;
  const std::pair<float, int> want[] = {{0.0f, 0}, {0.2f, 2}, {0.4f, 4}, {0.6f, 6}, {0.8f, 8}, {1.0f, 8}};
  for (const auto& w : want) {
    Probe p;
    run(in, knobs(w.first, 0.5f), &p);
    char name[40];
    std::snprintf(name, sizeof name, "voice count VOICES %.0f %%", static_cast<double>(w.first * 100.0f));
    ok &= report(name, p.active == w.second, "%.0f copies ticking (%.0f)", p.active, w.second);
  }

  // At 20 % the live copies are voice 0 and its raised copy, which read the tuning's first entries:
  // the entries of voices 1..3 can change without moving a sample.
  cv::CrowdParams a = knobs(0.2f, 0.5f);
  cv::CrowdParams b = a;
  for (size_t v = 1; v < 4; ++v) {
    b.tuning.pitchSt[v] = 4.0f;
    b.tuning.formantSt[v] = 5.0f;
    b.tuning.shelfDb[v] = 6.0f;
  }
  const Signal outA = run(in, a);
  const Signal outB = run(in, b);
  size_t differ = 0;
  for (size_t i = 0; i < outA.size(); ++i) differ += outA[i] != outB[i] ? 1 : 0;
  b = a;
  b.tuning.pitchSt[0] = 4.0f;
  const Signal outC = run(in, b);
  size_t moved = 0;
  for (size_t i = 0; i < outA.size(); ++i) moved += outA[i] != outC[i] ? 1 : 0;
  ok &= report("voice 0 first", differ == 0 && moved > 0,
               "voices 1..3 retuned change %.0f samples (0), voice 0 retuned changes %.0f (>0)",
               static_cast<double>(differ), static_cast<double>(moved));
  return ok;
}

// A solo sounds the soloed copies and nothing else, whatever VOICES says. Solo 0 leaves the plain
// law untouched, and all eight copies soloed at the top of the knob are that law's crowd.
// Voice 1 soloed at VOICES 20 %, without its raised copy.
cv::CrowdParams soloOne() {
  cv::CrowdParams a = knobs(0.2f, 0.5f);
  a.solo = 0b0010;
  return a;
}

// The soloed voice ticks alone. Voice 0 is silent and voice 1 sounds, so retuning voice 0 moves no
// sample and retuning voice 1 moves some.
bool soloCountAndVoice(const Signal& in) {
  Probe p;
  const cv::CrowdParams a = soloOne();
  const Signal outA = run(in, a, &p);
  bool ok = report("solo count", p.active == 1, "%.0f voices ticking at solo 0b0010, VOICES 20 %% (1)", p.active);
  cv::CrowdParams b = a;
  b.tuning.pitchSt[0] = 4.0f;
  b.tuning.formantSt[0] = 5.0f;
  b.tuning.shelfDb[0] = 6.0f;
  cv::CrowdParams c = a;
  c.tuning.pitchSt[1] = 4.0f;
  const size_t silent = differing(outA, run(in, b));
  const size_t sounds = differing(outA, run(in, c));
  ok &= report("solo voice", silent == 0 && sounds > 0,
               "voice 0 retuned changes %.0f samples (0), voice 1 retuned changes %.0f (>0)",
               static_cast<double>(silent), static_cast<double>(sounds));
  return ok;
}

// At the top the lead is gone and the one soloed copy stands alone.
bool soloTop(const Signal& in) {
  Probe p;
  cv::CrowdParams top = knobs(1.0f, 0.5f);
  top.solo = 0b0100;
  run(in, top, &p);
  return report("solo top", p.active == 1 && p.lead == 0.0f, "%.0f voices ticking (1), lead gain %.3g (0)", p.active, p.lead);
}

// Solo 0 is the plain law bit for bit, and all eight copies soloed at the top give the same crowd.
bool soloOff(const Signal& in) {
  const cv::CrowdParams plain = knobs(0.2f, 0.5f);
  cv::CrowdParams zero = plain;
  zero.solo = 0;
  const size_t differ = differing(run(in, plain), run(in, zero));
  cv::CrowdParams all = knobs(1.0f, 0.5f);
  const Signal outTop = run(in, all);
  all.solo = 0xFF;
  const size_t differTop = differing(outTop, run(in, all));
  return report("solo off", differ == 0 && differTop == 0,
                "solo 0 differs in %.0f samples (0), solo 0xFF at VOICES 100 %% differs in %.0f (0)",
                static_cast<double>(differ), static_cast<double>(differTop));
}

bool testSolo() {
  const Signal in = program(1);
  bool ok = soloCountAndVoice(in);
  ok &= soloTop(in);
  ok &= soloOff(in);
  return ok;
}

// The lag in ms of the output over the 150 ms starting at output sample `from`, searched within
// +-80 ms of `around`.
double measuredLagMs(const Signal& in, const Signal& out, int from, double around) {
  const int lo = std::max(0, msToSamples(around - 80.0));
  const int hi = msToSamples(around + 80.0);
  return 1000.0 * peakLag(in, out, from, from + msToSamples(150.0), lo, hi) / kRate;
}

// 220 Hz bursts with 120 ms of silence between them, at WIDTH 1. The lag scale goes from its default to
// 2 while burst 2 sounds in the copy's output, 100 ms after its first sample. The copy keeps its near
// lag to the end of that burst and takes the far lag in the gap, so bursts 3 to 5 sound at the far lag.
bool testLagAcrossGap() {
  constexpr float kFarScale = 2.0f;
  const Train tr{300, 300, 120, 6};
  const Signal in = bursts(220.0, tr);
  const cv::CrowdTuning d;
  bool ok = true;
  for (size_t v : {size_t{3}, size_t{1}}) {
    const double nearMs = lagMsAt(v, 1.0, static_cast<double>(d.lagScale));
    const double farMs = lagMsAt(v, 1.0, static_cast<double>(kFarScale));
    const int switchAt = (tr.start(2) + msToSamples(nearMs + 100.0)) / cv::kBlock * cv::kBlock;
    const Signal out = runSwitch(in, lagProbe(v, d.lagScale), lagProbe(v, kFarScale), switchAt);
    double worst = 0.0;
    double lag[6];
    for (int k = 0; k < tr.count; ++k) {
      const double want = k <= 2 ? nearMs : farMs;
      const int from = k == 2 ? switchAt + msToSamples(20.0) : tr.start(k) + msToSamples(want + 30.0);
      lag[k] = measuredLagMs(in, out, from, want);
      worst = std::max(worst, std::fabs(lag[k] - want));
    }
    char name[48];
    std::snprintf(name, sizeof name, "lag across a gap, copy %zu", v + 1);
    ok &= report(name, worst <= 0.5, "want %.0f ms before the gap and %.0f ms after; worst error %.2f ms (<= 0.5)", nearMs,
                 farMs, worst);
    std::printf("info copy %zu lag per burst %.2f %.2f %.2f(mid-burst switch) %.2f %.2f %.2f ms\n", v + 1, lag[0], lag[1],
                lag[2], lag[3], lag[4], lag[5]);
  }
  return ok;
}

// Timing scatter of 12 ms on copy 4 (105 ms lag at WIDTH 1) over ten bursts. Each burst's lag stays
// within 12 ms of 105 ms and differs from burst to burst. A second run with the copy's own pitch
// offset keeps its f0 within 10 cents of the offset inside every burst, so no lag change bends it.
struct ScatterCase {
  Train tr{300, 300, 120, 10};
  Signal in;
  size_t v = 3;
  double target = 0.0;  // the copy's lag in ms with no scatter
  cv::CrowdParams flat;
};

ScatterCase scatterCase() {
  ScatterCase c;
  c.in = bursts(220.0, c.tr);
  const cv::CrowdTuning d;
  c.target = lagMsAt(c.v, 1.0, static_cast<double>(d.lagScale));
  c.flat = lagProbe(c.v, d.lagScale);
  c.flat.tuning.scatterMs = 12.0f;
  return c;
}

bool scatterLag(const ScatterCase& c) {
  const Signal out = run(c.in, c.flat);
  double lag[10];
  double worst = 0.0;
  double lo = 1e9;
  double hi = -1e9;
  int changes = 0;
  for (int k = 0; k < c.tr.count; ++k) {
    lag[k] = measuredLagMs(c.in, out, c.tr.start(k) + msToSamples(c.target + 15.0 + 30.0), c.target);
    worst = std::max(worst, std::fabs(lag[k] - c.target));
    lo = std::min(lo, lag[k]);
    hi = std::max(hi, lag[k]);
    if (k > 0 && std::fabs(lag[k] - lag[k - 1]) > 1.0) ++changes;
  }
  std::printf("info scatter lag per burst");
  for (double l : lag) std::printf(" %.1f", l);
  std::printf(" ms\n");
  return report("scatter lag", worst <= 12.5 && changes >= 6,
                "worst |lag - %.0f| %.2f ms (<= 12.5), spread %.1f ms, %.0f of 9 bursts moved by > 1 ms (>= 6)", c.target,
                worst, hi - lo, static_cast<double>(changes));
}

bool scatterPitch(const ScatterCase& c) {
  const cv::CrowdTuning d;
  cv::CrowdParams pitched = c.flat;
  pitched.tuning.pitchSt[c.v] = d.pitchSt[c.v];
  const Signal outP = run(c.in, pitched);
  const double want = 220.0 * std::pow(2.0, static_cast<double>(d.pitchSt[c.v]) * kPitchTop / 12.0);
  double worstCents = 0.0;
  for (int k = 0; k < c.tr.count; ++k) {
    const int from = c.tr.start(k) + msToSamples(c.target + 15.0 + 30.0);
    const double hz = medianHzIn(outP, c.tr.start(k), from, from + msToSamples(150.0));
    worstCents = std::max(worstCents, hz > 0.0 ? std::fabs(cents(hz, want)) : 9999.0);
  }
  return report("scatter pitch", worstCents <= 10.0, "worst f0 error in a burst %.2f cents from %.2f Hz (<= 10)", worstCents,
                want);
}

bool testScatter() {
  const ScatterCase c = scatterCase();
  bool ok = scatterLag(c);
  ok &= scatterPitch(c);
  return ok;
}

// Pitch drift of 20 cents on copy 1 over a 6 s tone at WIDTH 1. The pitch factor puts the walk's peak
// at 20 x 2/3 cents. Its f0 over each 0.5 s window after the first second stays within that of its
// pitch offset, with 3 cents of slack for the measurement, and differs from window to window. With
// the drift at 0 the walk never moves off 0, whatever its hold.
bool testDrift() {
  const Signal in = tone(220.0, 6 * kRate);
  const cv::CrowdTuning d;
  cv::CrowdParams p = lagProbe(0, d.lagScale);
  p.tuning.pitchSt[0] = d.pitchSt[0];
  p.tuning.driftCents = 20.0f;
  const Signal out = run(in, p);
  const double base = 220.0 * std::pow(2.0, static_cast<double>(d.pitchSt[0]) * kPitchTop / 12.0);
  double worst = 0.0;
  double lo = 1e9;
  double hi = -1e9;
  std::printf("info drift f0 per 0.5 s window, cents from the offset:");
  for (int k = 2; k < 12; ++k) {
    const double c = cents(peakHz(out, k * kRate / 2, kRate / 2, 2.0 * base) / 2.0, base);
    std::printf(" %+.1f", c);
    worst = std::max(worst, std::fabs(c));
    lo = std::min(lo, c);
    hi = std::max(hi, c);
  }
  std::printf("\n");
  bool ok = report("drift range", worst <= 20.0 * kPitchTop + 3.0 && hi - lo >= 2.0,
                   "worst %.2f cents from the offset (<= 20 x 2/3 + 3), spread %.2f cents (>= 2)", worst, hi - lo);

  const Signal prog = program(3);
  cv::CrowdParams a = knobs(0.7f, 0.5f);
  cv::CrowdParams b = a;
  b.tuning.driftHoldS = 0.3f;
  Probe probe;
  const Signal outA = run(prog, a, &probe);
  const Signal outB = run(prog, b);
  size_t differ = 0;
  for (size_t i = 0; i < outA.size(); ++i) differ += outA[i] != outB[i] ? 1 : 0;
  ok &= report("drift off", differ == 0 && probe.drift == 0.0f, "drift 0: hold 2 s against 0.3 s differs in %.0f samples (0), largest drift %.3g cents (0)",
               static_cast<double>(differ), static_cast<double>(probe.drift));
  return ok;
}

// The tracker finds a period with Autotune off because the Crowd card's flag keeps it running, and
// stays at 0 without the flag.
bool testTrackerRuns() {
  const Signal in = tone(220.0, kRate);
  int firstBlock[2] = {-1, -1};
  for (int flag = 0; flag < 2; ++flag) {
    cv::PitchFxParams pp;
    pp.octave.on = false;
    pp.autotune.on = false;
    pp.crowdOn = flag == 1;
    gFx.reset();
    float scratch[cv::kBlock];
    for (int i = 0, b = 0; i + cv::kBlock <= static_cast<int>(in.size()); i += cv::kBlock, ++b) {
      gFx.process(&in[static_cast<size_t>(i)], scratch, cv::kBlock, pp);
      if (gFx.pitch().period > 0.0f && firstBlock[flag] < 0) firstBlock[flag] = b;
    }
  }
  const double ms = firstBlock[1] < 0 ? -1.0 : 1000.0 * (firstBlock[1] + 1) * cv::kBlock / kRate;
  return report("tracker runs", firstBlock[1] >= 0 && ms <= 60.0 && firstBlock[0] < 0,
                "period found after %.1f ms with the flag (<= 60), %.0f blocks without it (-1 = never)", ms,
                firstBlock[0]);
}

// Autotune on, then off, with the Crowd card on throughout. The voice reads Autotune's correction
// while it is on and drops it once Autotune is off, so its f0 goes back to the sung pitch within
// 200 ms of the switch. The tone sits 46 cents sharp of A3 in key C, so the correction is not 0.
bool testCorrectionReleases() {
  constexpr double kSungHz = 226.0;
  constexpr double kNoteHz = 220.0;
  constexpr int kWindow = kRate / 5;  // 200 ms
  const int switchAt = kRate / cv::kBlock * cv::kBlock;
  const Signal in = tone(kSungHz, 2 * kRate);
  cv::PitchFxParams on = passThrough();
  on.autotune.on = true;
  const cv::PitchFxParams off = passThrough();
  const Signal out = runBoth(in, isolated(0.0f), isolated(0.0f), on, off, switchAt);

  cv::PitchTracker t;
  double beforeErr = 9999.0;
  double worstAfter = 0.0;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&out[static_cast<size_t>(i)], n);
    const int end = i + n;
    if (end == switchAt) beforeErr = t.result().voiced ? cents(t.result().hz, kNoteHz) : 9999.0;
    if (end >= switchAt + kWindow) {
      worstAfter = std::max(worstAfter, t.result().voiced ? std::fabs(cents(t.result().hz, kSungHz)) : 9999.0);
    }
  }
  return report("correction releases", std::fabs(beforeErr) <= 10.0 && worstAfter <= 10.0,
                "on: f0 %+.2f cents from the corrected note (+-10); 200 ms after off: worst %.2f cents from the sung pitch (+-10)",
                beforeErr, worstAfter);
}

// Enhancements: level scatter and contour. Each is neutral at 0 (contour 1), and each stays inside
// the range of a human voice.

// FNV-1a over the bits of the samples, to compare runs.
uint64_t fnv(const Signal& x) {
  uint64_t h = 0xCBF29CE484222325ULL;
  for (float v : x) {
    uint32_t b;
    std::memcpy(&b, &v, sizeof b);
    for (int k = 0; k < 4; ++k) {
      h ^= (b >> (8 * k)) & 0xFFu;
      h *= 0x100000001B3ULL;
    }
  }
  return h;
}

// Every enhancement on at once.
void allEnhancements(cv::CrowdTuning& t) {
  t.levelScatterDb = 4.0f;
  t.contour = {0.8f, 0.8f, 0.8f, 0.8f};
}

// What the four runs of testEnhancementsDefaultExact hash to (Mac build), pinned for the eight-copy
// card (owner 2026-11-01): four raised copies 1.75 st over and 75 ms behind the tuned ones, every copy
// at the four-copy gain over sqrt 2, the ring at 225 ms and the trims set for the level rule, on the
// tracker of the pitch CPU pass (four hop sums, v2.00.78). Runs 1 to 3 start from knobs(), which
// neutralizes the human-voice set. Run 4 plays the shipped CrowdTuning{} unchanged. The engine must
// reproduce them to the bit. Update them only when the sound of a copy changes on purpose. Run 3 was
// re-pinned 2026-11-01 because its scatter moved from 15 to 12 ms when the tuning range became the
// ring's allowance, not for an engine change.
constexpr std::array<uint64_t, 4> kPinned = {0x9d220333937cb62bULL, 0xd50d29e677f4651fULL, 0x8d7fc1ce41837fb8ULL,
                                             0xbd5aac8433058c41ULL};

// Each run's hash against its pinned value.
bool checkPinned(const std::array<uint64_t, 4>& hashes) {
  const char* what[4] = {"180 Hz program VOICES 70 WIDTH 50", "burst program VOICES 80 WIDTH 50",
                         "burst program VOICES 60 WIDTH 80 scatter 12 drift 20",
                         "180 Hz program VOICES 70 WIDTH 50 on the shipped tuning"};
  bool ok = true;
  for (size_t i = 0; i < hashes.size(); ++i) {
    char name[40];
    std::snprintf(name, sizeof name, "default hash, run %zu", i + 1);
    const bool same = hashes[i] == kPinned[i];
    std::printf("info hash run %zu, %s: pinned %016llx, engine %016llx\n", i + 1, what[i],
                static_cast<unsigned long long>(kPinned[i]), static_cast<unsigned long long>(hashes[i]));
    ok &= report(name, same, "output hash equals the pinned value: %.0f (1)", same ? 1.0 : 0.0);
  }
  return ok;
}

// With every enhancement neutral the engine runs the code it ran without them, and a setting that
// only matters while its control is on moves no sample. The four recorded runs hash to the values
// pinned above.
bool testEnhancementsDefaultExact() {
  const Signal prog = program(3);
  const Signal burst = bursts(220.0, Train{300, 300, 120, 10});
  const cv::CrowdParams base = knobs(0.7f, 0.5f);
  cv::CrowdParams idle = base;
  idle.tuning.levelScatterDb = -1.0f; // clamps to 0
  idle.tuning.contour = {1.0f, 1.0f, 1.0f, 1.0f};
  const Signal a = run(prog, base);
  const Signal b = run(prog, idle);
  cv::CrowdParams burstBase = knobs(0.6f, 0.8f);
  burstBase.tuning.scatterMs = 12.0f;
  burstBase.tuning.driftCents = 20.0f;
  cv::CrowdParams burstIdle = burstBase;
  burstIdle.tuning = idle.tuning;
  burstIdle.tuning.scatterMs = 12.0f;
  burstIdle.tuning.driftCents = 20.0f;
  const Signal c = run(burst, burstBase);
  const Signal d = run(burst, burstIdle);
  const size_t diffProg = differing(a, b);
  const size_t diffBurst = differing(c, d);
  cv::CrowdParams shipped = base;
  shipped.tuning = cv::CrowdTuning{};
  const bool ok = report("enhancements neutral", diffProg == 0 && diffBurst == 0,
                         "inactive settings move %.0f samples of the 180 Hz program and %.0f of the burst program (0)",
                         static_cast<double>(diffProg), static_cast<double>(diffBurst));
  return checkPinned({fnv(a), fnv(run(burst, knobs(0.8f, 0.5f))), fnv(c), fnv(run(prog, shipped))}) && ok;
}

// The shipped defaults are the owner's working set (2026-10-31), and a run on them stays finite and
// within 2.5 dB of the neutral card's level.
bool testWorkingSetDefaults() {
  const cv::CrowdTuning d;
  const bool values = d.contour[0] == 0.8f && d.contour[1] == 1.2f && d.contour[2] == 0.7f && d.contour[3] == 1.1f &&
                      d.levelScatterDb == 2.0f && d.scatterMs == 12.0f && d.driftCents == 15.0f &&
                      d.widthKnee == 0.1f && d.widthKneeFrac == 0.012f && d.widthLagBottom == 0.5f &&
                      d.widthLagMs == 20.0f;
  bool ok = report("working set values", values,
                   "contour %.1f %.1f %.1f %.1f (0.8 1.2 0.7 1.1)", static_cast<double>(d.contour[0]),
                   static_cast<double>(d.contour[1]), static_cast<double>(d.contour[2]), static_cast<double>(d.contour[3]));
  const Signal in = program(3);
  cv::CrowdParams worked = knobs(0.7f, 0.5f);
  worked.tuning = d;
  const Signal a = run(in, knobs(0.7f, 0.5f));
  const Signal b = run(in, worked);
  bool finite = true;
  for (float x : b) finite &= std::isfinite(x) && std::fabs(x) < 4.0f;
  const double dbA = levelDb(in, a);
  const double dbB = levelDb(in, b);
  ok &= report("working set run", finite && std::fabs(dbB - dbA) <= 2.5 && differing(a, b) > 0,
               "finite %.0f, level %+.2f dB against the neutral card's %+.2f dB (within 2.5)", finite ? 1.0 : 0.0, dbB, dbA);
  return ok;
}

// RMS of x over [from, to).
double rmsIn(const Signal& x, int from, int to) { return rms(x, static_cast<size_t>(from), static_cast<size_t>(to)); }

// Level scatter of 3 dB on copy 4 over ten bursts. Each burst of the scattered run is compared with
// the same burst of the run with no scatter, so the ratio is the gain the gap drew. Every ratio
// stays inside +-3 dB, and the ten are not all the same.
struct LevelCase {
  Train tr{300, 300, 120, 10};
  Signal in;
  cv::CrowdParams flat;
  double lagMs = 0.0;  // copy 4's lag in ms
  Signal ref;          // the run with no scatter
};

LevelCase levelCase() {
  LevelCase c;
  c.in = bursts(220.0, c.tr);
  const cv::CrowdTuning d;
  c.lagMs = lagMsAt(3, 1.0, static_cast<double>(d.lagScale));
  c.flat = lagProbe(3, d.lagScale);
  c.ref = run(c.in, c.flat);
  return c;
}

bool levelScatterSpread(const LevelCase& c) {
  cv::CrowdParams scattered = c.flat;
  scattered.tuning.levelScatterDb = 3.0f;
  const Signal out = run(c.in, scattered);
  double lo = 1e9;
  double hi = -1e9;
  std::printf("info level scatter per burst dB re no scatter:");
  for (int k = 0; k < c.tr.count; ++k) {
    const int from = c.tr.start(k) + msToSamples(c.lagMs + 30.0);
    const int to = from + msToSamples(150.0);
    const double db = 20.0 * std::log10(rmsIn(out, from, to) / rmsIn(c.ref, from, to));
    std::printf(" %+.2f", db);
    lo = std::min(lo, db);
    hi = std::max(hi, db);
  }
  std::printf("\n");
  return report("level scatter", lo >= -3.1 && hi <= 3.1 && hi - lo >= 1.0,
                "per-burst gain %+.2f to %+.2f dB (inside +-3), spread %.2f dB (>= 1)", lo, hi, hi - lo);
}

// Scatter off, and a scatter that clamps to off, give the same samples.
bool levelScatterOff(const LevelCase& c) {
  cv::CrowdParams off = c.flat;
  off.tuning.levelScatterDb = 0.0f;
  cv::CrowdParams neg = c.flat;
  neg.tuning.levelScatterDb = -2.0f;
  const size_t differOff = differing(c.ref, run(c.in, off));
  const size_t differNeg = differing(c.ref, run(c.in, neg));
  return report("level scatter off", differOff == 0 && differNeg == 0,
                "0 dB and -2 dB differ from the default run in %.0f samples (0)", static_cast<double>(differNeg));
}

// The range stops at 4 dB, so 12 dB asked for is no wider than 4.
bool levelScatterRange(const LevelCase& c) {
  cv::CrowdParams wide = c.flat;
  wide.tuning.levelScatterDb = 12.0f;
  cv::CrowdParams four = c.flat;
  four.tuning.levelScatterDb = 4.0f;
  const size_t differ = differing(run(c.in, wide), run(c.in, four));
  return report("level scatter range", differ == 0, "12 dB asked for differs from 4 dB in %.0f samples (0)",
                static_cast<double>(differ));
}

bool testLevelScatter() {
  const LevelCase c = levelCase();
  bool ok = levelScatterSpread(c);
  ok &= levelScatterOff(c);
  ok &= levelScatterRange(c);
  return ok;
}

// A tone whose pitch swings sweepSt semitones either side of hz at sweepHz, ten harmonics at 1/k.
Signal sweepTone(double hz, double sweepSt, double sweepHz, int n) {
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double phase = 0.0;
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    const double f = hz * std::pow(2.0, sweepSt * std::sin(2.0 * kPi * sweepHz * i / kRate) / 12.0);
    phase += 2.0 * kPi * f / kRate;
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(k * phase) / k;
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  Signal out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

// Depth in semitones of the sweepHz component of x's pitch over `cycles` whole cycles from `fromS`,
// by a lock-in on the tracker's f0.
double sweepDepthSt(const Signal& x, double sweepHz, double fromS, int cycles) {
  cv::PitchTracker t;
  const int from = static_cast<int>(fromS * kRate);
  const int to = from + static_cast<int>(cycles / sweepHz * kRate);
  double re = 0.0;
  double im = 0.0;
  int count = 0;
  for (int i = 0; i + cv::kBlock <= static_cast<int>(x.size()) && i + cv::kBlock <= to; i += cv::kBlock) {
    t.push(&x[static_cast<size_t>(i)], cv::kBlock);
    const int end = i + cv::kBlock;
    if (end <= from || !t.result().voiced) continue;
    const double st = 12.0 * std::log2(t.result().hz / 220.0);
    const double ph = 2.0 * kPi * sweepHz * end / kRate;
    re += st * std::cos(ph);
    im += st * std::sin(ph);
    ++count;
  }
  return count > 0 ? 2.0 * std::hypot(re, im) / count : 0.0;
}

// Copy 1 alone at WIDTH 1, with contour c, on a 220 Hz tone swinging 1 st either side. The copy's pitch
// swing against the input's is the contour pulled toward 1 by the pitch factor at the top of WIDTH,
// 1 + (c - 1) x 2/3. The sweep runs at 1 Hz and at 3 Hz, since the copy's lag is a fifth of a cycle
// at 3 Hz and a copy that read the deviation its lag ago would miss by that much.
bool testContour() {
  bool ok = true;
  for (double sweepHz : {1.0, 3.0}) {
    const Signal in = sweepTone(220.0, 1.0, sweepHz, 7 * kRate);
    const double inSt = sweepDepthSt(in, sweepHz, 2.0, static_cast<int>(3.0 * sweepHz));
    for (float c : {0.6f, 1.0f, 1.4f}) {
      cv::CrowdParams p = lagProbe(0, cv::CrowdTuning{}.lagScale);
      p.tuning.contour[0] = c;
      const Signal out = run(in, p);
      const double outSt = sweepDepthSt(out, sweepHz, 2.0, static_cast<int>(3.0 * sweepHz));
      const double ratio = outSt / inSt;
      const double tol = c == 1.0f ? 0.10 : 0.15;
      const double want = 1.0 + (static_cast<double>(c) - 1.0) * pitchFactor(1.0);
      char name[48];
      std::snprintf(name, sizeof name, "contour %.1f at %.0f Hz", static_cast<double>(c), sweepHz);
      ok &= report(name, std::fabs(ratio / want - 1.0) <= tol,
                   "input swing %.3f st, copy swing %.3f st, ratio %.3f (want %.3f within 10-15 %%)", inSt, outSt, ratio,
                   want);
    }
  }

  // The range stops at 0.6 and 1.4, so a contour asked for beyond them is no wider.
  const Signal in = sweepTone(220.0, 1.0, 1.0, 3 * kRate);
  size_t beyond = 0;
  for (const auto& pair : {std::make_pair(3.0f, 1.4f), std::make_pair(0.0f, 0.6f)}) {
    cv::CrowdParams asked = lagProbe(0, cv::CrowdTuning{}.lagScale);
    cv::CrowdParams limit = asked;
    asked.tuning.contour[0] = pair.first;
    limit.tuning.contour[0] = pair.second;
    beyond += differing(run(in, asked), run(in, limit));
  }
  ok &= report("contour range", beyond == 0, "contour 3.0 and 0.0 differ from 1.4 and 0.6 in %.0f samples (0)",
               static_cast<double>(beyond));
  return ok;
}

// Copy v alone with the shipped human-voice set (timing scatter, drift, level scatter and contour),
// at WIDTH `width`.
cv::CrowdParams humanCopy(size_t v, float width) {
  const cv::CrowdTuning d;
  cv::CrowdParams p = alone(v);
  p.width = width;
  p.tuning.scatterMs = d.scatterMs;
  p.tuning.driftCents = d.driftCents;
  p.tuning.levelScatterDb = d.levelScatterDb;
  p.tuning.contour[0] = d.contour[v];
  return p;
}

// f0 of a copy in each 0.5 s window from second 1 to second 6, in cents from `base`.
std::vector<double> windowCents(const Signal& out, double base) {
  std::vector<double> c;
  for (int k = 2; k < 12; ++k) c.push_back(cents(peakHz(out, k * kRate / 2, kRate / 2, 2.0 * base) / 2.0, base));
  return c;
}

// The bottom tenth of WIDTH is for a melodic line, so a copy there is a plain double on the lead's
// pitch. The -2.5 st copy on the shipped set sits within 1 cent of the lead at WIDTH 0, 2.0 +- 1.5 cents
// flat of it at 0.1, and 166.7 +- 10 cents flat at 1 (two thirds of its offset). Drift scales with the
// pitch factor, so at WIDTH 0.05 a copy's f0 moves less than 1 cent from one 0.5 s window to the next,
// and at WIDTH 1 it moves as the drift test expects. Contour scales with the same factor, so at WIDTH
// 0.05 a copy with contour 0.6 or 1.4 swings within 2 % of the input's swing, and at WIDTH 1 within the
// contour test's bounds of 1 + (contour - 1) x 2/3.
bool melodicF0() {
  bool ok = true;
  const Signal tn = tone(220.0, 2 * kRate);
  const cv::CrowdTuning d;
  const double full = static_cast<double>(d.pitchSt[2]);
  const struct {
    float width;
    double wantCents;
    double tolCents;
  } rows[] = {{0.0f, 0.0, 1.0}, {0.1f, -2.0, 1.5}, {1.0f, 100.0 * full * pitchFactor(1.0), 10.0}};
  double measured[3] = {0.0, 0.0, 0.0};
  size_t i = 0;
  for (const auto& r : rows) {
    const double near = 220.0 * std::pow(2.0, full * pitchFactor(static_cast<double>(r.width)) / 12.0);
    measured[i] = cents(copyHz(run(tn, humanCopy(2, r.width)), near), 220.0);
    char name[48];
    std::snprintf(name, sizeof name, "melodic f0, WIDTH %.0f %%", static_cast<double>(r.width) * 100.0);
    ok &= report(name, std::fabs(measured[i] - r.wantCents) <= r.tolCents,
                 "-2.5 st copy f0 %+.2f cents from the lead, want %+.1f (+-%.0f)", measured[i], r.wantCents, r.tolCents);
    ++i;
  }
  const double nearHalf = 220.0 * std::pow(2.0, full * pitchFactor(0.5) / 12.0);
  const double halfCents = cents(copyHz(run(tn, humanCopy(2, 0.5f)), nearHalf), 220.0);
  std::printf("info melodic f0 from the lead, cents, -2.5 st copy, drift on: WIDTH 0 %+.2f, 0.1 %+.2f, 0.5 %+.2f, 1 %+.2f\n",
              measured[0], measured[1], halfCents, measured[2]);
  return ok;
}

// Copy 1 alone on a 6 s tone with 15 cents of drift at one WIDTH, and what its f0 did across the
// 0.5 s windows after the first second.
struct DriftRun {
  double move = 0.0;    // largest change of f0 from one window to the next, in cents
  double spread = 0.0;  // the highest window less the lowest
  double peak = 0.0;    // largest distance of a window from the copy's offset
  double drift = 0.0;   // largest drift any copy showed at the end of a block
};

DriftRun driftRun(const Signal& long6, float width) {
  const cv::CrowdTuning d;
  cv::CrowdParams p = lagProbe(0, d.lagScale);
  p.width = width;
  p.tuning.pitchSt[0] = d.pitchSt[0];
  p.tuning.driftCents = 15.0f;
  Probe probe;
  const Signal out = run(long6, p, &probe);
  const double base = 220.0 * std::pow(2.0, static_cast<double>(d.pitchSt[0]) * pitchFactor(static_cast<double>(width)) / 12.0);
  const std::vector<double> c = windowCents(out, base);
  DriftRun r;
  for (size_t k = 1; k < c.size(); ++k) r.move = std::max(r.move, std::fabs(c[k] - c[k - 1]));
  r.spread = *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end());
  r.peak = std::fabs(*std::max_element(c.begin(), c.end(), [](double a, double b) { return std::fabs(a) < std::fabs(b); }));
  r.drift = static_cast<double>(probe.drift);
  return r;
}

bool melodicDrift() {
  constexpr float kBottom = 0.05f;
  const Signal long6 = tone(220.0, 6 * kRate);
  const DriftRun bottom = driftRun(long6, kBottom);
  const DriftRun top = driftRun(long6, 1.0f);
  const double factor = pitchFactor(static_cast<double>(kBottom));
  const double limit = 15.0 * factor;
  bool ok = report("melodic drift range", bottom.drift <= limit + 1e-3,
                   "largest drift %.3f cents against 15 x %.4f = %.3f (<=)", bottom.drift, factor, limit);
  ok &= report("melodic drift, WIDTH 5 %", bottom.move < 1.0, "f0 moves at most %.2f cents from window to window (< 1)",
               bottom.move);
  ok &= report("drift at WIDTH 100 %", top.peak <= 13.0 && top.spread >= 2.0,
               "worst %.2f cents from the offset (<= 10 + 3), spread %.2f cents (>= 2)", top.peak, top.spread);
  return ok;
}

bool melodicContour() {
  const cv::CrowdTuning d;
  bool ok = true;
  for (double sweepHz : {1.0, 3.0}) {
    const Signal in = sweepTone(220.0, 1.0, sweepHz, 7 * kRate);
    const double inSt = sweepDepthSt(in, sweepHz, 2.0, static_cast<int>(3.0 * sweepHz));
    const struct {
      float width;
      float contour;
      double tol;
    } cases[] = {{0.05f, 0.6f, 0.02}, {0.05f, 1.4f, 0.02}, {1.0f, 0.6f, 0.15}, {1.0f, 1.4f, 0.15}};
    for (const auto& k : cases) {
      cv::CrowdParams p = lagProbe(0, d.lagScale);
      p.width = k.width;
      p.tuning.contour[0] = k.contour;
      const double ratio = sweepDepthSt(run(in, p), sweepHz, 2.0, static_cast<int>(3.0 * sweepHz)) / inSt;
      const double want = k.width < 1.0f ? 1.0 : 1.0 + (static_cast<double>(k.contour) - 1.0) * pitchFactor(1.0);
      char name[56];
      std::snprintf(name, sizeof name, "melodic contour %.1f, WIDTH %.0f %%, %.0f Hz", static_cast<double>(k.contour),
                    static_cast<double>(k.width) * 100.0, sweepHz);
      ok &= report(name, std::fabs(ratio / want - 1.0) <= k.tol, "copy swing %.3f of the input's, want %.2f (within %.2f)",
                   ratio, want, k.tol);
    }
  }
  return ok;
}

bool testMelodicBottom() {
  bool ok = melodicF0();
  ok &= melodicDrift();
  ok &= melodicContour();
  return ok;
}

// A WIDTH step moves the copy's grain playback rate over a few blocks and not in one jump. 512
// samples after a step from 0.1 to 1 the rate sits strictly between its two settled values, and a
// second later it equals the settled value of a run that held WIDTH 1 throughout, bit for bit.
bool testWidthGlides() {
  const Signal tn = tone(220.0, 2 * kRate);
  const int switchAt = kRate / cv::kBlock * cv::kBlock;
  cv::CrowdParams lo = alone(2);
  lo.width = 0.1f;
  cv::CrowdParams hi = alone(2);
  hi.width = 1.0f;
  run(tn, lo);
  const double rateLo = static_cast<double>(gCrowd.grainRate(0));
  run(tn, hi);
  const double rateHi = static_cast<double>(gCrowd.grainRate(0));
  const Signal soon(tn.begin(), tn.begin() + switchAt + 512);
  runSwitch(soon, lo, hi, switchAt);
  const double rateSoon = static_cast<double>(gCrowd.grainRate(0));
  runSwitch(tn, lo, hi, switchAt);
  const double rateLate = static_cast<double>(gCrowd.grainRate(0));
  return report("width glides", rateSoon < rateLo && rateSoon > rateHi && rateLate == rateHi,
                "grain rate %.5f at WIDTH 0.1, %.5f at 1; 512 samples after the step %.5f, a second later %.5f (equals the settled)",
                rateLo, rateHi, rateSoon, rateLate);
}

// Every copy alone on 220 Hz bursts, at WIDTH 0 and at WIDTH 1, sits at the lag of the WIDTH law: 24,
// 60, 42 and 45 ms at the bottom, 56, 140, 98 and 105 ms at the top (the longest copy 20 ms further
// back than 120 ms, the others in proportion), each within 0.5 ms.
constexpr double kWantLagMs[2][4] = {{24.0, 60.0, 42.0, 45.0}, {56.0, 140.0, 98.0, 105.0}};

bool widthLagRows() {
  const Train tr{300, 300, 120, 3};
  const Signal in = bursts(220.0, tr);
  const cv::CrowdTuning d;
  bool ok = true;
  for (int end = 0; end < 2; ++end) {
    const double* wantMs = kWantLagMs[end];
    double lag[4];
    double worst = 0.0;
    for (size_t v = 0; v < 4; ++v) {
      const Signal out = run(in, lagProbe(v, d.lagScale, static_cast<float>(end)));
      lag[v] = measuredLagMs(in, out, tr.start(1) + msToSamples(wantMs[v] + 30.0), wantMs[v]);
      worst = std::max(worst, std::fabs(lag[v] - wantMs[v]));
    }
    char name[40];
    std::snprintf(name, sizeof name, "width lag, WIDTH %d %%", end * 100);
    ok &= report(name, worst <= 0.5, "lags %.2f %.2f %.2f %.2f ms, each within 0.5 ms of the law", lag[0], lag[1], lag[2],
                 lag[3]);
    char law[40];
    std::snprintf(law, sizeof law, "width lag law, WIDTH %d %%", end * 100);
    double lawErr = 0.0;
    for (size_t v = 0; v < 4; ++v) lawErr = std::max(lawErr, std::fabs(lagMsAt(v, static_cast<double>(end), d.lagScale) - wantMs[v]));
    ok &= report(law, lawErr <= 0.01, "the test's law gives %.0f / %.0f / %.0f / %.0f ms to within 0.01 ms", wantMs[0],
                 wantMs[1], wantMs[2], wantMs[3]);
  }
  return ok;
}

// A WIDTH move from 0 to 1 in the middle of a burst changes the lag in the next gap and not before.
// Copy 2 goes from 60 to 140 ms, a move of 80 ms inside a 120 ms gap.
bool widthLagLandsInGap() {
  const cv::CrowdTuning d;
  const size_t v = 1;
  const Train five{300, 300, 120, 5};
  const Signal run5 = bursts(220.0, five);
  const int switchAt = (five.start(1) + msToSamples(120.0)) / cv::kBlock * cv::kBlock;
  const Signal out = runSwitch(run5, lagProbe(v, d.lagScale, 0.0f), lagProbe(v, d.lagScale, 1.0f), switchAt);
  double worst = 0.0;
  double lag[5];
  for (int k = 0; k < five.count; ++k) {
    const double want = k <= 1 ? kWantLagMs[0][v] : kWantLagMs[1][v];
    const int from = k == 1 ? switchAt + msToSamples(20.0) : five.start(k) + msToSamples(want + 30.0);
    lag[k] = measuredLagMs(run5, out, from, want);
    worst = std::max(worst, std::fabs(lag[k] - want));
  }
  std::printf("info width lag copy 2, WIDTH 0 to 1 mid-burst, lag per burst %.2f %.2f(mid-burst move) %.2f %.2f %.2f ms\n", lag[0],
              lag[1], lag[2], lag[3], lag[4]);
  return report("width lag lands in the gap", worst <= 0.5,
                "60 ms until the end of burst 2, 140 ms from burst 3; worst error %.2f ms (<= 0.5)", worst);
}

bool testWidthLag() {
  bool ok = widthLagRows();
  ok &= widthLagLandsInGap();
  return ok;
}

// At WIDTH 0 a copy is the input delayed by its lag and nothing else. Each copy alone at the top of
// VOICES, on the shipped set (offsets, drift, contour, level scatter and timing scatter on), has every
// sample of a steady tone equal to the gain-scaled input at the lag the copy took. The tone shelf is
// flat and the trim is off, and the reference goes through the same low cut, since that is a filter
// on the copy and not the slicing. The lag comes from a search over the input read by linear
// interpolation as the engine reads its ring, within half a period of the lag the engine reports
// (timing scatter makes it fractional), and the gain by least squares (level scatter draws one per
// copy). A copy at WIDTH 0.1 is the control: 2.0 cents of pitch offset moves it off the delayed input
// by far more.
struct Shift {
  double lag = 0.0;   // samples
  double gain = 0.0;
  double worst = 0.0;  // largest |out - gain * in(t - lag)| over the window
  double peak = 0.0;   // largest |out| over the window
};

double lerpAt(const Signal& x, double pos) {
  const double fl = std::floor(pos);
  const size_t i = static_cast<size_t>(fl);
  return x[i] + (pos - fl) * (static_cast<double>(x[i + 1]) - static_cast<double>(x[i]));
}

// Sum of squares left by out[t] - g * in(t - lag) over [from, to) at the best g, and that g.
double leftOver(const Signal& in, const Signal& out, int from, int to, double lag, double* gain) {
  double num = 0.0;
  double den = 0.0;
  double oo = 0.0;
  for (int t = from; t < to; ++t) {
    const double r = lerpAt(in, t - lag);
    const double o = out[static_cast<size_t>(t)];
    num += o * r;
    den += r * r;
    oo += o * o;
  }
  const double g = den > 0.0 ? num / den : 0.0;
  if (gain) *gain = g;
  return oo - g * num;
}

Shift fitShift(const Signal& in, const Signal& out, int from, int to, double nearLag, int halfRange) {
  double best = 1e300;
  int at = 0;
  for (int lag = static_cast<int>(nearLag) - halfRange; lag <= static_cast<int>(nearLag) + halfRange; ++lag) {
    const double r = leftOver(in, out, from, to, lag, nullptr);
    if (r < best) {
      best = r;
      at = lag;
    }
  }
  double lo = at - 1.0;
  double hi = at + 1.0;
  constexpr double kGolden = 0.6180339887498949;
  for (int i = 0; i < 40; ++i) {
    const double a = hi - kGolden * (hi - lo);
    const double b = lo + kGolden * (hi - lo);
    if (leftOver(in, out, from, to, a, nullptr) < leftOver(in, out, from, to, b, nullptr)) {
      hi = b;
    } else {
      lo = a;
    }
  }
  Shift s;
  s.lag = 0.5 * (lo + hi);
  leftOver(in, out, from, to, s.lag, &s.gain);
  for (int t = from; t < to; ++t) {
    s.worst = std::max(s.worst, std::fabs(out[static_cast<size_t>(t)] - s.gain * lerpAt(in, t - s.lag)));
    s.peak = std::max(s.peak, static_cast<double>(std::fabs(out[static_cast<size_t>(t)])));
  }
  return s;
}

cv::CrowdParams delayedCopy(size_t v, float width) {
  cv::CrowdParams p = knobs(1.0f, width);
  p.tuning = cv::CrowdTuning{};
  p.tuning.shelfDb = {0.0f, 0.0f, 0.0f, 0.0f};
  p.tuning.trimDb = 0.0f;
  p.tuning.trimBottomDb = 0.0f;
  p.solo = static_cast<uint8_t>(1u << v);
  return p;
}

// The input through the filter the copy's output passes, the low cut, so the engine's output compares
// with this signal delayed. A flat tone shelf takes weights of exactly 0 and adds nothing.
Signal throughOutputFilters(const Signal& in, const cv::CrowdTuning& t) {
  cv::Biquad cut;
  cut.setHighPass(t.hpHz, cv::kButterworthQ);
  Signal out(in.size());
  for (size_t i = 0; i < in.size(); ++i) out[i] = cut.run(in[i]);
  return out;
}

bool testWidthZeroIsDelay() {
  constexpr double kToneHz = 220.0;
  constexpr int kHalfRange = 109;  // half a period of the tone, in samples

  // What is left is the float rounding of the 120 Hz low cut, about 5e-5 on a 0.7 peak.
  constexpr double kLimit = 1e-4;
  const Signal in = tone(kToneHz, 2 * kRate);
  const int from = kRate / 2;
  const int to = from + kRate / 5;
  bool ok = true;
  double worst = 0.0;
  for (size_t v = 0; v < 4; ++v) {
    const cv::CrowdParams p = delayedCopy(v, 0.0f);
    const Signal out = run(in, p);
    const double engineLag = static_cast<double>(gCrowd.lagSamples(v));
    const Shift s = fitShift(throughOutputFilters(in, p.tuning), out, from, to, engineLag, kHalfRange);
    worst = std::max(worst, s.worst);
    char name[56];
    std::snprintf(name, sizeof name, "width 0 is a delay, copy %zu", v + 1);
    ok &= report(name, s.worst <= kLimit && s.peak > 0.2,
                 "engine lag %.3f samples, fitted lag %.3f, gain %.4f, worst |out - gain x delayed input| %.3g", engineLag,
                 s.lag, s.gain, s.worst);
  }

  // The control: the -2.5 st copy at WIDTH 0.1 is 2.0 cents off the lead and drifts away from any delayed
  // copy of it.
  const cv::CrowdParams pc = delayedCopy(2, 0.1f);
  const Signal out = run(in, pc);
  const Shift control =
      fitShift(throughOutputFilters(in, pc.tuning), out, from, to, static_cast<double>(gCrowd.lagSamples(2)), kHalfRange);
  ok &= report("width 0.1 is not a delay", control.worst > 100.0 * kLimit,
               "WIDTH 0.1, copy 3: worst |out - gain x delayed input| %.3g against %.3g at WIDTH 0", control.worst, worst);
  return ok;
}

// (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2) at hz, in double.
std::complex<double> biquadAt(double b0, double b1, double b2, double a1, double a2, double hz) {
  const std::complex<double> z = std::polar(1.0, -2.0 * kPi * hz / kRate);
  return (b0 + z * (b1 + z * b2)) / (1.0 + z * (a1 + z * a2));
}

// The four copies share one shelf filter. At the default tuning each copy's 1 + wh HP + wb BP stays
// within 0.15 dB and 1 degree of the RBJ high shelf it stands in for, from 20 Hz to 20 kHz, and a
// 0 dB shelf takes weights of exactly 0.
bool testSharedShelf() {
  namespace cd = cv::crowd_detail;
  const cv::CrowdTuning d;
  const cd::ShelfBasis b = cd::shelfBasis(d.shelfHz);
  double worstDb = 0.0;
  double worstDeg = 0.0;
  for (size_t v = 0; v < 4; ++v) {
    const cd::ShelfWeights w = cd::fitShelf(b, d.shelfHz, d.shelfDb[v]);
    cv::Biquad ref;
    ref.setHighShelf(d.shelfHz, d.shelfDb[v]);
    for (double hz = 20.0; hz <= 20000.0; hz *= 1.005) {
      const std::complex<double> high = biquadAt(b.h0, b.h1, b.h0, b.a1, b.a2, hz);
      const std::complex<double> band = biquadAt(b.q0, 0.0, -b.q0, b.a1, b.a2, hz);
      const std::complex<double> shared = 1.0 + static_cast<double>(w.high) * high + static_cast<double>(w.band) * band;
      const std::complex<double> ratio = shared / biquadAt(ref.b0, ref.b1, ref.b2, ref.a1, ref.a2, hz);
      worstDb = std::max(worstDb, std::fabs(20.0 * std::log10(std::abs(ratio))));
      worstDeg = std::max(worstDeg, std::fabs(std::arg(ratio)) * 180.0 / kPi);
    }
  }
  const cd::ShelfWeights flat = cd::fitShelf(b, d.shelfHz, 0.0f);
  const bool zero = flat.high == 0.0f && flat.band == 0.0f;
  return report("shared shelf", worstDb <= 0.15 && worstDeg <= 1.0 && zero,
                "four default shelves: worst %.3f dB (<= 0.15) and %.3f deg (<= 1) from 20 Hz to 20 kHz; 0 dB weights "
                "exactly 0: %.0f (1)",
                worstDb, worstDeg, zero ? 1.0 : 0.0);
}

// Every enhancement at its limit on the program and on the burst program: finite, bounded, and within
// 2.5 dB of the plain card.
bool testEnhancementsStable() {
  bool ok = true;
  for (int which = 0; which < 2; ++which) {
    const Signal in = which == 0 ? program(3) : bursts(220.0, Train{300, 300, 120, 10});
    cv::CrowdParams plain = knobs(0.7f, 0.5f);
    cv::CrowdParams all = plain;
    allEnhancements(all.tuning);
    all.tuning.contour = {0.6f, 1.4f, 0.6f, 1.4f};
    const Signal a = run(in, plain);
    const Signal b = run(in, all);
    bool finite = true;
    float peak = 0.0f;
    for (float v : b) {
      finite &= std::isfinite(v);
      peak = std::max(peak, std::fabs(v));
    }
    const size_t from = static_cast<size_t>(kRate);
    const double db = 20.0 * std::log10(rms(b, from, b.size()) / rms(a, from, a.size()));
    ok &= report(which == 0 ? "enhancements stable, program" : "enhancements stable, bursts",
                 finite && peak < 4.0f && std::fabs(db) <= 2.5, "finite %.0f, peak %.2f (< 4), level %+.2f dB re the plain card (+-2.5)",
                 finite ? 1.0 : 0.0, static_cast<double>(peak), db);
  }
  return ok;
}

// The correctness pass (owner 2026-10-31).

// Largest |x[k + 1] - x[k]| for k in [from, to - 1).
double maxStep(const Signal& x, int from, int to) {
  double m = 0.0;
  for (int k = from; k + 1 < to; ++k)
    m = std::max(m, static_cast<double>(std::fabs(x[static_cast<size_t>(k + 1)] - x[static_cast<size_t>(k)])));
  return m;
}

// The lowest out/in level over five 6 ms windows from `at`, in dB.
double lowestWindowDb(const Signal& in, const Signal& out, int at) {
  const int w = msToSamples(6.0);
  double low = 1e9;
  for (int k = 0; k < 5; ++k) {
    const size_t a = static_cast<size_t>(at + k * w);
    low = std::min(low, 20.0 * std::log10(rms(out, a, a + static_cast<size_t>(w)) / rms(in, a, a + static_cast<size_t>(w))));
  }
  return low;
}

// The card switched with crowdOn, as the registry and the emulator switch them, on the shipped tuning at
// WIDTH 1. The switch sits 50 ms after a phrase peak, so the 100 ms before it holds the peak's steps. Off:
// the largest step in the ramp block stays under the largest in the 100 ms before, because the copies
// keep the held period and ramp out. On: the same over the 10 ms the tracker takes to find a period and
// the ramp, and every 6 ms window of the 30 ms after the switch stays within 6 dB of the input, because
// the lead holds until the copies can play.
bool testSwitchWithTracker() {
  const Signal in = program(3);
  const int sw = 975 * cv::kBlock;
  bool ok = true;
  for (float voices : {1.0f, 0.5f}) {
    for (bool turnOn : {false, true}) {
      cv::CrowdParams on;
      on.on = true;
      on.voices = voices;
      cv::CrowdParams off = on;
      off.on = false;
      cv::PitchFxParams pOff = passThrough();
      pOff.crowdOn = false;
      const Signal out = turnOn ? runBoth(in, off, on, pOff, passThrough(), sw) : runBoth(in, on, off, passThrough(), pOff, sw);
      const double before = maxStep(out, sw - kRate / 10, sw);
      const double at = maxStep(out, sw - 1, sw + (turnOn ? msToSamples(10.0) : cv::kBlock + 1));
      const double low = turnOn ? lowestWindowDb(in, out, sw) : 0.0;
      char name[56];
      std::snprintf(name, sizeof name, "switch %s with the tracker, VOICES %.0f %%", turnOn ? "on" : "off",
                    static_cast<double>(voices) * 100.0);
      ok &= report(name, at <= before && low >= -6.0,
                   "largest step at the switch %.4f, in the 100 ms before %.4f; lowest 6 ms window %+.1f dB (>= -6)", at,
                   before, low);
    }
  }
  return ok;
}

// Lag landings per copy at VOICES 1 on the shipped tuning, WIDTH `from` before `switchAt` and `to` from
// it. The first landing counts.
struct Landings {
  std::array<int, 4> total{};
  std::array<int, 4> after{};
  std::array<int, 4> firstAfter{-1, -1, -1, -1};  // samples from the switch to the first landing
  std::array<float, 4> lag{};
};

Landings countLandings(const Signal& in, float from, float to, int switchAt) {
  gFx.reset();
  gCrowd.reset();
  Landings l;
  Signal out(cv::kBlock);
  float scratch[cv::kBlock];
  cv::CrowdParams p;
  p.on = true;
  p.voices = 1.0f;
  for (int i = 0; i + cv::kBlock <= static_cast<int>(in.size()); i += cv::kBlock) {
    p.width = i < switchAt ? from : to;
    gInProcess = true;
    gFx.process(&in[static_cast<size_t>(i)], scratch, cv::kBlock, passThrough());
    gCrowd.process(&in[static_cast<size_t>(i)], out.data(), cv::kBlock, p, gFx);
    gInProcess = false;
    for (size_t v = 0; v < 4; ++v) {
      if (gCrowd.lagSamples(v) == l.lag[v]) continue;
      l.lag[v] = gCrowd.lagSamples(v);
      ++l.total[v];
      if (i < switchAt) continue;
      ++l.after[v];
      if (l.firstAfter[v] < 0) l.firstAfter[v] = i - switchAt;
    }
  }
  return l;
}

// The gap rule over a noise floor. On the program, whose gaps sit on its -40 dBFS floor, WIDTH goes from
// 1 to 0 half way: every copy lands within 3 s of the move, copy 2 ends within its 12 ms of scatter of
// its 60 ms WIDTH 0 lag, and no copy lands more than 3 times a second, since the program has 2 gaps a
// second. On bursts with silent gaps every copy still lands in each of the last 9 gaps, and copy 2
// ends as near its WIDTH 0 lag.
bool testGapOverFloor() {
  bool ok = true;
  for (int which = 0; which < 2; ++which) {
    const Signal in = which == 0 ? program(10) : bursts(180.0, Train{100, 400, 150, 18});
    const int half = static_cast<int>(in.size()) / 2 / cv::kBlock * cv::kBlock;
    const Landings l = countLandings(in, 1.0f, 0.0f, half);
    int slowest = 0, most = 0, fewestAfter = 1 << 30;
    for (size_t v = 0; v < 4; ++v) {
      slowest = l.firstAfter[v] < 0 ? 1 << 30 : std::max(slowest, l.firstAfter[v]);
      most = std::max(most, l.total[v]);
      fewestAfter = std::min(fewestAfter, l.after[v]);
    }
    const double endMs = static_cast<double>(l.lag[1]) / cv::crowd_detail::kSmpPerMs;
    const bool lands = which == 0 ? slowest <= 3 * kRate && most <= 30 : fewestAfter >= 9;
    ok &= report(which == 0 ? "gap over a -40 dBFS floor" : "gap over silence", lands && std::fabs(endMs - 60.0) <= 12.5,
                 "first landing after the move within %.0f ms, landings per copy at most %.0f, after the move at least %.0f; "
                 "copy 2 ends at %.1f ms (60 +- 12.5)",
                 1000.0 * slowest / kRate, static_cast<double>(most), static_cast<double>(fewestAfter), endMs);
  }
  return ok;
}

// WIDTH holds the level. At VOICES 0.5 on the shipped tuning every WIDTH position stays within 0.5 dB of
// WIDTH 1 on 30 s of the program. The bottom of WIDTH holds exact doubles of the lead, a comb whose level
// on a steady tone hangs on each lag times f0, and the gaps redraw the lags twice a second, so the comb
// averages out over the 60 gaps of the run.
bool testWidthHoldsLevel() {
  const Signal in = program(30);
  cv::CrowdParams p;
  p.on = true;
  p.voices = 0.5f;
  p.width = 1.0f;
  const double top = levelDb(in, run(in, p));
  double worst = 0.0;
  std::printf("info level VOICES 50 %% against WIDTH 100 %% (%+.2f dB), dB:", top);
  for (float w : {0.0f, 0.02f, 0.05f, 0.1f, 0.2f, 0.3f, 0.5f, 0.7f}) {
    p.width = w;
    const double d = levelDb(in, run(in, p)) - top;
    std::printf(" WIDTH %.0f %% %+.2f", static_cast<double>(w) * 100.0, d);
    worst = std::max(worst, std::fabs(d));
  }
  std::printf("\n");
  return report("width holds the level", worst <= 0.5, "every WIDTH within %.2f dB of WIDTH 100 %% (<= 0.5)", worst);
}

// Normalised autocorrelation of x over [from, to) at lag.
double acorrAt(const Signal& x, size_t from, size_t to, int lag) {
  double s = 0.0, e0 = 0.0, e1 = 0.0;
  for (size_t i = from; i + static_cast<size_t>(lag) < to; ++i) {
    const double a = x[i];
    const double b = x[i + static_cast<size_t>(lag)];
    s += a * b;
    e0 += a * a;
    e1 += b * b;
  }
  return s / std::sqrt(e0 * e1 + 1e-30);
}

// Copy 2 alone at WIDTH 1 with the lead at 0 on `in`. share takes the move share after every block.
Signal runCopy2(const Signal& in, std::vector<float>& share) {
  cv::CrowdParams p = knobs(1.0f, 1.0f);
  p.tuning.voiceDb = 0.0f;
  p.solo = 1u << 1;
  gFx.reset();
  gCrowd.reset();
  Signal out(in.size());
  float scratch[cv::kBlock];
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) {
    gInProcess = true;
    gFx.process(&in[i], scratch, cv::kBlock, passThrough());
    gCrowd.process(&in[i], &out[i], cv::kBlock, p, gFx);
    gInProcess = false;
    share.push_back(gCrowd.moveShare(1));
  }
  return out;
}

// 1 s at 400 Hz, 1 s of the note, 0.5 s at 400 Hz.
Signal noteBetween(double hz) {
  Signal x = tone(400.0, kRate);
  const Signal note = tone(hz, kRate);
  const Signal back = tone(400.0, kRate / 2);
  x.insert(x.end(), note.begin(), note.end());
  x.insert(x.end(), back.begin(), back.end());
  return x;
}

// True when share passes strictly between 0 and 1 in blocks [from, to): a glide, not a jump.
bool glides(const std::vector<float>& share, size_t from, size_t to) {
  for (size_t b = from; b < to && b < share.size(); ++b) {
    if (share[b] > 0.0f && share[b] < 1.0f) return true;
  }
  return false;
}

// Notes outside the tracker's 70 Hz to 1 kHz. At 1300 Hz the tracker reads an octave down and at 60 Hz
// it reads unvoiced, and either way copy 2 goes plain: over the note's last 0.5 s it correlates above
// 0.95 with itself at the note's period, and its move share glides to 0 on the way in and back to 1
// on the way out. A 220 Hz voice keeps the share at exactly 1 and correlates above 0.95 at its shifted
// period.
bool testOutOfRange() {
  bool ok = true;
  const size_t noteFrom = kRate / cv::kBlock;
  const size_t backFrom = 2 * kRate / cv::kBlock;
  for (double hz : {1300.0, 60.0, 220.0}) {
    std::vector<float> share;
    share.reserve(3 * kRate / cv::kBlock);
    const Signal out = runCopy2(noteBetween(hz), share);
    const bool inRange = hz == 220.0;
    const double shifted = inRange ? std::pow(2.0, 2.75 * kPitchTop / 12.0) : 1.0;
    const int period = static_cast<int>(kRate / (hz * shifted) + 0.5);
    const double c = acorrAt(out, 3 * kRate / 2, 2 * kRate, period);
    const float lowest = *std::min_element(share.begin(), share.end());
    const bool path = inRange ? lowest == 1.0f
                              : share[backFrom - 1] == 0.0f && share.back() == 1.0f &&
                                    glides(share, noteFrom, backFrom) && glides(share, backFrom, share.size());
    char name[48];
    std::snprintf(name, sizeof name, "note at %.0f Hz", hz);
    ok &= report(name, c > 0.95 && path,
                 "copy 2 correlates %.3f at %.0f samples (> 0.95); move share lowest %.3f, its path as expected %.0f (1)", c,
                 static_cast<double>(period), static_cast<double>(lowest), path ? 1.0 : 0.0);
  }
  return ok;
}

// One voice on its own ring through calls of the given sizes, as the Crowd card drives it: each call
// writes its samples, then the voice renders them in kBlock chunks. While it renders, the call's own
// samples hold NaN. They overwrote the oldest, so a read past the valid span lands on them. Returns the
// NaN output samples, and the peak in `peak`.
size_t canaryReads(const Signal& x, const std::vector<int>& sizes, float ratio, double* peak) {
  namespace cd = cv::crowd_detail;
  std::unique_ptr<cv::VoiceRing> ring(new cv::VoiceRing());
  ring->fill(0.0f);
  cd::FormantPsolaVoice voice;
  voice.reset();
  Signal out(x.size(), 0.0f);
  int head = 0;
  size_t at = 0;
  for (size_t call = 0; at + static_cast<size_t>(sizes[call % sizes.size()]) <= x.size(); ++call) {
    const int n = sizes[call % sizes.size()];
    const int first = head;
    for (int j = 0; j < n; ++j) {
      (*ring)[static_cast<size_t>(head)] = std::nanf("");
      head = head + 1 == cv::kVoiceRingLen ? 0 : head + 1;
    }
    voice.prepare(kRate / 72.0f, ratio, 0.5f, cd::kDelayCeil, n);
    for (int c = 0; c < n; c += cv::kBlock)
      voice.render(*ring, cv::wrapOnce<cv::kVoiceRingLen>(first + c), std::min(cv::kBlock, n - c), &out[at + static_cast<size_t>(c)]);
    for (int j = 0; j < n; ++j) (*ring)[static_cast<size_t>(cv::wrapOnce<cv::kVoiceRingLen>(first + j))] = x[at + static_cast<size_t>(j)];
    at += static_cast<size_t>(n);
  }
  *peak = 0.0;
  size_t bad = 0;
  for (float v : out) {
    bad += std::isnan(v) ? 1 : 0;
    if (!std::isnan(v)) *peak = std::max(*peak, static_cast<double>(std::fabs(v)));
  }
  return bad;
}

// A grain never reads past the valid ring span. The audit's case at the voice: formant -12 st (rate
// 0.5), a 72 Hz voice, the lag at kDelayCeil, where a 200 ms delay lands with its scatter. Calls of
// kBlock, of mixed sizes under it, and of 200 samples. A ceiling of the ring less 4 samples, which
// leaves out the call's own samples, reads 1,054 NaN samples on the calls of kBlock.
bool testReadCeiling() {
  const Signal x = tone(72.0, 3 * kRate);
  const std::vector<std::vector<int>> patterns = {{cv::kBlock}, {17, cv::kBlock, 40}, {200}};
  size_t bad = 0;
  double quietest = 1e9;
  for (const std::vector<int>& sizes : patterns) {
    for (float ratio : {1.0f, 1.11f}) {
      double peak = 0.0;
      bad += canaryReads(x, sizes, ratio, &peak);
      quietest = std::min(quietest, peak);
    }
  }
  return report("read ceiling", bad == 0 && quietest > 0.1,
                "NaN samples read past the valid span %.0f (0); the quietest run peaks at %.2f (> 0.1)",
                static_cast<double>(bad), quietest);
}

// Each tuning value the engine clamps, set past its range, plays exactly as the end of its range.
bool testTuningRanges() {
  namespace cd = cv::crowd_detail;
  const Signal in = bursts(220.0, Train{100, 200, 120, 3});
  using Edit = void (*)(cv::CrowdTuning&, bool);
  const Edit edits[] = {
      [](cv::CrowdTuning& t, bool past) { t.scatterMs = past ? 100.0f : cd::kMaxScatterMs; },
      [](cv::CrowdTuning& t, bool past) { t.driftCents = past ? 200.0f : cd::kMaxDriftCents; },
      [](cv::CrowdTuning& t, bool past) { t.shelfDb[0] = past ? 40.0f : cd::kMaxShelfDb; },
      [](cv::CrowdTuning& t, bool past) { t.voiceDb = past ? -60.0f : cd::kMinVoiceDb; },
      [](cv::CrowdTuning& t, bool past) { t.trimDb = past ? 30.0f : cd::kMaxTrimDb; },
      [](cv::CrowdTuning& t, bool past) { t.trimBottomDb = past ? -30.0f : -cd::kMaxTrimDb; },
      [](cv::CrowdTuning& t, bool past) { t.lagScale = past ? 5.0f : cd::kMaxLagScale; },
      [](cv::CrowdTuning& t, bool past) { t.delayMs[1] = past ? 1.0f : cd::kMinDelayMs; },
  };
  size_t moved = 0;
  for (const Edit edit : edits) {
    cv::CrowdParams past = knobs(0.8f, 0.6f);
    cv::CrowdParams end = past;
    edit(past.tuning, true);
    edit(end.tuning, false);
    moved += differing(run(in, past), run(in, end));
  }
  return report("tuning ranges", moved == 0, "values past their range differ from the range's end in %.0f samples (0)",
                static_cast<double>(moved));
}

// The raised copies (owner 2026-11-01).

// Raised copy v + 4 alone, as lagProbe(v) plays tuned copy v. Its tuned copy's pitch entry sits at
// -1.75 st, so the raised copy plays on the lead's pitch and the cross-correlation has one peak.
cv::CrowdParams raisedProbe(size_t v, float lagScale) {
  cv::CrowdParams p = lagProbe(v, lagScale);
  p.solo = static_cast<uint8_t>(1u << (v + 4));
  p.tuning.pitchSt[v] = -1.75f;
  return p;
}

// Each raised copy lags its tuned copy by 75 ms. On 220 Hz bursts at WIDTH 1 each copy plays alone on
// the lead's pitch and its lag is read off the cross-correlation. The raised copies land at 131, 215,
// 173 and 180 ms.
bool testRaisedLag() {
  const Train tr{300, 300, 120, 3};
  const Signal in = bursts(220.0, tr);
  const cv::CrowdTuning d;
  double tuned[4];
  double raised[4];
  double worst = 0.0;
  for (size_t v = 0; v < 4; ++v) {
    const double want = lagMsAt(v, 1.0, static_cast<double>(d.lagScale));
    const double wantRaised = std::min(want + 75.0, static_cast<double>(cv::kCrowdMaxLagMs));
    tuned[v] = measuredLagMs(in, run(in, lagProbe(v, d.lagScale)), tr.start(1) + msToSamples(want + 30.0), want);
    const Signal out = run(in, raisedProbe(v, d.lagScale));
    raised[v] = measuredLagMs(in, out, tr.start(1) + msToSamples(wantRaised + 30.0), wantRaised);
    worst = std::max({worst, std::fabs(raised[v] - tuned[v] - 75.0), std::fabs(raised[v] - wantRaised)});
  }
  std::printf("info raised lag, tuned copies %.2f %.2f %.2f %.2f ms, raised copies %.2f %.2f %.2f %.2f ms\n", tuned[0],
              tuned[1], tuned[2], tuned[3], raised[0], raised[1], raised[2], raised[3]);
  return report("raised copies lag", worst <= 0.5,
                "worst error of a raised copy against its tuned copy + 75 ms and against the law %.2f ms (<= 0.5)", worst);
}

// 1 s of the 220 Hz tone, then 1 s of noise at -26 dBFS RMS, which the tracker reads as unvoiced.
Signal toneThenNoise() {
  Signal x = tone(220.0, 2 * kRate);
  uint64_t s = 0x2545F4914F6CDD1DULL;
  double b = 0.0;
  for (size_t i = kRate; i < x.size(); ++i) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    b = 0.9 * b + (static_cast<double>(s >> 11) / 4503599627370496.0 - 1.0);
    x[i] = static_cast<float>(0.05 * 0.436 * b * std::sqrt(3.0));
  }
  return x;
}

// One copy alone at WIDTH 1 on toneThenNoise(): the first unvoiced block after the tone, the first
// block whose move share sits under 1, and the voiced blocks after the first unvoiced one.
struct PlainRun {
  Signal out;
  int unvoicedAt = -1;
  int plainAt = -1;
  int voicedAfter = 0;
};

PlainRun runPlain(const Signal& in, size_t c) {
  cv::CrowdParams p = knobs(1.0f, 1.0f);
  p.solo = static_cast<uint8_t>(1u << c);
  gFx.reset();
  gCrowd.reset();
  PlainRun r;
  r.out.assign(in.size(), 0.0f);
  float scratch[cv::kBlock];
  for (int i = 0; i + cv::kBlock <= static_cast<int>(in.size()); i += cv::kBlock) {
    const size_t at = static_cast<size_t>(i);
    gInProcess = true;
    gFx.process(&in[at], scratch, cv::kBlock, passThrough());
    gCrowd.process(&in[at], &r.out[at], cv::kBlock, p, gFx);
    gInProcess = false;
    const bool voiced = gFx.pitch().voiced;
    if (r.unvoicedAt >= 0 && voiced) ++r.voicedAfter;
    if (r.unvoicedAt < 0 && i >= kRate && !voiced) r.unvoicedAt = i;
    if (r.plainAt < 0 && gCrowd.moveShare(c) < 1.0f) r.plainAt = i;
  }
  return r;
}

// A copy goes plain once the unvoiced run reaches its lag cap plus 12 ms of scatter: a tuned copy
// after 162 ms of unvoiced input, a raised copy after 237 ms, within a block. Neither clicks: the
// largest step over the 60 ms glide stays under the largest over 300 ms of the plain noise after it.
bool testPlainAfter() {
  const Signal in = toneThenNoise();
  bool ok = true;
  for (const std::pair<size_t, double> c : {std::pair<size_t, double>{1, 162.0}, {5, 237.0}}) {
    const PlainRun r = runPlain(in, c.first);
    const double ms = (r.plainAt - r.unvoicedAt + cv::kBlock) * 1000.0 / kRate;
    const double glide = maxStep(r.out, r.plainAt - 1, r.plainAt + msToSamples(60.0));
    const double plain = maxStep(r.out, r.plainAt + msToSamples(100.0), r.plainAt + msToSamples(400.0));
    char name[48];
    std::snprintf(name, sizeof name, "plain after, copy %zu, %.0f ms", c.first + 1, c.second);
    ok &= report(name, r.unvoicedAt > 0 && r.voicedAfter == 0 && std::fabs(ms - c.second) <= 1.5 && glide <= plain,
                 "plain after %.2f ms unvoiced (+- 1.5), voiced blocks in the noise %.0f (0); largest step in the "
                 "glide %.4f, in the plain noise %.4f",
                 ms, static_cast<double>(r.voicedAfter), glide, plain);
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testOffExact();
  ok &= testReset();
  ok &= testNoAlloc();
  ok &= testPitchMoves();
  ok &= testWidthLaw();
  ok &= testFadeOrder();
  ok &= testFormantMoves();
  ok &= testVoicesDiffer();
  ok &= testLevelLaw();
  ok &= testLeadLeaves();
  ok &= testVoiceCount();
  ok &= testSolo();
  ok &= testLagAcrossGap();
  ok &= testScatter();
  ok &= testDrift();
  ok &= testTrackerRuns();
  ok &= testCorrectionReleases();
  ok &= testEnhancementsDefaultExact();
  ok &= testWorkingSetDefaults();
  ok &= testLevelScatter();
  ok &= testContour();
  ok &= testMelodicBottom();
  ok &= testWidthGlides();
  ok &= testWidthLag();
  ok &= testWidthZeroIsDelay();
  ok &= testSharedShelf();
  ok &= testEnhancementsStable();
  ok &= testSwitchWithTracker();
  ok &= testGapOverFloor();
  ok &= testWidthHoldsLevel();
  ok &= testOutOfRange();
  ok &= testReadCeiling();
  ok &= testTuningRanges();
  ok &= testRaisedLag();
  ok &= testPlainAfter();
  std::printf("%s crowd\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
