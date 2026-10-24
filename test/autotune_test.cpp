// Autotune checks: passthrough, scale degrees, correction, out-of-scale notes,
// response time, Octave builds on the corrected note, hard tune, the toggle,
// no-alloc. No window, no audio device.

#include <algorithm>
#include <atomic>
#include <cmath>
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
constexpr double kA3 = 220.0;
constexpr double kSharp = 226.4;   // A3 + 50 cents
constexpr double kBb3 = 233.1;
constexpr double kB3 = 246.94;
constexpr double kC4 = 261.63;
constexpr int kSettle = cv::kSampleRate * 3 / 10;  // 300 ms

// Ten harmonics at 1/k amplitude, peak 0.5. hzAt gives the pitch per sample,
// so a step keeps the phase continuous.
template <typename F>
std::vector<float> tone(int n, F hzAt) {
  std::vector<double> x(static_cast<size_t>(n), 0.0);
  double phase = 0.0;
  double peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(k * phase) / k;
    phase += 2.0 * kPi * hzAt(i) / cv::kSampleRate;
    x[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  std::vector<float> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(0.5 * x[i] / peak);
  return out;
}

std::vector<float> tone(double hz, int n) {
  return tone(n, [hz](int) { return hz; });
}

double cents(double hz, double ref) { return 1200.0 * std::log2(hz / ref); }

// Median f0 after 300 ms, measured by a fresh tracker.
double medianHz(const std::vector<float>& x) {
  cv::PitchTracker t;
  std::vector<double> hz;
  const int total = static_cast<int>(x.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    t.push(&x[static_cast<size_t>(i)], n);
    if (i + n > kSettle && t.result().voiced) hz.push_back(t.result().hz);
  }
  if (hz.empty()) return 0.0;
  std::sort(hz.begin(), hz.end());
  return hz[hz.size() / 2];
}

// Tracker pitch and Autotune correction read after one block.
struct Hop {
  int end;
  float hz;
  float corr;
};

std::vector<float> run(cv::PitchFx& fx, const std::vector<float>& in,
                       const cv::PitchFxParams& p, std::vector<Hop>* hops = nullptr) {
  std::vector<float> out(in.size());
  if (hops) hops->reserve(in.size() / cv::kBlock + 1);
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    fx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
    if (hops) hops->push_back({i + n, fx.pitch().hz, fx.correctionSemis()});
  }
  return out;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0, double d = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::PitchFx gFx;

// Autotune alone in key C; Octave off.
cv::PitchFxParams autotuneOnly(float responseMs) {
  cv::PitchFxParams p;
  p.autotune.on = true;
  p.autotune.key = 0;
  p.autotune.responseMs = responseMs;
  p.octave.on = false;
  return p;
}

double outputHz(double inHz, const cv::PitchFxParams& p) {
  gFx.reset();
  return medianHz(run(gFx, tone(inHz, 2 * cv::kSampleRate), p));
}

double rms(const std::vector<float>& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / (to - from));
}

int countDiff(const std::vector<float>& a, const std::vector<float>& b, int from) {
  int n = 0;
  for (size_t i = static_cast<size_t>(from); i < a.size(); ++i)
    if (a[i] != b[i]) ++n;
  return n;
}

// 1. Off is bit-exact. On with an in-tune A3, the output matches the input
// delayed by the shifter latency; the lag is searched around kGrainDelay.
bool testPassthrough() {
  const std::vector<float> in = tone(kA3, cv::kSampleRate);
  cv::PitchFxParams off = autotuneOnly(60.0f);
  off.autotune.on = false;
  gFx.reset();
  const int diff = countDiff(in, run(gFx, in, off), cv::kSampleRate / 10);
  bool ok = report("passthrough off", diff == 0, "samples differing after 100 ms=%.0f (0)", diff);

  gFx.reset();
  const std::vector<float> out = run(gFx, in, autotuneOnly(60.0f));
  const int from = kSettle;
  const int to = static_cast<int>(in.size());
  const double ref = rms(in, from, to);
  double bestDb = 999.0;
  int bestLag = 0;
  for (int lag = cv::PsolaVoice::kGrainDelay - 500; lag <= cv::PsolaVoice::kGrainDelay + 500; ++lag) {
    double s = 0.0;
    for (int i = from; i < to; ++i) {
      const double d = out[static_cast<size_t>(i)] - in[static_cast<size_t>(i - lag)];
      s += d * d;
    }
    const double db = 20.0 * std::log10(std::sqrt(s / (to - from)) / ref + 1e-12);
    if (db < bestDb) {
      bestDb = db;
      bestLag = lag;
    }
  }
  ok &= report("passthrough in tune", bestDb <= -40.0,
               "|out - in| RMS=%.1f dB re input at lag %.0f samples (<= -40)", bestDb, bestLag);
  return ok;
}

