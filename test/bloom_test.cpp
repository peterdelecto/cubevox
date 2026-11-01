// Chatter checks (the Bloom engine): the word schedule (first capture, zero crossings, the 12 dB skip),
// the AMOUNT laws (slice length and loudness), two plays per slice with the cut drift, the LINGER tail
// law and its close, the one slice that repeats in the tail, the 150 ms fade a new word puts on the
// tail, the capture source against the dry path, the 2.5 s age limit, the own key, determinism, chunk
// independence, settled off, no-alloc. No window, no audio device.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "engine/bloom.h"
#include "engine/gate.h"

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
constexpr int kMs = cv::kSampleRate / 1000;
constexpr int kWord = 0;   // voice numbers, as the engine counts them
constexpr int kTail = 1;
constexpr int kFade = 2;
constexpr int kVoices = cv::Bloom::kVoices;

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0, double e = 0.0) {
  char detail[300];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::Bloom gBloom;

using Signal = std::vector<float>;

// One tone segment: amp over [fromMs, toMs).
struct Seg {
  int fromMs, toMs;
  float amp;
};

// A 220 Hz sine through the segments, silence elsewhere, `ms` long. The phase runs on across segments.
Signal sine(int ms, const std::vector<Seg>& segs, double hz = 220.0) {
  Signal x(static_cast<size_t>(ms) * kMs, 0.0f);
  for (const Seg& s : segs)
    for (int i = s.fromMs * kMs; i < s.toMs * kMs && i < static_cast<int>(x.size()); ++i)
      x[static_cast<size_t>(i)] = s.amp * static_cast<float>(std::sin(2.0 * kPi * hz * i / cv::kSampleRate));
  return x;
}

// A 220 Hz square through the segments. Every block peak equals the segment's amp, so the own key
// sees a level with no ripple.
Signal square(int ms, const std::vector<Seg>& segs) {
  Signal x(static_cast<size_t>(ms) * kMs, 0.0f);
  for (const Seg& s : segs)
    for (int i = s.fromMs * kMs; i < s.toMs * kMs && i < static_cast<int>(x.size()); ++i)
      x[static_cast<size_t>(i)] = (i / 109) % 2 ? s.amp : -s.amp;
  return x;
}

// A vowel-like tone: 190 Hz with six harmonics at 1/k, scaled to peak near 0.5.
Signal vowel(int ms, int fromMs, int toMs) {
  Signal x(static_cast<size_t>(ms) * kMs, 0.0f);
  for (int i = fromMs * kMs; i < toMs * kMs; ++i) {
    double v = 0.0;
    for (int k = 1; k <= 6; ++k) v += std::sin(2.0 * kPi * 190.0 * k * i / cv::kSampleRate) / k;
    x[static_cast<size_t>(i)] = static_cast<float>(0.27 * v);
  }
  return x;
}

// Key open over each [fromMs, toMs) in `runs`.
std::vector<char> keyOver(int ms, const std::vector<std::pair<int, int>>& runs) {
  std::vector<char> k(static_cast<size_t>(ms) * kMs, 0);
  for (const auto& r : runs)
    for (int i = r.first * kMs; i < r.second * kMs && i < static_cast<int>(k.size()); ++i) k[static_cast<size_t>(i)] = 1;
  return k;
}

std::vector<char> keyOver(int ms, int fromMs, int toMs) { return keyOver(ms, {{fromMs, toMs}}); }

// What a run records after each sample: the output, the summed gain and the pair scale, and for each
// voice its gain, whether it runs, the slice it plays (commit number, cut clock, length) and its
// latest splice clock; the word and loop flags, the commit and skip counters, and the clocks of
// every capture arm and write start.
struct Trace {
  Signal out, total, pair;
  std::array<Signal, kVoices> gain;
  std::array<std::vector<uint32_t>, kVoices> seq, cut, spliced;
  std::array<std::vector<int>, kVoices> len;
  std::array<std::vector<char>, kVoices> on;
  std::vector<char> word, loop;
  std::vector<int> taken, skipped, arms, writes;
};

// Runs in through b one sample per call, with the gate key per sample and `src` as the capture
// source when given.
Trace trace(cv::Bloom& b, const Signal& in, const std::vector<char>& key, cv::BloomParams p, const Signal* src = nullptr) {
  Trace t;
  p.keyFromGate = true;
  uint32_t lastArm = b.armedAt();
  uint32_t lastWrite = b.writeStartedAt();
  for (size_t i = 0; i < in.size(); ++i) {
    p.keyOpen = key[i] != 0;
    float y = 0.0f;
    gInProcess = true;
    if (src) b.process(&in[i], &(*src)[i], &y, 1, p);
    else b.process(&in[i], &y, 1, p);
    gInProcess = false;
    if (b.armedAt() != lastArm) t.arms.push_back(static_cast<int>(b.armedAt()));
    if (b.writeStartedAt() != lastWrite) t.writes.push_back(static_cast<int>(b.writeStartedAt()));
    lastArm = b.armedAt();
    lastWrite = b.writeStartedAt();
    t.out.push_back(y);
    t.total.push_back(b.wetGain());
    t.pair.push_back(b.pairScale());
    for (int k = 0; k < kVoices; ++k) {
      const size_t j = static_cast<size_t>(k);
      t.gain[j].push_back(b.voiceGain(k));
      t.seq[j].push_back(b.playingSeq(k));
      t.cut[j].push_back(b.playingCutAt(k));
      t.len[j].push_back(b.playingLen(k));
      t.spliced[j].push_back(b.spliceAt(k));
      t.on[j].push_back(b.voiceOn(k) ? 1 : 0);
    }
    t.word.push_back(b.inWord() ? 1 : 0);
    t.loop.push_back(b.looping() ? 1 : 0);
    t.taken.push_back(b.taken());
    t.skipped.push_back(b.skipped());
  }
  return t;
}

// The laws at AMOUNT a, written out again here.
double wetAt(double amount) { return amount > 0.0 ? 0.70 * std::pow(amount, 0.6) : 0.0; }

// AMOUNT's slice length in ms, before drift and LINGER's cap.
double nominalMs(double amount) { return 80.0 + (45.0 - 80.0) * amount; }

// The slice lengths in samples the laws allow: AMOUNT's length drifting by 3 % either way, under
// LINGER's cap (0.8 of its setting) and the 80 ms buffer, over the 35 ms floor, to within a sample.
struct SliceRange {
  int lo, hi;
};

SliceRange sliceRange(double amount, double tailMs) {
  auto clampMs = [&](double ms) { return std::fmin(std::fmax(std::fmin(ms, 0.8 * tailMs), 35.0), 80.0); };
  return {static_cast<int>(clampMs(nominalMs(amount) * 0.97) * kMs) - 1, static_cast<int>(clampMs(nominalMs(amount) * 1.03) * kMs) + 1};
}

double tauMs(double tailMs) {
  const double scaled = std::fmin(300.0, std::fmax(120.0, 1.5 * tailMs));
  return std::fmax(tailMs, scaled);
}

bool crossingAt(const Signal& in, int i) { return i > 0 && ((in[static_cast<size_t>(i - 1)] < 0.0f) != (in[static_cast<size_t>(i)] < 0.0f)); }

// First index where flags goes from 1 to 0 at or after `from`; -1 when it does not.
int fallAfter(const std::vector<char>& f, int from) {
  for (size_t i = static_cast<size_t>(from) + 1; i < f.size(); ++i)
    if (f[i - 1] && !f[i]) return static_cast<int>(i);
  return -1;
}

// One splice: the clock a voice started a slice, the commit number of that slice and its length.
struct Splice {
  int at;
  uint32_t seq;
  int len;
};

std::vector<Splice> splicesOf(const Trace& t, int voice) {
  const size_t k = static_cast<size_t>(voice);
  std::vector<Splice> v;
  for (size_t i = 1; i < t.spliced[k].size(); ++i)
    if (t.spliced[k][i] != t.spliced[k][i - 1] && t.seq[k][i] != 0)
      v.push_back({static_cast<int>(t.spliced[k][i]), t.seq[k][i], t.len[k][i]});
  return v;
}

// The first value in `v` at or after `from`; -1 when there is none.
int firstAtOrAfter(const std::vector<int>& v, int from) {
  for (int a : v)
    if (a >= from) return a;
  return -1;
}

// The first capture waits 40 ms, then up to 10 ms for a zero crossing, and the write starts on one.
bool testFirstCapture() {
  const int ms = 600;
  const Signal in = vowel(ms, 0, 300);
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 300), cv::BloomParams{});
  if (t.arms.empty() || t.writes.empty()) return report("first-capture", false, "no capture armed (%.0f arms)", 0.0);
  const int arm = t.arms[0], write = t.writes[0];
  const bool armOk = arm == 40 * kMs;
  const bool windowOk = write >= 40 * kMs && write <= 50 * kMs;
  const bool onCrossing = crossingAt(in, write) || write - arm == 10 * kMs;
  return report("first-capture", armOk && windowOk && onCrossing,
                "armed at %.2f ms, write starts %.2f ms after the key opens (40..50), on a zero crossing: %.0f",
                arm / double(kMs), write / double(kMs), onCrossing ? 1.0 : 0.0);
}

