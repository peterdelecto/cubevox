// Gang card checks: bit-exact off, no allocation, pitch moved by pitchSt, formant moved, two copies
// of different colour, the level law across VOICES, the lead leaving, the voice count, the solo,
// the lag following DISTANCE across a gap and holding inside a burst, timing scatter, pitch drift,
// the tracker running without Autotune, and a host timing against Unison. Every run feeds a PitchFx
// through its pass-through path first, so the ring is written and the tracker runs, as the chain
// does. No window, no audio device.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <utility>
#include <vector>

#include "engine/gang.h"
#include "engine/unison.h"

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

// Running a PitchFx and then the Gang card on the same blocks, in the chain's order.

cv::PitchFx gFx;
cv::Gang gGang;

struct Probe {
  float lead = -1.0f;
  int active = -1;
  float drift = 0.0f;  // largest |drift| in cents any copy showed at the end of any block
};

// The PitchFx params every run uses unless it says otherwise: pass-through with the tracker on.
cv::PitchFxParams passThrough() {
  cv::PitchFxParams pp;
  pp.octave.on = false;
  pp.gangOn = true;
  return pp;
}

// Gang card and PitchFx params both switch from their first to their second value at sample
// `switchAt` (a block boundary).
Signal runBoth(const Signal& in, const cv::GangParams& first, const cv::GangParams& second,
               const cv::PitchFxParams& pitchFirst, const cv::PitchFxParams& pitchSecond, int switchAt,
               Probe* probe = nullptr) {
  gFx.reset();
  gGang.reset();
  Signal out(in.size());
  float scratch[cv::kBlock];
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    const bool after = i >= switchAt;
    gInProcess = true;
    gFx.process(&in[static_cast<size_t>(i)], scratch, n, after ? pitchSecond : pitchFirst);
    gGang.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, after ? second : first, gFx);
    gInProcess = false;
    if (probe) {
      probe->lead = gGang.leadGain();
      probe->active = gGang.activeVoices();
      for (size_t v = 0; v < 4; ++v) probe->drift = std::max(probe->drift, std::fabs(gGang.driftCents(v)));
    }
  }
  return out;
}

Signal runSwitch(const Signal& in, const cv::GangParams& first, const cv::GangParams& second,
                 int switchAt, Probe* probe = nullptr) {
  return runBoth(in, first, second, passThrough(), passThrough(), switchAt, probe);
}

Signal run(const Signal& in, const cv::GangParams& p, Probe* probe = nullptr) {
  return runSwitch(in, p, p, static_cast<int>(in.size()), probe);
}

cv::GangParams knobs(float voices, float distance) {
  cv::GangParams p;
  p.on = true;
  p.voices = voices;
  p.distance = distance;
  return p;
}

// Voice 0 alone with nothing around it. voiceDb 0 gives one voice the whole level, so the lead is
// 0, and the low cut sits at the bottom of the filter band. The voice sits on the lead's pitch with
// a flat tone, so only its formant move is left.
cv::GangParams isolated(float formantSt) {
  cv::GangParams p = knobs(0.2f, 0.5f);
  cv::GangTuning& t = p.tuning;
  t.voiceDb = 0.0f;
  t.formantSt[0] = formantSt;
  t.pitchSt[0] = 0.0f;
  t.tiltDb[0] = 0.0f;
  t.hpHz = cv::kFilterMinHz;
  return p;
}

// Copy v alone at the lead's pitch and throat, flat, so its output is the lead moved only by its lag.
cv::GangParams lagProbe(size_t v, float distance) {
  cv::GangParams p = isolated(0.0f);
  p.solo = static_cast<uint8_t>(1u << v);
  p.distance = distance;
  p.tuning.pitchSt[v] = 0.0f;
  p.tuning.formantSt[v] = 0.0f;
  p.tuning.tiltDb[v] = 0.0f;
  return p;
}

