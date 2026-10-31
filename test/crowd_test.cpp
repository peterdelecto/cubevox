// Crowd card checks: bit-exact off, no allocation, pitch moved by pitchSt, formant moved, two copies
// of different colour, the WIDTH law, the level law across VOICES, the lead leaving, the voice
// count and fade order, the solo, the lag following its scale across a gap and holding inside a
// burst, timing scatter, pitch drift, the tracker running without Autotune, the three enhancements
// (every control at its default is the pinned engine bit for bit, level scatter, contour, the
// shadow ring, the pulse marks and the pulse cut on a glottal pulse train and the human-voice ranges),
// and a host timing against Unison. Every run feeds a PitchFx
// through its pass-through path first, so the ring is written and the tracker runs, as the chain
// does. No window, no audio device.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "engine/crowd.h"
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
      for (size_t v = 0; v < 4; ++v) probe->drift = std::max(probe->drift, std::fabs(gCrowd.driftCents(v)));
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

// The shipped defaults are the owner's working set (contour, level scatter, timing scatter, drift and
// epoch align all on). A test starts from this neutral tuning and turns on only what it measures.
void neutralize(cv::CrowdTuning& t) {
  t.scatterMs = 0.0f;
  t.driftCents = 0.0f;
  t.levelScatterDb = 0.0f;
  t.contour = {1.0f, 1.0f, 1.0f, 1.0f};
  t.epochAlign = false;
}

cv::CrowdParams knobs(float voices, float width) {
  cv::CrowdParams p;
  p.on = true;
  p.voices = voices;
  p.width = width;
  neutralize(p.tuning);
  return p;
}

// Voice 0 alone with nothing around it. voiceDb 0 gives one voice the whole level, so the lead is
// 0, and the low cut sits at the bottom of the filter band. The voice sits on the lead's pitch with
// a flat tone, so only its formant move is left. WIDTH sits at 1, so a pitchSt is the offset heard.
cv::CrowdParams isolated(float formantSt) {
  cv::CrowdParams p = knobs(0.2f, 1.0f);
  cv::CrowdTuning& t = p.tuning;
  t.voiceDb = 0.0f;
  t.formantSt[0] = formantSt;
  t.pitchSt[0] = 0.0f;
  t.tiltDb[0] = 0.0f;
  t.hpHz = cv::kFilterMinHz;
  return p;
}

// Copy v alone at the lead's pitch and throat, flat, so its output is the lead moved only by its lag.
cv::CrowdParams lagProbe(size_t v, float lagScale) {
  cv::CrowdParams p = isolated(0.0f);
  p.solo = static_cast<uint8_t>(1u << v);
  p.tuning.lagScale = lagScale;
  p.tuning.pitchSt[v] = 0.0f;
  p.tuning.formantSt[v] = 0.0f;
  p.tuning.tiltDb[v] = 0.0f;
  return p;
}