// The slice runs 80 ms at AMOUNT 0 to 45 ms at 100 %, linear, drifting 3 % either way from cut to
// cut; LINGER caps it at 0.8 of its setting, so a capped slice does not drift.
bool testSliceLaw() {
  struct Point {
    double amount, tail;
  };
  const Point pts[8] = {{0.02, 300.0}, {0.1, 300.0}, {0.25, 300.0}, {0.5, 300.0},
                        {0.75, 300.0}, {1.0, 2000.0}, {0.5, 60.0},  {0.5, 75.0}};
  bool ok = true;
  double meanMs[8] = {0.0}, spreadPct[8] = {0.0};
  for (int j = 0; j < 8; ++j) {
    const int ms = 1900;
    const Signal in = vowel(ms, 0, 1700);
    cv::BloomParams p;
    p.bloom = static_cast<float>(pts[j].amount);
    p.tailMs = static_cast<float>(pts[j].tail);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(ms, 0, 1500), p);
    const std::vector<Splice> sp = splicesOf(t, kWord);
    const SliceRange range = sliceRange(pts[j].amount, pts[j].tail);
    if (sp.size() < 8) {
      ok = false;
      continue;
    }
    int lo = 1 << 30, hi = 0;
    double sum = 0.0;
    for (const Splice& s : sp) {
      lo = std::min(lo, s.len);
      hi = std::max(hi, s.len);
      sum += s.len;
      if (s.len < range.lo || s.len > range.hi) ok = false;
    }
    meanMs[j] = sum / double(sp.size()) / kMs;
    spreadPct[j] = 100.0 * (hi - lo) / (nominalMs(pts[j].amount) * kMs);

    // Free of both ends of the law, the slice drifts, about AMOUNT's length on average. Under LINGER's
    // cap it holds one length.
    const bool capped = range.hi - range.lo <= 2;
    if (capped && hi != lo) ok = false;
    if (!capped && range.hi < 80 * kMs - 2) {
      if (spreadPct[j] < 1.0 || std::fabs(meanMs[j] - nominalMs(pts[j].amount)) > 0.015 * nominalMs(pts[j].amount)) ok = false;
    }
  }
  const bool law = report("slice-law", ok,
                          "mean slice at AMOUNT 0.25/0.5/1 = %.2f / %.2f / %.2f ms (71.25, 62.5, 45), drifting %.1f%% of its length across the word at AMOUNT 0.5 (within 3%% either way)",
                          meanMs[2], meanMs[3], meanMs[5], spreadPct[3]);
  const bool caps = report("slice-law-caps", spreadPct[6] == 0.0 && spreadPct[7] == 0.0,
                           "LINGER 60 and 75 at AMOUNT 0.5 cap the slice at %.2f and %.2f ms (48, 60), with no drift under the cap", meanMs[6], meanMs[7]);
  return law && caps;
}

// A slice plays exactly twice and the next one splices in at the slice boundary. The interval between
// cuts, two plays of the slice, drifts within 3 % of its law and is not constant. Each write starts
// within 10 ms of its arm, and none arm after the key closes.
bool testTwoPlays() {
  struct Point {
    double amount, tail;
  };
  const Point pts[4] = {{0.5, 300.0}, {0.1, 300.0}, {1.0, 300.0}, {0.5, 60.0}};
  bool ok = true;
  bool varied = true;
  double worstLo = 1e9, worstHi = 0.0, worstSpliceGap = 0.0;
  for (const Point& pt : pts) {
    const int ms = 1900;
    const Signal in = vowel(ms, 0, 1700);
    cv::BloomParams p;
    p.bloom = static_cast<float>(pt.amount);
    p.tailMs = static_cast<float>(pt.tail);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(ms, 0, 1500), p);
    const int end = fallAfter(t.word, 0);
    const std::vector<Splice> sp = splicesOf(t, kWord);
    if (t.arms.size() < 8 || sp.size() < 8 || end < 0) {
      ok = false;
      continue;
    }

    // Every splice after the first lands exactly two plays of the slice before it, and the interval
    // stays within 3 % of two slices at AMOUNT's length (LINGER's cap holds it fixed below that).
    const SliceRange range = sliceRange(pt.amount, pt.tail);
    const double nominal = 2.0 * std::fmin(nominalMs(pt.amount), 0.8 * pt.tail) * kMs;
    bool allSame = true;
    for (size_t i = 1; i < sp.size(); ++i) {
      const int gap = sp[i].at - sp[i - 1].at;
      worstSpliceGap = std::fmax(worstSpliceGap, std::fabs(double(gap - 2 * sp[i - 1].len)));
      worstLo = std::fmin(worstLo, gap / nominal);
      worstHi = std::fmax(worstHi, gap / nominal);
      if (gap != 2 * sp[i - 1].len || sp[i - 1].len < range.lo || sp[i - 1].len > range.hi) ok = false;
      if (gap != sp[1].at - sp[0].at) allSame = false;
    }
    if (allSame && range.hi - range.lo > 2) varied = false;
    for (size_t i = 0; i < t.writes.size() && i < t.arms.size(); ++i)
      if (t.writes[i] < t.arms[i] || t.writes[i] > t.arms[i] + 10 * kMs) ok = false;
    if (t.arms.back() >= end) ok = false;  // no arm after the word closed
  }
  return report("two-plays", ok && varied && worstLo >= 0.969 && worstHi <= 1.031,
                "splices land exactly two plays apart (worst %.0f samples off); the interval runs %.4f..%.4f of two slices at AMOUNT's length (0.97..1.03) and varies: %.0f",
                worstSpliceGap, worstLo, worstHi, varied ? 1.0 : 0.0);
}