// Voice v of the default tuning, moved into slot 0 and played alone.
cv::GangParams alone(size_t v) {
  cv::GangParams p = isolated(0.0f);
  const cv::GangTuning d;
  p.tuning.pitchSt[0] = d.pitchSt[v];
  p.tuning.formantSt[0] = d.formantSt[v];
  p.tuning.tiltDb[0] = d.tiltDb[v];
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
  cv::GangParams off;
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
  const cv::GangParams p = knobs(0.7f, 0.5f);
  const Signal a = run(in, p);
  const Signal b = run(in, p);
  size_t differ = 0;
  for (size_t i = 0; i < a.size(); ++i) differ += a[i] != b[i] ? 1 : 0;
  return report("reset", differ == 0, "a second run after reset() differs in %.0f samples (0)", static_cast<double>(differ));
}

bool testNoAlloc() {
  const Signal in = program(1);
  run(in, knobs(0.9f, 0.5f));  // aborts on any new/delete inside process()
  cv::GangParams moving = knobs(0.3f, 0.9f);
  moving.tuning.scatterMs = 20.0f;
  moving.tuning.driftCents = 30.0f;
  run(in, moving);
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// One voice alone lands on the lead's pitch moved by its pitchSt, with the voice's own formant and
// tone shelf in place.
bool testPitchMoves() {
  const Signal in = tone(220.0, 2 * kRate);
  bool ok = true;
  for (size_t v = 0; v < 2; ++v) {
    const cv::GangParams p = alone(v);
    const double want = 220.0 * std::pow(2.0, static_cast<double>(p.tuning.pitchSt[0]) / 12.0);
    const double hz = medianHz(run(in, p));
    const double err = hz > 0.0 ? cents(hz, want) : 9999.0;
    char name[40];
    std::snprintf(name, sizeof name, "pitch moves, voice %zu", v);
    ok &= report(name, std::fabs(err) <= 10.0, "f0=%.2f Hz, want %.2f Hz, err=%.2f cents (+-10)", hz, want, err);
  }
  return ok;
}

// isolated() with the voice's lag set to 90 ms. The centroid of a steady vowel at a shifted
// formant depends on how the grain reads line up with the period grid, so this probe holds the
// lag fixed whatever the default delays are.
cv::GangParams formantProbe(float formantSt) {
  cv::GangParams p = isolated(formantSt);
  p.tuning.delayMs[0] = 90.0f;
  return p;
}

// The formant moves of a throat shorter than the lead's (+1 st, no default copy has one) and of
// voice 0 (the longest, -1.75 st) against a flat voice, pitch on the lead.
bool testFormantMoves() {
  const Signal in = vowel(220.0, 2 * kRate);
  const cv::GangTuning d;
  const double flat = centroidHz(run(in, formantProbe(0.0f)), 220.0, 1, 10);
  const double up = centroidHz(run(in, formantProbe(1.0f)), 220.0, 1, 10);
  const double down = centroidHz(run(in, formantProbe(d.formantSt[0])), 220.0, 1, 10);
  return report("formant moves", up >= 1.03 * flat && down <= 0.94 * flat,
                "centroid 0 st=%.1f Hz, +1 st formant x%.3f (>=1.03), voice 0 formant x%.3f (<=0.94)", flat,
                up / flat, down / flat);
}

// Voice 0 alone (the longest throat, -1.75 st) and voice 2 alone (the highest centroid of the
// four) are two different colours: the spectral centroid of each, over the harmonics of its own
// f0 up to the 20th, differs by at least 5 %. All four default formants are below the lead's, so
// the widest default pair is 7.3 % apart.
bool testVoicesDiffer() {
  const Signal in = tone(220.0, 2 * kRate);
  const size_t pick[2] = {0, 2};
  double c[2];
  for (size_t i = 0; i < 2; ++i) {
    const cv::GangParams p = alone(pick[i]);
    const double f0 = 220.0 * std::pow(2.0, static_cast<double>(p.tuning.pitchSt[0]) / 12.0);
    c[i] = centroidHz(run(in, p), f0, 1, 20);
  }
  const double diff = std::fabs(c[1] - c[0]) / std::min(c[0], c[1]);
  return report("voices differ", diff >= 0.05, "centroid voice 0 %.1f Hz, voice 2 %.1f Hz, differ %.1f %% (>=5)", c[0],
                c[1], 100.0 * diff);
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
  for (float d : {0.0f, 1.0f}) {
    const double db = levelDb(in, run(in, knobs(1.0f, d)));
    std::printf("info level VOICES 100 %% DISTANCE %.0f %%  %+.2f dB re input\n", static_cast<double>(d * 100.0f), db);
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
  const cv::GangTuning t;
  const double g2 = std::pow(10.0, t.voiceDb / 10.0);
  const double wantFour = std::sqrt(1.0 - 4.0 * g2);
  const double wantOne = std::sqrt(1.0 - g2);
  bool ok = report("lead leaves", top.lead == 0.0f, "lead gain at VOICES 100 %% = %.3g (0)", static_cast<double>(top.lead));
  ok &= report("lead law", std::fabs(four.lead - wantFour) < 0.01 && std::fabs(one.lead - wantOne) < 0.01,
               "lead at 80 %% %.3f (want %.3f), at 20 %% %.3f (want %.3f)", static_cast<double>(four.lead), wantFour,
               static_cast<double>(one.lead), wantOne);
  return ok;
}

bool testVoiceCount() {
  const Signal in = program(1);
  bool ok = true;
  const std::pair<float, int> want[] = {{0.0f, 0}, {0.2f, 1}, {0.4f, 2}, {0.6f, 3}, {0.8f, 4}, {1.0f, 4}};
  for (const auto& w : want) {
    Probe p;
    run(in, knobs(w.first, 0.5f), &p);
    char name[40];
    std::snprintf(name, sizeof name, "voice count VOICES %.0f %%", static_cast<double>(w.first * 100.0f));
    ok &= report(name, p.active == w.second, "%.0f voices ticking (%.0f)", p.active, w.second);
  }

  // At 20 % the live voice is voice 0: voices 1..3 can change without moving a sample.
  cv::GangParams a = knobs(0.2f, 0.5f);
  cv::GangParams b = a;
  for (size_t v = 1; v < 4; ++v) {
    b.tuning.pitchSt[v] = 4.0f;
    b.tuning.formantSt[v] = 5.0f;
    b.tuning.tiltDb[v] = 6.0f;
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

// A solo sounds the soloed voices and nothing else, whatever VOICES says. Solo 0 leaves the plain
// law untouched, and all four voices soloed at the top of the knob are that law's gang.
bool testSolo() {
  const Signal in = program(1);
  Probe p;
  cv::GangParams a = knobs(0.2f, 0.5f);
  a.solo = 0b0010;
  const Signal outA = run(in, a, &p);
  bool ok = report("solo count", p.active == 1, "%.0f voices ticking at solo 0b0010, VOICES 20 %% (1)", p.active);

  // Voice 0 is silent and voice 1 sounds: retuning voice 0 moves no sample, retuning voice 1 moves some.
  cv::GangParams b = a;
  b.tuning.pitchSt[0] = 4.0f;
  b.tuning.formantSt[0] = 5.0f;
  b.tuning.tiltDb[0] = 6.0f;
  const Signal outB = run(in, b);
  cv::GangParams c = a;
  c.tuning.pitchSt[1] = 4.0f;
  const Signal outC = run(in, c);
  size_t silent = 0;
  size_t sounds = 0;
  for (size_t i = 0; i < outA.size(); ++i) {
    silent += outA[i] != outB[i] ? 1 : 0;
    sounds += outA[i] != outC[i] ? 1 : 0;
  }
  ok &= report("solo voice", silent == 0 && sounds > 0,
               "voice 0 retuned changes %.0f samples (0), voice 1 retuned changes %.0f (>0)",
               static_cast<double>(silent), static_cast<double>(sounds));

  // At the top the lead is gone and the one soloed copy stands alone.
  cv::GangParams top = knobs(1.0f, 0.5f);
  top.solo = 0b0100;
  run(in, top, &p);
  ok &= report("solo top", p.active == 1 && p.lead == 0.0f, "%.0f voices ticking (1), lead gain %.3g (0)", p.active, p.lead);

  // Solo 0 is the plain law bit for bit, and all four voices soloed at the top give the same gang.
  const cv::GangParams plain = knobs(0.2f, 0.5f);
  cv::GangParams zero = plain;
  zero.solo = 0;
  const Signal outP = run(in, plain);
  const Signal outZ = run(in, zero);
  cv::GangParams all = knobs(1.0f, 0.5f);
  const Signal outTop = run(in, all);
  all.solo = 0b1111;
  const Signal outAll = run(in, all);
  size_t differ = 0;
  size_t differTop = 0;
  for (size_t i = 0; i < outP.size(); ++i) {
    differ += outP[i] != outZ[i] ? 1 : 0;
    differTop += outTop[i] != outAll[i] ? 1 : 0;
  }
  ok &= report("solo off", differ == 0 && differTop == 0,
               "solo 0 differs in %.0f samples (0), solo 0b1111 at VOICES 100 %% differs in %.0f (0)",
               static_cast<double>(differ), static_cast<double>(differTop));
  return ok;
}

// The lag in ms of the output over the 150 ms starting at output sample `from`, searched within
// +-80 ms of `around`.
double measuredLagMs(const Signal& in, const Signal& out, int from, double around) {
  const int lo = std::max(0, msToSamples(around - 80.0));
  const int hi = msToSamples(around + 80.0);
  return 1000.0 * peakLag(in, out, from, from + msToSamples(150.0), lo, hi) / kRate;
}

// 220 Hz bursts with 120 ms of silence between them. DISTANCE goes from 0 to 1 in the middle of
// burst 2. The copy keeps its near lag to the end of that burst and takes the far lag in the gap, so
// bursts 3 to 5 sound at the far lag.
bool testLagAcrossGap() {
  const Train tr{300, 300, 120, 6};
  const Signal in = bursts(220.0, tr);
  const cv::GangTuning d;
  bool ok = true;
  for (size_t v : {size_t{3}, size_t{2}}) {
    const double nearMs = d.delayMs[v];
    const double farMs = std::min(2.0 * static_cast<double>(d.delayMs[v]), static_cast<double>(d.maxLagMs));
    const int switchAt = (tr.start(2) + msToSamples(120.0)) / cv::kBlock * cv::kBlock;
    const Signal out = runSwitch(in, lagProbe(v, 0.0f), lagProbe(v, 1.0f), switchAt);
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

// Timing scatter of 15 ms on copy 4 (75 ms at DISTANCE 0) over ten bursts. Each burst's lag stays
// within 15 ms of 75 ms and differs from burst to burst. A second run with the copy's own pitch
// offset keeps its f0 within 10 cents of the offset inside every burst, so no lag change bends it.
bool testScatter() {
  const Train tr{300, 300, 120, 10};
  const Signal in = bursts(220.0, tr);
  const cv::GangTuning d;
  const size_t v = 3;
  const double target = d.delayMs[v];
  cv::GangParams flat = lagProbe(v, 0.0f);
  flat.tuning.scatterMs = 15.0f;
  const Signal out = run(in, flat);
  double lag[10];
  double worst = 0.0;
  double lo = 1e9;
  double hi = -1e9;
  int changes = 0;
  for (int k = 0; k < tr.count; ++k) {
    lag[k] = measuredLagMs(in, out, tr.start(k) + msToSamples(target + 15.0 + 30.0), target);
    worst = std::max(worst, std::fabs(lag[k] - target));
    lo = std::min(lo, lag[k]);
    hi = std::max(hi, lag[k]);
    if (k > 0 && std::fabs(lag[k] - lag[k - 1]) > 1.0) ++changes;
  }
  std::printf("info scatter lag per burst");
  for (double l : lag) std::printf(" %.1f", l);
  std::printf(" ms\n");
  bool ok = report("scatter lag", worst <= 15.5 && changes >= 6,
                   "worst |lag - %.0f| %.2f ms (<= 15.5), spread %.1f ms, %.0f of 9 bursts moved by > 1 ms (>= 6)", target,
                   worst, hi - lo, static_cast<double>(changes));

  cv::GangParams pitched = flat;
  pitched.tuning.pitchSt[v] = d.pitchSt[v];
  const Signal outP = run(in, pitched);
  const double want = 220.0 * std::pow(2.0, static_cast<double>(d.pitchSt[v]) / 12.0);
  double worstCents = 0.0;
  for (int k = 0; k < tr.count; ++k) {
    const int from = tr.start(k) + msToSamples(target + 15.0 + 30.0);
    const double hz = medianHzIn(outP, tr.start(k), from, from + msToSamples(150.0));
    worstCents = std::max(worstCents, hz > 0.0 ? std::fabs(cents(hz, want)) : 9999.0);
  }
  ok &= report("scatter pitch", worstCents <= 10.0, "worst f0 error in a burst %.2f cents from %.2f Hz (<= 10)", worstCents,
               want);
  return ok;
}

// Pitch drift of 20 cents on copy 1 over a 6 s tone. Its f0 over each 0.5 s window after the first
// second stays within 20 cents of its pitch offset, with 3 cents of slack for the measurement, and
// differs from window to window. With the drift at 0 the walk never moves off 0, whatever its hold.
bool testDrift() {
  const Signal in = tone(220.0, 6 * kRate);
  const cv::GangTuning d;
  cv::GangParams p = lagProbe(0, 0.5f);
  p.tuning.pitchSt[0] = d.pitchSt[0];
  p.tuning.driftCents = 20.0f;
  const Signal out = run(in, p);
  const double base = 220.0 * std::pow(2.0, static_cast<double>(d.pitchSt[0]) / 12.0);
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
  bool ok = report("drift range", worst <= 23.0 && hi - lo >= 2.0, "worst %.2f cents from the offset (<= 20 + 3), spread %.2f cents (>= 2)",
                   worst, hi - lo);

  const Signal prog = program(3);
  cv::GangParams a = knobs(0.7f, 0.5f);
  cv::GangParams b = a;
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

// The tracker finds a period with Autotune off because the Gang card's flag keeps it running, and
// stays at 0 without the flag.
bool testTrackerRuns() {
  const Signal in = tone(220.0, kRate);
  int firstBlock[2] = {-1, -1};
  for (int flag = 0; flag < 2; ++flag) {
    cv::PitchFxParams pp;
    pp.octave.on = false;
    pp.autotune.on = false;
    pp.gangOn = flag == 1;
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

// Autotune on, then off, with the Gang card on throughout. The voice reads Autotune's correction
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

// Host timing, printed and not asserted. Unison is 46.9k cycles per block on the board, so the
// ratio gives the first board estimate for the Gang card. PitchFx runs outside the timed calls.
double secondsOfBlocks(const Signal& in, bool useGang) {
  using Clock = std::chrono::steady_clock;
  cv::PitchFx* fx = new cv::PitchFx();
  cv::Gang* gang = new cv::Gang();
  cv::Unison* uni = new cv::Unison();
  cv::PitchFxParams pp;
  pp.octave.on = false;
  pp.gangOn = true;
  const cv::GangParams dp = knobs(0.8f, 0.5f);
  cv::UnisonParams up;
  up.on = true;
  up.depth = 0.5f;
  gang->reset();
  uni->reset();
  Signal out(in.size());
  float scratch[cv::kBlock];
  double total = 0.0;
  const int len = static_cast<int>(in.size());
  for (int i = 0; i + cv::kBlock <= len; i += cv::kBlock) {
    fx->process(&in[static_cast<size_t>(i)], scratch, cv::kBlock, pp);
    const Clock::time_point t0 = Clock::now();
    if (useGang) {
      gang->process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, dp, *fx);
    } else {
      uni->process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, up);
    }
    total += std::chrono::duration<double>(Clock::now() - t0).count();
  }
  delete uni;
  delete gang;
  delete fx;
  return total;
}

void printTiming() {
  const Signal in = program(10);
  secondsOfBlocks(in, true);  // warm-up
  secondsOfBlocks(in, false);
  const double d = std::min(secondsOfBlocks(in, true), secondsOfBlocks(in, true));
  const double u = std::min(secondsOfBlocks(in, false), secondsOfBlocks(in, false));
  const double blocks = 10.0 * kRate / cv::kBlock;
  std::printf("info timing 10 s: Gang VOICES 80 %% %.1f ms (%.2f us per block), Unison DEPTH 50 %% %.1f ms (%.2f us per block)\n",
              1000.0 * d, 1e6 * d / blocks, 1000.0 * u, 1e6 * u / blocks);
  std::printf("info timing ratio Gang / Unison = %.2f  (board estimate %.0fk cycles per block at Unison 46.9k)\n",
              d / u, 46.9 * d / u);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testOffExact();
  ok &= testReset();
  ok &= testNoAlloc();
  ok &= testPitchMoves();
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
  printTiming();
  std::printf("%s gang\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
