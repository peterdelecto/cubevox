// Checks the scope on the host: the trigger that holds a vowel still, the column rule that keeps the
// trace one line, the scale and the silence level, the ring's order, and a two-thread
// stress that a read never mixes two moments. Spec: docs/specs/2026-11-06-scope-design.md.

#include "support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/scope.h"
#include "engine/dsp.h"

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kTickSamples = 1920;  // one UI tick, 40 ms
constexpr int kTicks = 100;

// The stress test's writer waits this long between blocks. The box writes one every 1333 us.
constexpr auto kPause = std::chrono::microseconds(40);

using Samples = std::array<float, kScopeReadSamples>;

// A sung vowel at fundamental `f0` Hz that glides by `glide` Hz per sample: harmonics 1 to 10 weighted
// around 700 Hz, in cosine phase so each cycle has one strongest peak, as a glottal pulse has. It
// crosses zero several times a cycle. normalise() scales it to a peak of 1 over two cycles.
struct Vowel {
  double f0 = 98.0;
  double glide = 0.0;
  double dc = 0.0;       // offset, in units of the peak
  double breath = 0.0;   // noise RMS, in units of the vowel's RMS
  double peakScale = 1.0;
  double rms = 1.0;

  // Cycles elapsed at sample n.
  double cycles(double n) const { return (f0 * n + 0.5 * glide * n * n) / kFs; }

  double raw(double n) const {
    const double theta = 2.0 * kPi * cycles(n);
    double sum = 0.0;
    for (int h = 1; h <= 10; ++h) {
      const double hz = h * f0;
      const double w = std::exp(-0.5 * (hz - 700.0) * (hz - 700.0) / (250.0 * 250.0));
      sum += w * std::cos(h * theta);
    }
    return sum;
  }

  // A hash of n in -1..1, the same value whichever window asks for it.
  static double noise(int64_t n) {
    uint64_t x = static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    return static_cast<double>(x >> 11) / 4503599627370496.0 - 1.0;  // 2^52
  }

  void normalise() {
    double peak = 0.0;
    double sum = 0.0;
    const int span = static_cast<int>(2.0 * kFs / f0) + 1;
    for (int n = 0; n < span; ++n) {
      const double v = raw(n);
      peak = std::max(peak, std::fabs(v));
      sum += v * v;
    }
    peakScale = 1.0 / peak;
    rms = std::sqrt(sum / span) * peakScale;
  }

  double at(int64_t n) const {
    return peakScale * raw(static_cast<double>(n)) + dc + breath * rms * 1.7320508 * noise(n);
  }
};

Vowel makeVowel(double f0, double glide = 0.0, double dc = 0.0, double breath = 0.0) {
  Vowel v;
  v.f0 = f0;
  v.glide = glide;
  v.dc = dc;
  v.breath = breath;
  v.normalise();
  return v;
}

// The read buffer for the tick whose newest sample is `newest`.
void fill(const Vowel& v, int64_t newest, Samples& out) {
  for (int i = 0; i < kScopeReadSamples; ++i) {
    out[static_cast<size_t>(i)] = static_cast<float>(v.at(newest - (kScopeReadSamples - 1) + i));
  }
}

// The worst distance, in samples, between where the frame starts in a tick and where it started in
// the first tick, measured in cycles of the vowel so a shift by whole periods counts as no shift.
double worstStartDrift(const Vowel& v, double hysteresis) {
  Samples buf;
  double first = 0.0;
  double worst = 0.0;
  for (int t = 0; t < kTicks; ++t) {
    const int64_t newest = 20000 + static_cast<int64_t>(t) * kTickSamples;
    fill(v, newest, buf);
    const int start = scopeStart(buf.data(), static_cast<float>(hysteresis));
    const double at = static_cast<double>(newest - (kScopeReadSamples - 1) + start);
    const double phase = v.cycles(at);
    if (t == 0) first = phase;
    const double instantF = v.f0 + v.glide * at;
    const double drift = (phase - first) - std::round(phase - first);  // in cycles, wrapped
    worst = std::max(worst, std::fabs(drift) * kFs / instantF);
  }
  return worst;
}