// 2. Scale degrees: every semitone of C major from C4 to B4 in key C. A scale note is its own
// degree; a note between two scale notes sits a tie away from both and snaps to the lower one.
bool testDegrees() {
  constexpr int kDegreeOf[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
  constexpr float kDevOf[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
  int bad = 0;
  for (int s = 0; s < 12; ++s) {
    const float hz = 261.6256f * std::exp2(static_cast<float>(s) / 12.0f);
    const cv::Degree d = cv::degreeOf(hz, 0);
    if (d.index != kDegreeOf[s] || std::fabs(d.dev - kDevOf[s]) > 1e-3f) ++bad;
  }
  // G major (KEY position 1) holds F#, so F#4 is its own degree there.
  const cv::Degree fs = cv::degreeOf(369.994f, 1);
  const bool fsOk = fs.index == 6 && std::fabs(fs.dev) < 1e-3f;
  return report("scale degrees", bad == 0 && fsOk, "wrong degrees in C=%.0f (0), F#4 in G degree %.0f (6)", bad,
                fs.index);
}

// 3. A3 + 50 cents pulls to A3.
bool testCorrection() {
  const double got = outputHz(kSharp, autotuneOnly(20.0f));
  const double err = got > 0.0 ? cents(got, kA3) : 9999.0;
  return report("correction", std::fabs(err) <= 5.0, "in=%.2f Hz, out=%.2f Hz, err vs A3=%.2f cents (+-5)",
                kSharp, got, err);
}

// 4. Bb3 is not in C major: Autotune pulls it to A3 or B3.
bool testScale() {
  const double got = outputHz(kBb3, autotuneOnly(20.0f));
  const double errA = got > 0.0 ? cents(got, kA3) : 9999.0;
  const double errB = got > 0.0 ? cents(got, kB3) : 9999.0;
  return report("scale Bb3 in C", std::fabs(errA) <= 5.0 || std::fabs(errB) <= 5.0,
                "out=%.2f Hz, err vs A3=%.2f, vs B3=%.2f cents (one within +-5)", got, errA, errB);
}

// 5. Step A3 to A3 + 50 cents at 1 s, response 200 ms. Time runs from the
// first hop where the tracker reports > 225 Hz to the hop where the
// correction has moved 63 % of the way to its final value.
bool testResponse() {
  constexpr int kStep = cv::kSampleRate;
  const std::vector<float> in =
      tone(3 * cv::kSampleRate, [](int i) { return i < kStep ? kA3 : kSharp; });
  std::vector<Hop> hops;
  gFx.reset();
  run(gFx, in, autotuneOnly(200.0f), &hops);

  size_t first = 0;
  while (first < hops.size() && !(hops[first].end > kStep && hops[first].hz > 225.0f)) ++first;
  if (first == hops.size()) return report("response 200 ms", false, "tracker never reported > 225 Hz", 0.0);
  const double start = hops[first - 1].corr;
  const double final = hops.back().corr;
  const double goal = start + 0.63 * (final - start);
  double hitMs = -1.0;
  for (size_t h = first; h < hops.size(); ++h) {
    if (std::fabs(hops[h].corr - start) >= std::fabs(goal - start)) {
      const Hop& a = hops[h - 1];
      const Hop& b = hops[h];
      const double end = a.end + (b.end - a.end) * (goal - a.corr) / (b.corr - a.corr);
      hitMs = (end - hops[first].end) * 1000.0 / cv::kSampleRate;
      break;
    }
  }
  return report("response 200 ms", hitMs >= 150.0 && hitMs <= 250.0,
                "63%% of %.1f cents at %.1f ms after the tracker saw the step (150..250)",
                (final - start) * 100.0, hitMs);
}

// 6. Octave +3 on the corrected note: A3 + 50 cents gives C4, not C4 + 50. The
// granular shifter holds +-15 cents.
bool testOctaveFollows() {
  cv::PitchFxParams p = autotuneOnly(20.0f);
  p.octave.on = true;
  p.octave.mix = 1.0f;
  p.octave.semitones = 3;
  const double got = outputHz(kSharp, p);
  const double err = got > 0.0 ? cents(got, kC4) : 9999.0;
  return report("feeds octave", std::fabs(err) <= 15.0,
                "out=%.2f Hz, err vs C4=%.2f cents (+-15)", got, err);
}

// 7. Hard tune at the shipped defaults: A3 with a 5 Hz, +-30 cent vibrato comes
// out held on A3. Adam's own box measures about 2 cents median off the note.
bool testHardTune() {
  const std::vector<float> in = tone(3 * cv::kSampleRate, [](int i) {
    return kA3 * std::exp2(0.3 / 12.0 * std::sin(2.0 * kPi * 5.0 * i / cv::kSampleRate));
  });
  cv::PitchFxParams p = autotuneOnly(cv::AutotuneParams{}.responseMs);
  gFx.reset();
  const std::vector<float> out = run(gFx, in, p);
  cv::PitchTracker t;
  double sum = 0.0;
  int n = 0;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    t.push(&out[static_cast<size_t>(i)], std::min(cv::kBlock, total - i));
    if (i > kSettle && t.result().voiced) {
      const double c = cents(t.result().rawHz, kA3);
      sum += c * c;
      ++n;
    }
  }
  const double rmsCents = n > 0 ? std::sqrt(sum / n) : 9999.0;
  return report("hard tune holds vibrato", rmsCents <= 4.0,
                "+-30 cent vibrato in, %.2f cents RMS off A3 out (<= 4)", rmsCents);
}

// Largest |second difference| of x over [from, to): a cut in the waveform shows as a spike here.
double peakCurvature(const std::vector<float>& x, int from, int to) {
  double worst = 0.0;
  for (int i = std::max(from, 2); i < to; ++i) {
    const size_t k = static_cast<size_t>(i);
    worst = std::max(worst, std::fabs(static_cast<double>(x[k]) - 2.0 * x[k - 1] + x[k - 2]));
  }
  return worst;
}

// Autotune off for gapBlocks and on again over a held A3 + 50 cents. The output crossfades between
// the corrected voice and the dry input, so it may bend no harder than either does alone. Returns
// the peak from the off edge to 100 ms past the on edge over the larger steady peak.
double toggleRatio(int gapBlocks) {
  const int offAt = cv::kSampleRate / cv::kBlock * cv::kBlock;
  const int total = offAt + 2 * cv::kSampleRate;
  const std::vector<float> in = tone(kSharp, total);
  std::vector<float> out(in.size());
  const cv::PitchFxParams on = autotuneOnly(20.0f);
  cv::PitchFxParams off = on;
  off.autotune.on = false;
  gFx.reset();
  for (int i = 0; i < total; i += cv::kBlock) {
    const bool gap = i >= offAt && i < offAt + gapBlocks * cv::kBlock;
    gInProcess = true;
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, gap ? off : on);
    gInProcess = false;
  }
  const int window = cv::kSampleRate / 2;
  const double steady = std::max({peakCurvature(out, offAt - window, offAt), peakCurvature(out, total - window, total),
                                  peakCurvature(in, 0, total)});
  return peakCurvature(out, offAt, offAt + gapBlocks * cv::kBlock + cv::kSampleRate / 10) / steady;
}

// 8. A flick inside the crossfade keeps the running voice; a long gap lets the crossfade settle,
// and the voice then starts clean once the tracker has rebuilt. The fade's own slope adds under
// 1 %; a voice that loses its period mid-fade reads about 18 times steady state.
bool testToggle() {
  constexpr double kMaxRatio = 1.05;
  double worst = 0.0;
  int worstGap = 0;
  for (int gap : {1, 4, 15, 60, 300}) {
    const double r = toggleRatio(gap);
    if (r >= worst) {
      worst = r;
      worstGap = gap;
    }
  }
  return report("toggle", worst <= kMaxRatio,
                "off for 1.3 to 400 ms: peak curvature x%.3f of steady state at %.0f blocks off (<= %.2f)", worst,
                worstGap, kMaxRatio);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testDegrees();
  ok &= testCorrection();
  ok &= testScale();
  ok &= testResponse();
  ok &= testOctaveFollows();
  ok &= testHardTune();
  ok &= testToggle();
  // Every process() call above ran under the allocation guard.
  ok &= report("no allocation", true, "process() ran under the new/delete guard", 0.0);
  std::printf("%s autotune\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
