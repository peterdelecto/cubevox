// Note onsets on the shared pitch tracker: at a note that starts after a gap, the held period goes to the new
// note within about 25 ms, with no step when the tracker itself first reads voiced. Synthetic onsets, noise
// that must stay unvoiced, and Adam's three clips. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "engine/pitch.h"

#include "clip_util.h"
#include "ringed_tracker.h"
#include "signal_util.h"

#ifndef CUBEVOX_SAMPLES_DIR
#define CUBEVOX_SAMPLES_DIR "samples"
#endif

namespace {

using Signal = std::vector<float>;

constexpr double kPi = 3.14159265358979323846;
constexpr int kSr = cv::kSampleRate;
constexpr int kBlk = cv::kBlock;
constexpr float kFloor = 0.0031623f;  // -50 dBFS, the tracker's voicing floor
constexpr double kMsPerBlock = 1000.0 * kBlk / kSr;
constexpr float kClarity = cv::PitchTracker::kOnsetClarity;

// Ten harmonics at 1/k amplitude, peak 0.5, as pitch_test's tones. attack ramps the level up over that many
// samples.
Signal tone(double hz, int n, int attack = 0) {
  Signal x(static_cast<size_t>(n));
  double phase = 0.0, peak = 0.0;
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(k * phase) / k;
    phase += 2.0 * kPi * hz / kSr;
    x[static_cast<size_t>(i)] = static_cast<float>(s);
    peak = std::max(peak, std::fabs(s));
  }
  for (int i = 0; i < n; ++i) {
    const double ramp = attack > 0 && i < attack ? static_cast<double>(i) / attack : 1.0;
    x[static_cast<size_t>(i)] = static_cast<float>(0.5 * x[static_cast<size_t>(i)] / peak * ramp);
  }
  return x;
}

void append(Signal& to, const Signal& more) { to.insert(to.end(), more.begin(), more.end()); }

double semis(double a, double b) { return 12.0 * std::log2(a / b); }

// What a pitch card sees after each block: the held period, whether the tracker voices, and the level of the
// newest 5 ms.
struct Trace {
  std::vector<float> period;
  std::vector<unsigned char> voiced;
  std::vector<float> level;
  std::vector<int> runs;     // onset estimates run so far
  std::vector<float> bestDn; // the last estimate's lowest normalised difference
};

