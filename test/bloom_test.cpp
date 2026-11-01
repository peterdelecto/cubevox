// Bloom checks: the word schedule (first capture, the 125 ms grid, zero crossings, the 12 dB skip), the
// AMOUNT and LINGER laws, the tail law and its close, the rotation of the word's last four slices,
// a word during a tail, the third word's 20 ms fade, the capture source against the dry path, the
// 2.5 s age limit, the own key, determinism, chunk independence, settled off, no-alloc. No window,
// no audio device.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "engine/bloom.h"

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
constexpr int kInterval = 6000;  // 125 ms, two slices of 3,000 samples

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0) {
  char detail[220];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
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

// What a run records after each sample: the output, each player's gain and state and the slice it
// plays (commit number, cut clock, splice clock), the word and loop flags, the commit and skip
// counters, and the clocks of every capture arm and write start.
struct Trace {
  Signal out, total, pair;
  std::array<Signal, 2> gain;
  std::array<std::vector<uint32_t>, 2> seq, cut, spliced;
  std::array<std::vector<char>, 2> active, tail, kept;
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
    for (int k = 0; k < 2; ++k) {
      const size_t j = static_cast<size_t>(k);
      t.gain[j].push_back(b.playerGain(k));
      t.seq[j].push_back(b.playingSeq(k));
      t.cut[j].push_back(b.playingCutAt(k));
      t.spliced[j].push_back(b.spliceAt(k));
      t.active[j].push_back(b.playerActive(k) ? 1 : 0);
      t.tail[j].push_back(b.playerInTail(k) ? 1 : 0);
      t.kept[j].push_back(static_cast<char>(b.keptCount(k)));
    }
    t.word.push_back(b.inWord() ? 1 : 0);
    t.loop.push_back(b.looping() ? 1 : 0);
    t.taken.push_back(b.taken());
    t.skipped.push_back(b.skipped());
  }
  return t;
}

// The laws at AMOUNT a and LINGER tailMs, written out again here.
double wetAt(double amount) { return amount > 0.0 ? 0.70 * std::pow(amount, 0.6) : 0.0; }

int sliceSamples(double tailMs) {
  double ms = std::fmin(62.5, 0.8 * tailMs);
  ms = std::fmax(ms, 35.0);
  return static_cast<int>(ms * kMs);
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

// One splice: the clock a player started a slice and the commit number of that slice.
struct Splice {
  int at;
  uint32_t seq;
};

std::vector<Splice> splicesOf(const Trace& t, int player) {
  const size_t k = static_cast<size_t>(player);
  std::vector<Splice> v;
  for (size_t i = 1; i < t.spliced[k].size(); ++i)
    if (t.spliced[k][i] != t.spliced[k][i - 1] && t.seq[k][i] != 0) v.push_back({static_cast<int>(t.spliced[k][i]), t.seq[k][i]});
  return v;
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

// Re-captures arm exactly one interval apart, two slice lengths, whatever AMOUNT is, each write
// starts within 10 ms of its arm, and none arm after the key closes.
bool testInterval() {
  bool ok = true;
  double worstLo = 1e9, worstHi = 0.0;
  struct Point {
    double amount, tail;
  };
  const Point pts[4] = {{0.5, 300.0}, {0.05, 300.0}, {1.0, 300.0}, {0.5, 60.0}};
  for (const Point& pt : pts) {
    const int ms = 1500;
    const Signal in = sine(ms, {{0, 1200, 0.5f}});
    cv::BloomParams p;
    p.bloom = static_cast<float>(pt.amount);
    p.tailMs = static_cast<float>(pt.tail);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(ms, 0, 1200), p);
    const int want = 2 * sliceSamples(pt.tail);
    const int end = fallAfter(t.word, 0);
    if (t.arms.size() < 6 || end < 0) {
      ok = false;
      continue;
    }
    for (size_t i = 1; i < t.arms.size(); ++i) {
      const int gap = t.arms[i] - t.arms[i - 1];
      worstLo = std::fmin(worstLo, gap);
      worstHi = std::fmax(worstHi, gap);
      if (gap != want) ok = false;
    }
    for (size_t i = 0; i < t.writes.size() && i < t.arms.size(); ++i)
      if (t.writes[i] < t.arms[i] || t.writes[i] > t.arms[i] + 10 * kMs) ok = false;
    if (t.arms.back() >= end) ok = false;  // no arm after the word closed
  }
  return report("interval", ok, "arm gaps at AMOUNT 0.05/0.5/1 and LINGER 300, and AMOUNT 0.5 at LINGER 60: %.0f..%.0f samples, each equal to two slices (6000, and 4608), none after the key closes",
                worstLo, worstHi);
}

// The slice is 62.5 ms whatever AMOUNT is; LINGER caps it at 0.8 of its setting below 78 ms.
bool testGrainLaw() {
  struct Point {
    double amount, tail;
  };
  const Point pts[5] = {{0.5, 300.0}, {1.0, 2000.0}, {0.5, 60.0}, {0.05, 300.0}, {0.5, 75.0}};
  bool ok = true;
  double got[5];
  for (int j = 0; j < 5; ++j) {
    const int ms = 700;
    const Signal in = sine(ms, {{0, 600, 0.5f}});
    cv::BloomParams p;
    p.bloom = static_cast<float>(pts[j].amount);
    p.tailMs = static_cast<float>(pts[j].tail);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(ms, 0, 600), p);
    int done = -1;
    for (size_t i = 0; i < t.taken.size(); ++i)
      if (t.taken[i] > 0) {
        done = static_cast<int>(i);
        break;
      }
    if (done < 0 || t.writes.empty()) {
      ok = false;
      got[j] = 0.0;
      continue;
    }
    got[j] = done - t.writes[0] + 1;
    if (static_cast<int>(got[j]) != sliceSamples(pts[j].tail)) ok = false;
  }
  return report("grain-law", ok, "slice samples at (AMOUNT, LINGER) (0.5, 300) / (1, 2000) / (0.5, 60) / (0.5, 75) = %.0f / %.0f / %.0f / %.0f (3000, 3000, 2304, 2880)",
                got[0], got[1], got[2], got[4]);
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
// the slice length. The silent slices that follow are all skipped.
bool testLoop() {
  const int ms = 900;
  const Signal in = sine(ms, {{0, 100, 0.5f}});
  cv::BloomParams p;
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 800), p);
  int done = -1;
  for (size_t i = 0; i < t.taken.size(); ++i)
    if (t.taken[i] > 0) {
      done = static_cast<int>(i);
      break;
    }
  if (done < 0 || t.writes.empty()) return report("loop", false, "no slice taken (%.0f)", 0.0);
  const int len = done - t.writes[0] + 1;
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

