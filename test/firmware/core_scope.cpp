// Checks the scope on the host: the trigger that holds a vowel still, the column rule that keeps the
// trace one line, the scale and the silence rule, the ring's order and energy, and a two-thread
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

#include "core/level.h"
#include "core/output_stage.h"
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

  // The dip before a crossing must go under -hysteresis.
  check(scopeStart(stepAt(500, 0.04f).data(), 0.05f) == kFree, "scope trigger: a dip of 4 % does not arm a 5 % trigger");
  check(scopeStart(stepAt(500, 0.06f).data(), 0.05f) == 500, "scope trigger: a dip of 6 % arms a 5 % trigger");

  // A sine's start is a rising crossing, the one before its largest peak.
  Samples sine;
  for (int i = 0; i < kScopeReadSamples; ++i) sine[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * 3.14159265f * 150.0f * static_cast<float>(i) / 48000.0f + 0.7f);
  const int start = scopeStart(sine.data(), 0.025f);
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
  const float topDb = -10.0f;  // the scale's top is the crest x 0.316
  const Samples* inputs[] = {&random, &clipped, &square, &alternating};
  const char* names[] = {"random", "clipped", "full-scale square", "full-scale alternating"};
  for (int n = 0; n < 4; ++n) {
    const ScopeFrame f = scopeFrame(inputs[n]->data(), topDb, false);
    check(connected(f) && inRange(f), (std::string("scope columns: ") + names[n] + " input gives one line, every neighbouring pair sharing a row, inside rows 10 to 54").c_str());
  }
  const ScopeFrame hard = scopeFrame(alternating.data(), -60.0f, false);
  check(hard.column[10].lo == -kScopeHalfRows && hard.column[10].hi == kScopeHalfRows,
        "scope columns: a swing past the scale in both directions clips to rows 10 and 54");

  // The first column meets its own first sample, so it is its own run and a single sample is one row.
  Samples flat;
  flat.fill(0.0f);
  bool zeros = true;
  const ScopeFrame line = scopeFrame(flat.data(), -20.0f, false);
  for (const ScopeRun& run : line.column) zeros = zeros && run.lo == 0 && run.hi == 0;
  check(zeros, "scope columns: a frame of zeros is row 32 in every column");
  const ScopeFrame silent = scopeFrame(random.data(), -20.0f, true);
  bool silentZeros = true;
  for (const ScopeRun& run : silent.column) silentZeros = silentZeros && run.lo == 0 && run.hi == 0;
  check(silentZeros, "scope columns: a tick under the floor gives the line whatever the samples hold");

  // Every 8th sample at the top of the scale and the rest at the bottom: each column is tall and the
  // runs still join.
  Samples comb;
  for (int i = 0; i < kScopeReadSamples; ++i) comb[static_cast<size_t>(i)] = (i % 8 == 0) ? 1.0f : -1.0f;
  check(connected(scopeFrame(comb.data(), -20.0f, false)), "scope columns: a comb across the scale stays joined");
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
  constexpr double kCrest = static_cast<double>(kScopeCrest);

  // At the loudest tick the scale's top is the crest times the tick's RMS, so a vowel whose peak over
  // RMS is under the crest does not clip. The synthetic vowel reaches 22 x its own crest / the crest.
  const Vowel v = makeVowel(147.0);
  Samples vowel;
  fill(v, 30000, vowel);
  double sum = 0.0;
  double peak = 0.0;
  for (float sample : vowel) {
    const double x = static_cast<double>(sample);
    sum += x * x;
    peak = std::max(peak, std::fabs(x));
  }
  const double rms = std::sqrt(sum / static_cast<double>(vowel.size()));
  const double vowelCrest = peak / rms;
  const float topDb = 20.0f * std::log10(static_cast<float>(rms));
  const Reach reach = reachOf(scopeFrame(vowel.data(), topDb, false));
  const int wantVowel = static_cast<int>(std::lround(kScopeHalfRows * vowelCrest / kCrest));
  check(vowelCrest < kCrest && reach.up < kScopeHalfRows && std::abs(reach.up - wantVowel) <= 1,
        ("scope scale: a vowel of crest " + std::to_string(vowelCrest) + " at its loudest tick reaches " + std::to_string(wantVowel) +
         " rows and does not clip at the crest " + std::to_string(kScopeCrest) + " (up " + std::to_string(reach.up) + ")").c_str());

  // A shout 12 dB over the window top flattens on the top and bottom rows for that frame.
  const Samples shout = sineOfRms(0.1f);
  const Reach shouted = reachOf(scopeFrame(shout.data(), -32.0f, false));
  check(shouted.up == kScopeHalfRows && shouted.down == kScopeHalfRows, "scope scale: a shout 12 dB over the window top flattens on rows 10 and 54");

  // A sine at -20 dBFS RMS, with the window top at -20 dBFS, reaches round(22 x 1.414 / crest) rows.
  const Samples sine = sineOfRms(0.1f);
  const Reach sineReach = reachOf(scopeFrame(sine.data(), -20.0f, false));
  const int want = static_cast<int>(std::lround(22.0 * 1.41421356 / kCrest));
  check(sineReach.up == want && sineReach.down == want,
        ("scope scale: a sine at -20 dBFS RMS reaches " + std::to_string(want) + " rows each side at the crest " + std::to_string(kScopeCrest)).c_str());

  // The scale follows the window top: the same sine against a top 6 dB higher reaches half as far.
  const Reach quieter = reachOf(scopeFrame(sine.data(), -14.0f, false));
  check(std::abs(quieter.up - (want + 1) / 2) <= 1, "scope scale: a window top 6 dB higher halves the reach");

  // A held note 20 dB under a loud tick goes to the line once the floor has risen to it, about 5 s on,
  // and is drawn until then. The silent flag comes from the output meter, as the face reads it.
  level::OutputMeter meter;
  const auto energyOf = [](float db) {
    level::OutputEnergy e;
    e.samples = kTickSamples;
    e.sumSquares = std::pow(10.0f, db / 10.0f) * static_cast<float>(e.samples);
    return e;
  };
  for (int i = 0; i < 25; ++i) meter.update(energyOf(-20.0f), 40.0f);
  const Samples held = sineOfRms(0.01f);
  float levelNow = meter.update(energyOf(-40.0f), 40.0f);
  check(levelNow > 0.0f && reachOf(scopeFrame(held.data(), meter.top(), levelNow <= 0.0f)).up > 0,
        "scope scale: the first tick of a note 20 dB under the top is still drawn");
  int ticksToLine = 1;
  while (levelNow > 0.0f && ticksToLine < 1000) {
    levelNow = meter.update(energyOf(-40.0f), 40.0f);
    ++ticksToLine;
  }
  const Reach gone = reachOf(scopeFrame(held.data(), meter.top(), levelNow <= 0.0f));
  check(levelNow == 0.0f && gone.up == 0 && gone.down == 0 && ticksToLine >= 75 && ticksToLine <= 200,
        "scope scale: a held note at -40 dBFS under a -20 dBFS top goes to the line in about 5 s");
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
  check(ring->blocks() == 105, "scope ring: the block count is the blocks written");
}