void checkTriggerSteady() {
  const double hysteresis = 0.05;
  const double tones[] = {98.0, 147.0, 220.0, 49.0};
  for (double f0 : tones) {
    const double drift = worstStartDrift(makeVowel(f0), hysteresis);
    char what[160];
    std::snprintf(what, sizeof what, "scope trigger: a vowel at %.0f Hz starts on the same cycle feature each tick, worst drift %.2f samples", f0, drift);
    check(drift <= 1.0, what);
  }
  const double dc = worstStartDrift(makeVowel(98.0, 0.0, 0.02), hysteresis);
  check(dc <= 1.0, "scope trigger: a 2 % offset does not move the start");
  const double breath = worstStartDrift(makeVowel(98.0, 0.0, 0.0, 0.1), hysteresis);
  char what[160];
  std::snprintf(what, sizeof what, "scope trigger: breath at -20 dB moves the start by %.2f samples, under 3", breath);
  check(breath <= 3.0, what);
  const double glide = worstStartDrift(makeVowel(98.0, 12.0 / (kTicks * kTickSamples)), hysteresis);
  std::snprintf(what, sizeof what, "scope trigger: a glide from 98 to 110 Hz moves the start by %.2f samples, under 1.5", glide);
  check(glide <= 1.5, what);
  const double glideBreath = worstStartDrift(makeVowel(147.0, -20.0 / (kTicks * kTickSamples), 0.02, 0.1), hysteresis);
  std::snprintf(what, sizeof what, "scope trigger: a falling glide with breath and offset moves the start by %.2f samples, under 3", glideBreath);
  check(glideBreath <= 3.0, what);
}

// A sample is -1 for the first `crossAt` samples and +1 from there on, with a high point after.
Samples stepAt(int crossAt, float depth) {
  Samples s;
  for (int i = 0; i < kScopeReadSamples; ++i) s[static_cast<size_t>(i)] = i < crossAt ? -depth : 0.5f;
  s[1000] = 1.0f;
  return s;
}

void checkTriggerEdges() {
  constexpr int kOldest = kScopeReadSamples - kScopeSearchBack;
  constexpr int kFree = kScopeReadSamples - kScopeFrameSamples;

  // No crossing at all: all positive, all negative and all zero run free.
  Samples positive;
  positive.fill(0.4f);
  Samples negative;
  negative.fill(-0.4f);
  Samples zero;
  zero.fill(0.0f);
  check(scopeStart(positive.data(), 0.05f) == kFree && scopeStart(negative.data(), 0.05f) == kFree &&
            scopeStart(zero.data(), 0.05f) == kFree,
        "scope trigger: a span with no crossing free-runs on the newest 824 samples");

  // The peak must lie in the span, but its crossing may lie before the span, so a crossing at the
  // span's old end and either side of it all lock and never flip to free-running. Only a read
  // without a crossing (the first sample cannot be one) runs free.
  check(scopeStart(stepAt(kOldest, 1.0f).data(), 0.05f) == kOldest, "scope trigger: a crossing at the span's old end locks");
  check(scopeStart(stepAt(kOldest + 1, 1.0f).data(), 0.05f) == kOldest + 1, "scope trigger: a crossing one sample inside locks");
  check(scopeStart(stepAt(kOldest - 1, 1.0f).data(), 0.05f) == kOldest - 1, "scope trigger: a crossing one sample before the span still locks");
  check(scopeStart(stepAt(1, 1.0f).data(), 0.05f) == 1, "scope trigger: a crossing at the second sample of the read locks");
  check(scopeStart(stepAt(0, 1.0f).data(), 0.05f) == kFree, "scope trigger: no sample before the first one, so no crossing and the frame runs free");

  // The dip before a crossing must go under -hysteresis times the largest sample in the span, so the
  // trigger follows the level of the signal.
  check(scopeStart(stepAt(500, 0.04f).data(), 0.05f) == kFree, "scope trigger: a dip of 4 % does not arm a 5 % trigger");
  check(scopeStart(stepAt(500, 0.06f).data(), 0.05f) == 500, "scope trigger: a dip of 6 % arms a 5 % trigger");
  for (float gain : {0.001f, 1000.0f}) {
    Samples shallow = stepAt(500, 0.04f);
    Samples deep = stepAt(500, 0.06f);
    for (float& v : shallow) v *= gain;
    for (float& v : deep) v *= gain;
    check(scopeStart(shallow.data(), 0.05f) == kFree && scopeStart(deep.data(), 0.05f) == 500,
          ("scope trigger: the 5 % arming holds for a signal scaled by " + std::to_string(gain)).c_str());
  }

  // A sine's start is a rising crossing, the one before its largest peak.
  Samples sine;
  for (int i = 0; i < kScopeReadSamples; ++i) sine[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * 3.14159265f * 150.0f * static_cast<float>(i) / 48000.0f + 0.7f);
  const int start = scopeStart(sine.data(), 0.05f);
  check(start >= kOldest && start <= kFree && sine[static_cast<size_t>(start)] >= 0.0f && sine[static_cast<size_t>(start - 1)] < 0.0f,
        "scope trigger: a sine starts on a rising zero crossing inside the span");
}