Trace runTrace(const Signal& x) {
  Trace t;
  RingedTracker tr;
  std::vector<double> prefix(x.size() + 1, 0.0);
  for (size_t i = 0; i < x.size(); ++i) prefix[i + 1] = prefix[i] + static_cast<double>(x[i]) * x[i];
  const int total = static_cast<int>(x.size());
  for (int i = 0; i + kBlk <= total; i += kBlk) {
    tr.push(&x[static_cast<size_t>(i)], kBlk);
    const int end = i + kBlk;
    const int from = std::max(0, end - kSr / 200);
    const double e = (prefix[static_cast<size_t>(end)] - prefix[static_cast<size_t>(from)]) / (end - from);
    t.period.push_back(tr.result().period);
    t.voiced.push_back(tr.result().voiced ? 1 : 0);
    t.level.push_back(static_cast<float>(std::sqrt(e)));
    t.runs.push_back(tr.onsetRunsForTest());
    t.bestDn.push_back(tr.onsetBestDnForTest());
  }
  return t;
}

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0) {
  char detail[300];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// 1. A 147 Hz note, a gap, then a note at toHz: the held period stays on the old note until one jump to the new
// note within 25 ms of the onset, and no later block steps by more than 0.2 st until 150 ms after it.
constexpr double kJumpMs = 25.0;
constexpr double kMaxStepSt = 0.2;

struct OnsetRun {
  bool ok = false;
  double jumpMs = 0.0;
  double jumpErrSt = 0.0;
  double worstStepSt = 0.0;
  double worstBeforeSt = 0.0;
};

OnsetRun syntheticOnset(double fromHz, double toHz, int attack, int gapSamples, int shift) {
  Signal x(static_cast<size_t>(kSr / 20 + shift), 0.0f);
  append(x, tone(fromHz, kSr * 3 / 10));
  append(x, Signal(static_cast<size_t>(gapSamples), 0.0f));
  const int onset = static_cast<int>(x.size());
  append(x, tone(toHz, kSr * 4 / 10, attack));
  const Trace t = runTrace(x);
  OnsetRun r;
  const size_t onsetBlock = static_cast<size_t>(onset / kBlk);  // the block that holds the onset sample
  const float before = t.period[onsetBlock - 1];
  size_t jump = 0;
  for (size_t b = onsetBlock; b < t.period.size(); ++b) {
    if (std::fabs(semis(t.period[b], before)) > kMaxStepSt) {
      jump = b;
      break;
    }
  }
  if (jump == 0) return r;
  r.jumpMs = (static_cast<double>((jump + 1) * kBlk) - onset) * 1000.0 / kSr;
  r.jumpErrSt = semis(kSr / t.period[jump], toHz);
  // The held pitch holds the old note until the jump.
  for (size_t b = onsetBlock; b < jump; ++b)
    r.worstBeforeSt = std::max(r.worstBeforeSt, std::fabs(semis(before, t.period[b])));
  const size_t last = std::min(t.period.size() - 1, jump + static_cast<size_t>(150.0 / kMsPerBlock));
  for (size_t b = jump + 1; b <= last; ++b)
    r.worstStepSt = std::max(r.worstStepSt, std::fabs(semis(t.period[b], t.period[b - 1])));
  r.ok = r.jumpMs <= kJumpMs && std::fabs(r.jumpErrSt) < kMaxStepSt && r.worstStepSt <= kMaxStepSt &&
         r.worstBeforeSt <= kMaxStepSt;
  return r;
}

bool testSyntheticOnsets() {
  bool ok = true;
  const int shifts[] = {0, 17, 101, 200, 311};
  for (int shift : shifts) {
    const OnsetRun r = syntheticOnset(147.0, 196.0, 0, kSr * 60 / 1000, shift);
    char name[100];
    std::snprintf(name, sizeof name, "onset 147 -> 196 Hz, 60 ms gap, shift %d", shift);
    ok &= report(name, r.ok, "jump at %.1f ms (<= 25), error %.3f st (< 0.2), worst later step %.3f st (<= 0.2), "
                 "held-before drift %.3f st (<= 0.2)", r.jumpMs, r.jumpErrSt, r.worstStepSt, r.worstBeforeSt);
  }
  struct Pair {
    double from, to;
    int attack;
  };
  const Pair pairs[] = {{196.0, 147.0, 0}, {220.0, 330.0, 0}, {330.0, 220.0, 0}, {147.0, 196.0, 240}, {123.0, 185.0, 0},
                        {147.0, 440.0, 0}, {147.0, 110.0, 0}};
  for (const Pair& p : pairs) {
    const OnsetRun r = syntheticOnset(p.from, p.to, p.attack, kSr * 60 / 1000, 33);
    char name[100];
    std::snprintf(name, sizeof name, "onset %.0f -> %.0f Hz, attack %d", p.from, p.to, p.attack);
    const double limit = p.attack > 0 ? kJumpMs + 1000.0 * p.attack / kSr : kJumpMs;
    const bool pass = r.jumpMs <= limit && std::fabs(r.jumpErrSt) < kMaxStepSt && r.worstStepSt <= kMaxStepSt &&
                      r.worstBeforeSt <= kMaxStepSt;
    ok &= report(name, pass, "jump at %.1f ms (<= %.0f), error %.3f st (< 0.2), worst later step %.3f st (<= 0.2)",
                 r.jumpMs, limit, r.jumpErrSt, r.worstStepSt);
  }
  return ok;
}

// With no period held yet, the first note after a reset waits for the tracker, so the cards start on a voiced
// frame as before. A note after a held one takes the estimate.
bool testFirstNote() {
  Signal x(static_cast<size_t>(kSr / 20), 0.0f);
  append(x, tone(196.0, kSr * 3 / 10));
  const Trace t = runTrace(x);
  int early = 0;
  for (size_t b = 0; b < t.period.size() && !t.voiced[b]; ++b) early += t.period[b] != 0.0f;
  return report("first note after a reset", early == 0, "blocks with a period before the first voiced one: %.0f (0)",
                early);
}

// 2. Noise at speech level and coloured noise stay unvoiced and leave the held period where it was.
Signal colouredNoise(size_t n, float rms, float pole, uint32_t seed) {
  Signal w = testutil::noise(n, 1.0f, seed);
  Signal y(n);
  float s = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    s = pole * s + (1.0f - pole) * w[i];
    y[i] = s;
  }
  double e = 0.0;
  for (float v : y) e += static_cast<double>(v) * v;
  const float g = rms / static_cast<float>(std::sqrt(e / static_cast<double>(n)));
  for (float& v : y) v *= g;
  return y;
}