// The wet level runs 0.70 * AMOUNT^0.6 through the word, and AMOUNT 0 is silent.
bool testWetLaw() {
  bool ok = true;
  double worst = 0.0;
  const double amounts[5] = {0.0, 0.1, 0.5, 0.75, 1.0};
  for (double a : amounts) {
    const int ms = 700;
    const Signal in = vowel(ms, 0, 600);
    cv::BloomParams p;
    p.bloom = static_cast<float>(a);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(ms, 0, 600), p);
    const double got = t.total[static_cast<size_t>(400 * kMs)];
    worst = std::fmax(worst, std::fabs(got - wetAt(a)));
    if (std::fabs(got - wetAt(a)) > 1e-5) ok = false;
    if (a == 0.0) {
      for (size_t i = 0; i < t.out.size(); ++i)
        if (t.out[i] != in[i]) ok = false;
    }
  }
  return report("wet-law", ok, "gain through the word vs 0.70*AMOUNT^0.6 at 0/0.1/0.5/0.75/1: worst %.2g off (<1e-5), AMOUNT 0 is the input bit for bit (50 %% reads %.4f)",
                worst, wetAt(0.5));
}

// A slice more than 12 dB under the looping one is skipped; one 10 dB under is taken.
bool testSkip() {
  const int ms = 1500;
  const float quiet = 0.5f * std::pow(10.0f, -13.0f / 20.0f);
  const float near = 0.5f * std::pow(10.0f, -10.0f / 20.0f);
  const Signal in = sine(ms, {{0, 400, 0.5f}, {400, 900, quiet}, {900, 1400, near}});
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 1400), cv::BloomParams{});
  auto at = [&](const std::vector<int>& v, int msAt) { return v[static_cast<size_t>(msAt * kMs)]; };
  const int takenQuiet = at(t.taken, 900) - at(t.taken, 400);
  const int skippedQuiet = at(t.skipped, 900) - at(t.skipped, 400);
  const int takenNear = at(t.taken, 1400) - at(t.taken, 900);
  const int skippedNear = at(t.skipped, 1400) - at(t.skipped, 900);
  const bool ok = takenQuiet == 0 && skippedQuiet >= 3 && takenNear >= 3 && skippedNear == 0;
  return report("skip-12db", ok, "-13 dB slices: %.0f taken, %.0f skipped (0, >=3); -10 dB slices: %.0f taken, %.0f skipped (>=3, 0)",
                takenQuiet, skippedQuiet, takenNear, skippedNear);
}

// After a silent stretch the slice keeps looping: a loop plays at unity speed, so the wet repeats at
// the slice length. The silent slices that follow are all skipped, and the grid goes on two plays at a time.
bool testLoop() {
  const int ms = 900;
  const Signal in = sine(ms, {{0, 100, 0.5f}});
  cv::BloomParams p;
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 800), p);
  const int len = t.len[kWord][static_cast<size_t>(400 * kMs)];
  if (len == 0) return report("loop", false, "no slice looping at 400 ms (%.0f)", 0.0);
  double peak = 0.0, worst = 0.0;
  for (int i = 400 * kMs; i < 600 * kMs; ++i) {
    peak = std::fmax(peak, std::fabs(t.out[static_cast<size_t>(i)]));
    worst = std::fmax(worst, std::fabs(t.out[static_cast<size_t>(i)] - t.out[static_cast<size_t>(i + len)]));
  }
  const bool silentSkipped = t.taken.back() == 1 && t.skipped.back() >= 2;
  return report("loop", peak > 0.05 && worst < 0.01 * peak && silentSkipped,
                "wet repeats at the %.0f-sample slice: worst difference %.2g of peak %.3f (<1%%), silent slices skipped: %.0f",
                len, worst, peak, silentSkipped ? 1.0 : 0.0);
}

// After the word the wet gain follows wet * e^(-t/tau), switches to the 80 ms fade at life 0.2, and
// ends at exactly 0 with the loop stopped. The tails here end before the age limit takes over.
bool testTail() {
  bool ok = true;
  double worstRel = 0.0, endMs = 0.0, wantEndMs = 0.0;
  struct Case {
    double tail;
    int wordMs, spanMs;
  };
  const Case cases[3] = {{300.0, 700, 4000}, {60.0, 700, 4000}, {1000.0, 200, 4500}};
  for (const Case& c : cases) {
    const Signal in = vowel(c.spanMs, 0, c.wordMs);
    cv::BloomParams p;
    p.tailMs = static_cast<float>(c.tail);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(c.spanMs, 0, c.wordMs), p);
    const int w = fallAfter(t.word, 0);
    if (w < 0) {
      ok = false;
      continue;
    }
    const double wet = wetAt(0.5);
    const double tau = tauMs(c.tail) * kMs;
    const double tSwitch = tau * std::log(5.0);
    // The decay step is a float, so the switch to the fade lands within 0.5 % of its time. The fade
    // is checked against the switch the engine made.
    int sw = 0;
    while (static_cast<size_t>(w + sw) < t.total.size() && t.total[static_cast<size_t>(w + sw)] > 0.2 * wet) ++sw;
    if (std::fabs(sw - tSwitch) > 0.005 * tSwitch) ok = false;
    for (int j = 0; j < sw; j += 97) {
      const double want = wet * std::exp(-(j + 1) / tau);
      const double rel = std::fabs(t.total[static_cast<size_t>(w + j)] - want) / want;
      worstRel = std::fmax(worstRel, rel);
      if (rel > 0.015) ok = false;
    }
    const double staticTau = 80.0 * kMs;
    for (int j = sw; j < static_cast<int>(sw + 5 * staticTau); j += 211) {
      const double want = wet * 0.2 * std::exp(-(j - sw) / staticTau);
      if (want < 0.002) break;
      const double rel = std::fabs(t.total[static_cast<size_t>(w + j)] - want) / want;
      worstRel = std::fmax(worstRel, rel);
      if (rel > 0.015) ok = false;
    }
    int zero = -1;
    for (size_t i = static_cast<size_t>(w); i < t.total.size(); ++i)
      if (t.total[i] == 0.0f) {
        zero = static_cast<int>(i) - w;
        break;
      }
    wantEndMs = (tSwitch + staticTau * std::log(wet * 0.2 / 0.001)) / kMs;
    endMs = zero < 0 ? -1.0 : zero / double(kMs);
    bool stays = zero >= 0;
    for (size_t i = static_cast<size_t>(w + zero + 1); stays && i < t.total.size(); ++i)
      if (t.total[i] != 0.0f || t.loop[i]) stays = false;
    for (size_t i = static_cast<size_t>(w) + 1; i < static_cast<size_t>(w + (zero < 0 ? 0 : zero)); ++i)
      if (t.total[i] > t.total[i - 1]) ok = false;
    if (!stays || std::fabs(endMs - wantEndMs) > 5.0) ok = false;
  }
  return report("tail-law", ok, "gain vs wet*life: worst %.2f%% off (<1.5%%), fade switch at life 0.2 within 0.5%%; last tail ends at %.0f ms, law %.0f ms",
                worstRel * 100.0, endMs, wantEndMs);
}