// True when the runs of two neighbouring columns share a row.
bool sharesRow(const ScopeRun& a, const ScopeRun& b) { return std::max(a.lo, b.lo) <= std::min(a.hi, b.hi); }

bool connected(const ScopeFrame& f) {
  for (int c = 1; c < kScopeColumns; ++c) {
    if (!sharesRow(f.column[static_cast<size_t>(c - 1)], f.column[static_cast<size_t>(c)])) return false;
  }
  return true;
}

bool inRange(const ScopeFrame& f) {
  for (const ScopeRun& run : f.column) {
    if (run.lo > run.hi || run.lo < -kScopeHalfRows || run.hi > kScopeHalfRows) return false;
  }
  return true;
}

void checkColumns() {
  uint32_t seed = 987654321u;
  const auto next = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / 16777216.0f * 2.0f - 1.0f;
  };

  Samples random;
  Samples clipped;
  Samples square;
  Samples alternating;
  for (int i = 0; i < kScopeReadSamples; ++i) {
    const size_t k = static_cast<size_t>(i);
    random[k] = 0.3f * next();
    clipped[k] = 40.0f * next();
    square[k] = ((i / 3) % 2) ? 1.0f : -1.0f;
    alternating[k] = (i % 2) ? 1.0f : -1.0f;
  }
  const Samples* inputs[] = {&random, &clipped, &square, &alternating};
  const char* names[] = {"random", "clipped", "full-scale square", "full-scale alternating"};
  for (int n = 0; n < 4; ++n) {
    const ScopeFrame f = scopeFrame(inputs[n]->data(), 0.0f);
    check(connected(f) && inRange(f), (std::string("scope columns: ") + names[n] + " input gives one line, every neighbouring pair sharing a row, inside rows 10 to 54").c_str());
  }
  const ScopeFrame hard = scopeFrame(alternating.data(), 0.0f);
  check(hard.column[10].lo == -kScopeHalfRows && hard.column[10].hi == kScopeHalfRows,
        "scope columns: a full swing in both directions spans rows 10 and 54");

  // The first column meets its own first sample, so it is its own run and a single sample is one row.
  Samples flat;
  flat.fill(0.0f);
  bool zeros = true;
  const ScopeFrame line = scopeFrame(flat.data(), 0.0f);
  for (const ScopeRun& run : line.column) zeros = zeros && run.lo == 0 && run.hi == 0;
  check(zeros && line.scale == 0.0f, "scope columns: a frame of zeros after a scale of 0 is row 32 in every column");
  const ScopeFrame held = scopeFrame(flat.data(), 1.0f);
  bool heldZeros = true;
  for (const ScopeRun& run : held.column) heldZeros = heldZeros && run.lo == 0 && run.hi == 0;
  check(heldZeros, "scope columns: a frame of zeros under a positive last scale is row 32 in every column");
  for (float bad : {-1.0f, std::nanf("")}) {
    const ScopeFrame fresh = scopeFrame(random.data(), bad);
    check(connected(fresh) && inRange(fresh) && fresh.scale > 0.0f,
          "scope columns: a last scale that is negative or not a number is as a scale of 0");
  }
  Samples hiss;
  for (size_t k = 0; k < hiss.size(); ++k) hiss[k] = 0.0019f * next();
  const ScopeFrame silent = scopeFrame(hiss.data(), 1.0f);
  bool silentZeros = true;
  for (const ScopeRun& run : silent.column) silentZeros = silentZeros && run.lo == 0 && run.hi == 0;
  check(silentZeros && silent.scale == 0.0f, "scope columns: a frame with its peak under -54 dBFS gives the line and a scale of 0 whatever the last scale");

  // Every 8th sample at the top of the scale and the rest at the bottom: each column is tall and the
  // runs still join.
  Samples comb;
  for (int i = 0; i < kScopeReadSamples; ++i) comb[static_cast<size_t>(i)] = (i % 8 == 0) ? 1.0f : -1.0f;
  check(connected(scopeFrame(comb.data(), 0.0f)), "scope columns: a comb across the scale stays joined");
}

