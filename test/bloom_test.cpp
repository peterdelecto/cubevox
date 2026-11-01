// Bloom checks: the word schedule (first capture, re-capture interval, zero crossings, the 12 dB skip),
// the tail law and its close, a word during the tail, the grain laws, the loop, tail-only, the own
// key, determinism, chunk independence, settled off, no-alloc. No window, no audio device.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
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

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0) {
  char detail[200];
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

// Key open over [fromMs, toMs).
std::vector<char> keyOver(int ms, int fromMs, int toMs) {
  std::vector<char> k(static_cast<size_t>(ms) * kMs, 0);
  for (int i = fromMs * kMs; i < toMs * kMs; ++i) k[static_cast<size_t>(i)] = 1;
  return k;
}

// What a run records: the wet gain after each sample, the word and loop flags, the grain and skip
// counters, and the clocks of every capture arm and write start.
struct Trace {
  Signal out, gain;
  std::vector<char> word, loop;
  std::vector<int> taken, skipped, arms, writes;
};

// Runs in through b one sample per call, with the gate key per sample.
Trace trace(cv::Bloom& b, const Signal& in, const std::vector<char>& key, cv::BloomParams p) {
  Trace t;
  p.keyFromGate = true;
  uint32_t lastArm = b.armedAt();
  uint32_t lastWrite = b.writeStartedAt();
  for (size_t i = 0; i < in.size(); ++i) {
    p.keyOpen = key[i] != 0;
    float y = 0.0f;
    gInProcess = true;
    b.process(&in[i], &y, 1, p);
    gInProcess = false;
    if (b.armedAt() != lastArm) t.arms.push_back(static_cast<int>(b.armedAt()));
    if (b.writeStartedAt() != lastWrite) t.writes.push_back(static_cast<int>(b.writeStartedAt()));
    lastArm = b.armedAt();
    lastWrite = b.writeStartedAt();
    t.out.push_back(y);
    t.gain.push_back(b.wetGain());
    t.word.push_back(b.inWord() ? 1 : 0);
    t.loop.push_back(b.looping() ? 1 : 0);
    t.taken.push_back(b.taken());
    t.skipped.push_back(b.skipped());
  }
  return t;
}

// The spec's laws at BLOOM k and TAIL tailMs, written out again here.
struct Expect {
  double wet, grainMs, intervalMs;
};

Expect lawAt(double k, double tailMs) {
  const double x = k < 0.05 ? 0.0 : (k - 0.05) / 0.95;
  const double ramp = k < 0.05 ? k / 0.05 : 1.0;
  double grain = 80.0 + (45.0 - 80.0) * x;
  if (grain > 0.8 * tailMs) grain = 0.8 * tailMs;
  if (grain < 35.0) grain = 35.0;
  return {ramp * (0.25 + 0.45 * x), grain, 180.0 + (55.0 - 180.0) * x};
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

// Re-captures arm every interval plus or minus 5 %, each write starts within 10 ms of its arm, and
// none arm after the key closes.
bool testInterval() {
  bool ok = true;
  double worstLo = 9.0, worstHi = 0.0;
  const double ks[3] = {0.5, 0.05, 1.0};
  for (double k : ks) {
    const int ms = 1500;
    const Signal in = sine(ms, {{0, 1200, 0.5f}});
    cv::BloomParams p;
    p.bloom = static_cast<float>(k);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(ms, 0, 1200), p);
    const double want = lawAt(k, p.tailMs).intervalMs * kMs;
    const int end = fallAfter(t.word, 0);
    if (t.arms.size() < 6 || end < 0) {
      ok = false;
      continue;
    }
    for (size_t i = 1; i < t.arms.size(); ++i) {
      const double r = (t.arms[i] - t.arms[i - 1]) / want;
      worstLo = std::fmin(worstLo, r);
      worstHi = std::fmax(worstHi, r);
      if (r < 0.95 - 1e-9 || r > 1.05 + 1e-9) ok = false;
    }
    for (size_t i = 0; i < t.writes.size() && i < t.arms.size(); ++i)
      if (t.writes[i] < t.arms[i] || t.writes[i] > t.arms[i] + 10 * kMs) ok = false;
    if (t.arms.back() >= end) ok = false;  // no arm after the word closed
  }
  return report("interval", ok, "arm gaps over the interval at BLOOM 0.05/0.5/1: %.3f..%.3f (0.95..1.05), none after the key closes",
                worstLo, worstHi);
}

// The grain length follows BLOOM and TAIL: the write runs from its start to the splice.
bool testGrainLaw() {
  struct Point {
    double k, tail;
  };
  const Point pts[4] = {{0.5, 300.0}, {1.0, 2000.0}, {0.5, 60.0}, {0.0525, 300.0}};
  bool ok = true;
  double got[4];
  for (int j = 0; j < 4; ++j) {
    const int ms = 700;
    const Signal in = sine(ms, {{0, 600, 0.5f}});
    cv::BloomParams p;
    p.bloom = static_cast<float>(pts[j].k);
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
    got[j] = (done - t.writes[0] + 1) / double(kMs);
    if (std::fabs(got[j] - lawAt(pts[j].k, pts[j].tail).grainMs) > 0.1) ok = false;
  }
  return report("grain-law", ok, "grain ms at (0.5, 300)/(1, 2000)/(0.5, 60)/(0.0525, 300) = %.2f / %.2f / %.2f / %.2f", got[0],
                got[1], got[2], got[3]);
}

// A slice more than 12 dB under the looping grain is skipped; one 10 dB under is taken.
bool testSkip() {
  const int ms = 1500;
  const float quiet = 0.5f * std::pow(10.0f, -13.0f / 20.0f);
  const float near = 0.5f * std::pow(10.0f, -10.0f / 20.0f);
  const Signal in = sine(ms, {{0, 400, 0.5f}, {400, 900, quiet}, {900, 1400, near}});
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 1400), cv::BloomParams{});
  auto at = [&](const std::vector<int>& v, int msAt) { return v[static_cast<size_t>(msAt * kMs)]; };
  const int takenQuiet = at(t.taken, 900) - at(t.taken, 480);
  const int skippedQuiet = at(t.skipped, 900) - at(t.skipped, 480);
  const int takenNear = at(t.taken, 1400) - at(t.taken, 1000);
  const int skippedNear = at(t.skipped, 1400) - at(t.skipped, 1000);
  const bool ok = takenQuiet == 0 && skippedQuiet >= 2 && takenNear >= 2 && skippedNear == 0;
  return report("skip-12db", ok, "-13 dB slices: %.0f taken, %.0f skipped (0, >=2); -10 dB slices: %.0f taken, %.0f skipped (>=2, 0)",
                takenQuiet, skippedQuiet, takenNear, skippedNear);
}