// After the word, the word's last four slices play in the order they were sung, each for one
// interval, spliced on the word's grid, round and round until the tail closes.
bool testRotation() {
  const int ms = 4000;
  const Signal in = vowel(ms, 0, 900);
  cv::BloomParams p;
  p.tailMs = 600.0f;
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 830), p);
  const int w = fallAfter(t.word, 0);
  const std::vector<Splice> sp = splicesOf(t, 0);

  // Seven slices are cut in the word (commit numbers 1 to 7) and none is under way when it ends at
  // 880 ms, so 4 to 7 are kept. The word ends with the 7th looping, and the rotation then runs
  // 4, 5, 6, 7, 4, ... one interval apart, on the grid the word kept.
  const bool four = w > 0 && t.kept[0][static_cast<size_t>(w)] == 4;
  const uint32_t want[8] = {4, 5, 6, 7, 4, 5, 6, 7};
  size_t firstTail = 0;
  while (firstTail < sp.size() && sp[firstTail].at < w) ++firstTail;
  bool order = sp.size() >= firstTail + 8 && firstTail >= 1 && sp[firstTail - 1].seq == 7;
  bool grid = order;
  for (size_t i = 0; order && i < 8; ++i)
    if (sp[firstTail + i].seq != want[i]) order = false;
  for (size_t i = firstTail; grid && i < sp.size(); ++i)
    if (sp[i].at - sp[i - 1].at != kInterval) grid = false;
  const bool closed = t.active[0].back() == 0;

  // A capture under way at the word's end is dropped, and it has already overwritten the oldest
  // slice, so a word that ends there keeps three: 5, 6, 7.
  gBloom.reset();
  const Trace d = trace(gBloom, in, keyOver(ms, 0, 900), p);
  const int wd = fallAfter(d.word, 0);
  const std::vector<Splice> spd = splicesOf(d, 0);
  size_t firstDrop = 0;
  while (firstDrop < spd.size() && spd[firstDrop].at < wd) ++firstDrop;
  const uint32_t wantDrop[6] = {5, 6, 7, 5, 6, 7};
  bool dropped = wd > 0 && d.kept[0][static_cast<size_t>(wd)] == 3 && spd.size() >= firstDrop + 6 && firstDrop >= 1 &&
                 spd[firstDrop - 1].seq == 7;
  for (size_t i = 0; dropped && i < 6; ++i)
    if (spd[firstDrop + i].seq != wantDrop[i]) dropped = false;

  // Fewer than four: a short word keeps two slices and rotates them, 2 then 1 then 2.
  const int msShort = 2500;
  const Signal inShort = vowel(msShort, 0, 300);
  gBloom.reset();
  const Trace s = trace(gBloom, inShort, keyOver(msShort, 0, 260), p);
  const std::vector<Splice> sps = splicesOf(s, 0);
  const int ws = fallAfter(s.word, 0);
  const bool keptTwo = ws > 0 && s.kept[0][static_cast<size_t>(ws)] == 2;
  bool two = sps.size() >= 6 && sps[0].seq == 1 && sps[1].seq == 2;
  for (size_t i = 2; two && i < sps.size(); ++i) {
    if (sps[i].seq != (i % 2 == 0 ? 1u : 2u)) two = false;
    if (sps[i].at - sps[i - 1].at != kInterval) two = false;
  }

  // One slice keeps looping with no splice.
  const Signal inOne = vowel(msShort, 0, 140);
  gBloom.reset();
  const Trace o = trace(gBloom, inOne, keyOver(msShort, 0, 130), p);
  const std::vector<Splice> spo = splicesOf(o, 0);
  const bool one = spo.size() == 1 && spo[0].seq == 1 && o.loop[static_cast<size_t>(500 * kMs)] && o.active[0].back() == 0;
  return report("rotation", order && grid && four && closed && dropped && two && keptTwo && one,
                "last four of seven slices rotate 4,5,6,7,4,5,6,7 on a 6000-sample grid: %.0f; a capture under way at the end leaves three: %.0f; a two-slice word rotates 2,1,2,1: %.0f; one slice just loops: %.0f",
                order && grid && four && closed ? 1.0 : 0.0, dropped ? 1.0 : 0.0, two && keptTwo ? 1.0 : 0.0, one ? 1.0 : 0.0);
}