// A 150 Hz sine of RMS `rms` filling a read.
Samples sineOfRms(float rms) {
  Samples s;
  for (int i = 0; i < kScopeReadSamples; ++i) {
    s[static_cast<size_t>(i)] = 1.41421356f * rms * std::sin(2.0f * 3.14159265f * static_cast<float>(i % 320) / 320.0f);
  }
  return s;
}

struct Reach {
  int up = 0;
  int down = 0;
};

Reach reachOf(const ScopeFrame& f) {
  Reach r;
  for (const ScopeRun& run : f.column) {
    r.up = std::max(r.up, -static_cast<int>(run.lo));
    r.down = std::max(r.down, static_cast<int>(run.hi));
  }
  return r;
}

void checkScale() {
  constexpr float kFall = 0.70794578f;  // 3 dB

  // The frame's own peak sets the scale, so a sine at -20 dBFS and one at -40 dBFS each reach rows 10
  // and 54, and the scale handed back is the peak.
  for (float rms : {0.1f, 0.01f}) {
    const Samples tone = sineOfRms(rms);
    const ScopeFrame f = scopeFrame(tone.data(), 0.0f);
    const Reach reach = reachOf(f);
    check(reach.up == kScopeHalfRows && reach.down == kScopeHalfRows && std::fabs(f.scale - 1.41421356f * rms) < 1e-4f * rms,
          ("scope scale: a sine of " + std::to_string(rms) + " RMS reaches rows 10 and 54 and returns its peak as the scale").c_str());
  }

  // The scale rises at once: a frame 20 dB over the last one fills the height.
  const Samples quietTone = sineOfRms(0.01f);
  const Samples loudTone = sineOfRms(0.1f);
  const float quietScale = scopeFrame(quietTone.data(), 0.0f).scale;
  const ScopeFrame rise = scopeFrame(loudTone.data(), quietScale);
  check(reachOf(rise).up == kScopeHalfRows && std::fabs(rise.scale - 0.141421356f) < 1e-5f,
        "scope scale: a frame 20 dB louder than the last fills the height and sets the scale at once");

  // A frame at half the previous frame's peak falls 3 dB and draws at 70 % of the height.
  const Samples half = sineOfRms(0.05f);
  const float loudScale = scopeFrame(loudTone.data(), 0.0f).scale;
  const ScopeFrame fell = scopeFrame(half.data(), loudScale);
  const int want = static_cast<int>(std::lround(kScopeHalfRows * 0.5 / static_cast<double>(kFall)));
  check(std::fabs(fell.scale - loudScale * kFall) < 1e-5f && std::abs(reachOf(fell).up - want) <= 1 && reachOf(fell).up < kScopeHalfRows,
        ("scope scale: a frame at half the last peak falls 3 dB and reaches " + std::to_string(want) + " rows").c_str());

  // A frame 12 dB under the last one is short at first and fills the height by the fourth frame.
  const Samples under = sineOfRms(0.1f * 0.2511886f);
  float scale = loudScale;
  int reached[5] = {};
  for (int k = 1; k <= 4; ++k) {
    const ScopeFrame f = scopeFrame(under.data(), scale);
    scale = f.scale;
    reached[k] = reachOf(f).up;
  }
  check(reached[1] < reached[2] && reached[2] < reached[3] && reached[3] < kScopeHalfRows && reached[4] == kScopeHalfRows,
        ("scope scale: a frame 12 dB under the last grows through " + std::to_string(reached[1]) + ", " + std::to_string(reached[2]) + ", " +
         std::to_string(reached[3]) + " rows to " + std::to_string(reached[4]) + " on the fourth").c_str());

  // A vowel fills the height at any level; the scale is its own peak.
  const Vowel v = makeVowel(147.0);
  Samples vowel;
  fill(v, 30000, vowel);
  check(reachOf(scopeFrame(vowel.data(), 0.0f)).up >= kScopeHalfRows - 1, "scope scale: a vowel reaches the top within a row");

  // A sine at -48 dBFS peak is over the silence level and under the minimum scale, so it draws at
  // 9 rows (22 rows * 10^(-8/20) / 0.01 rounds to 9) and the scale handed back is the minimum.
  const float minPeak = cv::dbToGain(-48.0f);
  const Samples faint = sineOfRms(minPeak / 1.41421356f);
  const ScopeFrame faintFrame = scopeFrame(faint.data(), 0.0f);
  check(reachOf(faintFrame).up == 9 && reachOf(faintFrame).down == 9 && faintFrame.scale == kScopeMinScale,
        "scope scale: a sine at -48 dBFS draws at 9 rows on the minimum scale of -40 dBFS");

  // The constants are the levels the spec names.
  check(std::fabs(kScopeSilencePeak - cv::dbToGain(-54.0f)) < 1e-7f && std::fabs(kScopeMinScale - cv::dbToGain(-40.0f)) < 1e-7f,
        "scope scale: the silence level is -54 dBFS and the minimum scale is -40 dBFS");

  // A sine just under the silence level is the line, and one just over it is not.
  const Samples under54 = sineOfRms(cv::dbToGain(-54.2f) / 1.41421356f);
  const Samples over54 = sineOfRms(cv::dbToGain(-53.8f) / 1.41421356f);
  const ScopeFrame lineFrame = scopeFrame(under54.data(), 0.5f);
  check(reachOf(lineFrame).up == 0 && reachOf(lineFrame).down == 0 && lineFrame.scale == 0.0f,
        "scope scale: a frame with its peak at -54.2 dBFS is the line and returns a scale of 0");
  check(scopeFrame(over54.data(), 0.0f).scale > 0.0f && reachOf(scopeFrame(over54.data(), 0.0f)).up >= 4,
        "scope scale: a frame with its peak at -53.8 dBFS draws");
}