// White noise through a two-pole resonator at hz (a whispered vowel's formant) with pole radius r.
Signal resonantNoise(size_t n, double hz, double r, float rms, uint32_t seed) {
  const Signal w = testutil::noise(n, 1.0f, seed);
  Signal y(n);
  const double a1 = 2.0 * r * std::cos(2.0 * kPi * hz / kSr), a2 = -r * r;
  double y1 = 0.0, y2 = 0.0, e = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double v = static_cast<double>(w[i]) + a1 * y1 + a2 * y2;
    y2 = y1;
    y1 = v;
    y[i] = static_cast<float>(v);
    e += v * v;
  }
  const float g = rms / static_cast<float>(std::sqrt(e / static_cast<double>(n)));
  for (float& v : y) v *= g;
  return y;
}

bool noiseStaysUnvoiced(const char* name, const Signal& noise, bool afterNote = true) {
  Signal x(static_cast<size_t>(kSr / 20), 0.0f);
  if (afterNote) {
    append(x, tone(147.0, kSr * 3 / 10));
    append(x, Signal(static_cast<size_t>(kSr * 60 / 1000), 0.0f));
  }
  const size_t start = x.size();
  append(x, noise);
  const Trace t = runTrace(x);
  const size_t first = start / kBlk;
  const float held = t.period[first - 1];
  int voiced = 0, moved = 0;
  for (size_t b = first + 4; b < t.period.size(); ++b) {  // the 21 ms level window still holds the note's tail
    voiced += t.voiced[b];
    moved += t.period[b] != held;
  }
  return report(name, voiced == 0 && moved == 0, "voiced blocks %.0f (0), blocks off the held period %.0f (0)", voiced,
                moved);
}