// A word during a tail leaves the old tail alone to fade and close on its own while the new word starts
// its own loop on the other player.
bool testOverlap() {
  const int ms = 2400;
  const Signal in = vowel(ms, 0, 2200);
  const int second = 1000;
  cv::BloomParams p;
  gBloom.reset();
  const Trace ref = trace(gBloom, in, keyOver(ms, 0, 600), p);
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, {{0, 600}, {second, 1500}}), p);
  const int s = second * kMs;
  const double wet = wetAt(0.5);

  // The old tail is the player the reference ran, sample for sample.
  bool same = true;
  for (size_t i = static_cast<size_t>(s) - 100; i < in.size(); ++i)
    if (t.gain[0][i] != ref.gain[0][i] || t.seq[0][i] != ref.seq[0][i] || t.active[0][i] != ref.active[0][i]) same = false;
  const bool aliveAtKey = t.tail[0][static_cast<size_t>(s)] && t.gain[0][static_cast<size_t>(s)] > 0.0f;
  const int tailEnd = fallAfter(t.active[0], s);
  const bool closesOnItsOwn = tailEnd > s + 100 * kMs && tailEnd == fallAfter(ref.active[0], s);

  // The new word runs on player 1: gain up in 5 ms, first capture 40 to 50 ms after the key.
  bool climbs = t.active[1][static_cast<size_t>(s)] != 0;
  for (int i = s + 1; i < s + 240; ++i)
    if (t.gain[1][static_cast<size_t>(i)] < t.gain[1][static_cast<size_t>(i - 1)]) climbs = false;
  const bool reaches = t.gain[1][static_cast<size_t>(s + 245)] > 0.99 * wet;
  int arm = -1, write = -1;
  for (int a : t.arms)
    if (a >= s) {
      arm = a;
      break;
    }
  for (int w : t.writes)
    if (w >= s) {
      write = w;
      break;
    }
  const bool captureOk = arm == s + 40 * kMs && write >= s + 40 * kMs && write <= s + 50 * kMs;

  // Both loops sound together, and the old tail keeps its slices through the new word.
  bool together = false;
  for (int i = s + 200 * kMs; i < tailEnd; i += 97)
    if (t.gain[0][static_cast<size_t>(i)] > 0.0f && t.gain[1][static_cast<size_t>(i)] > 0.0f && t.seq[0][static_cast<size_t>(i)] != 0 &&
        t.seq[1][static_cast<size_t>(i)] != 0)
      together = true;
  const bool differs = [&] {
    double d = 0.0;
    for (int i = s + 200 * kMs; i < tailEnd; ++i) d = std::fmax(d, std::fabs(t.out[static_cast<size_t>(i)] - ref.out[static_cast<size_t>(i)]));
    return d > 0.01;
  }();

  // Two loops sounding together each play 3 dB down (0.708), one loop plays at unity, and the scale
  // is back at 1 once the old tail has closed.
  const double pairWant = std::pow(10.0, -3.0 / 20.0);
  bool pairDown = true;
  for (int i = s + 300 * kMs; i < tailEnd - 100 * kMs; i += 97)
    if (std::fabs(t.pair[static_cast<size_t>(i)] - pairWant) > 0.01) pairDown = false;
  bool pairOne = true;
  for (int i = 0; i < s + 100 * kMs; ++i)
    if (t.pair[static_cast<size_t>(i)] != 1.0f) pairOne = false;
  const bool pairBack = t.pair[static_cast<size_t>(tailEnd + 400 * kMs)] == 1.0f;
  return report("overlap", same && aliveAtKey && closesOnItsOwn && climbs && reaches && captureOk && together && differs && pairDown && pairOne && pairBack,
                "old tail equals the no-second-word run sample for sample and closes at %.0f ms; new word climbs in 5 ms: %.0f, first capture %.2f ms after the key; two loops at -3 dB: %.0f",
                tailEnd / double(kMs), climbs && reaches ? 1.0 : 0.0, (write - s) / double(kMs), pairDown && pairOne && pairBack ? 1.0 : 0.0);
}