// An echo tail decays from -20 to -60 dBFS over 2 s. The newest sample steps one tick (1920 samples)
// at a time. The ticks whose read holds nothing over the silence level give the line, the ticks whose
// newest 824 samples hold something over it draw, and the draw then the line is one switch.
void checkTail() {
  constexpr int kTailSamples = 96000;
  std::vector<float> tail(static_cast<size_t>(kTailSamples));
  for (int i = 0; i < kTailSamples; ++i) {
    const float db = -20.0f - 20.0f * static_cast<float>(i) / 48000.0f;
    tail[static_cast<size_t>(i)] = cv::dbToGain(db) * std::sin(2.0f * 3.14159265f * static_cast<float>(i % 320) / 320.0f);
  }
  const auto peakOf = [&tail](int from, int to) {
    float peak = 0.0f;
    for (int i = from; i < to; ++i) peak = std::max(peak, std::fabs(tail[static_cast<size_t>(i)]));
    return peak;
  };

  Samples read;
  float scale = 0.0f;
  int drawn = 0;
  int switches = 0;
  bool wasDrawn = true;
  bool follows = true;
  bool thin = true;
  for (int newest = kScopeReadSamples; newest <= kTailSamples; newest += kTickSamples) {
    std::copy(tail.begin() + (newest - kScopeReadSamples), tail.begin() + newest, read.begin());
    const ScopeFrame f = scopeFrame(read.data(), scale);
    scale = f.scale;
    const Reach reach = reachOf(f);
    const bool draws = f.scale > 0.0f;
    if (peakOf(newest - kScopeFrameSamples, newest) >= kScopeSilencePeak) follows = follows && draws;
    if (peakOf(newest - kScopeReadSamples, newest) < kScopeSilencePeak) follows = follows && !draws;
    if (!draws) follows = follows && reach.up == 0 && reach.down == 0;
    if (draws) thin = thin && std::max(reach.up, reach.down) >= 1;
    if (draws) ++drawn;
    if (draws != wasDrawn) ++switches;
    wasDrawn = draws;
  }
  check(follows, "scope tail: every frame over -54 dBFS draws and every frame under it is the line");
  check(thin, "scope tail: a drawn frame is at least one row tall");
  check(drawn >= 40 && drawn <= 44 && switches == 1,
        ("scope tail: the tail draws for " + std::to_string(drawn) + " ticks, about 1.7 s, then goes to the line once").c_str());
}