bool testNoise() {
  bool ok = true;
  const size_t n = static_cast<size_t>(kSr * 2);
  ok &= noiseStaysUnvoiced("white noise, rms 0.17 (-15 dBFS)", testutil::noise(n, 0.3f));
  ok &= noiseStaysUnvoiced("white noise, rms 0.03 (-30 dBFS)", testutil::noise(n, 0.05f, 7u));
  ok &= noiseStaysUnvoiced("white noise, rms 0.006 (-44 dBFS)", testutil::noise(n, 0.01f, 99u));
  ok &= noiseStaysUnvoiced("white noise from silence, rms 0.17", testutil::noise(n, 0.3f, 5u), false);
  const float poles[] = {0.5f, 0.8f, 0.95f, 0.99f};
  for (float pole : poles) {
    for (float rms : {0.2f, 0.03f}) {
      char name[100];
      std::snprintf(name, sizeof name, "low-passed noise pole %.2f, rms %.2f", static_cast<double>(pole),
                    static_cast<double>(rms));
      ok &= noiseStaysUnvoiced(name, colouredNoise(n, rms, pole, 12345u));
    }
  }
  // Breath: bursts of 80 ms with 50 ms of silence between.
  Signal bursts;
  for (int k = 0; k < 8; ++k) {
    append(bursts, colouredNoise(static_cast<size_t>(kSr * 80 / 1000), 0.05f, 0.8f, 77u + static_cast<uint32_t>(k)));
    append(bursts, Signal(static_cast<size_t>(kSr * 50 / 1000), 0.0f));
  }
  ok &= noiseStaysUnvoiced("breath bursts, 80 ms each", bursts);

  // The worst case for cost: loud unvoiced sound that never ends, so the estimate runs for as long as it plays.
  {
    Signal x(static_cast<size_t>(kSr / 20), 0.0f);
    append(x, tone(147.0, kSr * 3 / 10));
    append(x, Signal(static_cast<size_t>(kSr * 60 / 1000), 0.0f));
    const size_t from = x.size() / kBlk;
    append(x, testutil::noise(static_cast<size_t>(kSr * 4), 0.3f, 31u));
    const Trace t = runTrace(x);
    const double seconds = static_cast<double>(t.period.size() - from) * kMsPerBlock / 1000.0;
    const double perSecond = (t.runs.back() - t.runs[from]) / seconds;
    ok &= report("continuous loud noise, estimate runs", perSecond <= 200.0,
                 "%.0f runs in %.2f s, %.0f a second (<= 200, a quarter of the blocks)", t.runs.back(), seconds,
                 perSecond);
  }

  // A hundred bursts each of white, low-passed and whispered-vowel noise: none may take a period, and the
  // closest any estimate came to a clear dip is reported.
  struct Maker {
    const char* name;
    Signal (*make)(size_t, uint32_t);
  };
  const Maker makers[] = {
      {"white", [](size_t n, uint32_t s) { return colouredNoise(n, 0.05f, 0.0f, s); }},
      {"low-passed 0.8", [](size_t n, uint32_t s) { return colouredNoise(n, 0.05f, 0.8f, s); }},
      {"low-passed 0.95", [](size_t n, uint32_t s) { return colouredNoise(n, 0.05f, 0.95f, s); }},
      {"whisper 500 Hz", [](size_t n, uint32_t s) { return resonantNoise(n, 500.0, 0.97, 0.05f, s); }},
      {"whisper 900 Hz", [](size_t n, uint32_t s) { return resonantNoise(n, 900.0, 0.98, 0.05f, s); }},
      {"whisper 1800 Hz", [](size_t n, uint32_t s) { return resonantNoise(n, 1800.0, 0.98, 0.05f, s); }},
      {"whisper 300 Hz", [](size_t n, uint32_t s) { return resonantNoise(n, 300.0, 0.97, 0.05f, s); }},
  };
  for (const Maker& m : makers) {
    Signal x(static_cast<size_t>(kSr / 20), 0.0f);
    for (int k = 0; k < 100; ++k) {
      append(x, m.make(static_cast<size_t>(kSr * 80 / 1000), 1000u + static_cast<uint32_t>(k)));
      append(x, Signal(static_cast<size_t>(kSr * 50 / 1000), 0.0f));
    }
    const Trace t = runTrace(x);
    int voiced = 0, moved = 0;
    float closest = 1.0f;
    for (size_t b = 1; b < t.period.size(); ++b) {
      voiced += t.voiced[b];
      moved += t.period[b] != 0.0f;
      if (t.runs[b] != t.runs[b - 1]) closest = std::min(closest, t.bestDn[b]);
    }
    char name[100];
    std::snprintf(name, sizeof name, "100 bursts of %s noise", m.name);
    ok &= report(name, voiced == 0 && moved == 0,
                 "voiced blocks %.0f (0), blocks with a period %.0f (0), closest estimate difference %.3f", voiced,
                 moved, static_cast<double>(closest));
  }
  return ok;
}