// Voice v of the default tuning, moved into slot 0 and played alone.
cv::CrowdParams alone(size_t v) {
  cv::CrowdParams p = isolated(0.0f);
  const cv::CrowdTuning d;
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
  moving.tuning.scatterMs = 20.0f;
  moving.tuning.driftCents = 30.0f;
  run(in, moving);
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// One voice alone lands on the lead's pitch moved by its pitchSt at WIDTH 1, with the voice's own
// formant and tone shelf in place.
bool testPitchMoves() {
  const Signal in = tone(220.0, 2 * kRate);
  bool ok = true;
  for (size_t v = 0; v < 4; ++v) {
    const cv::CrowdParams p = alone(v);
    const double want = 220.0 * std::pow(2.0, static_cast<double>(p.tuning.pitchSt[0]) / 12.0);
    const double hz = medianHz(run(in, p));
    const double err = hz > 0.0 ? cents(hz, want) : 9999.0;
    char name[40];
    std::snprintf(name, sizeof name, "pitch moves, voice %zu", v);
    ok &= report(name, std::fabs(err) <= 10.0, "f0=%.2f Hz, want %.2f Hz, err=%.2f cents (+-10)", hz, want, err);
  }
  return ok;
}

// The WIDTH factors of the owner's law, written out here so the test does not lean on the engine's
// code. Both are linear through (0, floor), (0.1, 0.08) and (1, 1), the pitch floor 0.003 and the
// formant floor 0.
double pitchFactor(double w) { return w <= 0.1 ? 0.003 + 0.077 * (w / 0.1) : 0.08 + 0.92 * ((w - 0.1) / 0.9); }
double formantFactor(double w) { return w <= 0.1 ? 0.08 * (w / 0.1) : 0.08 + 0.92 * ((w - 0.1) / 0.9); }

// f0 of a copy of the 220 Hz tone, read at its second harmonic over second 1..2 and halved, near
// the f0 wanted.
double copyHz(const Signal& out, double want) { return peakHz(out, kRate, kRate, 2.0 * want) / 2.0; }

// The -3 st copy (index 2, formant -1 st) alone at WIDTH 0, 0.1, 0.5 and 1. Its pitch offset is -3 st
// times the pitch factor, about 1 cent at 0, 24 cents at 0.1 (the knee) and 0.489 of the full offset
// at 0.5. Its formant move is the log of its grain playback rate over the same log at WIDTH 1, read
// from the engine: none at 0, 8 % at 0.1 and 0.489 at 0.5. (A spectral centroid does not read it,
// because the grain windows make the centroid's slope depend on the pitch ratio; at WIDTH 0 the
// output itself shows it, which equals the output of a copy with no formant offset.) Lag and tone
// shelf stay where they are, so the lag reads the same at every WIDTH.
bool testWidthLaw() {
  const Signal tn = tone(220.0, 2 * kRate);
  const Signal vow = vowel(220.0, 2 * kRate);
  const cv::CrowdTuning d;
  const size_t v = 2;
  const double full = static_cast<double>(d.pitchSt[v]);
  const struct {
    float width;
    double tolCents;
  } rows[] = {{0.0f, 2.0}, {0.1f, 3.0}, {0.5f, 10.0}, {1.0f, 10.0}};
  constexpr double kMoveTol = 0.02;
  bool ok = true;
  float lagAt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  double rate[4] = {0.0, 0.0, 0.0, 0.0};
  double hz[4] = {0.0, 0.0, 0.0, 0.0};
  size_t i = 0;
  for (const auto& r : rows) {
    const double w = static_cast<double>(r.width);
    cv::CrowdParams p = alone(v);
    p.width = r.width;
    const Signal out = run(tn, p);
    lagAt[i] = gCrowd.lagSamples(0);
    rate[i] = static_cast<double>(gCrowd.grainRate(0));
    const double want = 220.0 * std::pow(2.0, full * pitchFactor(w) / 12.0);
    hz[i] = copyHz(out, want);
    const double err = cents(hz[i], want);
    char name[40];
    std::snprintf(name, sizeof name, "width law, WIDTH %.0f %%", w * 100.0);
    ok &= report(name, std::fabs(err) <= r.tolCents, "f0=%.3f Hz, want %.3f Hz, err=%.2f cents (+-%.0f)", hz[i], want, err,
                 r.tolCents);
    ++i;
  }
  for (size_t k = 0; k < 4; ++k) {
    const double w = static_cast<double>(rows[k].width);
    const double move = std::log(rate[k]) / std::log(rate[3]);
    char name[48];
    std::snprintf(name, sizeof name, "width law formant, WIDTH %.0f %%", w * 100.0);
    ok &= report(name, std::fabs(move - formantFactor(w)) <= kMoveTol, "formant move %.3f of full, want %.3f (+-%.2f)", move,
                 formantFactor(w), kMoveTol);
    std::printf("info width %.2f copy 3 f0 %.3f Hz, %+.2f cents from the 220 Hz lead, formant move %.1f %% of full\n", w, hz[k],
                cents(hz[k], 220.0), 100.0 * move);
  }

  // At WIDTH 0 the vowel through the copy is the vowel through a copy with no formant offset.
  cv::CrowdParams withFormant = alone(v);
  withFormant.width = 0.0f;
  cv::CrowdParams noFormant = withFormant;
  noFormant.tuning.formantSt[0] = 0.0f;
  const Signal a = run(vow, withFormant);
  const Signal b = run(vow, noFormant);
  size_t differ = 0;
  for (size_t k = 0; k < a.size(); ++k) differ += a[k] != b[k] ? 1 : 0;
  ok &= report("width law no formant at 0", differ == 0, "WIDTH 0 with a -1 st formant differs from one with none in %.0f samples (0)",
               static_cast<double>(differ));
  const double wantLag = static_cast<double>(d.delayMs[0] * d.lagScale) * cv::crowd_detail::kSmpPerMs;
  ok &= report("width keeps the lag",
               lagAt[0] == lagAt[1] && lagAt[1] == lagAt[2] && lagAt[2] == lagAt[3] &&
                   std::fabs(static_cast<double>(lagAt[0]) - wantLag) < 1.0,
               "lag at WIDTH 0 / 0.1 / 0.5 / 1 = %.1f / %.1f / %.1f / %.1f samples", static_cast<double>(lagAt[0]),
               static_cast<double>(lagAt[1]), static_cast<double>(lagAt[2]), static_cast<double>(lagAt[3]));
  return ok;
}

// VOICES 20, 40, 60 and 80 % bring in the copies in index order, the -1.75 st copy first, then the
// +2.75 st, the -3 st and the +1 st. At each step the output carries a line at the f0 of every
// copy that has come in and none at the f0 of the copies still to come. It runs at WIDTH 1 and at
// WIDTH 0.5, where the offsets are the pitch factor 0.489 of their full values.
bool testFadeOrder() {
  const Signal in = tone(220.0, 2 * kRate);
  const cv::CrowdTuning d;
  bool ok = true;
  for (float width : {1.0f, 0.5f}) {
    for (int n = 1; n <= 4; ++n) {
      const Signal out = run(in, knobs(0.2f * static_cast<float>(n), width));
      double amp[4];
      double top = 0.0;
      for (size_t v = 0; v < 4; ++v) {
        const double st = static_cast<double>(d.pitchSt[v]) * pitchFactor(static_cast<double>(width));
        amp[v] = goertzel(&out[kRate], kRate, 220.0 * std::pow(2.0, st / 12.0));
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

// Voice 0 alone (the longest throat, -1.75 st) and voice 1 alone (the highest centroid of the
// four) are two different colours: the spectral centroid of each, over the harmonics of its own
// f0 up to the 20th, differs by at least 5 %. All four default formants are below the lead's.
bool testVoicesDiffer() {
  const Signal in = tone(220.0, 2 * kRate);
  const size_t pick[2] = {0, 1};
  double c[2];
  for (size_t i = 0; i < 2; ++i) {
    const cv::CrowdParams p = alone(pick[i]);
    const double f0 = 220.0 * std::pow(2.0, static_cast<double>(p.tuning.pitchSt[0]) / 12.0);
    c[i] = centroidHz(run(in, p), f0, 1, 20);
  }
  const double diff = std::fabs(c[1] - c[0]) / std::min(c[0], c[1]);
  return report("voices differ", diff >= 0.05, "centroid voice 0 %.1f Hz, voice 1 %.1f Hz, differ %.1f %% (>=5)", c[0],
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
  cv::CrowdParams a = knobs(0.2f, 0.5f);
  cv::CrowdParams b = a;
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
// law untouched, and all four voices soloed at the top of the knob are that law's crowd.
bool testSolo() {
  const Signal in = program(1);
  Probe p;
  cv::CrowdParams a = knobs(0.2f, 0.5f);
  a.solo = 0b0010;
  const Signal outA = run(in, a, &p);
  bool ok = report("solo count", p.active == 1, "%.0f voices ticking at solo 0b0010, VOICES 20 %% (1)", p.active);

  // Voice 0 is silent and voice 1 sounds: retuning voice 0 moves no sample, retuning voice 1 moves some.
  cv::CrowdParams b = a;
  b.tuning.pitchSt[0] = 4.0f;
  b.tuning.formantSt[0] = 5.0f;
  b.tuning.tiltDb[0] = 6.0f;
  const Signal outB = run(in, b);
  cv::CrowdParams c = a;
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
  cv::CrowdParams top = knobs(1.0f, 0.5f);
  top.solo = 0b0100;
  run(in, top, &p);
  ok &= report("solo top", p.active == 1 && p.lead == 0.0f, "%.0f voices ticking (1), lead gain %.3g (0)", p.active, p.lead);

  // Solo 0 is the plain law bit for bit, and all four voices soloed at the top give the same crowd.
  const cv::CrowdParams plain = knobs(0.2f, 0.5f);
  cv::CrowdParams zero = plain;
  zero.solo = 0;
  const Signal outP = run(in, plain);
  const Signal outZ = run(in, zero);
  cv::CrowdParams all = knobs(1.0f, 0.5f);
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

// 220 Hz bursts with 120 ms of silence between them. The lag scale goes from its default to 2 in
// the middle of burst 2. The copy keeps its near lag to the end of that burst and takes the far lag
// in the gap, so bursts 3 to 5 sound at the far lag.
bool testLagAcrossGap() {
  constexpr float kFarScale = 2.0f;
  const Train tr{300, 300, 120, 6};
  const Signal in = bursts(220.0, tr);
  const cv::CrowdTuning d;
  bool ok = true;
  for (size_t v : {size_t{3}, size_t{1}}) {
    const double nearMs = static_cast<double>(d.delayMs[v] * d.lagScale);
    const double farMs = std::min(static_cast<double>(d.delayMs[v] * kFarScale), static_cast<double>(d.maxLagMs));
    const int switchAt = (tr.start(2) + msToSamples(120.0)) / cv::kBlock * cv::kBlock;
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

// Timing scatter of 15 ms on copy 4 (90 ms lag) over ten bursts. Each burst's lag stays within
// 15 ms of 90 ms and differs from burst to burst. A second run with the copy's own pitch offset
// keeps its f0 within 10 cents of the offset inside every burst, so no lag change bends it.
bool testScatter() {
  const Train tr{300, 300, 120, 10};
  const Signal in = bursts(220.0, tr);
  const cv::CrowdTuning d;
  const size_t v = 3;
  const double target = static_cast<double>(d.delayMs[v] * d.lagScale);
  cv::CrowdParams flat = lagProbe(v, d.lagScale);
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

  cv::CrowdParams pitched = flat;
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
  const cv::CrowdTuning d;
  cv::CrowdParams p = lagProbe(0, d.lagScale);
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

// Enhancements: level scatter, contour and epoch alignment. Each is neutral at 0 (contour 1, epoch
// align off), and each stays inside the range of a human voice.

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

// Every enhancement on at once, for the timing.
void allEnhancements(cv::CrowdTuning& t) {
  t.levelScatterDb = 4.0f;
  t.contour = {0.8f, 0.8f, 0.8f, 0.8f};
  t.epochAlign = true;
}

size_t differing(const Signal& a, const Signal& b) {
  size_t n = 0;
  for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i] ? 1 : 0;
  return n;
}

// With every enhancement neutral the engine runs the code it ran without them, and a setting that
// only matters while its control is on moves no sample. The hashes are for the record.
bool testEnhancementsDefaultExact() {
  const Signal prog = program(3);
  const Signal burst = bursts(220.0, Train{300, 300, 120, 10});
  const cv::CrowdParams base = knobs(0.7f, 0.5f);
  cv::CrowdParams idle = base;
  idle.tuning.epochSearch = 0.1f;     // matters only while epochAlign is on
  idle.tuning.levelScatterDb = -1.0f; // clamps to 0
  idle.tuning.contour = {1.0f, 1.0f, 1.0f, 1.0f};
  const Signal a = run(prog, base);
  const Signal b = run(prog, idle);
  cv::CrowdParams burstBase = knobs(0.6f, 0.8f);
  burstBase.tuning.scatterMs = 15.0f;
  burstBase.tuning.driftCents = 20.0f;
  cv::CrowdParams burstIdle = burstBase;
  burstIdle.tuning = idle.tuning;
  burstIdle.tuning.scatterMs = 15.0f;
  burstIdle.tuning.driftCents = 20.0f;
  const Signal c = run(burst, burstBase);
  const Signal d = run(burst, burstIdle);
  const size_t diffProg = differing(a, b);
  const size_t diffBurst = differing(c, d);
  std::printf("info hash 180 Hz program VOICES 70 WIDTH 50 %016llx\n", static_cast<unsigned long long>(fnv(a)));
  std::printf("info hash burst program VOICES 80 WIDTH 50 %016llx\n",
               static_cast<unsigned long long>(fnv(run(burst, knobs(0.8f, 0.5f)))));
  std::printf("info hash burst program VOICES 60 WIDTH 80 scatter 15 drift 20 %016llx\n",
               static_cast<unsigned long long>(fnv(c)));
  return report("enhancements neutral", diffProg == 0 && diffBurst == 0,
                "inactive settings move %.0f samples of the 180 Hz program and %.0f of the burst program (0)",
                static_cast<double>(diffProg), static_cast<double>(diffBurst));
}

// The shipped defaults are the owner's working set (2026-10-31), and a run on them stays finite and
// within 2.5 dB of the neutral card's level.
bool testWorkingSetDefaults() {
  const cv::CrowdTuning d;
  const bool values = d.contour[0] == 0.8f && d.contour[1] == 1.2f && d.contour[2] == 0.7f && d.contour[3] == 1.1f &&
                      d.levelScatterDb == 2.0f && d.scatterMs == 12.0f && d.driftCents == 15.0f && d.epochAlign &&
                      d.widthKnee == 0.1f && d.widthKneeFrac == 0.08f;
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
bool testLevelScatter() {
  const Train tr{300, 300, 120, 10};
  const Signal in = bursts(220.0, tr);
  const cv::CrowdTuning d;
  const size_t v = 3;
  const double lagMs = static_cast<double>(d.delayMs[v] * d.lagScale);
  const cv::CrowdParams flat = lagProbe(v, d.lagScale);
  cv::CrowdParams scattered = flat;
  scattered.tuning.levelScatterDb = 3.0f;
  const Signal ref = run(in, flat);
  const Signal out = run(in, scattered);
  double lo = 1e9;
  double hi = -1e9;
  std::printf("info level scatter per burst dB re no scatter:");
  for (int k = 0; k < tr.count; ++k) {
    const int from = tr.start(k) + msToSamples(lagMs + 30.0);
    const int to = from + msToSamples(150.0);
    const double db = 20.0 * std::log10(rmsIn(out, from, to) / rmsIn(ref, from, to));
    std::printf(" %+.2f", db);
    lo = std::min(lo, db);
    hi = std::max(hi, db);
  }
  std::printf("\n");
  bool ok = report("level scatter", lo >= -3.1 && hi <= 3.1 && hi - lo >= 1.0,
                   "per-burst gain %+.2f to %+.2f dB (inside +-3), spread %.2f dB (>= 1)", lo, hi, hi - lo);

  // Scatter off, and a scatter that clamps to off, give the same samples.
  cv::CrowdParams off = flat;
  off.tuning.levelScatterDb = 0.0f;
  cv::CrowdParams neg = flat;
  neg.tuning.levelScatterDb = -2.0f;
  ok &= report("level scatter off", differing(ref, run(in, off)) == 0 && differing(ref, run(in, neg)) == 0,
               "0 dB and -2 dB differ from the default run in %.0f samples (0)",
               static_cast<double>(differing(ref, run(in, neg))));

  // The range stops at 4 dB, so 12 dB asked for is no wider than 4.
  cv::CrowdParams wide = flat;
  wide.tuning.levelScatterDb = 12.0f;
  cv::CrowdParams four = flat;
  four.tuning.levelScatterDb = 4.0f;
  ok &= report("level scatter range", differing(run(in, wide), run(in, four)) == 0,
               "12 dB asked for differs from 4 dB in %.0f samples (0)",
               static_cast<double>(differing(run(in, wide), run(in, four))));
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

// Copy 1 alone, with contour c, on a 220 Hz tone swinging 1 st either side. The copy's pitch swing
// against the input's is c. The sweep runs at 1 Hz and at 3 Hz, since the copy's lag is a fifth of a
// cycle at 3 Hz and a copy that read the deviation its lag ago would miss by that much.
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
      char name[48];
      std::snprintf(name, sizeof name, "contour %.1f at %.0f Hz", static_cast<double>(c), sweepHz);
      ok &= report(name, std::fabs(ratio / static_cast<double>(c) - 1.0) <= tol,
                   "input swing %.3f st, copy swing %.3f st, ratio %.3f (the contour within %.2f)", inSt, outSt, ratio,
                   tol);
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

// A glottal pulse train: the derivative of a Rosenberg flow pulse at each period, through two formant
// resonators. jitter is the relative standard deviation of the period.
Signal pulseTrain(double f0, double jitter, int n, std::vector<double>* closures = nullptr) {
  const double period = kRate / f0;
  const double tp = 0.45 * period;
  const double tn = 0.12 * period;
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  uint64_t s = 0x2545F4914F6CDD1DULL;
  for (double t0 = 0.0; t0 < n;) {
    if (closures) closures->push_back(t0 + tp + 0.5 * tn);
    for (int i = std::max(0, static_cast<int>(std::ceil(t0))); i < n && i < t0 + tp + tn; ++i) {
      const double u = i - t0;
      x[static_cast<size_t>(i)] += u < tp ? kPi / (2.0 * tp) * std::sin(kPi * u / tp)
                                          : -kPi / (2.0 * tn) * std::sin(kPi * (u - tp) / (2.0 * tn));
    }
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const double r = static_cast<double>(s >> 11) / 4503599627370496.0 - 1.0;
    t0 += period * (1.0 + jitter * std::sqrt(3.0) * r);
  }
  const double formants[2][2] = {{700.0, 90.0}, {1200.0, 110.0}};
  for (const auto& f : formants) {
    const double r = std::exp(-kPi * f[1] / kRate);
    const double a1 = 2.0 * r * std::cos(2.0 * kPi * f[0] / kRate);
    const double a2 = -r * r;
    double y1 = 0.0;
    double y2 = 0.0;
    for (double& v : x) {
      const double y = (1.0 - r) * v + a1 * y1 + a2 * y2;
      y2 = y1;
      y1 = y;
      v = y;
    }
  }
  double peak = 0.0;
  for (double v : x) peak = std::max(peak, std::fabs(v));
  Signal out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.4 * x[i] / peak);
  return out;
}

// Harmonic to interharmonic energy over harmonics 2..10 of f0, in dB, on x[from, to) under a Hann
// window. Each band is +-0.1 f0 wide on a 1 Hz grid, harmonic bands centred on h f0 and interharmonic
// ones on (h + 0.5) f0, and the ratio is of the mean power per grid point.
double harmonicRatioDb(const Signal& x, double f0, int from, int to) {
  const int n = to - from;
  Signal w(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    w[static_cast<size_t>(i)] = static_cast<float>(x[static_cast<size_t>(from + i)] * (0.5 - 0.5 * std::cos(2.0 * kPi * i / n)));
  double harmonic = 0.0;
  double between = 0.0;
  for (int h = 2; h <= 10; ++h) {
    for (double df = -0.1 * f0; df <= 0.1 * f0; df += 1.0) {
      const double a = goertzel(w.data(), n, h * f0 + df);
      const double b = goertzel(w.data(), n, (h + 0.5) * f0 + df);
      harmonic += a * a;
      between += b * b;
    }
  }
  return 10.0 * std::log10(harmonic / between);
}

// One copy of the pulse train: copy 1 alone with the given pitch and formant, grid or pulse cut.
Signal epochCopy(const Signal& in, float pitchSt, float formantSt, bool epoch, float search = 0.25f) {
  cv::CrowdParams p = lagProbe(0, cv::CrowdTuning{}.lagScale);
  p.tuning.pitchSt[0] = pitchSt;
  p.tuning.formantSt[0] = formantSt;
  p.tuning.epochAlign = epoch;
  p.tuning.epochSearch = search;
  return run(in, p);
}

// On a jittery 150 Hz glottal pulse train, a copy with its throat 2 st short sounds cleaner cut on
// the pulses than on the period grid. And a copy at the pitch and throat of a lower, longer-throated
// person (-3 st, -1.5 st) keeps the input's harmonic-to-interharmonic ratio to within 3 dB.
bool testEpochAlign() {
  constexpr double kJitter = 0.004;
  const Signal in = pulseTrain(150.0, kJitter, 5 * kRate);
  const int from = kRate;
  const int to = 4 * kRate;
  const double inDb = harmonicRatioDb(in, 150.0, from, to);

  const Signal grid = epochCopy(in, 0.0f, 2.0f, false);
  const Signal pulse = epochCopy(in, 0.0f, 2.0f, true);
  const double gridDb = harmonicRatioDb(grid, 150.0, from, to);
  const double pulseDb = harmonicRatioDb(pulse, 150.0, from, to);
  bool ok = report("epoch align cleaner", pulseDb - gridDb >= 2.0,
                   "formant +2 st: harmonic to interharmonic %.1f dB cut on the grid, %.1f dB cut on the pulses, better by "
                   "%.1f dB (>= 2)",
                   gridDb, pulseDb, pulseDb - gridDb);

  const double lowHz = 150.0 * std::pow(2.0, -3.0 / 12.0);
  const Signal low = epochCopy(in, -3.0f, -1.5f, true);
  const Signal lowGrid = epochCopy(in, -3.0f, -1.5f, false);
  const double lowDb = harmonicRatioDb(low, lowHz, from, to);
  const double lowGridDb = harmonicRatioDb(lowGrid, lowHz, from, to);
  ok &= report("epoch align keeps the voice", inDb - lowDb <= 3.0,
               "input %.1f dB; copy at -3 st, formant -1.5 st %.1f dB cut on the pulses (%.1f dB on the grid), %.1f dB under the "
               "input (<= 3)",
               inDb, lowDb, lowGridDb, inDb - lowDb);

  // The search stops at 0.3 of a period either side, and the grid copy does not read the search.
  ok &= report("epoch search range",
               differing(epochCopy(in, 0.0f, 2.0f, true, 0.9f), epochCopy(in, 0.0f, 2.0f, true, 0.3f)) == 0 &&
                   differing(epochCopy(in, 0.0f, 2.0f, false, 0.1f), grid) == 0,
               "0.9 against 0.3 differs in %.0f samples (0); a grid cut with another search differs in %.0f (0)",
               static_cast<double>(differing(epochCopy(in, 0.0f, 2.0f, true, 0.9f), epochCopy(in, 0.0f, 2.0f, true, 0.3f))),
               static_cast<double>(differing(epochCopy(in, 0.0f, 2.0f, false, 0.1f), grid)));
  return ok;
}

// The shadow ring is the input ring through a one-pole at 800 Hz, written at the same index, and
// the input ring itself is untouched.
bool testShadowRing() {
  const Signal in = program(1);
  gFx.reset();
  float scratch[cv::kBlock];
  for (int i = 0; i + cv::kBlock <= static_cast<int>(in.size()); i += cv::kBlock)
    gFx.process(&in[static_cast<size_t>(i)], scratch, cv::kBlock, passThrough());
  const double a = 1.0 - std::exp(-2.0 * kPi * cv::PitchFx::kShadowHz / kRate);
  double y = 0.0;
  double worst = 0.0;
  size_t ringDiffers = 0;
  const int total = static_cast<int>(in.size()) / cv::kBlock * cv::kBlock;
  for (int i = 0; i < total; ++i) {
    y += a * (static_cast<double>(in[static_cast<size_t>(i)]) - y);
    if (i < total - cv::kVoiceRingLen + 100) continue;
    const size_t at = static_cast<size_t>(i % cv::kVoiceRingLen);
    worst = std::max(worst, std::fabs(static_cast<double>(gFx.shadow()[at]) - y));
    ringDiffers += gFx.ring()[at] != in[static_cast<size_t>(i)] ? 1 : 0;
  }
  return report("shadow ring", worst <= 1e-5 && ringDiffers == 0,
                "shadow against a one-pole at 800 Hz: worst error %.3g (<= 1e-5), input ring differs in %.0f samples (0)",
                worst, static_cast<double>(ringDiffers));
}

// The track puts a mark on every glottal closure of a jittery 150 Hz pulse train, to within a
// sample of where the jittery pulse really falls.
bool testEpochMarks() {
  std::vector<double> closures;
  const Signal in = pulseTrain(150.0, 0.004, 3 * kRate, &closures);
  gFx.reset();
  cv::crowd_detail::EpochTrack track;
  float scratch[cv::kBlock];
  for (int i = 0; i + cv::kBlock <= static_cast<int>(in.size()); i += cv::kBlock) {
    gFx.process(&in[static_cast<size_t>(i)], scratch, cv::kBlock, passThrough());
    track.advance(gFx.shadow(), gFx.head(), cv::kBlock, gFx.pitch().period, 0.25f);
  }
  const uint32_t now = track.clock() - 1;
  std::vector<double> marks;
  float last = -1.0f;
  for (float centre = 1000.0f; centre < 10000.0f; centre += 1.0f) {
    const float d = track.snap(now, centre, 320.0f);
    if (d != last && d != centre) marks.push_back(static_cast<double>(now) - static_cast<double>(d));
    last = d;
  }
  double sum = 0.0;
  double sum2 = 0.0;
  for (double m : marks) {
    double best = 1e9;
    double off = 0.0;
    for (double c : closures) {
      if (std::fabs(m - c) < best) {
        best = std::fabs(m - c);
        off = m - c;
      }
    }
    sum += off;
    sum2 += off * off;
  }
  const double n = static_cast<double>(marks.size());
  const double mean = n > 0.0 ? sum / n : 0.0;
  const double sd = n > 0.0 ? std::sqrt(std::max(0.0, sum2 / n - mean * mean)) : 99.0;
  return report("epoch marks", n >= 25.0 && sd <= 1.0,
                "%.0f marks sit %.2f samples after a closure on average, spread %.2f samples (<= 1; the pulses themselves jitter 1.3)",
                n, mean, sd);
}

// The pulses decide where a grain is cut and the grid decides when it is laid down, so each copy keeps
// its pitch cut on the pulses: the f0 of every default copy on a steady tone, and the swing of a copy
// on a gliding tone.
bool testEpochPitch() {
  const Signal steady = tone(220.0, 2 * kRate);
  bool ok = true;
  double worst = 0.0;
  for (size_t v = 0; v < 4; ++v) {
    cv::CrowdParams p = alone(v);
    p.tuning.epochAlign = true;
    const double want = 220.0 * std::pow(2.0, static_cast<double>(p.tuning.pitchSt[0]) / 12.0);
    const double hz = medianHz(run(steady, p));
    worst = std::max(worst, hz > 0.0 ? std::fabs(cents(hz, want)) : 9999.0);
  }
  ok &= report("epoch pitch steady", worst <= 10.0, "worst f0 error of the four copies cut on the pulses %.2f cents (<= 10)",
               worst);

  const Signal glide = sweepTone(220.0, 1.0, 1.0, 7 * kRate);
  cv::CrowdParams p = lagProbe(0, cv::CrowdTuning{}.lagScale);
  p.tuning.epochAlign = true;
  const double inSt = sweepDepthSt(glide, 1.0, 2.0, 3);
  const double outSt = sweepDepthSt(run(glide, p), 1.0, 2.0, 3);
  ok &= report("epoch pitch glide", std::fabs(outSt / inSt - 1.0) <= 0.1,
               "input swing %.3f st, copy swing cut on the pulses %.3f st (within 10 %%)", inSt, outSt);
  return ok;
}

// Cut on the pulses, a copy sits on the nearest pulse to its grid mark, so its lag moves by at most
// half a period from the lag it has on the grid. Copy 4 on 220 Hz bursts (a 4.5 ms period).
bool testEpochLag() {
  const Train tr{300, 300, 120, 6};
  const Signal in = bursts(220.0, tr);
  const cv::CrowdTuning d;
  const size_t v = 3;
  const double lagMs = static_cast<double>(d.delayMs[v] * d.lagScale);
  cv::CrowdParams p = lagProbe(v, d.lagScale);
  p.tuning.epochAlign = true;
  const Signal out = run(in, p);
  double worst = 0.0;
  for (int k = 1; k < tr.count; ++k) {
    const int from = tr.start(k) + msToSamples(lagMs + 30.0);
    worst = std::max(worst, std::fabs(measuredLagMs(in, out, from, lagMs) - lagMs));
  }
  const double halfPeriodMs = 500.0 / 220.0;
  return report("epoch lag", worst <= halfPeriodMs + 0.5, "worst lag error cut on the pulses %.2f ms (<= half a period + 0.5 = %.2f)",
                worst, halfPeriodMs + 0.5);
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

// Host timing, printed and not asserted. Unison is 46.9k cycles per block on the board, so the
// ratio gives the first board estimate for the Crowd card. PitchFx runs outside the timed calls.
// Which 0 is Unison, 1 the Crowd card with every enhancement at its default, 2 the Crowd card with
// every enhancement on.
double secondsOfBlocks(const Signal& in, int which) {
  using Clock = std::chrono::steady_clock;
  cv::PitchFx* fx = new cv::PitchFx();
  cv::Crowd* crowd = new cv::Crowd();
  cv::Unison* uni = new cv::Unison();
  cv::PitchFxParams pp;
  pp.octave.on = false;
  pp.crowdOn = true;
  cv::CrowdParams dp = knobs(0.8f, 0.5f);
  if (which == 2) allEnhancements(dp.tuning);
  if (which == 3) dp.tuning = cv::CrowdTuning{};
  cv::UnisonParams up;
  up.on = true;
  up.depth = 0.5f;
  crowd->reset();
  uni->reset();
  Signal out(in.size());
  float scratch[cv::kBlock];
  double total = 0.0;
  const int len = static_cast<int>(in.size());
  for (int i = 0; i + cv::kBlock <= len; i += cv::kBlock) {
    fx->process(&in[static_cast<size_t>(i)], scratch, cv::kBlock, pp);
    const Clock::time_point t0 = Clock::now();
    if (which > 0) {
      crowd->process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, dp, *fx);
    } else {
      uni->process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, up);
    }
    total += std::chrono::duration<double>(Clock::now() - t0).count();
  }
  delete uni;
  delete crowd;
  delete fx;
  return total;
}

void printTiming() {
  const Signal in = program(10);
  for (int which = 0; which < 4; ++which) secondsOfBlocks(in, which);  // warm-up
  const double d = std::min(secondsOfBlocks(in, 1), secondsOfBlocks(in, 1));
  const double u = std::min(secondsOfBlocks(in, 0), secondsOfBlocks(in, 0));
  const double a = std::min(secondsOfBlocks(in, 2), secondsOfBlocks(in, 2));
  const double w = std::min(secondsOfBlocks(in, 3), secondsOfBlocks(in, 3));
  const double blocks = 10.0 * kRate / cv::kBlock;
  std::printf("info timing 10 s: Crowd VOICES 80 %% %.1f ms (%.2f us per block), Unison DEPTH 50 %% %.1f ms (%.2f us per block)\n",
              1000.0 * d, 1e6 * d / blocks, 1000.0 * u, 1e6 * u / blocks);
  std::printf("info timing ratio Crowd / Unison = %.2f  (board estimate %.0fk cycles per block at Unison 46.9k)\n",
              d / u, 46.9 * d / u);
  std::printf("info timing enhancements off %.2f us per block, all on %.2f us per block (x%.2f)\n", 1e6 * d / blocks,
              1e6 * a / blocks, a / d);
  std::printf("info timing working set defaults %.2f us per block (x%.2f over enhancements off)\n", 1e6 * w / blocks, w / d);
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
  ok &= testShadowRing();
  ok &= testEpochMarks();
  ok &= testEpochAlign();
  ok &= testEpochPitch();
  ok &= testEpochLag();
  ok &= testEnhancementsStable();
  printTiming();
  std::printf("%s crowd\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