// The ring returns the newest samples in order across the wrap, with zeros ahead of a ring that has
// not filled.
void checkRingOrder() {
  auto ring = std::make_unique<ScopeRing>();
  Samples out;
  check(ring->read(out.data()) && std::all_of(out.begin(), out.end(), [](float v) { return v == 0.0f; }),
        "scope ring: a new ring reads as zeros");

  std::array<float, cv::kBlock> block;
  int next = 0;
  const auto writeBlocks = [&](int blocks) {
    for (int b = 0; b < blocks; ++b) {
      for (float& v : block) v = static_cast<float>(next++);
      ring->write(block.data());
    }
  };
  writeBlocks(5);
  check(ring->read(out.data()), "scope ring: a read of a part-filled ring succeeds");
  const int lead = kScopeReadSamples - 5 * cv::kBlock;
  bool partial = true;
  for (int i = 0; i < kScopeReadSamples; ++i) {
    const float want = i < lead ? 0.0f : static_cast<float>(i - lead);
    partial = partial && out[static_cast<size_t>(i)] == want;
  }
  check(partial, "scope ring: five blocks read as zeros ahead of the five, oldest first");

  writeBlocks(100);  // wraps the 32 slots three times over
  check(ring->read(out.data()), "scope ring: a read after wrapping succeeds");
  bool ordered = true;
  for (int i = 0; i < kScopeReadSamples; ++i) {
    ordered = ordered && out[static_cast<size_t>(i)] == static_cast<float>(next - kScopeReadSamples + i);
  }
  check(ordered, "scope ring: after wrap-around the read is the newest 1984 samples in order");
}

// One thread writes blocks whose samples all hold the block number (from 1), the other reads as fast
// as it can. Every read that succeeds is a run of consecutive block numbers, the oldest possibly
// zero padding, never a mix of two moments.
void checkRingStress() {
  constexpr int kBlocksToWrite = 30000;
  auto ring = std::make_unique<ScopeRing>();
  std::atomic<bool> done{false};
  std::thread writer([&ring, &done]() {
    std::array<float, cv::kBlock> block;
    for (int b = 1; b <= kBlocksToWrite; ++b) {
      block.fill(static_cast<float>(b));
      ring->write(block.data());
      const auto until = std::chrono::steady_clock::now() + kPause;
      while (std::chrono::steady_clock::now() < until) {
      }
    }
    done.store(true, std::memory_order_release);
  });

  uint32_t reads = 0;
  uint32_t failed = 0;
  uint32_t torn = 0;
  Samples out;
  while (!done.load(std::memory_order_acquire)) {
    if (!ring->read(out.data())) {
      ++failed;
      continue;
    }
    ++reads;
    for (int j = 0; j < kScopeReadBlocks; ++j) {
      const float* blk = out.data() + j * cv::kBlock;
      bool constant = true;
      for (int i = 1; i < cv::kBlock; ++i) constant = constant && blk[i] == blk[0];
      const float prev = j == 0 ? blk[0] - 1.0f : out[static_cast<size_t>(j * cv::kBlock - 1)];
      const bool consecutive = blk[0] == prev + 1.0f || (blk[0] == 0.0f && prev == 0.0f);
      if (!constant || !consecutive) ++torn;
    }
  }
  writer.join();
  check(reads > 100 && torn == 0, ("scope ring: " + std::to_string(reads) + " reads against a writing thread, " +
                                   std::to_string(failed) + " retried out, " + std::to_string(torn) + " torn blocks").c_str());
}

}  // namespace

void checkScope() {
  checkTriggerSteady();
  checkTriggerEdges();
  checkColumns();
  checkScale();
  checkTail();
  checkRingOrder();
  checkRingStress();
}