// After a silent stretch the grain keeps looping: a loop plays at unity speed, so the wet repeats at
// the grain length. The silent slices that follow are all skipped.
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
  if (done < 0 || t.writes.empty()) return report("loop", false, "no grain taken (%.0f)", 0.0);
  const int len = done - t.writes[0] + 1;
  double peak = 0.0, worst = 0.0;
  for (int i = 400 * kMs; i < 600 * kMs; ++i) {
    peak = std::fmax(peak, std::fabs(t.out[static_cast<size_t>(i)]));
    worst = std::fmax(worst, std::fabs(t.out[static_cast<size_t>(i)] - t.out[static_cast<size_t>(i + len)]));
  }
  const bool silentSkipped = t.taken.back() == 1 && t.skipped.back() >= 2;
  return report("loop", peak > 0.05 && worst < 0.01 * peak && silentSkipped,
                "wet repeats at the %.0f-sample grain: worst difference %.2g of peak %.3f (<1%%), silent slices skipped: %.0f",
                len, worst, peak, silentSkipped ? 1.0 : 0.0);
}

// After the word the wet gain follows wet * e^(-t/tau), switches to the 80 ms fade at life 0.2, and
// ends at exactly 0 with the loop stopped.
bool testTail() {
  const int ms = 4000;
  bool ok = true;
  double worstRel = 0.0, endMs = 0.0, wantEndMs = 0.0;
  const double tails[4] = {300.0, 60.0, 1000.0, 2000.0};
  for (double tail : tails) {
    const int span = tail > 1500.0 ? 6000 : (tail > 500.0 ? 4500 : ms);
    const Signal in = vowel(span, 0, 700);
    cv::BloomParams p;
    p.tailMs = static_cast<float>(tail);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(span, 0, 700), p);
    const int w = fallAfter(t.word, 0);
    if (w < 0) {
      ok = false;
      continue;
    }
    const Expect e = lawAt(0.5, tail);
    const double tau = tauMs(tail) * kMs;
    const double tSwitch = tau * std::log(5.0);
    // The decay step is a float, so the switch to the fade lands within 0.5 % of its time. The fade
    // is checked against the switch the engine made.
    int sw = 0;
    while (static_cast<size_t>(w + sw) < t.gain.size() && t.gain[static_cast<size_t>(w + sw)] > 0.2 * e.wet) ++sw;
    if (std::fabs(sw - tSwitch) > 0.005 * tSwitch) ok = false;
    for (int j = 0; j < sw; j += 97) {
      const double want = e.wet * std::exp(-(j + 1) / tau);
      const double rel = std::fabs(t.gain[static_cast<size_t>(w + j)] - want) / want;
      worstRel = std::fmax(worstRel, rel);
      if (rel > 0.015) ok = false;
    }
    const double staticTau = 80.0 * kMs;
    for (int j = sw; j < static_cast<int>(sw + 5 * staticTau); j += 211) {
      const double want = e.wet * 0.2 * std::exp(-(j - sw) / staticTau);
      if (want < 0.002) break;
      const double rel = std::fabs(t.gain[static_cast<size_t>(w + j)] - want) / want;
      worstRel = std::fmax(worstRel, rel);
      if (rel > 0.015) ok = false;
    }
    int zero = -1;
    for (size_t i = static_cast<size_t>(w); i < t.gain.size(); ++i)
      if (t.gain[i] == 0.0f) {
        zero = static_cast<int>(i) - w;
        break;
      }
    wantEndMs = (tSwitch + staticTau * std::log(e.wet * 0.2 / 0.001)) / kMs;
    endMs = zero < 0 ? -1.0 : zero / double(kMs);
    bool stays = zero >= 0;
    for (size_t i = static_cast<size_t>(w + zero + 1); stays && i < t.gain.size(); ++i)
      if (t.gain[i] != 0.0f || t.loop[i]) stays = false;
    for (size_t i = static_cast<size_t>(w) + 1; i < static_cast<size_t>(w + (zero < 0 ? 0 : zero)); ++i)
      if (t.gain[i] > t.gain[i - 1]) ok = false;
    if (!stays || std::fabs(endMs - wantEndMs) > 5.0) ok = false;
  }
  return report("tail-law", ok, "gain vs wet*life: worst %.2f%% off (<1.5%%), fade switch at life 0.2 within 0.5%%; last tail ends at %.0f ms, law %.0f ms",
                worstRel * 100.0, endMs, wantEndMs);
}