// 3. Adam's clips. An onset is the first block whose newest 5 ms hold at least the voicing floor after 10 quiet
// blocks, with the tracker unvoiced. It is a note when the tracker reads voiced within 300 ms, and a breath or
// consonant when it does not. Per note the test logs
//   1. the delay from the onset to the tracker's first voiced reading,
//   2. the handoff step, the largest single-block change of the held period from that block on for 16 blocks
//      (21 ms), which is what a card plays as a step when the tracker takes over,
//   3. the settle time, from the onset to the block where the held period is within 0.5 st of the value it has
//      21 ms after the voiced reading, and stays there.
struct ClipStats {
  int onsets = 0;
  int over1 = 0;
  int nonNotes = 0;
  int accepted = 0;       // blocks where the held period moved while unvoiced: an estimate took it
  int falseAccepted = 0;  // of those, the ones the tracker did not follow with a voiced reading within 300 ms
  int runs = 0;           // onset estimates run
  double seconds = 0.0;
  float closestNonNoteDn = 1.0f;
  float worstAcceptedDn = 0.0f;
  double worstStep = 0.0;
  double worstDelayMs = 0.0;
  double sumDelayMs = 0.0;
  double worstSettleMs = 0.0;
  double sumSettleMs = 0.0;
};

constexpr int kGapBlocks = 8;       // quiet blocks that make a rest, 10.7 ms
constexpr int kGiveUpBlocks = 225;  // 300 ms
constexpr int kHandoffBlocks = 16;  // 21 ms
constexpr double kOverSt = 1.0;

void clipOnsets(const char* label, const Signal& x, ClipStats& s, bool verbose) {
  const Trace t = runTrace(x);
  const int nb = static_cast<int>(t.period.size());
  auto p = [&](int k) { return t.period[static_cast<size_t>(k)]; };
  int quiet = 0;
  bool seen = false;  // the tracker has voiced earlier in the clip, so a note before this one exists
  for (int b = 1; b < nb; ++b) {
    seen = seen || t.voiced[static_cast<size_t>(b - 1)];
    const bool loud = t.level[static_cast<size_t>(b)] >= kFloor;
    if (!loud) {
      ++quiet;
      continue;
    }
    const int was = quiet;
    quiet = 0;
    if (was < kGapBlocks || t.voiced[static_cast<size_t>(b - 1)] || t.voiced[static_cast<size_t>(b)]) continue;
    int v = -1;
    for (int k = b; k < std::min(nb, b + kGiveUpBlocks); ++k) {
      if (t.voiced[static_cast<size_t>(k)]) {
        v = k;
        break;
      }
    }
    if (v < 0) {
      ++s.nonNotes;
      float closest = 1.0f;  // the lowest normalised difference any estimate in the window reached
      for (int k = b; k < std::min(nb, b + kGiveUpBlocks); ++k)
        if (t.runs[static_cast<size_t>(k)] != t.runs[static_cast<size_t>(k - 1)])
          closest = std::min(closest, t.bestDn[static_cast<size_t>(k)]);
      s.closestNonNoteDn = std::min(s.closestNonNoteDn, closest);
      if (verbose) std::printf("    %s  non-note at %.3f s, closest estimate difference %.3f\n", label, b * kMsPerBlock / 1000.0, static_cast<double>(closest));
      continue;
    }
    if (!seen || v + kHandoffBlocks >= nb) continue;  // the first note of a clip has no pitch to step from
    ++s.onsets;
    const double delay = (v - b + 1) * kMsPerBlock;
    double step = 0.0;
    for (int k = v; k < v + kHandoffBlocks; ++k) step = std::max(step, std::fabs(semis(p(k), p(k - 1))));
    const float settled = p(v + kHandoffBlocks);
    int settle = v + kHandoffBlocks;
    for (int k = settle; k >= b && std::fabs(semis(p(k), settled)) <= 0.5; --k) settle = k;
    const double settleMs = (settle - b + 1) * kMsPerBlock;
    s.over1 += step > kOverSt;
    s.worstStep = std::max(s.worstStep, step);
    s.worstDelayMs = std::max(s.worstDelayMs, delay);
    s.sumDelayMs += delay;
    s.worstSettleMs = std::max(s.worstSettleMs, settleMs);
    s.sumSettleMs += settleMs;
    int first = -1;  // the first block before the voiced reading where the held period moved
    for (int k = b; k < v; ++k) {
      if (p(k) != p(k - 1)) {
        first = k;
        break;
      }
    }
    if (verbose && step > 0.4) {
      std::printf("      trace from voiced-%d, st vs settled, * = voiced:", 14);
      for (int k = v - 14; k < v + kHandoffBlocks + 1; ++k)
        std::printf(" %.2f%s", semis(p(k), settled), t.voiced[static_cast<size_t>(k)] ? "*" : "");
      std::printf("\n");
    }
    if (verbose)
      std::printf("    %s  onset %.3f s  voiced after %6.1f ms  handoff step %5.2f st  settled after %6.1f ms  "
                  "held period moved after %s%.1f ms\n",
                  label, b * kMsPerBlock / 1000.0, delay, step, settleMs, first < 0 ? "never " : "",
                  first < 0 ? 0.0 : (first - b + 1) * kMsPerBlock);
  }
  // Every estimate that took the period, and whether a voiced reading followed.
  for (int k = 1; k < nb; ++k) {
    if (t.voiced[static_cast<size_t>(k)] || p(k) == p(k - 1)) continue;
    ++s.accepted;
    s.worstAcceptedDn = std::max(s.worstAcceptedDn, t.bestDn[static_cast<size_t>(k)]);
    bool followed = false;
    for (int j = k; j < std::min(nb, k + kGiveUpBlocks) && !followed; ++j) followed = t.voiced[static_cast<size_t>(j)];
    if (verbose && followed) {
      int j = k;
      while (!t.voiced[static_cast<size_t>(j)]) ++j;
      std::printf("    %s  estimate at %.3f s moves the held period %.2f st, the tracker voices %.1f ms later\n", label,
                  k * kMsPerBlock / 1000.0, semis(p(k), p(k - 1)), (j - k) * kMsPerBlock);
    }
    if (!followed && k + kGiveUpBlocks < nb) {
      ++s.falseAccepted;
      if (verbose) std::printf("    %s  estimate at %.3f s not followed by a voiced reading\n", label, k * kMsPerBlock / 1000.0);
    }
  }
  s.runs += t.runs.back();
  s.seconds += nb * kMsPerBlock / 1000.0;
}