// The tail repeats one slice, the newest the word cut, with no splice and no rotation. The word ends at
// every offset across two cut intervals, so some ends fall between a cut finishing and its splice.
bool testTailSlice() {
  const int ms = 2600;
  cv::BloomParams p;
  p.tailMs = 600.0f;
  bool one = true, newest = true, noSplice = true, closes = true;
  int runs = 0, noTail = 0, pending = 0;
  double worstRepeat = 1.0;
  for (int closeAt = 480; closeAt < 730; closeAt += 5) {
    // The input stops with the key, so the tail is the only sound after the word.
    const Signal in = vowel(ms, 0, closeAt);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(ms, 0, closeAt), p);
    const int w = fallAfter(t.word, 0);
    if (w < 0) {
      one = false;
      continue;
    }
    ++runs;
    const size_t at = static_cast<size_t>(w);
    if (t.taken[at] == 0) {
      ++noTail;
      if (t.on[kTail][at]) one = false;
      continue;
    }

    // The tail plays the newest slice cut before the word ended, from the word's own voice.
    const uint32_t want = static_cast<uint32_t>(t.taken[at]);
    if (!t.on[kTail][at] || t.on[kWord][at] || t.seq[kTail][at] != want) newest = false;
    int last = -1;
    for (size_t i = at; i < t.on[kTail].size(); ++i) {
      if (!t.on[kTail][i]) break;
      last = static_cast<int>(i);
      if (t.seq[kTail][i] != want) one = false;
      if (t.spliced[kTail][i] != t.spliced[kTail][at]) noSplice = false;
    }
    if (last < 0 || t.on[kTail].back()) closes = false;

    // A cut that was done and waiting for its splice plays at the word's end.
    if (t.spliced[kTail][at] == static_cast<uint32_t>(w)) ++pending;

    // The wet repeats at the slice length once the splice crossfade is over: the 2,400 samples
    // starting 20 ms after the word correlate with the 2,400 that follow one slice later.
    const int len = t.len[kTail][at];
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (int j = 0; j < 2400; ++j) {
      const double x = t.out[static_cast<size_t>(w + 20 * kMs + j)], y = t.out[static_cast<size_t>(w + 20 * kMs + len + j)];
      ab += x * y;
      aa += x * x;
      bb += y * y;
    }
    worstRepeat = std::fmin(worstRepeat, ab / std::sqrt(aa * bb));
  }
  return report("tail-slice", one && newest && noSplice && closes && runs > 40 && pending >= 2 && worstRepeat > 0.99,
                "%.0f word ends (%.0f cut nothing, %.0f with a finished cut waiting): the tail plays one slice, the newest cut, with no splice: %.0f; one slice later the wet correlates at least %.4f",
                runs, noTail, pending, one && newest && noSplice ? 1.0 : 0.0, worstRepeat);
}

// The voice stops while the key stays open, as it does through the Input gate's hold and Decay. The cuts
// taken after the stop hold silence and are skipped, so the tail repeats the last cut that carries the voice.
bool testTailAfterStop() {
  const int ms = 1500;
  const Signal in = vowel(ms, 0, 400);
  cv::BloomParams p;
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 450), p);
  const int w = fallAfter(t.word, 0);
  if (w < 0 || !t.on[kTail][static_cast<size_t>(w)]) return report("tail-after-stop", false, "no tail after the word (%.0f)", 0.0);
  const uint32_t seq = t.seq[kTail][static_cast<size_t>(w)];
  float peak = 0.0f;
  uint32_t cutAt = 0;
  for (int slot = 0; slot < cv::Bloom::kSlices; ++slot) {
    if (!gBloom.sliceValid(slot) || gBloom.sliceSeq(slot) != seq) continue;
    cutAt = gBloom.sliceCutAt(slot);
    for (int k = 0; k < gBloom.sliceLen(slot); ++k) peak = std::fmax(peak, std::fabs(gBloom.sliceData(slot)[k]));
  }
  float voicePeak = 0.0f;
  for (size_t i = 0; i < static_cast<size_t>(400 * kMs); ++i) voicePeak = std::fmax(voicePeak, std::fabs(in[i]));
  const bool voiced = peak > 0.8f * voicePeak && cutAt < static_cast<uint32_t>(400 * kMs);
  const int skippedAfter = t.skipped[static_cast<size_t>(w)] - t.skipped[static_cast<size_t>(400 * kMs)];
  return report("tail-after-stop", voiced && skippedAfter >= 1 && seq == static_cast<uint32_t>(t.taken[static_cast<size_t>(w)]),
                "the voice stops at 400 ms with the key open to 450 ms: %.0f silent cut(s) skipped, the tail repeats the slice cut at %.1f ms (voiced), peak %.2f of the voice's %.2f",
                skippedAfter, cutAt / double(kMs), peak, voicePeak);
}

// 16-bit mono PCM WAV, any chunks before data. Empty on a missing or other file.
Signal readWav(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0) return {};
  size_t pos = 12;
  while (pos + 8 <= b.size()) {
    uint32_t len;
    std::memcpy(&len, &b[pos + 4], 4);
    if (std::memcmp(&b[pos], "data", 4) == 0) {
      const size_t n = std::min<size_t>(len, b.size() - pos - 8) / 2;
      Signal x(n);
      for (size_t i = 0; i < n; ++i) {
        int16_t s;
        std::memcpy(&s, &b[pos + 8 + 2 * i], 2);
        x[i] = static_cast<float>(s) / 32768.0f;
      }
      return x;
    }
    pos += 8 + len + (len & 1);
  }
  return {};
}

