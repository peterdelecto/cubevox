// Gang card checks: bit-exact off, no allocation, pitch held, formant moved, the level law across
// VOICES, the lead leaving, the voice count, DISTANCE timing, the tracker running without Autotune,
// and a host timing against Unison. Every run feeds a PitchFx through its pass-through path first,
// so the ring is written and the tracker runs, as the chain does. No window, no audio device.

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
// 0, and the filters and the room are flat or silent.
cv::GangParams isolated(float formantSt) {
  cv::GangParams p = knobs(0.2f, 0.5f);
  cv::GangTuning& t = p.tuning;
  t.voiceDb = 0.0f;
  t.formantSt[0] = formantSt;
  t.detuneCents[0] = 0.0f;
  t.lfoCents = 0.0f;
  t.hpNearHz = t.hpFarHz = cv::kFilterMinHz;
  t.shelfNearDb = t.shelfFarDb = 0.0f;
  t.roomNearDb = t.roomFarDb = -120.0f;
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

// Median f0 of a signal over seconds 1..2, from a fresh tracker.
double medianHz(const Signal& x) {
  cv::PitchTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&x[static_cast<size_t>(i)], n);
    if (i + n > kRate && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

// Lag in samples where out[t] best matches in[t - lag], over the output window [from, to).
int peakLag(const Signal& in, const Signal& out, int from, int to, int maxLag) {
  int best = 0;
  double bestScore = -1.0;
  for (int lag = 0; lag <= maxLag; ++lag) {
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
  run(in, knobs(0.3f, 0.9f));
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// The one live voice is isolated, so the output is that voice alone. A formant move must leave
// the pitch where it was.
bool testPitchStays() {
  const Signal in = tone(220.0, 2 * kRate);
  bool ok = true;
  for (float st : {1.5f, -1.5f, 0.0f}) {
    const double hz = medianHz(run(in, isolated(st)));
    const double err = hz > 0.0 ? cents(hz, 220.0) : 9999.0;
    char name[40];
    std::snprintf(name, sizeof name, "pitch stays, formant %+.1f st", static_cast<double>(st));
    ok &= report(name, std::fabs(err) <= 10.0, "f0=%.2f Hz, err=%.2f cents (+-10)", hz, err);
  }
  return ok;
}

bool testFormantMoves() {
  const Signal in = vowel(220.0, 2 * kRate);
  const double flat = centroidHz(run(in, isolated(0.0f)), 220.0, 1, 10);
  const double up = centroidHz(run(in, isolated(1.5f)), 220.0, 1, 10);
  const double down = centroidHz(run(in, isolated(-1.5f)), 220.0, 1, 10);
  return report("formant moves", up >= 1.06 * flat && down <= 0.94 * flat,
                "centroid 0 st=%.1f Hz, +1.5 st x%.3f (>=1.06), -1.5 st x%.3f (<=0.94)", flat, up / flat, down / flat);
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
    b.tuning.detuneCents[v] = 400.0f;
    b.tuning.formantSt[v] = 5.0f;
  }
  const Signal outA = run(in, a);
  const Signal outB = run(in, b);
  size_t differ = 0;
  for (size_t i = 0; i < outA.size(); ++i) differ += outA[i] != outB[i] ? 1 : 0;
  b = a;
  b.tuning.detuneCents[0] = 400.0f;
  const Signal outC = run(in, b);
  size_t moved = 0;
  for (size_t i = 0; i < outA.size(); ++i) moved += outA[i] != outC[i] ? 1 : 0;
  ok &= report("voice 0 first", differ == 0 && moved > 0,
               "voices 1..3 retuned change %.0f samples (0), voice 0 retuned changes %.0f (>0)",
               static_cast<double>(differ), static_cast<double>(moved));
  return ok;
}

bool testDistanceTiming() {
  const Signal in = irregular(220.0, 2 * kRate);
  cv::GangParams near = isolated(0.0f);
  near.distance = 0.0f;
  cv::GangParams far = near;
  far.distance = 1.0f;
  const int lagNear = peakLag(in, run(in, near), kRate, kRate + kRate / 2, 4000);
  const int lagFar = peakLag(in, run(in, far), kRate, kRate + kRate / 2, 4000);
  const double msNear = 1000.0 * lagNear / kRate;
  const double msFar = 1000.0 * lagFar / kRate;
  const double wantNear = 28.0 * 0.8;
  const double wantFar = 28.0 * 1.3;
  return report("distance timing", std::fabs(msNear - wantNear) <= 0.5 && std::fabs(msFar - wantFar) <= 0.5,
                "peak at d=0 %.2f ms (%.1f), at d=1 %.2f ms (%.1f), +-0.5", msNear, wantNear, msFar, wantFar);
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
  ok &= testPitchStays();
  ok &= testFormantMoves();
  ok &= testLevelLaw();
  ok &= testLeadLeaves();
  ok &= testVoiceCount();
  ok &= testDistanceTiming();
  ok &= testTrackerRuns();
  ok &= testCorrectionReleases();
  printTiming();
  std::printf("%s gang\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