bool testClips() {
  bool whole = true;
  const std::vector<Signal> clips = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (clips.empty() || !whole) {
    std::printf("SKIP clips  samples/ missing or incomplete\n");
    return true;
  }
  ClipStats s;
  const char* names[] = {"clip1", "clip2", "clip3"};
  for (size_t i = 0; i < clips.size(); ++i) clipOnsets(names[i], clips[i], s, std::getenv("ONSET_VERBOSE") != nullptr);
  std::printf("INFO clips  notes %d, handoff step over 1 st %d, worst step %.2f st, delay to voiced: worst %.1f ms "
              "mean %.1f ms, settle: worst %.1f ms mean %.1f ms\n",
              s.onsets, s.over1, s.worstStep, s.worstDelayMs, s.onsets ? s.sumDelayMs / s.onsets : 0.0,
              s.worstSettleMs, s.onsets ? s.sumSettleMs / s.onsets : 0.0);
  std::printf("INFO clips  estimates took the period %d times, %d not followed by a voiced reading; non-note onsets %d, "
              "closest estimate difference %.3f, worst accepted %.3f (clear below %.2f); %d estimate runs in %.1f s, "
              "%.0f a second\n",
              s.accepted, s.falseAccepted, s.nonNotes, static_cast<double>(s.closestNonNoteDn),
              static_cast<double>(s.worstAcceptedDn), static_cast<double>(1.0f - kClarity), s.runs, s.seconds,
              s.runs / s.seconds);
  return report("clips: no handoff step over 1 st, no estimate without a voiced reading after it",
                s.onsets > 0 && s.over1 == 0 && s.falseAccepted == 0,
                "notes %.0f, steps over 1 st %.0f (0), estimates not followed by a voiced reading %.0f (0)", s.onsets,
                s.over1, s.falseAccepted);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testClips();
  ok &= testSyntheticOnsets();
  ok &= testFirstNote();
  ok &= testNoise();
  std::printf("%s pitch_onset\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