// Adam's clips through the Input gate at -55 dBFS with Decay 20 ms and 2 s, the gate's detector as the
// word key and its output as the source, as the box runs them with no pitch card, at LINGER 60 ms,
// 300 ms and 2 s. Between a word's end and its tail's close the tail plays one slice and never
// changes it, and the one it plays is the newest cut, which carries the voice and not the gate's
// silence after it. The same holds for a tail that fades under the next word.
bool testTailOnClips() {
  const char* names[] = {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"};
  const float lingers[3] = {60.0f, 300.0f, 2000.0f};
  const float decays[2] = {20.0f, 2000.0f};
  constexpr size_t kPad = cv::kSampleRate * 2 / 5;
  constexpr size_t kRoom = static_cast<size_t>(cv::kSampleRate) * 4;
  int rows = 0, tails = 0, changes = 0, notNewest = 0, withTail = 0;
  double worstDb = 0.0, bestDb = -200.0;
  for (const char* name : names) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) {
      std::printf("SKIP tail-on-clips %s  samples/ is not in this checkout\n", name);
      continue;
    }
    Signal in(kPad, 0.0f);
    in.insert(in.end(), clip.begin(), clip.end());
    in.resize(in.size() + kRoom, 0.0f);
    for (const float linger : lingers) {
      for (const float decay : decays) {
        cv::Gate gate;
        cv::GateParams gp;
        gp.on = true;
        gp.thresholdDb = -55.0f;
        gp.tuning = cv::inputGateTuning();
        gp.tuning.releaseMs = decay;
        cv::BloomParams bp;
        bp.keyFromGate = true;
        bp.tailMs = linger;
        gate.reset();
        gBloom.reset();
        Signal gated(in.size()), out(in.size());
        uint32_t tailSeq = 0;
        uint32_t tailSplice = 0;
        std::array<uint32_t, 2> fadeSeq = {0, 0};
        float wordPeak = 0.0f;
        bool wasWord = false;
        ++rows;
        for (size_t i = 0; i < in.size(); i += cv::kBlock) {
          const int n = static_cast<int>(std::min<size_t>(cv::kBlock, in.size() - i));
          gate.process(&in[i], &gated[i], n, gp);
          bp.keyOpen = gate.open();
          gBloom.process(&gated[i], &gated[i], &out[i], n, bp);
          for (int j = 0; j < n; ++j) wordPeak = std::fmax(wordPeak, std::fabs(gated[i + static_cast<size_t>(j)]));
          if (gBloom.inWord() && !wasWord) wordPeak = 0.0f;
          wasWord = gBloom.inWord();

          // A tail starts when the word's voice moves over, and from then on its slice must not change.
          if (gBloom.voiceOn(kTail)) {
            if (tailSeq == 0) {
              tailSeq = gBloom.playingSeq(kTail);
              tailSplice = gBloom.spliceAt(kTail);
              ++tails;
              if (tailSeq != static_cast<uint32_t>(gBloom.taken())) ++notNewest;

              // The slice's peak against the loudest the gate passed during the word.
              for (int slot = 0; slot < cv::Bloom::kSlices; ++slot) {
                if (!gBloom.sliceValid(slot) || gBloom.sliceSeq(slot) != tailSeq) continue;
                float peak = 0.0f;
                for (int k = 0; k < gBloom.sliceLen(slot); ++k) peak = std::fmax(peak, std::fabs(gBloom.sliceData(slot)[k]));
                if (wordPeak > 0.0f && peak > 0.0f) {
                  const double db = 20.0 * std::log10(peak / wordPeak);
                  worstDb = std::fmin(worstDb, db);
                  bestDb = std::fmax(bestDb, db);
                  ++withTail;
                }
              }
            } else if (gBloom.playingSeq(kTail) != tailSeq || gBloom.spliceAt(kTail) != tailSplice) {
              ++changes;
            }
          } else {
            tailSeq = 0;
          }
          for (int k = 0; k < 2; ++k) {
            const uint32_t seq = gBloom.voiceOn(kFade + k) ? gBloom.playingSeq(kFade + k) : 0u;
            if (seq != 0 && fadeSeq[static_cast<size_t>(k)] != 0 && seq != fadeSeq[static_cast<size_t>(k)]) ++changes;
            fadeSeq[static_cast<size_t>(k)] = seq;
          }
        }
      }
    }
  }
  if (rows == 0) return report("tail-on-clips", true, "skipped, samples/ is not in this checkout (%.0f rows)", 0.0);
  const bool ok = report("tail-on-clips", changes == 0 && notNewest == 0 && tails >= rows,
                         "%.0f rows (3 clips x LINGER 60/300/2000 ms x Decay 20/2000 ms), %.0f tails: slice changes after a word's end %.0f (0), tails that are not the newest cut %.0f (0)",
                         rows, tails, changes, notNewest);
  std::printf("NOTE tail-on-clips  the tail slice's peak runs %.1f to %.1f dB against the loudest sample the gate passed in its word, over %d tails\n",
              worstDb, bestDb, withTail);
  return ok;
}

// A new word starts while a tail sounds. The tail falls to 0 in a straight line over 150 ms while the
// new word's loop starts, both play 3 dB down while they sound together, and the two never overlap
// longer than the fade.
bool testNextWord() {
  const int ms = 2600;
  const Signal in = vowel(ms, 0, 2400);
  const int second = 1000;
  const int s = second * kMs;
  cv::BloomParams p;
  p.bloom = 0.5f;
  gBloom.reset();
  const Trace ref = trace(gBloom, in, keyOver(ms, 0, 600), p);
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, {{0, 600}, {second, 1700}}), p);
  const double wet = wetAt(0.5);

  // Until the key returns the run is the reference run, sample for sample.
  bool same = true;
  for (size_t i = 0; i < static_cast<size_t>(s); ++i)
    if (t.out[i] != ref.out[i]) same = false;
  const bool tailAlive = t.on[kTail][static_cast<size_t>(s - 1)] && t.gain[kTail][static_cast<size_t>(s - 1)] > 0.0f;

  // The tail moves to the first fading voice and falls in a straight line to 0 within 150 ms.
  const bool moved = !t.on[kTail][static_cast<size_t>(s + 1)] && t.on[kFade][static_cast<size_t>(s + 1)] && t.on[kWord][static_cast<size_t>(s + 1)];
  int zero = -1;
  for (int i = s; i < s + 400 * kMs; ++i)
    if (t.gain[kFade][static_cast<size_t>(i)] == 0.0f && i > s + 1) {
      zero = i - s;
      break;
    }
  const double step = t.gain[kFade][static_cast<size_t>(s + 1)] - t.gain[kFade][static_cast<size_t>(s + 2)];
  bool straight = zero > 0 && step > 0.0;
  for (int i = s + 1; straight && i < s + zero - 3; ++i)
    if (std::fabs((t.gain[kFade][static_cast<size_t>(i)] - t.gain[kFade][static_cast<size_t>(i + 1)]) - step) > 1e-6) straight = false;
  const bool fadeTime = zero >= 150 * kMs - 4 && zero <= 150 * kMs + 4;

  // The new word climbs in 5 ms, cuts its first slice 40 ms in, and every slice it plays was cut after the key.
  bool climbs = true;
  for (int i = s + 1; i < s + 240; ++i)
    if (t.gain[kWord][static_cast<size_t>(i)] < t.gain[kWord][static_cast<size_t>(i - 1)]) climbs = false;
  const bool reaches = t.gain[kWord][static_cast<size_t>(s + 245)] > 0.99 * wet;
  const int arm = firstAtOrAfter(t.arms, s);
  const int write = firstAtOrAfter(t.writes, s);
  const bool captureOk = arm == s + 40 * kMs && write >= s + 40 * kMs && write <= s + 50 * kMs;
  bool fresh = true;
  for (size_t i = static_cast<size_t>(s); i < t.cut[kWord].size(); ++i)
    if (t.seq[kWord][i] != 0 && t.cut[kWord][i] < static_cast<uint32_t>(s)) fresh = false;

  // Both sound together for less than the fade, 3 dB down while they do (the 20 ms smoother takes
  // most of that), and the scale is back at 1 afterwards.
  int together = 0;
  for (int i = s; i < s + 400 * kMs; ++i)
    if (t.gain[kFade][static_cast<size_t>(i)] > 0.0f && t.gain[kWord][static_cast<size_t>(i)] > 0.0f && t.seq[kWord][static_cast<size_t>(i)] != 0 &&
        t.seq[kFade][static_cast<size_t>(i)] != 0)
      ++together;
  const double pairWant = std::pow(10.0, -3.0 / 20.0);
  const bool pairDown = t.pair[static_cast<size_t>(s + 149 * kMs)] < 1.0 - 0.7 * (1.0 - pairWant);
  const bool pairOne = t.pair[static_cast<size_t>(s + 100 * kMs)] == 1.0f && t.pair[static_cast<size_t>(s + 400 * kMs)] == 1.0f;
  const bool apart = together > 0 && together <= 150 * kMs;
  return report("next-word", same && tailAlive && moved && straight && fadeTime && climbs && reaches && captureOk && fresh && pairDown && pairOne && apart,
                "tail reaches 0 after %.2f ms in a straight line (150): %.0f; both sound %.1f ms (<=150) and the pair is down to %.3f by 149 ms; first capture %.2f ms after the key",
                zero / double(kMs), straight ? 1.0 : 0.0, together / double(kMs), t.pair[static_cast<size_t>(s + 149 * kMs)], (write - s) / double(kMs));
}