// The energy of a known signal equals what the DAC-side tally summed at unity trim, and the take
// covers the blocks written since the last one, at most 31.
void checkRingEnergy() {
  auto ring = std::make_unique<ScopeRing>();
  float dac = 0.0f;
  for (int b = 0; b < 30; ++b) {
    std::array<float, cv::kBlock> block;
    for (int i = 0; i < cv::kBlock; ++i) {
      block[static_cast<size_t>(i)] = 0.4f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(b * cv::kBlock + i) / 48000.0f);
    }
    ring->write(block.data());
    dac += emitOutput(block.data(), cv::kBlock, GainRamp{1.0f, 0.0f}, [](int, float) {});
  }
  const level::OutputEnergy energy = ring->takeEnergy();
  check(energy.samples == 30u * cv::kBlock && std::fabs(energy.sumSquares - dac) < 1e-3f * dac,
        "scope ring: the energy of 30 blocks equals the old output tally at unity trim");
  check(ring->takeEnergy().samples == 0, "scope ring: a second take with no new block is empty");

  std::array<float, cv::kBlock> loud;
  loud.fill(0.5f);
  for (int b = 0; b < 50; ++b) ring->write(loud.data());
  const level::OutputEnergy late = ring->takeEnergy();
  check(late.samples == static_cast<uint32_t>(kScopeReadBlocks * cv::kBlock) &&
            std::fabs(late.sumSquares - 0.25f * static_cast<float>(late.samples)) < 1e-3f,
        "scope ring: a take that came 50 blocks late covers the newest 31");
  ring->writeSilence();
  const level::OutputEnergy quiet = ring->takeEnergy();
  check(quiet.samples == cv::kBlock && quiet.sumSquares == 0.0f, "scope ring: a silent block adds 64 samples of no energy");
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
  const level::OutputEnergy energy = ring->takeEnergy();
  check(energy.samples == static_cast<uint32_t>(kScopeReadBlocks * cv::kBlock), "scope ring: the last take covers 31 blocks");
}

}  // namespace

void checkScope() {
  checkTriggerSteady();
  checkTriggerEdges();
  checkColumns();
  checkScale();
  checkRingOrder();
  checkRingEnergy();
  checkRingStress();
}