// A word during the tail restarts the sequence: the gain climbs from where it sits to the wet level
// in 5 ms, and the first capture lands 40 to 50 ms after the key opens again.
bool testRestart() {
  const int ms = 2000;
  const int second = 1000;
  const Signal in = vowel(ms, 0, 1700);
  std::vector<char> key = keyOver(ms, 0, 600);
  for (int i = second * kMs; i < 1600 * kMs; ++i) key[static_cast<size_t>(i)] = 1;
  gBloom.reset();
  const Trace t = trace(gBloom, in, key, cv::BloomParams{});
  const int s = second * kMs;
  const double wet = lawAt(0.5, 300.0).wet;
  const double before = t.gain[static_cast<size_t>(s - 1)];
  const bool tailAlive = t.loop[static_cast<size_t>(s - 1)] != 0;
  bool climbs = before > 0.0 && before < 0.8 * wet;
  for (int i = s + 1; i < s + 240; ++i)
    if (t.gain[static_cast<size_t>(i)] < t.gain[static_cast<size_t>(i - 1)]) climbs = false;
  const bool reaches = t.gain[static_cast<size_t>(s + 245)] > 0.99 * wet;
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
  return report("restart", tailAlive && climbs && reaches && captureOk,
                "tail gain %.3f at the new key (wet %.3f), back at wet within 5 ms: %.0f, first capture %.2f ms after the key (40..50)",
                before, wet, reaches ? 1.0 : 0.0, (write - s) / double(kMs));
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
    if (t.gain[static_cast<size_t>(i)] != 0.0f) silent = false;
  const double wet = lawAt(0.5, 300.0).wet;
  const bool blooms = w > 0 && t.gain[static_cast<size_t>(w + 245)] > 0.95 * wet * std::exp(-245.0 / (300.0 * kMs)) &&
                      t.loop[static_cast<size_t>(w)];
  return report("tail-only", silent && blooms, "gain 0 under the word: %.0f, gain at 5 ms after the word %.3f (wet %.3f)",
                silent ? 1.0 : 0.0, w > 0 ? t.gain[static_cast<size_t>(w + 245)] : 0.0, wet);
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

// Two renders from reset are byte-identical, and with a constant key the call size does not matter.
bool testDeterminism() {
  const int ms = 2500;
  const Signal in = vowel(ms, 100, 1500);
  const std::vector<char> key = keyOver(ms, 100, 1500);
  gBloom.reset();
  const Trace a = trace(gBloom, in, key, cv::BloomParams{});
  gBloom.reset();
  const Trace b = trace(gBloom, in, key, cv::BloomParams{});

  bool chunked = true;
  const std::vector<char> open(in.size(), 1);
  Signal ref;
  const int sizes[4] = {1, 7, 64, 1000};
  for (int c : sizes) {
    gBloom.reset();
    cv::BloomParams p;
    p.keyFromGate = true;
    p.keyOpen = true;
    Signal out(in.size());
    for (size_t i = 0; i < in.size(); i += static_cast<size_t>(c)) {
      const int n = static_cast<int>(in.size() - i < static_cast<size_t>(c) ? in.size() - i : static_cast<size_t>(c));
      gBloom.process(&in[i], &out[i], n, p);
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

  cv::BloomParams zero = on;
  zero.bloom = 0.0f;
  gBloom.reset();
  gBloom.process(in.data(), out.data(), static_cast<int>(in.size()), zero);
  const bool knobZero = out == in;
  return report("settled-off", fromReset && wasWet && tailCopy && knobZero && !gBloom.looping(),
                "off from reset == input: %.0f, off after running == input and loop stopped: %.0f, BLOOM 0 == input: %.0f",
                fromReset ? 1.0 : 0.0, wasWet && tailCopy ? 1.0 : 0.0, knobZero ? 1.0 : 0.0);
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

bool testNoAlloc() {
  const Signal in = vowel(400, 0, 400);
  gBloom.reset();
  Signal out(in.size());
  cv::BloomParams p;
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    const int n = static_cast<int>(in.size() - i < static_cast<size_t>(cv::kBlock) ? in.size() - i : static_cast<size_t>(cv::kBlock));
    gInProcess = true;
    gBloom.process(&in[i], &out[i], n, p);
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
  ok &= testSkip();
  ok &= testLoop();
  ok &= testTail();
  ok &= testRestart();
  ok &= testTailOnly();
  ok &= testOwnKey();
  ok &= testNoClicks();
  ok &= testDeterminism();
  ok &= testSettledOff();
  ok &= testToggleAndRange();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