// A word that starts while an earlier tail still fades gives that fade the 20 ms yield, and the tail it
// finds takes the 150 ms fade. The engine never holds more than three slices.
bool testYield() {
  const int ms = 2000;
  const Signal in = vowel(ms, 0, 1900);
  cv::BloomParams p;
  p.tailMs = 2000.0f;
  p.bloom = 1.0f;

  // Word one ends at 400 ms and its tail fades from 600 ms, when word two starts. Word two ends at
  // 700 ms with a slice, and word three starts at 705 ms, 45 ms before the first fade would have ended.
  const std::vector<std::pair<int, int>> runs = {{0, 350}, {600, 650}, {705, 1500}};
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, runs), p);
  const int s2 = 600 * kMs, s3 = 705 * kMs;
  const bool tail1 = t.on[kTail][static_cast<size_t>(s2 - 1)];
  const bool secondTail = t.on[kTail][static_cast<size_t>(s3 - 1)] && t.on[kFade][static_cast<size_t>(s3 - 1)];

  // The first fade would end 150 ms after 600 ms; at 705 ms it has 45 ms left and is cut to 20 ms.
  int zeroA = -1, zeroB = -1;
  for (int i = s3; i < s3 + 400 * kMs; ++i) {
    if (zeroA < 0 && t.gain[kFade][static_cast<size_t>(i)] == 0.0f) zeroA = i - s3;
    if (zeroB < 0 && t.gain[kFade + 1][static_cast<size_t>(i)] == 0.0f && i > s3 + 2) zeroB = i - s3;
  }
  const bool yielded = zeroA >= 20 * kMs - 4 && zeroA <= 20 * kMs + 4;
  const bool newFade = zeroB >= 150 * kMs - 4 && zeroB <= 150 * kMs + 4;
  return report("yield", tail1 && secondTail && yielded && newFade,
                "an older fade yields to 0 in %.2f ms (20) and the newer tail fades in %.2f ms (150); tails present when word three starts: %.0f",
                zeroA / double(kMs), zeroB / double(kMs), secondTail ? 1.0 : 0.0);
}

// Slices come from `src`; the dry path is `in`.
bool testSource() {
  const int ms = 1200;
  const Signal in = vowel(ms, 0, 1100);
  const Signal src = sine(ms, {{0, 1100, 0.4f}}, 330.0);
  const std::vector<char> key = keyOver(ms, 0, 1000);
  cv::BloomParams p;
  gBloom.reset();
  const Trace split = trace(gBloom, in, key, p, &src);

  // Captured slices equal the source through the 11 kHz low-pass, starting clear at their cut clock.
  // Run to a dozen points in the word and read the slices that hold a finished cut.
  cv::BloomParams q = p;
  q.keyFromGate = true;
  q.keyOpen = true;
  Signal y(in.size());
  int checked = 0;
  bool match = true;
  for (int stopMs = 400; stopMs < 700; stopMs += 25) {
    gBloom.reset();
    for (size_t i = 0; i < static_cast<size_t>(stopMs) * kMs; ++i) gBloom.process(&in[i], &src[i], &y[i], 1, q);
    for (int slot = 0; slot < cv::Bloom::kSlices; ++slot) {
      if (!gBloom.sliceValid(slot)) continue;
      ++checked;
      const uint32_t cut = gBloom.sliceCutAt(slot);
      cv::Biquad lp;
      lp.setLowPass(p.tuning.lowpassHz, p.tuning.lowpassQ);
      for (int j = 0; j < gBloom.sliceLen(slot); ++j)
        if (std::fabs(gBloom.sliceData(slot)[j] - lp.run(src[cut + static_cast<uint32_t>(j)])) > 1e-5f) match = false;
    }
  }

  // Before the first slice plays the output is the dry input under the trim, whatever `src` is.
  gBloom.reset();
  const Trace same = trace(gBloom, in, key, p);
  bool dry = true;
  for (int i = 0; i < 100 * kMs; ++i)
    if (split.out[static_cast<size_t>(i)] != same.out[static_cast<size_t>(i)]) dry = false;
  const bool wetDiffers = [&] {
    double d = 0.0;
    for (size_t i = 300 * kMs; i < 900 * kMs; ++i) d = std::fmax(d, std::fabs(split.out[i] - same.out[i]));
    return d > 0.05;
  }();

  // A call with src == in is the call without it.
  gBloom.reset();
  Signal a(in.size()), b(in.size());
  q.keyOpen = true;
  gBloom.process(in.data(), a.data(), static_cast<int>(in.size()), q);
  gBloom.reset();
  gBloom.process(in.data(), in.data(), b.data(), static_cast<int>(in.size()), q);
  const bool equalCall = a == b;
  return report("source", match && checked >= 12 && dry && wetDiffers && equalCall,
                "%.0f finished slices equal the source at their cut clock: %.0f; output before the first loop is the dry input's, and the loop follows the source: %.0f; src == in equals the short call: %.0f",
                checked, match ? 1.0 : 0.0, dry && wetDiffers ? 1.0 : 0.0, equalCall ? 1.0 : 0.0);
}