// A third word fades the oldest tail out over 20 ms, takes its player, and leaves the newer tail alone.
bool testThirdWord() {
  const int ms = 3200;
  const Signal in = vowel(ms, 0, 3000);
  const int third = 1400;
  const int s = third * kMs;
  cv::BloomParams p;
  p.tailMs = 2000.0f;
  gBloom.reset();
  const Trace ref = trace(gBloom, in, keyOver(ms, {{0, 400}, {700, 1100}}), p);
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, {{0, 400}, {700, 1100}, {third, 1800}}), p);

  // The newer tail is untouched.
  bool newer = true;
  for (size_t i = static_cast<size_t>(s) - 100; i < in.size(); ++i)
    if (t.gain[1][i] != ref.gain[1][i] || t.seq[1][i] != ref.seq[1][i]) newer = false;

  // The oldest tail falls in a straight line to 0 within 20 ms.
  const bool bothAlive = t.tail[0][static_cast<size_t>(s - 1)] && t.tail[1][static_cast<size_t>(s - 1)] && t.gain[0][static_cast<size_t>(s - 1)] > 0.0f;
  int zero = -1;
  for (int i = s; i < s + 100 * kMs; ++i)
    if (t.gain[0][static_cast<size_t>(i)] == 0.0f) {
      zero = i - s;
      break;
    }
  const double step = t.gain[0][static_cast<size_t>(s)] - t.gain[0][static_cast<size_t>(s + 1)];
  bool straight = zero > 0 && step > 0.0;
  for (int i = s + 1; straight && i < s + zero - 2; ++i)
    if (std::fabs((t.gain[0][static_cast<size_t>(i)] - t.gain[0][static_cast<size_t>(i + 1)]) - step) > 1e-6) straight = false;
  const bool fadeTime = zero >= 20 * kMs - 3 && zero <= 20 * kMs + 3;

  // The word then opens on that player: gain back at the wet level, first capture 40 to 50 ms after
  // the key, and every slice it keeps was cut after the key.
  const bool wordOn = t.word[static_cast<size_t>(s + 30 * kMs)] && t.gain[0][static_cast<size_t>(s + 30 * kMs)] > 0.99 * wetAt(0.5);
  int arm = -1, write = -1;
  for (int a : t.arms)
    if (a >= s) {
      arm = a;
      break;
    }
  for (int w : t.writes)
    if (w >= s) {
      write = w;
      break;
    }
  const bool captureOk = arm == s + 40 * kMs && write >= s + 40 * kMs && write <= s + 50 * kMs;
  gBloom.reset();
  cv::BloomParams q = p;
  q.keyFromGate = true;
  Signal y(static_cast<size_t>(s + 400 * kMs));
  for (int i = 0; i < static_cast<int>(y.size()); ++i) {
    q.keyOpen = i < 400 * kMs || (i >= 700 * kMs && i < 1100 * kMs) || i >= s;
    gBloom.process(&in[static_cast<size_t>(i)], &y[static_cast<size_t>(i)], 1, q);
  }
  bool fresh = gBloom.keptCount(0) > 0;
  for (int slot = 0; slot < cv::Bloom::kKept; ++slot)
    if (gBloom.sliceValid(0, slot) && gBloom.sliceCutAt(0, slot) < static_cast<uint32_t>(s)) fresh = false;
  return report("third-word", newer && bothAlive && straight && fadeTime && wordOn && captureOk && fresh,
                "oldest tail reaches 0 after %.2f ms in a straight line (20): %.0f, newer tail equals the two-word run: %.0f, first capture %.2f ms after the key",
                zero / double(kMs), straight ? 1.0 : 0.0, newer ? 1.0 : 0.0, (write - s) / double(kMs));
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
  // Run to the middle of the word to read them.
  gBloom.reset();
  cv::BloomParams q = p;
  q.keyFromGate = true;
  q.keyOpen = true;
  Signal y(in.size());
  const size_t mid = 600 * kMs;
  for (size_t i = 0; i < mid; ++i) gBloom.process(&in[i], &src[i], &y[i], 1, q);
  int checked = 0;
  bool match = true;
  for (int slot = 0; slot < cv::Bloom::kKept; ++slot) {
    if (!gBloom.sliceValid(0, slot)) continue;
    ++checked;
    const uint32_t cut = gBloom.sliceCutAt(0, slot);
    cv::Biquad lp;
    lp.setLowPass(p.tuning.lowpassHz, p.tuning.lowpassQ);
    for (int j = 0; j < gBloom.sliceLen(0, slot); ++j)
      if (std::fabs(gBloom.sliceData(0, slot)[j] - lp.run(src[cut + static_cast<uint32_t>(j)])) > 1e-5f) match = false;
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
  return report("source", match && checked >= 3 && dry && wetDiffers && equalCall,
                "%.0f kept slices equal the source at their cut clock: %.0f; output before the first loop is the dry input's, and the loop follows the source: %.0f; src == in equals the short call: %.0f",
                checked, match ? 1.0 : 0.0, dry && wetDiffers ? 1.0 : 0.0, equalCall ? 1.0 : 0.0);
}