// No slice plays more than 2.5 s after it was cut. A loud slice with a quieter tone held after it
// loops while every later slice is skipped, and is replaced before the limit. A tail's one slice
// closes at the limit over 80 ms.
bool testAgeLimit() {
  const int ms = 4500;
  const float quiet = 0.5f * std::pow(10.0f, -20.0f / 20.0f);
  const Signal in = sine(ms, {{0, 300, 0.5f}, {300, 4400, quiet}});
  cv::BloomParams p;
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 4400), p);
  const uint32_t limit = static_cast<uint32_t>(2500 * kMs);
  uint32_t worstAge = 0;
  for (size_t i = 0; i < in.size(); ++i)
    if (t.seq[kWord][i] != 0) worstAge = std::max<uint32_t>(worstAge, static_cast<uint32_t>(i) - t.cut[kWord][i]);
  const uint32_t loud = t.seq[kWord][static_cast<size_t>(1000 * kMs)];
  const bool stillLoud = loud != 0 && t.seq[kWord][static_cast<size_t>(2000 * kMs)] == loud;
  int changeAt = -1;
  for (size_t i = static_cast<size_t>(1000 * kMs); i < in.size(); ++i)
    if (t.seq[kWord][i] != loud) {
      changeAt = static_cast<int>(i);
      break;
    }
  const uint32_t loudCut = t.cut[kWord][static_cast<size_t>(1000 * kMs)];
  const bool replaced = changeAt > 0 && static_cast<uint32_t>(changeAt) < loudCut + limit && static_cast<uint32_t>(changeAt) > loudCut + limit - 400 * kMs &&
                        t.seq[kWord][static_cast<size_t>(3500 * kMs)] > loud && t.on[kWord][static_cast<size_t>(3500 * kMs)];

  // A tail at LINGER 2 s: its slice closes at the limit, short of the 3.58 s the law gives, in an 80
  // ms straight fall.
  const int span = 6000;
  const Signal tin = vowel(span, 0, 900);
  cv::BloomParams q;
  q.tailMs = 2000.0f;
  gBloom.reset();
  const Trace u = trace(gBloom, tin, keyOver(span, 0, 800), q);
  const int w = fallAfter(u.word, 0);
  const uint32_t tailCut = u.cut[kTail][static_cast<size_t>(w)];
  int end = -1;
  for (size_t i = static_cast<size_t>(w); i < u.gain[kTail].size(); ++i)
    if (u.gain[kTail][i] == 0.0f && !u.on[kTail][i]) {
      end = static_cast<int>(i);
      break;
    }
  bool fall = end > 4000;
  for (int i = end - 3700; fall && i < end - 2; ++i) {
    const double d0 = u.gain[kTail][static_cast<size_t>(i)] - u.gain[kTail][static_cast<size_t>(i + 1)];
    const double d1 = u.gain[kTail][static_cast<size_t>(i + 1)] - u.gain[kTail][static_cast<size_t>(i + 2)];
    if (d0 <= 0.0 || std::fabs(d0 - d1) > 1e-6) fall = false;
  }
  uint32_t tailAge = 0;
  for (size_t i = static_cast<size_t>(w); i < u.cut[kTail].size(); ++i)
    if (u.seq[kTail][i] != 0) tailAge = std::max<uint32_t>(tailAge, static_cast<uint32_t>(i) - u.cut[kTail][i]);
  const double tailMsEnd = (end - w) / double(kMs);
  const bool inLimit = end > 0 && static_cast<uint32_t>(end) <= tailCut + limit && tailAge <= limit;
  const bool shorter = tailMsEnd < 3000.0 && tailMsEnd > 1500.0;
  return report("age-limit", worstAge <= limit && stillLoud && replaced && fall && inLimit && shorter,
                "held quieter tone: oldest slice played %.0f ms at most (<= 2500), loud slice still loops at 2 s and is replaced at %.0f ms; LINGER 2 s tail closes %.0f ms after the word (law 3581), last 80 ms a straight fall: %.0f",
                worstAge / double(kMs), changeAt / double(kMs), tailMsEnd, fall ? 1.0 : 0.0);
}

// With tailOnly the loop stays silent under the word and blooms when the word ends.
bool testTailOnly() {
  const int ms = 1400;
  const Signal in = vowel(ms, 0, 500);
  cv::BloomParams p;
  p.tuning.tailOnly = true;
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 500), p);
  const int w = fallAfter(t.word, 0);
  bool silent = w > 0;
  for (int i = 0; silent && i < w; ++i)
    if (t.total[static_cast<size_t>(i)] != 0.0f) silent = false;
  const double wet = wetAt(0.5);
  const bool blooms = w > 0 && t.total[static_cast<size_t>(w + 245)] > 0.95 * wet * std::exp(-245.0 / (300.0 * kMs)) &&
                      t.loop[static_cast<size_t>(w)];
  return report("tail-only", silent && blooms, "gain 0 under the word: %.0f, gain at 5 ms after the word %.3f (wet %.3f)",
                silent ? 1.0 : 0.0, w > 0 ? t.total[static_cast<size_t>(w + 245)] : 0.0, wet);
}

// The own key: a tone at -40 dBFS opens, one at -50 dBFS does not, -47 dBFS holds an open key, and
// the key closes 50 ms after the level drops.
bool testOwnKey() {
  auto amp = [](double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); };
  auto wordAfter = [&](double db, int lenMs) {
    gBloom.reset();
    cv::BloomParams p;
    const Signal in = square(lenMs, {{0, lenMs, amp(db)}});
    float out[cv::kBlock];
    for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) gBloom.process(&in[i], out, cv::kBlock, p);
    return gBloom.inWord();
  };
  const bool loud = wordAfter(-40.0, 20);
  const bool quiet = !wordAfter(-50.0, 20);
  const bool between = !wordAfter(-46.0, 20);

  gBloom.reset();
  cv::BloomParams p;
  const Signal in = square(600, {{0, 100, amp(-40.0)}, {100, 300, amp(-47.0)}});
  float out[cv::kBlock];
  int closed = -1;
  for (int i = 0; i + cv::kBlock <= static_cast<int>(in.size()); i += cv::kBlock) {
    gBloom.process(&in[static_cast<size_t>(i)], out, cv::kBlock, p);
    if (i >= 100 * kMs && !gBloom.inWord() && closed < 0) closed = i;
  }
  const bool holds = closed >= 300 * kMs + 50 * kMs - 2 * cv::kBlock && closed <= 300 * kMs + 50 * kMs + 2 * cv::kBlock;
  return report("own-key", loud && quiet && between && holds,
                "opens at -40 dBFS: %.0f, not at -50 or -46: %.0f, holds at -47 dBFS and closes %.1f ms after the level drops (50)",
                loud ? 1.0 : 0.0, quiet && between ? 1.0 : 0.0, closed < 0 ? -1.0 : (closed - 300 * kMs) / double(kMs));
}

// The wet path stays free of clicks: the splices crossfade and the passes fade. The largest wet step
// within a splice crossfade is compared with the largest wet step everywhere else in the word.
bool testNoClicks() {
  const int ms = 1200;
  const Signal in = vowel(ms, 0, 1000);
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 1000), cv::BloomParams{});
  const std::vector<Splice> sp = splicesOf(t, kWord);
  std::vector<char> near(t.out.size(), 0);
  for (const Splice& s : sp)
    for (int i = s.at - 2; i < s.at + 300 && i < static_cast<int>(near.size()); ++i) near[static_cast<size_t>(i)] = 1;
  double atSplice = 0.0, elsewhere = 0.0;
  for (int i = 300 * kMs; i < 900 * kMs; ++i) {
    const size_t k = static_cast<size_t>(i);
    const double step = std::fabs((t.out[k] - in[k]) - (t.out[k - 1] - in[k - 1]));
    if (near[k]) atSplice = std::fmax(atSplice, step);
    else elsewhere = std::fmax(elsewhere, step);
  }
  return report("no-clicks", sp.size() >= 5 && atSplice < 1.5 * elsewhere,
                "largest wet step in a splice crossfade %.4f against %.4f elsewhere in the word (<1.5x)", atSplice, elsewhere);
}

bool sameBytes(const Signal& a, const Signal& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }

// Two renders from reset are byte-identical, also through a tail fading under a new word, and with a
// constant key the call size does not matter.
bool testDeterminism() {
  const int ms = 4000;
  const Signal in = vowel(ms, 100, 3500);
  const std::vector<char> key = keyOver(ms, {{100, 700}, {1200, 1800}, {2300, 2900}});
  cv::BloomParams p;
  p.tailMs = 1000.0f;
  gBloom.reset();
  const Trace a = trace(gBloom, in, key, p);
  gBloom.reset();
  const Trace b = trace(gBloom, in, key, p);

  bool chunked = true;
  Signal ref;
  const int sizes[4] = {1, 7, 64, 1000};
  for (int c : sizes) {
    gBloom.reset();
    cv::BloomParams q;
    q.keyFromGate = true;
    q.keyOpen = true;
    Signal out(in.size());
    for (size_t i = 0; i < in.size(); i += static_cast<size_t>(c)) {
      const int n = static_cast<int>(in.size() - i < static_cast<size_t>(c) ? in.size() - i : static_cast<size_t>(c));
      gBloom.process(&in[i], &out[i], n, q);
    }
    if (ref.empty()) ref = out;
    else if (!sameBytes(ref, out)) chunked = false;
  }
  return report("determinism", sameBytes(a.out, b.out) && chunked, "two renders byte-identical: %.0f, calls of 1/7/64/1000 samples byte-identical: %.0f",
                sameBytes(a.out, b.out) ? 1.0 : 0.0, chunked ? 1.0 : 0.0);
}

// Settled off is a plain copy, tested with ==, from reset and after a running stage switches off.
bool testSettledOff() {
  const int ms = 3000;
  const Signal in = vowel(ms, 0, 2000);
  cv::BloomParams off;
  off.on = false;
  off.keyFromGate = true;
  off.keyOpen = true;
  Signal out(in.size());
  gBloom.reset();
  gBloom.process(in.data(), out.data(), static_cast<int>(in.size()), off);
  const bool fromReset = out == in;

  cv::BloomParams on;
  on.keyFromGate = true;
  on.keyOpen = true;
  gBloom.reset();
  const size_t half = 1000 * kMs;
  gBloom.process(in.data(), out.data(), static_cast<int>(half), on);
  const bool wasWet = gBloom.looping();
  gBloom.process(in.data() + half, out.data() + half, static_cast<int>(in.size() - half), off);
  bool tailCopy = true;
  for (size_t i = 2000 * kMs; i < in.size(); ++i)
    if (out[i] != in[i]) tailCopy = false;

  // A tail fading under a new word and the new word itself, then switched off: both stop.
  const Signal two = vowel(ms, 0, 2800);
  cv::BloomParams a = on;
  a.tailMs = 2000.0f;
  gBloom.reset();
  for (size_t i = 0; i < 760 * kMs; ++i) {
    a.keyOpen = i < 400 * kMs || i >= 700 * kMs;
    gBloom.process(&two[i], &out[i], 1, a);
  }
  const bool bothRan = gBloom.voiceOn(kWord) && gBloom.voiceOn(kFade);
  a.on = false;
  gBloom.process(two.data() + 760 * kMs, out.data() + 760 * kMs, 2240 * kMs, a);
  bool twoCopy = true;
  for (size_t i = 2800 * kMs; i < two.size(); ++i)
    if (out[i] != two[i]) twoCopy = false;
  bool stopped = !gBloom.looping() && !gBloom.inWord();
  for (int k = 0; k < kVoices; ++k) stopped = stopped && !gBloom.voiceOn(k);

  cv::BloomParams zero = on;
  zero.bloom = 0.0f;
  gBloom.reset();
  gBloom.process(in.data(), out.data(), static_cast<int>(in.size()), zero);
  const bool knobZero = out == in;
  return report("settled-off", fromReset && wasWet && tailCopy && bothRan && twoCopy && stopped && knobZero && !gBloom.looping(),
                "off from reset == input: %.0f, off after running == input and loop stopped: %.0f, word and fading tail off == input: %.0f, AMOUNT 0 == input: %.0f",
                fromReset ? 1.0 : 0.0, wasWet && tailCopy ? 1.0 : 0.0, bothRan && twoCopy && stopped ? 1.0 : 0.0, knobZero ? 1.0 : 0.0);
}

// A quick off and on keeps the running word; extreme input stays finite and bounded.
bool testToggleAndRange() {
  const int ms = 1500;
  const Signal in = vowel(ms, 0, 1200);
  cv::BloomParams p;
  p.keyFromGate = true;
  p.keyOpen = true;
  gBloom.reset();
  Signal out(in.size());
  gBloom.process(in.data(), out.data(), 500 * kMs, p);
  const int takenBefore = gBloom.taken();
  p.on = false;
  gBloom.process(in.data() + 500 * kMs, out.data() + 500 * kMs, 20 * kMs, p);
  p.on = true;
  gBloom.process(in.data() + 520 * kMs, out.data() + 520 * kMs, 100 * kMs, p);
  const bool keeps = gBloom.looping() && gBloom.inWord() && gBloom.taken() >= takenBefore;

  Signal loud(static_cast<size_t>(ms) * kMs);
  for (size_t i = 0; i < loud.size(); ++i) loud[i] = (i / 37) % 2 ? 4.0f : -4.0f;
  gBloom.reset();
  Signal y(loud.size());
  gBloom.process(loud.data(), y.data(), static_cast<int>(loud.size()), p);
  double peak = 0.0;
  bool finite = true;
  for (size_t i = 0; i < y.size(); ++i) {
    finite = finite && std::isfinite(y[i]);
    peak = std::fmax(peak, std::fabs(y[i]));
  }
  return report("toggle-range", keeps && finite && peak < 4.4, "off and on within 20 ms keeps the loop: %.0f; loud input (4.0) stays finite, output peak %.2f (<4.4)",
                keeps ? 1.0 : 0.0, peak);
}

// Three slices of 3,840 samples and the small state around them.
bool testStorage() {
  const size_t slices = static_cast<size_t>(cv::Bloom::kSlices) * cv::Bloom::kSliceMax * sizeof(float);
  return report("storage", cv::Bloom::kSlices == 3 && cv::Bloom::kSliceMax == 3840 && slices == 46080 && sizeof(cv::Bloom) < 48000,
                "sizeof(Bloom) %.0f B, of which three slices of 3,840 samples %.0f B (<48,000 in all)", static_cast<double>(sizeof(cv::Bloom)),
                static_cast<double>(slices));
}

bool testNoAlloc() {
  const Signal in = vowel(400, 0, 400);
  gBloom.reset();
  Signal out(in.size());
  cv::BloomParams p;
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    const int n = static_cast<int>(in.size() - i < static_cast<size_t>(cv::kBlock) ? in.size() - i : static_cast<size_t>(cv::kBlock));
    gInProcess = true;
    gBloom.process(&in[i], &in[i], &out[i], n, p);
    gInProcess = false;
  }
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testFirstCapture();
  ok &= testSliceLaw();
  ok &= testTwoPlays();
  ok &= testWetLaw();
  ok &= testSkip();
  ok &= testLoop();
  ok &= testTail();
  ok &= testTailSlice();
  ok &= testTailAfterStop();
  ok &= testTailOnClips();
  ok &= testNextWord();
  ok &= testYield();
  ok &= testSource();
  ok &= testAgeLimit();
  ok &= testTailOnly();
  ok &= testOwnKey();
  ok &= testNoClicks();
  ok &= testDeterminism();
  ok &= testSettledOff();
  ok &= testToggleAndRange();
  ok &= testStorage();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