// No slice plays more than 2.5 s after it was cut. A loud slice with a quieter tone held after it
// loops while every later slice is skipped, and is replaced before the limit. A tail's slices leave the
// rotation at the limit and the empty rotation closes over 80 ms.
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
    if (t.seq[0][i] != 0) worstAge = std::max<uint32_t>(worstAge, static_cast<uint32_t>(i) - t.cut[0][i]);
  const uint32_t loud = t.seq[0][static_cast<size_t>(1000 * kMs)];
  const bool stillLoud = loud != 0 && t.seq[0][static_cast<size_t>(2000 * kMs)] == loud;
  int changeAt = -1;
  for (size_t i = static_cast<size_t>(1000 * kMs); i < in.size(); ++i)
    if (t.seq[0][i] != loud) {
      changeAt = static_cast<int>(i);
      break;
    }
  const uint32_t loudCut = t.cut[0][static_cast<size_t>(1000 * kMs)];
  const bool replaced = changeAt > 0 && static_cast<uint32_t>(changeAt) < loudCut + limit && static_cast<uint32_t>(changeAt) > loudCut + limit - 400 * kMs &&
                        t.seq[0][static_cast<size_t>(3500 * kMs)] > loud && t.active[0][static_cast<size_t>(3500 * kMs)];

  // A tail at LINGER 2 s: the slices leave the rotation at the limit and the loop closes there, short
  // of the 3.58 s the law gives, in an 80 ms straight fall.
  const int span = 6000;
  const Signal tin = vowel(span, 0, 900);
  cv::BloomParams q;
  q.tailMs = 2000.0f;
  gBloom.reset();
  const Trace u = trace(gBloom, tin, keyOver(span, 0, 800), q);
  const int w = fallAfter(u.word, 0);
  uint32_t lastCut = 0;
  for (size_t i = 0; i < u.cut[0].size(); ++i) lastCut = std::max(lastCut, u.cut[0][i]);
  int end = -1;
  for (size_t i = static_cast<size_t>(w); i < u.gain[0].size(); ++i)
    if (u.gain[0][i] == 0.0f && !u.active[0][i]) {
      end = static_cast<int>(i);
      break;
    }
  bool fall = end > 4000;
  for (int i = end - 3700; fall && i < end - 2; ++i) {
    const double d0 = u.gain[0][static_cast<size_t>(i)] - u.gain[0][static_cast<size_t>(i + 1)];
    const double d1 = u.gain[0][static_cast<size_t>(i + 1)] - u.gain[0][static_cast<size_t>(i + 2)];
    if (d0 <= 0.0 || std::fabs(d0 - d1) > 1e-6) fall = false;
  }
  const double tailMsEnd = (end - w) / double(kMs);
  const bool inLimit = end > 0 && static_cast<uint32_t>(end) <= lastCut + limit;
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

// The wet path stays free of clicks: the splices crossfade and the passes fade.
bool testNoClicks() {
  const int ms = 1200;
  const Signal in = vowel(ms, 0, 1000);
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 1000), cv::BloomParams{});
  double inJump = 0.0, wetJump = 0.0;
  for (size_t i = 1; i < t.out.size(); ++i) {
    const double w0 = t.out[i - 1] - in[i - 1], w1 = t.out[i] - in[i];
    wetJump = std::fmax(wetJump, std::fabs(w1 - w0));
    inJump = std::fmax(inJump, std::fabs(double(in[i]) - in[i - 1]));
  }
  return report("no-clicks", wetJump < 1.5 * inJump, "largest wet step %.4f against the input's own %.4f (<1.5x)", wetJump, inJump);
}

bool sameBytes(const Signal& a, const Signal& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }

// Two renders from reset are byte-identical, also through a rotation and an overlap, and with a
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

  // Two players running, then switched off: both stop.
  const Signal two = vowel(ms, 0, 2800);
  cv::BloomParams a = on;
  a.tailMs = 2000.0f;
  gBloom.reset();
  for (size_t i = 0; i < 1800 * kMs; ++i) {
    a.keyOpen = i < 400 * kMs || (i >= 700 * kMs && i < 1100 * kMs);
    gBloom.process(&two[i], &out[i], 1, a);
  }
  const bool bothRan = gBloom.playerActive(0) && gBloom.playerActive(1);
  a.on = false;
  gBloom.process(two.data() + 1800 * kMs, out.data() + 1800 * kMs, 1200 * kMs, a);
  bool twoCopy = true;
  for (size_t i = 2800 * kMs; i < two.size(); ++i)
    if (out[i] != two[i]) twoCopy = false;
  const bool stopped = !gBloom.playerActive(0) && !gBloom.playerActive(1) && !gBloom.looping() && !gBloom.inWord();

  cv::BloomParams zero = on;
  zero.bloom = 0.0f;
  gBloom.reset();
  gBloom.process(in.data(), out.data(), static_cast<int>(in.size()), zero);
  const bool knobZero = out == in;
  return report("settled-off", fromReset && wasWet && tailCopy && bothRan && twoCopy && stopped && knobZero && !gBloom.looping(),
                "off from reset == input: %.0f, off after running == input and loop stopped: %.0f, two players off == input: %.0f, AMOUNT 0 == input: %.0f",
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
    peak = std::fmax(peak, std::fabs(y[i] - loud[i]));
  }
  return report("toggle-range", keeps && finite && peak < 2.0, "off and on within 20 ms keeps the loop: %.0f; loud input stays finite, wet peak %.2f (<2)",
                keeps ? 1.0 : 0.0, peak);
}

// Eight slices of 3,000 samples and the small state around them.
bool testStorage() {
  const size_t slices = static_cast<size_t>(cv::Bloom::kPlayers * cv::Bloom::kKept) * cv::Bloom::kSliceMax * sizeof(float);
  return report("storage", slices == 96000 && sizeof(cv::Bloom) < 98000, "sizeof(Bloom) %.0f B, of which eight slices %.0f B (<98,000 in all)",
                static_cast<double>(sizeof(cv::Bloom)), static_cast<double>(slices));
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
  ok &= testInterval();
  ok &= testGrainLaw();
  ok &= testWetLaw();
  ok &= testSkip();
  ok &= testLoop();
  ok &= testTail();
  ok &= testRotation();
  ok &= testOverlap();
  ok &= testThirdWord();
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
