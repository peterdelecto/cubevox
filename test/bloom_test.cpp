// Chatter checks (the Bloom engine). Word schedule: first-capture, two-plays, skip-12db, loop, leading-edge,
// rearm-once. AMOUNT laws: slice-law, wet-law. LENGTH tail: tail-law, tail-20ms, tail-slice,
// tail-after-stop, tail-on-clips, age-limit, loop-under-word. Cuts with a gap: straddle, clip-gaps. A new word
// and the key: next-word, yield, key-blip, key-hold, own-key. Output: source, no-clicks, dry-unity,
// knob-moves.
// Repeatability: determinism, call-sizes, clock-wrap, settled-off, toggle-range, fuzz. Memory: storage, no-alloc.
// No window, no audio device.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "engine/bloom.h"
#include "engine/gate.h"

#include "alloc_guard.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMs = cv::kSampleRate / 1000;
constexpr int kWord = cv::Bloom::kWord;
constexpr int kTail = cv::Bloom::kTail;
constexpr int kFade = cv::Bloom::kFade;
constexpr int kVoices = cv::Bloom::kVoices;
constexpr int kFadeSmp = 278;  // the pass window's fade, 5.8 ms at 48 kHz

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0, double e = 0.0) {
  char detail[512];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// samples/ is git-ignored, so CI has none. A check that needs Adam's clips prints SKIP, never PASS, when the
// folder is absent, and FAILS when the folder is there and a clip in it is missing or empty.
bool skipNoClips(const char* name) {
  std::error_code ec;
  if (std::filesystem::is_directory(CUBEVOX_SAMPLES_DIR, ec)) {
    std::printf("FAIL %s  a clip in samples/ is missing or empty\n", name);
    return false;
  }
  std::printf("SKIP %s  samples/ is not in this checkout\n", name);
  return true;
}

static cv::Bloom gBloom;

using Signal = std::vector<float>;

// A slider setting for the laws: AMOUNT 0..1 and LENGTH in ms.
struct Point {
  double amount, lengthMs;
};

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

// `x` with silence outside bursts of `onMs` that start at `firstMs` and then every `periodMs`.
Signal bursts(const Signal& x, int firstMs, int onMs, int periodMs) {
  Signal y(x.size(), 0.0f);
  for (size_t i = 0; i < x.size(); ++i) {
    const int ms = static_cast<int>(i) / kMs - firstMs;
    if (ms >= 0 && ms % periodMs < onMs) y[i] = x[i];
  }
  return y;
}

// `x` later by `ms`, as a pitch stage's lag delays its output.
Signal delayed(const Signal& x, int ms) {
  Signal y(x.size(), 0.0f);
  for (size_t i = static_cast<size_t>(ms) * kMs; i < x.size(); ++i) y[i] = x[i - static_cast<size_t>(ms) * kMs];
  return y;
}

// Key open over each [fromMs, toMs) in `runs`.
std::vector<char> keyOver(int ms, const std::vector<std::pair<int, int>>& runs) {
  std::vector<char> k(static_cast<size_t>(ms) * kMs, 0);
  for (const auto& r : runs)
    for (int i = r.first * kMs; i < r.second * kMs && i < static_cast<int>(k.size()); ++i) k[static_cast<size_t>(i)] = 1;
  return k;
}

std::vector<char> keyOver(int ms, int fromMs, int toMs) { return keyOver(ms, {{fromMs, toMs}}); }

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

// Adam's three clips, each with 0.4 s of silence ahead and `roomSmp` samples of it after. Empty when
// samples/ is missing.
std::vector<Signal> loadClips(size_t roomSmp) {
  const char* names[] = {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"};
  std::vector<Signal> clips;
  for (const char* name : names) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) return {};
    Signal x(static_cast<size_t>(cv::kSampleRate) * 2 / 5, 0.0f);
    x.insert(x.end(), clip.begin(), clip.end());
    x.resize(x.size() + roomSmp, 0.0f);
    clips.push_back(std::move(x));
  }
  return clips;
}

// xorshift64, for the scripted histories.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<uint32_t>(s >> 16);
  }
  double uni() { return (next() & 0xFFFFFF) / double(0x1000000); }
  double logUni(double lo, double hi) { return lo * std::pow(hi / lo, uni()); }
  int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
};

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
    const bool keyOpen = key[i] != 0;
    float y = 0.0f;
    gInProcess = true;
    if (src) b.process(&in[i], &(*src)[i], &y, 1, p, keyOpen);
    else b.process(&in[i], &y, 1, p, keyOpen);
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

// AMOUNT's slice length in ms, before drift and LENGTH's cap.
double nominalMs(double amount) { return 80.0 + (45.0 - 80.0) * amount; }

// The slice lengths in samples the laws allow: AMOUNT's length drifting by 3 % either way, under
// LENGTH's cap (0.8 of its setting) and the 80 ms buffer, over the 35 ms floor, to within a sample.
struct SliceRange {
  int lo, hi;
};

SliceRange sliceRange(double amount, double lengthMs) {
  auto clampMs = [&](double ms) { return std::fmin(std::fmax(std::fmin(ms, 0.8 * lengthMs), 35.0), 80.0); };
  return {static_cast<int>(clampMs(nominalMs(amount) * 0.97) * kMs) - 1, static_cast<int>(clampMs(nominalMs(amount) * 1.03) * kMs) + 1};
}

// The tail's life time constant and its faster fade's, both in ms.
double tauMs(double lengthMs) {
  const double scaled = std::fmin(300.0, 1.5 * lengthMs);
  return std::fmax(lengthMs, scaled);
}

double staticTauMs(double lengthMs) { return std::fmin(80.0, lengthMs); }

// How far a tail's end may sit from the law's: 5 ms, or 0.5 % of the law's end when that is more. The
// decay step is a float one-pole, so its time constant lands within 0.5 % of the law's.
double endTolMs(double wantMs) { return std::fmax(5.0, 0.005 * wantMs); }

// Where the law ends the tail, in ms after the word: life falls to 0.2 on the first constant, then the
// faster fade runs the wet from 0.2 of its level to the close at 0.001.
double lawEndMs(double lengthMs, double wet) {
  return tauMs(lengthMs) * std::log(5.0) + staticTauMs(lengthMs) * std::log(wet * 0.2 / 0.001);
}

// The window a slice carries: a smoothstep fade over its first and last 5.8 ms, written out again here.
double passWindowAt(int pos, int len) {
  const int edge = std::min(pos, len - 1 - pos);
  if (edge >= kFadeSmp) return 1.0;
  const double x = edge / double(kFadeSmp);
  return x * x * (3.0 - 2.0 * x);
}

bool crossingAt(const Signal& in, int i) { return i > 0 && ((in[static_cast<size_t>(i - 1)] < 0.0f) != (in[static_cast<size_t>(i)] < 0.0f)); }

// First index where flags goes from 1 to 0 at or after `from`; -1 when it does not.
int fallAfter(const std::vector<char>& f, int from) {
  for (size_t i = static_cast<size_t>(from) + 1; i < f.size(); ++i)
    if (f[i - 1] && !f[i]) return static_cast<int>(i);
  return -1;
}

// The first value in `v` at or after `from`; -1 when there is none.
int firstAtOrAfter(const std::vector<int>& v, int from) {
  for (int a : v)
    if (a >= from) return a;
  return -1;
}

// The first sample at or after `from` where voice `k` plays a slice; -1 when none does.
int firstPlay(const Trace& t, int k, int from) {
  const std::vector<uint32_t>& seq = t.seq[static_cast<size_t>(k)];
  for (size_t i = static_cast<size_t>(from); i < seq.size(); ++i)
    if (seq[i] != 0) return static_cast<int>(i);
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

// The Input gate in front of the Chatter card as the box runs them with no pitch card: the gate's
// detector state is the word key and its output is both the dry path and the slice source, 64
// samples a call.
struct GatedChatter {
  cv::Gate gate;
  cv::GateParams gp;
  cv::BloomParams bp;

  GatedChatter(float thresholdDb, float decayMs, float amount, float lengthMs) {
    gp.on = true;
    gp.thresholdDb = thresholdDb;
    gp.tuning = cv::inputGateTuning();
    gp.tuning.releaseMs = decayMs;
    bp.keyFromGate = true;
    bp.amount = amount;
    bp.lengthMs = lengthMs;
    gate.reset();
    gBloom.reset();
  }

  void run(const float* in, float* gated, float* out, int n) {
    gate.process(in, gated, n, gp);
    gBloom.process(gated, gated, out, n, bp, gate.open());
  }
};

// The slot holding the slice voice `k` plays; -1 when it plays none.
int slotPlayedBy(int k) {
  const uint32_t seq = gBloom.playingSeq(k);
  for (int slot = 0; seq != 0 && slot < cv::Bloom::kSlices; ++slot)
    if (gBloom.sliceValid(slot) && gBloom.sliceSeq(slot) == seq) return slot;
  return -1;
}

float slicePeak(int slot) {
  float peak = 0.0f;
  for (int j = 0; j < gBloom.sliceLen(slot); ++j) peak = std::fmax(peak, std::fabs(gBloom.sliceData(slot)[j]));
  return peak;
}

// The share of a slice's 2 ms windows whose peak sits more than 20 dB under the slice's own peak.
double silentShare(int slot) {
  constexpr int kWin = 2 * kMs;
  const float peak = slicePeak(slot);
  int windows = 0, quiet = 0;
  for (int s = 0; s + kWin <= gBloom.sliceLen(slot); s += kWin) {
    float pk = 0.0f;
    for (int j = s; j < s + kWin; ++j) pk = std::fmax(pk, std::fabs(gBloom.sliceData(slot)[j]));
    ++windows;
    quiet += pk < 0.1f * peak ? 1 : 0;
  }
  return windows ? double(quiet) / windows : 0.0;
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

// What the splices of a 1.5 s word say about the slice length at one setting.
struct SliceStats {
  bool ok = true;
  double meanMs = 0.0, spreadPct = 0.0;
};

SliceStats sliceStats(const Point& pt) {
  SliceStats s;
  const int ms = 1900;
  cv::BloomParams p;
  p.amount = static_cast<float>(pt.amount);
  p.lengthMs = static_cast<float>(pt.lengthMs);
  gBloom.reset();
  const Trace t = trace(gBloom, vowel(ms, 0, 1700), keyOver(ms, 0, 1500), p);
  const std::vector<Splice> sp = splicesOf(t, kWord);
  const SliceRange range = sliceRange(pt.amount, pt.lengthMs);
  if (sp.size() < 8) {
    s.ok = false;
    return s;
  }
  int lo = 1 << 30, hi = 0;
  double sum = 0.0;
  for (const Splice& x : sp) {
    lo = std::min(lo, x.len);
    hi = std::max(hi, x.len);
    sum += x.len;
    if (x.len < range.lo || x.len > range.hi) s.ok = false;
  }
  s.meanMs = sum / double(sp.size()) / kMs;
  s.spreadPct = 100.0 * (hi - lo) / (nominalMs(pt.amount) * kMs);

  // Free of both ends of the law, the slice drifts, about AMOUNT's length on average. Under LENGTH's
  // cap it holds one length.
  const bool capped = range.hi - range.lo <= 2;
  if (capped && hi != lo) s.ok = false;
  if (!capped && range.hi < 80 * kMs - 2) {
    if (s.spreadPct < 1.0 || std::fabs(s.meanMs - nominalMs(pt.amount)) > 0.015 * nominalMs(pt.amount)) s.ok = false;
  }
  return s;
}

// The slice runs 80 ms at AMOUNT 0 to 45 ms at 100 %, linear, drifting 3 % either way from cut to
// cut; LENGTH caps it at 0.8 of its setting, so a capped slice does not drift.
bool testSliceLaw() {
  const Point pts[8] = {{0.02, 300.0}, {0.1, 300.0}, {0.25, 300.0}, {0.5, 300.0},
                        {0.75, 300.0}, {1.0, 2000.0}, {0.5, 60.0},  {0.5, 75.0}};
  SliceStats s[8];
  bool ok = true;
  for (int j = 0; j < 8; ++j) {
    s[j] = sliceStats(pts[j]);
    ok = ok && s[j].ok;
  }
  const bool law = report("slice-law", ok,
                          "mean slice at AMOUNT 0.25/0.5/1 = %.2f / %.2f / %.2f ms (71.25, 62.5, 45), drifting %.1f%% of its length across the word at AMOUNT 0.5 (within 3%% either way)",
                          s[2].meanMs, s[3].meanMs, s[5].meanMs, s[3].spreadPct);
  const bool caps = report("slice-law-caps", s[6].spreadPct == 0.0 && s[7].spreadPct == 0.0,
                           "LENGTH 60 and 75 at AMOUNT 0.5 cap the slice at %.2f and %.2f ms (48, 60), with no drift under the cap", s[6].meanMs, s[7].meanMs);
  return law && caps;
}

// What the splices and writes of a word say about the two-play grid at several settings.
struct PlayStats {
  bool ok = true, varied = true;
  double lo = 1e9, hi = 0.0, worstGap = 0.0;
};

void checkTwoPlays(const Point& pt, PlayStats& s) {
  const int ms = 1900;
  cv::BloomParams p;
  p.amount = static_cast<float>(pt.amount);
  p.lengthMs = static_cast<float>(pt.lengthMs);
  gBloom.reset();
  const Trace t = trace(gBloom, vowel(ms, 0, 1700), keyOver(ms, 0, 1500), p);
  const int end = fallAfter(t.word, 0);
  const std::vector<Splice> sp = splicesOf(t, kWord);
  if (t.arms.size() < 8 || sp.size() < 8 || end < 0) {
    s.ok = false;
    return;
  }

  // Every splice after the first lands exactly two plays of the slice before it, and the interval
  // stays within 3 % of two slices at AMOUNT's length (LENGTH's cap holds it fixed below that).
  const SliceRange range = sliceRange(pt.amount, pt.lengthMs);
  const double nominal = 2.0 * std::fmin(nominalMs(pt.amount), 0.8 * pt.lengthMs) * kMs;
  bool allSame = true;
  for (size_t i = 1; i < sp.size(); ++i) {
    const int gap = sp[i].at - sp[i - 1].at;
    s.worstGap = std::fmax(s.worstGap, std::fabs(double(gap - 2 * sp[i - 1].len)));
    s.lo = std::fmin(s.lo, gap / nominal);
    s.hi = std::fmax(s.hi, gap / nominal);
    if (gap != 2 * sp[i - 1].len || sp[i - 1].len < range.lo || sp[i - 1].len > range.hi) s.ok = false;
    if (gap != sp[1].at - sp[0].at) allSame = false;
  }
  if (allSame && range.hi - range.lo > 2) s.varied = false;
  for (size_t i = 0; i < t.writes.size() && i < t.arms.size(); ++i)
    if (t.writes[i] < t.arms[i] || t.writes[i] > t.arms[i] + 10 * kMs) s.ok = false;
  if (t.arms.back() >= end) s.ok = false;  // no arm after the word closed
}

// A slice plays exactly twice and the next one splices in at the slice boundary. The interval between
// cuts, two plays of the slice, drifts within 3 % of its law and is not constant. Each write starts
// within 10 ms of its arm, and none arm after the key closes.
bool testTwoPlays() {
  const Point pts[4] = {{0.5, 300.0}, {0.1, 300.0}, {1.0, 300.0}, {0.5, 60.0}};
  PlayStats s;
  for (const Point& pt : pts) checkTwoPlays(pt, s);
  return report("two-plays", s.ok && s.varied && s.lo >= 0.969 && s.hi <= 1.031,
                "splices land exactly two plays apart (worst %.0f samples off); the interval runs %.4f..%.4f of two slices at AMOUNT's length (0.97..1.03) and varies: %.0f",
                s.worstGap, s.lo, s.hi, s.varied ? 1.0 : 0.0);
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
    p.amount = static_cast<float>(a);
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

// One LENGTH setting of the tail law: the word's length, and how long the run lasts.
struct TailCase {
  double lengthMs;
  int wordMs, spanMs;
};

// The gain after the word follows wet * e^(-t/tau) until life reaches 0.2, then the faster fade. The
// switch lands within 0.5 % of its time. Returns the worst relative error; clears `ok` on a miss.
double tailGainError(const Trace& t, int w, double lengthMs, bool& ok) {
  const double wet = wetAt(0.5);
  const double tau = tauMs(lengthMs) * kMs;
  const double tSwitch = tau * std::log(5.0);

  // The decay step is a float, so the switch lands within 0.5 % of its time. The fade is checked
  // against the switch the engine made.
  int sw = 0;
  while (static_cast<size_t>(w + sw) < t.total.size() && t.total[static_cast<size_t>(w + sw)] > 0.2 * wet) ++sw;
  if (std::fabs(sw - tSwitch) > 0.005 * tSwitch) ok = false;
  double worst = 0.0;
  for (int j = 0; j < sw; j += 97) {
    const double want = wet * std::exp(-(j + 1) / tau);
    const double rel = std::fabs(t.total[static_cast<size_t>(w + j)] - want) / want;
    worst = std::fmax(worst, rel);
    if (rel > 0.015) ok = false;
  }
  const double staticTau = staticTauMs(lengthMs) * kMs;
  for (int j = sw; j < static_cast<int>(sw + 5 * staticTau); j += 211) {
    const double want = wet * 0.2 * std::exp(-(j - sw) / staticTau);
    if (want < 0.002) break;
    const double rel = std::fabs(t.total[static_cast<size_t>(w + j)] - want) / want;
    worst = std::fmax(worst, rel);
    if (rel > 0.015) ok = false;
  }
  return worst;
}

// Where the tail closes, in ms after the word, or -1. It ends at the law's end, stays at 0 with the
// loop stopped, and the gain never rises on the way.
double tailEndMs(const Trace& t, int w, double lengthMs, bool& ok) {
  int zero = -1;
  for (size_t i = static_cast<size_t>(w); i < t.total.size(); ++i)
    if (t.total[i] == 0.0f) {
      zero = static_cast<int>(i) - w;
      break;
    }
  const double endMs = zero < 0 ? -1.0 : zero / double(kMs);
  const double wantMs = lawEndMs(lengthMs, wetAt(0.5));
  bool stays = zero >= 0;
  for (size_t i = static_cast<size_t>(w + zero + 1); stays && i < t.total.size(); ++i)
    if (t.total[i] != 0.0f || t.loop[i]) stays = false;
  for (size_t i = static_cast<size_t>(w) + 1; i < static_cast<size_t>(w + (zero < 0 ? 0 : zero)); ++i)
    if (t.total[i] > t.total[i - 1]) ok = false;
  if (!stays || std::fabs(endMs - wantMs) > endTolMs(wantMs)) ok = false;
  return endMs;
}

// After the word the wet gain follows wet * e^(-t/tau), switches to the faster fade (80 ms, or LENGTH
// when that is shorter) at life 0.2, and ends at exactly 0 with the loop stopped. LENGTH 20, 40, 80,
// 300, 2000 and 3000 ms each end at the law's own end. The age limit does not reach a tail.
bool testTail() {
  bool ok = true;
  double worstRel = 0.0, endMs = 0.0, wantEndMs = 0.0;
  const TailCase cases[6] = {{20.0, 700, 1500},  {40.0, 700, 1500},  {80.0, 700, 2000},
                             {300.0, 700, 4000}, {2000.0, 200, 4500}, {3000.0, 200, 6500}};
  for (const TailCase& c : cases) {
    const Signal in = vowel(c.spanMs, 0, c.wordMs);
    cv::BloomParams p;
    p.lengthMs = static_cast<float>(c.lengthMs);
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(c.spanMs, 0, c.wordMs), p);
    const int w = fallAfter(t.word, 0);
    if (w < 0) {
      ok = false;
      continue;
    }
    worstRel = std::fmax(worstRel, tailGainError(t, w, c.lengthMs, ok));
    endMs = tailEndMs(t, w, c.lengthMs, ok);
    wantEndMs = lawEndMs(c.lengthMs, wetAt(0.5));
  }
  return report("tail-law", ok, "LENGTH 20/40/80/300/2000/3000 ms: gain vs wet*life worst %.2f%% off (<1.5%%), fade switch at life 0.2 within 0.5%%; last tail (3 s) ends at %.0f ms, law %.0f ms",
                worstRel * 100.0, endMs, wantEndMs);
}

// LENGTH 20 ms: the life constant is 30 ms and the faster fade 20 ms, so the tail closes at the law's end,
// about 139 ms after the word. The word ends at nine offsets, so the close lands at different points of
// the pass. The input is silent by then, so the output is the wet alone, and the last sample before the
// close sits under the engine's close level of 0.001 (-60 dBFS).
bool testShortTail() {
  const int ms = 1200;
  const int offsets[9] = {0, 5, 9, 14, 20, 27, 33, 41, 52};
  const double wantMs = lawEndMs(20.0, wetAt(0.5));
  bool ok = true;
  double worstEnd = 0.0, worstClose = 0.0, wordStep = 0.0;
  for (const int off : offsets) {
    const int wordMs = 700 + off;
    const Signal in = vowel(ms, 0, wordMs);
    cv::BloomParams p;
    p.lengthMs = 20.0f;
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(ms, 0, wordMs), p);
    const int w = fallAfter(t.word, 0);
    int end = -1;
    for (size_t i = static_cast<size_t>(w < 0 ? 0 : w); i < t.total.size(); ++i)
      if (t.total[i] == 0.0f) {
        end = static_cast<int>(i);
        break;
      }
    if (w < 0 || end < 0) {
      ok = false;
      continue;
    }
    const double endMs = (end - w) / double(kMs);
    worstEnd = std::fmax(worstEnd, std::fabs(endMs - wantMs));
    const size_t e = static_cast<size_t>(end);
    worstClose = std::fmax(worstClose, std::fmax(std::fabs(t.out[e - 1]), std::fabs(static_cast<double>(t.out[e]) - t.out[e - 1])));
    for (int i = 300 * kMs; i < 650 * kMs; ++i) {
      const size_t k = static_cast<size_t>(i);
      wordStep = std::fmax(wordStep, std::fabs(static_cast<double>(t.out[k]) - t.out[k - 1]));
    }
  }
  return report("tail-20ms", ok && worstEnd <= 5.0 && worstClose < 0.001,
                "LENGTH 20 ms tail closes within %.1f ms of the law's end (%.1f ms after the word, <= 5), the last sample before the close and the step at it stay under %.5f (< 0.001; the word's largest sample step is %.4f)",
                worstEnd, wantMs, worstClose, wordStep);
}

// What one word end at `closeAt` ms leaves, for a vowel that stops with the key (LENGTH 600 ms).
struct TailEnd {
  bool ran = false, tail = false, newest = true, oneSlice = true, closes = true;
  int skippedAfter = 0;    // cuts skipped between the voice's stop and the word's end
  double gapShare = 0.0;   // the part of the tail's slice cut after the voice stopped
  double repeat = 1.0;     // correlation of the wet one slice later
};

// The wet repeats at the slice length once the splice crossfade is over: the 2,400 samples starting
// 20 ms after the word correlate with the 2,400 that follow one slice (`len` samples) later.
double repeatCorrelation(const Signal& out, int w, int len) {
  double ab = 0.0, aa = 0.0, bb = 0.0;
  for (int j = 0; j < 2400; ++j) {
    const double x = out[static_cast<size_t>(w + 20 * kMs + j)], y = out[static_cast<size_t>(w + 20 * kMs + len + j)];
    ab += x * y;
    aa += x * x;
    bb += y * y;
  }
  return ab / std::sqrt(aa * bb);
}

TailEnd runTailEnd(int closeAt) {
  const int ms = 2600;
  cv::BloomParams p;
  p.lengthMs = 600.0f;
  TailEnd e;

  // The input stops with the key, so the tail is the only sound after the word.
  const Signal in = vowel(ms, 0, closeAt);
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, closeAt), p);
  const int w = fallAfter(t.word, 0);
  if (w < 0) return e;
  e.ran = true;
  const size_t at = static_cast<size_t>(w);
  e.skippedAfter = t.skipped[at] - t.skipped[static_cast<size_t>(closeAt) * kMs];
  if (t.taken[at] == 0) {
    e.oneSlice = !t.on[kTail][at];
    return e;
  }

  // The tail plays the newest slice committed before the word ended, from the word's own voice.
  e.tail = true;
  const uint32_t want = static_cast<uint32_t>(t.taken[at]);
  e.newest = t.on[kTail][at] && !t.on[kWord][at] && t.seq[kTail][at] == want;
  int last = -1;
  for (size_t i = at; i < t.on[kTail].size() && t.on[kTail][i]; ++i) {
    last = static_cast<int>(i);
    if (t.seq[kTail][i] != want || t.spliced[kTail][i] != t.spliced[kTail][at]) e.oneSlice = false;
  }
  e.closes = last >= 0 && !t.on[kTail].back();
  const int len = t.len[kTail][at];
  e.gapShare = std::fmax(0.0, (double(t.cut[kTail][at]) + len - double(closeAt) * kMs) / len);
  e.repeat = repeatCorrelation(t.out, w, len);
  return e;
}

// The tail repeats one slice, the newest the word committed, with no splice and no rotation, and the
// slice holds the voice to its end: a cut that straddles the voice's stop has a gap and is skipped,
// so no more than an eighth of the repeated slice (12.5 %) was cut after the voice stopped. The word
// ends at every offset across two cut intervals.
bool testTailSlice() {
  int runs = 0, noTail = 0, straddled = 0;
  bool all = true;
  double worstGap = 0.0, worstRepeat = 1.0;
  for (int closeAt = 480; closeAt < 730; closeAt += 5) {
    const TailEnd e = runTailEnd(closeAt);
    if (!e.ran) {
      all = false;
      continue;
    }
    ++runs;
    all = all && e.newest && e.oneSlice && e.closes;
    noTail += e.tail ? 0 : 1;
    straddled += e.skippedAfter > 0 ? 1 : 0;
    if (!e.tail) continue;
    worstGap = std::fmax(worstGap, e.gapShare);
    worstRepeat = std::fmin(worstRepeat, e.repeat);
  }
  return report("tail-slice", all && runs > 40 && worstGap <= 0.126 && straddled >= 10 && worstRepeat > 0.99,
                "%.0f word ends (%.0f cut nothing): the tail plays one slice, the newest cut, with no splice; at most %.1f%% of it cut after the voice stopped (12.5), cuts skipped after the stop in %.0f runs; one slice later the wet correlates at least %.4f",
                runs, noTail, worstGap * 100.0, straddled, worstRepeat);
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
    peak = slicePeak(slot);
  }
  float voicePeak = 0.0f;
  for (size_t i = 0; i < static_cast<size_t>(400 * kMs); ++i) voicePeak = std::fmax(voicePeak, std::fabs(in[i]));
  const bool voiced = peak > 0.8f * voicePeak && cutAt < static_cast<uint32_t>(400 * kMs);
  const int skippedAfter = t.skipped[static_cast<size_t>(w)] - t.skipped[static_cast<size_t>(400 * kMs)];
  return report("tail-after-stop", voiced && skippedAfter >= 1 && seq == static_cast<uint32_t>(t.taken[static_cast<size_t>(w)]),
                "the voice stops at 400 ms with the key open to 450 ms: %.0f silent cut(s) skipped, the tail repeats the slice cut at %.1f ms (voiced), peak %.2f of the voice's %.2f",
                skippedAfter, cutAt / double(kMs), peak, voicePeak);
}

// What the clip rows count: runs, tails, slice changes after a word's end, tails that are not the
// newest cut, and how far the tail slice's peak sits under the loudest sample the gate passed.
struct ClipTotals {
  int rows = 0, tails = 0, changes = 0, notNewest = 0, withTail = 0;
  double worstDb = 0.0, bestDb = -200.0;
};

// One run's view of the tail: its slice and splice when it began, the slices of the fading voices.
struct TailWatch {
  uint32_t seq = 0, splice = 0;
  std::array<uint32_t, 2> fadeSeq = {0, 0};
  float wordPeak = 0.0f;
  bool wasWord = false;
  size_t wordStart = 0, pos = 0;  // where the word began and where the run stands, in samples
};

// The tail slice's peak against the loudest sample the gate passed during the word. A word held past
// 2.3 s is left out: its loop can take a quieter cut to beat the 2.5 s age limit. So is a word under
// 150 ms, a breath or a click whose loudest sample the cut can miss.
void notePeak(const TailWatch& w, ClipTotals& c) {
  const int slot = slotPlayedBy(kTail);
  if (slot < 0 || w.wordPeak <= 0.0f || slicePeak(slot) <= 0.0f) return;
  if (w.pos - w.wordStart > static_cast<size_t>(cv::kSampleRate) * 23 / 10) return;
  if (w.pos - w.wordStart < static_cast<size_t>(cv::kSampleRate) * 3 / 20) return;
  const double db = 20.0 * std::log10(slicePeak(slot) / w.wordPeak);
  c.worstDb = std::fmin(c.worstDb, db);
  c.bestDb = std::fmax(c.bestDb, db);
  ++c.withTail;
}

// A tail starts when the word's voice moves over, and from then on its slice must not change.
void watchTail(TailWatch& w, ClipTotals& c) {
  if (gBloom.voiceOn(kTail)) {
    if (w.seq == 0) {
      w.seq = gBloom.playingSeq(kTail);
      w.splice = gBloom.spliceAt(kTail);
      ++c.tails;
      if (w.seq != static_cast<uint32_t>(gBloom.taken())) ++c.notNewest;
      notePeak(w, c);
    } else if (gBloom.playingSeq(kTail) != w.seq || gBloom.spliceAt(kTail) != w.splice) {
      ++c.changes;
    }
  } else {
    w.seq = 0;
  }
  for (int k = 0; k < 2; ++k) {
    const uint32_t seq = gBloom.voiceOn(kFade + k) ? gBloom.playingSeq(kFade + k) : 0u;
    uint32_t& before = w.fadeSeq[static_cast<size_t>(k)];
    if (seq != 0 && before != 0 && seq != before) ++c.changes;
    before = seq;
  }
}

void runClipRow(const Signal& in, float lengthMs, float decayMs, ClipTotals& c) {
  GatedChatter g(-55.0f, decayMs, 0.5f, lengthMs);
  Signal gated(in.size()), out(in.size());
  TailWatch w;
  ++c.rows;
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    const int n = static_cast<int>(std::min<size_t>(cv::kBlock, in.size() - i));
    g.run(&in[i], &gated[i], &out[i], n);
    for (int j = 0; j < n; ++j) w.wordPeak = std::fmax(w.wordPeak, std::fabs(gated[i + static_cast<size_t>(j)]));
    if (gBloom.inWord() && !w.wasWord) {
      w.wordPeak = 0.0f;
      w.wordStart = i;
    }
    if (!gBloom.inWord() && w.wasWord) w.seq = 0;  // a word's end starts its own tail, whatever tail it overtakes
    w.wasWord = gBloom.inWord();
    w.pos = i;
    watchTail(w, c);
  }
}

// Adam's clips through the Input gate at -55 dBFS with Decay 20 ms and 2 s, the gate's detector as the
// word key and its output as the source, as the box runs them with no pitch card, at LENGTH 20 ms,
// 60 ms, 300 ms, 2 s and 3 s. Between a word's end and its tail's close the tail plays one slice and
// never changes it, and the one it plays is the newest cut, which carries the voice and not the gate's
// silence after it, within 20 dB of the loudest sample of a word shorter than 2.3 s. The same holds
// for a tail that fades under the next word.
bool testTailOnClips() {
  const std::vector<Signal> clips = loadClips(static_cast<size_t>(cv::kSampleRate) * 6);
  if (clips.empty()) return skipNoClips("tail-on-clips");
  const float lengths[5] = {20.0f, 60.0f, 300.0f, 2000.0f, 3000.0f};
  const float decays[2] = {20.0f, 2000.0f};
  ClipTotals c;
  for (const Signal& in : clips)
    for (const float length : lengths)
      for (const float decay : decays) runClipRow(in, length, decay, c);
  return report("tail-on-clips", c.changes == 0 && c.notNewest == 0 && c.tails >= c.rows && c.worstDb > -20.0,
                "%.0f rows (3 clips x LENGTH 20/60/300/2000/3000 ms x Decay 20/2000 ms), %.0f tails: slice changes after a word's end %.0f (0), tails that are not the newest cut %.0f (0), the tail slice's peak runs %.1f dB at the lowest against the loudest gate sample of its word, words under 2.3 s (> -20)",
                c.rows, c.tails, c.changes, c.notNewest, c.worstDb);
}

// The part of the tail's slice that was cut after the voice stopped, for a vowel that stops at `stopMs`
// while the Input gate's key rides out its hold. -1 when the word left no tail.
double straddleGap(float amount, int stopMs) {
  const Signal in = vowel(1200, 0, stopMs);
  GatedChatter g(-55.0f, cv::inputGateTuning().releaseMs, amount, 300.0f);
  Signal gated(in.size()), out(in.size());
  bool wasWord = false;
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    const int n = static_cast<int>(std::min<size_t>(cv::kBlock, in.size() - i));
    g.run(&in[i], &gated[i], &out[i], n);
    if (wasWord && !gBloom.inWord()) {
      if (!gBloom.voiceOn(kTail)) return -1.0;
      const double len = gBloom.playingLen(kTail);
      const double end = double(gBloom.playingCutAt(kTail)) + len;
      return std::fmax(0.0, (end - double(stopMs) * kMs) / len);
    }
    wasWord = gBloom.inWord();
  }
  return -1.0;
}

// A vowel stops at each of 120 times, 300 to 419 ms, while the Input gate's key rides out its hold.
// The cut that straddles the stop has a gap, so the tail repeats the last full slice: none of the 120
// tails has over 15 % of its slice cut after the voice stopped, at four AMOUNT settings.
bool testStraddle() {
  const float amounts[4] = {0.25f, 0.5f, 0.75f, 1.0f};
  int over[4] = {0, 0, 0, 0}, tails = 0;
  double worst = 0.0;
  for (int j = 0; j < 4; ++j) {
    for (int stopMs = 300; stopMs < 420; ++stopMs) {
      const double gap = straddleGap(amounts[j], stopMs);
      if (gap < 0.0) continue;
      ++tails;
      over[j] += gap > 0.15 ? 1 : 0;
      worst = std::fmax(worst, gap);
    }
  }
  return report("straddle", tails == 480 && over[0] + over[1] + over[2] + over[3] == 0,
                "tails with over 15%% of the slice cut after the voice stopped, of 120 at AMOUNT 25/50/75/100 %%: %.0f / %.0f / %.0f / %.0f (0), worst %.1f%% (12.5)",
                over[0], over[1], over[2], over[3], worst * 100.0);
}

// Tails counted over the clips at one Input gate setting, and the ones with over 15 % and 35 % of
// their slice's 2 ms windows silent.
struct GapCount {
  int tails = 0, over15 = 0, over35 = 0;
};

void countClipGaps(const Signal& in, float thresholdDb, float amount, GapCount& c) {
  GatedChatter g(thresholdDb, 20.0f, amount, 300.0f);
  Signal gated(in.size()), out(in.size());
  bool wasWord = false;
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    const int n = static_cast<int>(std::min<size_t>(cv::kBlock, in.size() - i));
    g.run(&in[i], &gated[i], &out[i], n);
    if (wasWord && !gBloom.inWord() && slotPlayedBy(kTail) >= 0) {
      const double silent = silentShare(slotPlayedBy(kTail));
      ++c.tails;
      c.over15 += silent > 0.15 ? 1 : 0;
      c.over35 += silent > 0.35 ? 1 : 0;
    }
    wasWord = gBloom.inWord();
  }
}

// Adam's clips through the Input gate at -55, -45 and -35 dBFS, at AMOUNT 50 % and 100 %. The tails
// that repeat a slice with a long silent stretch were 21 of 60 before the gap rule, up to 73 % silent.
// Now no tail has over 35 %, and each row has at most one over 15 %, where the silent share is
// counted in 2 ms windows 20 dB under the slice's peak (the rule itself works at 12 dB on the
// slice's first and last eighth).
bool testClipGaps() {
  const std::vector<Signal> clips = loadClips(static_cast<size_t>(cv::kSampleRate));
  if (clips.empty()) return skipNoClips("clip-gaps");
  const float thresholds[3] = {-55.0f, -45.0f, -35.0f};
  const float amounts[2] = {0.5f, 1.0f};
  bool ok = true;
  int tails = 0, over15 = 0, over35 = 0;
  for (const float thr : thresholds) {
    for (const float amount : amounts) {
      GapCount row;
      for (const Signal& in : clips) countClipGaps(in, thr, amount, row);
      ok = ok && row.over35 == 0 && row.over15 <= 1;
      tails += row.tails;
      over15 += row.over15;
      over35 += row.over35;
    }
  }
  return report("clip-gaps", ok && tails >= 40, "%.0f tails over 6 rows (3 gate thresholds x AMOUNT 50/100 %%, >= 40): %.0f with over 15%% silent windows (<= 1 a row), %.0f over 35%% (0)",
                tails, over15, over35);
}

// The Chatter card after a pitch stage whose output lags the key by 60 ms (the stage lags 23 to 36 ms;
// 60 ms puts the voice's start inside the first cut's first eighth). That cut starts before the voice
// does, so it has a gap and the word cuts again once: the first loop plays a cut that starts after the
// voice, about one interval later than with no lag.
bool testLeadingEdge() {
  const int ms = 800;
  const Signal in = vowel(ms, 0, 500);
  const Signal lagged = delayed(in, 60);
  cv::BloomParams p;
  gBloom.reset();
  const Trace flat = trace(gBloom, in, keyOver(ms, 0, 500), p, &in);
  gBloom.reset();
  const Trace lag = trace(gBloom, in, keyOver(ms, 0, 500), p, &lagged);
  const int fFlat = firstPlay(flat, kWord, 0), fLag = firstPlay(lag, kWord, 0);
  if (fFlat < 0 || fLag < 0) return report("leading-edge", false, "a word played no slice (%.0f)", 0.0);
  const double delayMs = (fLag - fFlat) / double(kMs);
  const uint32_t cutAt = lag.cut[kWord][static_cast<size_t>(fLag)];
  const bool ok = flat.skipped[static_cast<size_t>(fFlat)] == 0 && lag.skipped[static_cast<size_t>(fLag)] == 1 && cutAt >= static_cast<uint32_t>(100 * kMs) &&
                  flat.cut[kWord][static_cast<size_t>(fFlat)] < static_cast<uint32_t>(60 * kMs) && delayMs > 50.0 && delayMs < 100.0;
  return report("leading-edge", ok,
                "voice lagged 60 ms: the first cut is skipped once (1), the first loop plays a cut that starts at %.1f ms (after the voice) and sounds %.1f ms later than with no lag (50..100)",
                cutAt / double(kMs), delayMs);
}

// Bursts of 20 ms every 100 ms leave a gap in every cut, because no cut holds the voice in both its
// first and its last eighth. The word's first cut is cut again once and the second is kept despite
// its gap, so the first loop plays after exactly two cuts; the cuts after it are skipped.
bool testRearmOnce() {
  const int ms = 1200;
  const Signal in = bursts(vowel(ms, 0, 1100), 70, 20, 100);
  cv::BloomParams p;
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 1000), p);
  const int first = firstPlay(t, kWord, 0);
  if (first < 0) return report("rearm-once", false, "no slice played (%.0f)", 0.0);
  int armsBefore = 0;
  for (int a : t.arms) armsBefore += a <= first ? 1 : 0;
  const size_t at = static_cast<size_t>(first);
  const bool ok = armsBefore == 2 && t.taken[at] == 1 && t.skipped[at] == 1 && t.taken.back() == 1 && t.skipped.back() >= 4 &&
                  first > 175 * kMs && first < 195 * kMs;
  return report("rearm-once", ok,
                "gappy cuts: %.0f cuts before the first loop (2), which plays at %.1f ms (175..195); later cuts all skipped: %.0f taken in all (1), %.0f skipped (>= 4)",
                armsBefore, first / double(kMs), t.taken.back(), t.skipped.back());
}

// A new word starts while a tail sounds, 1,000 ms in.
struct NextWordRun {
  Trace ref, t;
  int s = 1000 * kMs;  // the key returns
  int first = -1;      // the new word plays its first slice
};

NextWordRun runNextWord() {
  const int ms = 2600;
  const Signal in = vowel(ms, 0, 2400);
  cv::BloomParams p;
  p.amount = 0.5f;
  NextWordRun r;
  gBloom.reset();
  r.ref = trace(gBloom, in, keyOver(ms, 0, 600), p);
  gBloom.reset();
  r.t = trace(gBloom, in, keyOver(ms, {{0, 600}, {1000, 1700}}), p);
  r.first = firstPlay(r.t, kWord, r.s);
  return r;
}

// Until the new word plays a slice the tail runs as if no key had come, sample for sample, and the first
// slice plays after the first capture's 40 ms, its crossing wait of 10 ms and the cut's length.
bool tailWaitsForSlice(const NextWordRun& r) {
  if (r.first < 0) return false;
  bool same = true;
  for (int i = 0; i < r.first; ++i) {
    const size_t k = static_cast<size_t>(i);
    same = same && r.t.out[k] == r.ref.out[k];
    if (i >= r.s) same = same && r.t.on[kTail][k] && !r.t.on[kFade][k] && r.t.gain[kTail][k] == r.ref.gain[kTail][k];
  }
  const int len = r.t.len[kWord][static_cast<size_t>(r.first)];
  return same && r.first == r.s + 50 * kMs + len;
}

// At the first slice the tail moves to a fading voice and falls to 0 in a straight line over 150 ms.
// Returns the fall time in samples, or -1 when the fall is not straight.
int straightFall(const NextWordRun& r) {
  const size_t f = static_cast<size_t>(r.first);
  if (r.t.on[kTail][f] || !r.t.on[kFade][f] || !r.t.on[kWord][f]) return -1;
  int zero = -1;
  for (int i = r.first + 2; i < r.first + 400 * kMs; ++i)
    if (r.t.gain[kFade][static_cast<size_t>(i)] == 0.0f) {
      zero = i - r.first;
      break;
    }
  const double step = r.t.gain[kFade][f] - r.t.gain[kFade][f + 1];
  if (zero <= 0 || step <= 0.0) return -1;
  for (int i = r.first; i < r.first + zero - 3; ++i)
    if (std::fabs((r.t.gain[kFade][static_cast<size_t>(i)] - r.t.gain[kFade][static_cast<size_t>(i + 1)]) - step) > 1e-6) return -1;
  return zero;
}

// The new word climbs in 5 ms, cuts its first slice 40 ms in, and every slice it plays was cut after the key.
bool wordStartsClean(const NextWordRun& r) {
  const Trace& t = r.t;
  const size_t s = static_cast<size_t>(r.s);
  bool climbs = true;
  for (size_t i = s + 1; i < s + 240; ++i)
    if (t.gain[kWord][i] < t.gain[kWord][i - 1]) climbs = false;
  const bool reaches = t.gain[kWord][s + 245] > 0.99 * wetAt(0.5);
  const int arm = firstAtOrAfter(t.arms, r.s);
  const int write = firstAtOrAfter(t.writes, r.s);
  bool fresh = true;
  for (size_t i = s; i < t.cut[kWord].size(); ++i)
    if (t.seq[kWord][i] != 0 && t.cut[kWord][i] < static_cast<uint32_t>(r.s)) fresh = false;
  return climbs && reaches && fresh && arm == r.s + 40 * kMs && write >= r.s + 40 * kMs && write <= r.s + 50 * kMs;
}

// Both sound together for less than the fade, 3 dB down while they do (the 20 ms smoother takes most
// of that), and the pair scale is 1 before the first slice and back at 1 afterwards.
bool pairDuringOverlap(const NextWordRun& r, double& togetherMs) {
  int together = 0;
  for (int i = r.s; i < r.s + 400 * kMs; ++i) {
    const size_t k = static_cast<size_t>(i);
    if (r.t.gain[kFade][k] > 0.0f && r.t.gain[kWord][k] > 0.0f && r.t.seq[kWord][k] != 0 && r.t.seq[kFade][k] != 0) ++together;
  }
  togetherMs = together / double(kMs);
  const double pairWant = std::pow(10.0, -3.0 / 20.0);
  const bool down = r.t.pair[static_cast<size_t>(r.first + 140 * kMs)] < 1.0 - 0.7 * (1.0 - pairWant);
  const bool one = r.t.pair[static_cast<size_t>(r.first - 1)] == 1.0f && r.t.pair[static_cast<size_t>(r.s + 600 * kMs)] == 1.0f;
  return down && one && together > 0 && together <= 150 * kMs;
}

// A new word starts while a tail sounds. The tail stays where it is, and the word's output is the tail's
// alone, until the word plays its first slice (a key that never gives one leaves the tail alone). Then the tail
// falls to 0 in a straight line over 150 ms, both play 3 dB down while they sound together, and the two never
// overlap longer than the fade.
bool testNextWord() {
  const NextWordRun r = runNextWord();
  const bool waits = tailWaitsForSlice(r);
  const int fall = waits ? straightFall(r) : -1;
  double togetherMs = 0.0;
  const bool pairs = waits && pairDuringOverlap(r, togetherMs);
  const bool fadeTime = fall >= 150 * kMs - 4 && fall <= 150 * kMs + 4;
  return report("next-word", waits && fall > 0 && fadeTime && wordStartsClean(r) && pairs,
                "the tail runs on untouched until the word's first slice at %.2f ms after the key; it reaches 0 after %.2f ms in a straight line (150); both sound %.1f ms (<=150)",
                (r.first - r.s) / double(kMs), fall / double(kMs), togetherMs);
}

// A word that plays its first slice while an earlier tail still fades gives that fade the 20 ms yield,
// and the tail it finds takes the 150 ms fade.
bool testYield() {
  const int ms = 2000;
  const Signal in = vowel(ms, 0, 1900);
  cv::BloomParams p;
  p.lengthMs = 2000.0f;
  p.amount = 1.0f;

  // Word one ends at 400 ms. Word two opens at 600 ms and plays its first slice at 695 ms, so word one's tail
  // fades from there until 845 ms; the word ends at 700 ms. Word three opens at 705 ms and plays its first slice
  // at 800 ms, 45 ms before the first fade would have ended.
  const std::vector<std::pair<int, int>> runs = {{0, 350}, {600, 650}, {705, 1500}};
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, runs), p);
  const int s2 = 600 * kMs, s3 = 705 * kMs;
  const int p3 = firstPlay(t, kWord, s3);
  const bool tail1 = t.on[kTail][static_cast<size_t>(s2 - 1)];
  const bool secondTail = t.on[kTail][static_cast<size_t>(s3 - 1)] && t.on[kFade][static_cast<size_t>(s3 - 1)];
  if (p3 < 0) return report("yield", false, "word three played no slice (%.0f)", 0.0);

  // The first fade would end 150 ms after 695 ms; at 800 ms it has 45 ms left and is cut to 20 ms.
  int zeroA = -1, zeroB = -1;
  for (int i = p3; i < p3 + 400 * kMs; ++i) {
    if (zeroA < 0 && t.gain[kFade][static_cast<size_t>(i)] == 0.0f) zeroA = i - p3;
    if (zeroB < 0 && t.gain[kFade + 1][static_cast<size_t>(i)] == 0.0f && i > p3 + 2) zeroB = i - p3;
  }
  const bool yielded = zeroA >= 20 * kMs - 4 && zeroA <= 20 * kMs + 4;
  const bool newFade = zeroB >= 150 * kMs - 4 && zeroB <= 150 * kMs + 4;
  return report("yield", tail1 && secondTail && yielded && newFade,
                "an older fade yields to 0 in %.2f ms (20) and the newer tail fades in %.2f ms (150); tail and fade both sound when word three opens: %.0f",
                zeroA / double(kMs), zeroB / double(kMs), secondTail ? 1.0 : 0.0);
}

// A 3 s tail and a key blip that never gives a slice, 40 ms of key with a click under it. The word the
// blip opens ends at its hold with no slice, so the tail runs as if the blip had not come: the voice's
// gain and slice match the run without it in every sample, no fading voice starts, and the loop ends in
// the same sample.
bool testKeyBlip() {
  const int ms = 6500;
  const Signal quiet = vowel(ms, 0, 400);
  Signal blipIn = quiet;
  for (int i = 1000 * kMs; i < 1040 * kMs; ++i) blipIn[static_cast<size_t>(i)] = 0.1f * std::sin(0.3f * static_cast<float>(i));
  cv::BloomParams p;
  p.lengthMs = 3000.0f;
  gBloom.reset();
  const Trace plain = trace(gBloom, quiet, keyOver(ms, 0, 400), p);
  gBloom.reset();
  const Trace blip = trace(gBloom, blipIn, keyOver(ms, {{0, 400}, {1000, 1040}}), p);
  bool same = plain.gain[kTail] == blip.gain[kTail] && plain.seq[kTail] == blip.seq[kTail];
  bool noFade = true, wordRan = false;
  for (size_t i = 0; i < blip.on[kFade].size(); ++i) {
    noFade = noFade && !blip.on[kFade][i];
    wordRan = wordRan || (i >= static_cast<size_t>(1000 * kMs) && blip.word[i]);
  }
  auto endOf = [](const Trace& t) {
    for (size_t i = static_cast<size_t>(500 * kMs); i < t.on[kTail].size(); ++i)
      if (!t.on[kTail][i]) return static_cast<int>(i);
    return -1;
  };
  same = same && endOf(plain) > 0 && endOf(plain) == endOf(blip) && blip.taken.back() == plain.taken.back();
  return report("key-blip", same && noFade && wordRan,
                "a 40 ms key blip at 1 s opens a word (%.0f) that plays nothing: the 3 s tail matches the run without it sample for sample (%.0f), no fading voice starts (%.0f), the loop ends at %.0f ms",
                wordRan ? 1.0 : 0.0, same ? 1.0 : 0.0, noFade ? 1.0 : 0.0, endOf(blip) / double(kMs));
}

// How many words a key gap of `gapMs` leaves, and the largest wet step in 350..650 ms. The input is silent
// and the slices come from `src`, so the output is the wet path alone.
struct HoldRun {
  int words = 0;
  double step = 0.0;
};

HoldRun runHoldGap(int gapMs) {
  const int ms = 2600;
  const Signal src = vowel(ms, 0, 1400);
  const Signal zero(src.size(), 0.0f);
  cv::BloomParams p;
  gBloom.reset();
  const Trace t = trace(gBloom, zero, keyOver(ms, {{0, 400}, {400 + gapMs, 1400}}), p, &src);
  HoldRun h;
  for (size_t i = 1; i < t.word.size(); ++i) h.words += t.word[i] && !t.word[i - 1] ? 1 : 0;
  h.words += t.word[0] ? 1 : 0;
  for (size_t i = static_cast<size_t>(350 * kMs); i < static_cast<size_t>(650 * kMs); ++i)
    h.step = std::fmax(h.step, std::fabs(static_cast<double>(t.out[i]) - t.out[i - 1]));
  return h;
}

// The key holds 50 ms (2,400 samples) after it falls. A gap of 49 ms or less is one word; 50 ms or more
// ends the word and starts another, with the tail sounding between them. At every gap the largest wet
// step stays under 0.03, where a tail that dropped out of the mix for a block steps 0.05 or more.
bool testKeyHold() {
  const int gaps[10] = {10, 30, 45, 49, 50, 52, 55, 60, 80, 120};
  bool ok = true;
  double worst = 0.0;
  int wordsAt49 = 0, wordsAt50 = 0;
  for (const int g : gaps) {
    const HoldRun h = runHoldGap(g);
    ok = ok && h.words == (g < 50 ? 1 : 2) && h.step < 0.03;
    worst = std::fmax(worst, h.step);
    if (g == 49) wordsAt49 = h.words;
    if (g == 50) wordsAt50 = h.words;
  }
  return report("key-hold", ok, "key gaps of 10..120 ms: words at 49 ms %.0f (1), at 50 ms %.0f (2); largest wet step %.4f (< 0.03)", wordsAt49, wordsAt50, worst);
}

// The wet path of a finished word's tail while AMOUNT and LENGTH move every 40 ms between (0.3, 1.5 s)
// and (1.0, 3 s) on call boundaries of 64 samples, or held at the second setting. Returns the largest
// sample step from 450 ms to 2.5 s. The input is silent and the slices come from `src`.
double tailStep(bool move) {
  const int ms = 3000;
  const Signal src = vowel(ms, 0, 400);
  const Signal zero(src.size(), 0.0f);
  Signal out(src.size());
  cv::BloomParams p;
  p.keyFromGate = true;
  p.amount = 1.0f;
  p.lengthMs = 3000.0f;
  gBloom.reset();
  for (size_t i = 0; i < src.size(); i += cv::kBlock) {
    if (move) {
      const bool high = (i / (40 * kMs) % 2) == 0;
      p.amount = high ? 1.0f : 0.3f;
      p.lengthMs = high ? 3000.0f : 1500.0f;
    }
    gBloom.process(&zero[i], &src[i], &out[i], cv::kBlock, p, i < static_cast<size_t>(400 * kMs));
  }
  double step = 0.0;
  for (size_t i = static_cast<size_t>(450 * kMs); i < static_cast<size_t>(2500 * kMs); ++i)
    step = std::fmax(step, std::fabs(static_cast<double>(out[i]) - out[i - 1]));
  return step;
}

// Moving AMOUNT and LENGTH through a tail every 40 ms adds no click: its largest sample step stays
// within 10 % of the largest step of the same tail at the louder setting held still.
bool testKnobMoves() {
  const double held = tailStep(false), moved = tailStep(true);
  return report("knob-moves", moved <= 1.1 * held, "largest wet step with AMOUNT and LENGTH moving every 40 ms %.4f, held at the louder setting %.4f (<= 1.1x)", moved, held);
}

// Slices come from `src`; the dry path is `in`. Returns how many finished slices were checked, and
// whether each equals the source through the 11 kHz low-pass and the pass window, starting clear at its
// cut clock. The word runs to a dozen points and the slices that hold a finished cut are read.
int slicesMatchSource(const Signal& in, const Signal& src, bool& match) {
  cv::BloomParams q;
  q.keyFromGate = true;
  Signal y(in.size());
  int checked = 0;
  for (int stopMs = 400; stopMs < 700; stopMs += 25) {
    gBloom.reset();
    for (size_t i = 0; i < static_cast<size_t>(stopMs) * kMs; ++i) gBloom.process(&in[i], &src[i], &y[i], 1, q, true);
    for (int slot = 0; slot < cv::Bloom::kSlices; ++slot) {
      if (!gBloom.sliceValid(slot)) continue;
      ++checked;
      const uint32_t cut = gBloom.sliceCutAt(slot);
      cv::Biquad lp;
      lp.setLowPass(cv::Bloom::kLowpassHz, cv::Bloom::kLowpassQ);
      const int len = gBloom.sliceLen(slot);
      for (int j = 0; j < len; ++j) {
        const double want = lp.run(src[cut + static_cast<uint32_t>(j)]) * passWindowAt(j, len);
        if (std::fabs(gBloom.sliceData(slot)[j] - want) > 1e-5) match = false;
      }
    }
  }
  return checked;
}

bool testSource() {
  const int ms = 1200;
  const Signal in = vowel(ms, 0, 1100);
  const Signal src = sine(ms, {{0, 1100, 0.4f}}, 330.0);
  const std::vector<char> key = keyOver(ms, 0, 1000);
  cv::BloomParams p;
  gBloom.reset();
  const Trace split = trace(gBloom, in, key, p, &src);
  bool match = true;
  const int checked = slicesMatchSource(in, src, match);

  // Before the first slice plays the output is the dry input, whatever `src` is.
  gBloom.reset();
  const Trace same = trace(gBloom, in, key, p);
  bool dry = true;
  for (int i = 0; i < 100 * kMs; ++i)
    if (split.out[static_cast<size_t>(i)] != same.out[static_cast<size_t>(i)]) dry = false;
  double d = 0.0;
  for (size_t i = 300 * kMs; i < 900 * kMs; ++i) d = std::fmax(d, std::fabs(split.out[i] - same.out[i]));

  // A call with src == in is the call without it.
  p.keyFromGate = true;
  gBloom.reset();
  Signal a(in.size()), b(in.size());
  gBloom.process(in.data(), a.data(), static_cast<int>(in.size()), p, true);
  gBloom.reset();
  gBloom.process(in.data(), in.data(), b.data(), static_cast<int>(in.size()), p, true);
  const bool equalCall = a == b;
  return report("source", match && checked >= 12 && dry && d > 0.05 && equalCall,
                "%.0f finished slices equal the source at their cut clock: %.0f; output before the first loop is the dry input's, and the loop follows the source: %.0f; src == in equals the short call: %.0f",
                checked, match ? 1.0 : 0.0, dry && d > 0.05 ? 1.0 : 0.0, equalCall ? 1.0 : 0.0);
}

// A loud slice with a quieter tone held after it, which skips every later slice. Returns the oldest age
// any slice played at, whether the loud slice still loops at 2 s, and where it was replaced.
struct HeldTone {
  uint32_t worstAge = 0;
  bool stillLoud = false, replaced = false;
  int changeAt = -1;
};

HeldTone runHeldTone(uint32_t limit) {
  const int ms = 4500;
  const float quiet = 0.5f * std::pow(10.0f, -20.0f / 20.0f);
  const Signal in = sine(ms, {{0, 300, 0.5f}, {300, 4400, quiet}});
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 4400), cv::BloomParams{});
  HeldTone h;
  for (size_t i = 0; i < in.size(); ++i)
    if (t.seq[kWord][i] != 0) h.worstAge = std::max<uint32_t>(h.worstAge, static_cast<uint32_t>(i) - t.cut[kWord][i]);
  const uint32_t loud = t.seq[kWord][static_cast<size_t>(1000 * kMs)];
  h.stillLoud = loud != 0 && t.seq[kWord][static_cast<size_t>(2000 * kMs)] == loud;
  for (size_t i = static_cast<size_t>(1000 * kMs); i < in.size(); ++i)
    if (t.seq[kWord][i] != loud) {
      h.changeAt = static_cast<int>(i);
      break;
    }
  const uint32_t loudCut = t.cut[kWord][static_cast<size_t>(1000 * kMs)];
  h.replaced = h.changeAt > 0 && static_cast<uint32_t>(h.changeAt) < loudCut + limit && static_cast<uint32_t>(h.changeAt) > loudCut + limit - 400 * kMs &&
               t.seq[kWord][static_cast<size_t>(3500 * kMs)] > loud && t.on[kWord][static_cast<size_t>(3500 * kMs)];
  return h;
}

// A tail at LENGTH 2 s or 3 s: its slice plays past the limit, the gain follows the law at the clock
// where the limit would have closed it (the slice's cut plus 2.5 s less the 80 ms close), and the
// tail closes at the law's end, 3.58 s and 5.19 s after the word.
struct LongTail {
  bool ok = true;
  double endMs = 0.0, ageMs = 0.0;
};

LongTail runLongTail(double lengthMs, uint32_t limit) {
  LongTail r;
  const int span = 7000;
  const Signal in = vowel(span, 0, 900);
  cv::BloomParams q;
  q.lengthMs = static_cast<float>(lengthMs);
  gBloom.reset();
  const Trace u = trace(gBloom, in, keyOver(span, 0, 800), q);
  const int w = fallAfter(u.word, 0);
  if (w < 0) {
    r.ok = false;
    return r;
  }
  int end = -1;
  for (size_t i = static_cast<size_t>(w); i < u.gain[kTail].size(); ++i)
    if (u.gain[kTail][i] == 0.0f && !u.on[kTail][i]) {
      end = static_cast<int>(i);
      break;
    }
  uint32_t tailAge = 0;
  for (size_t i = static_cast<size_t>(w); end > 0 && i < static_cast<size_t>(end); ++i)
    if (u.seq[kTail][i] != 0) tailAge = std::max<uint32_t>(tailAge, static_cast<uint32_t>(i) - u.cut[kTail][i]);
  const size_t at = u.cut[kTail][static_cast<size_t>(w)] + limit - 80 * kMs;
  const double want = wetAt(0.5) * std::exp(-(static_cast<double>(at) - w) / (tauMs(lengthMs) * kMs));
  const double lawRel = at < u.gain[kTail].size() ? std::fabs(u.gain[kTail][at] - want) / want : 1.0;
  r.endMs = end < 0 ? -1.0 : (end - w) / double(kMs);
  r.ageMs = tailAge / double(kMs);
  const double lawEnd = lawEndMs(lengthMs, wetAt(0.5));
  r.ok = end >= 0 && tailAge > limit && lawRel <= 0.015 && std::fabs(r.endMs - lawEnd) <= endTolMs(lawEnd);
  return r;
}

// During a word no slice plays more than 2.5 s after it was cut. A loud slice with a quieter tone held
// after it loops while every later slice is skipped, and is replaced before the limit. A tail has no
// limit: at LENGTH 2 s and 3 s its one slice plays past 2.5 s of age and the tail runs the law's whole
// fade.
bool testAgeLimit() {
  const uint32_t limit = static_cast<uint32_t>(2500 * kMs);
  const HeldTone h = runHeldTone(limit);
  const LongTail a = runLongTail(2000.0, limit), b = runLongTail(3000.0, limit);
  return report("age-limit", h.worstAge <= limit && h.stillLoud && h.replaced && a.ok && b.ok,
                "held quieter tone: oldest slice played %.0f ms at most (<= 2500), loud slice still loops at 2 s and is replaced at %.0f ms; tails run past the limit, LENGTH 2 s closes %.0f ms after the word (law 3581) and 3 s closes %.0f ms (law 5190), the 3 s tail's slice played %.0f ms",
                h.worstAge / double(kMs), h.changeAt / double(kMs), a.endMs, b.endMs, b.ageMs);
}

// The loop plays under the word: its gain reaches the wet level within 5 ms of the first slice, and
// the wet gain at the word's end carries on into the tail at the law's gain.
bool testLoopUnderWord() {
  const int ms = 1400;
  const Signal in = vowel(ms, 0, 500);
  gBloom.reset();
  const Trace t = trace(gBloom, in, keyOver(ms, 0, 500), cv::BloomParams{});
  const int w = fallAfter(t.word, 0);
  const int first = firstPlay(t, kWord, 0);
  if (w < 0 || first < 0 || first >= w) return report("loop-under-word", false, "no slice played under the word (%.0f)", 0.0);
  const double wet = wetAt(0.5);
  const double lengthMs = cv::BloomParams{}.lengthMs;
  const bool loud = t.total[static_cast<size_t>(first + 5 * kMs)] > 0.99 * wet;
  const bool carries = t.total[static_cast<size_t>(w + 245)] > 0.95 * wet * std::exp(-245.0 / (lengthMs * kMs)) && t.loop[static_cast<size_t>(w)];
  return report("loop-under-word", loud && carries, "first slice at %.1f ms, gain at its 5 ms %.3f (wet %.3f), gain 5 ms after the word %.3f",
                first / double(kMs), t.total[static_cast<size_t>(first + 5 * kMs)], wet, t.total[static_cast<size_t>(w + 245)]);
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

// The dry passes at unity at every AMOUNT, and the trim moves the wet alone. With silent slices a
// word's output is the input bit for bit at AMOUNT 25, 50 and 100 %. With real slices, 6 dB less trim
// halves what the output adds to the input and leaves the input in it untouched.
bool testDryUnity() {
  const int ms = 1500;
  const Signal in = vowel(ms, 0, 1200);
  const Signal silent(in.size(), 0.0f);
  bool dryExact = true;
  for (const float amount : {0.25f, 0.5f, 1.0f}) {
    cv::BloomParams p;
    p.amount = amount;
    gBloom.reset();
    const Trace t = trace(gBloom, in, keyOver(ms, 0, 1200), p, &silent);
    dryExact = dryExact && t.out == in;
  }
  cv::BloomParams loud, quiet;
  quiet.tuning.trimDb = loud.tuning.trimDb - 6.0f;
  gBloom.reset();
  const Trace a = trace(gBloom, in, keyOver(ms, 0, 1200), loud);
  gBloom.reset();
  const Trace b = trace(gBloom, in, keyOver(ms, 0, 1200), quiet);
  const double half = std::pow(10.0, -6.0 / 20.0);
  double worst = 0.0, wet = 0.0;
  for (size_t i = 0; i < in.size(); ++i) {
    const double wa = static_cast<double>(a.out[i]) - in[i], wb = static_cast<double>(b.out[i]) - in[i];
    worst = std::fmax(worst, std::fabs(wb - half * wa));
    wet = std::fmax(wet, std::fabs(wa));
  }
  return report("dry-unity", dryExact && wet > 0.05 && worst < 1e-6,
                "silent slices leave the input bit for bit at AMOUNT 25/50/100 %%: %.0f; 6 dB less trim halves the wet to within %.2g (< 1e-6), wet peak %.3f",
                dryExact ? 1.0 : 0.0, worst, wet);
}

bool sameBytes(const Signal& a, const Signal& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }

// Two renders from reset are byte-identical, also through a tail fading under a new word, and with a
// constant key the call size does not matter.
bool testDeterminism() {
  const int ms = 4000;
  const Signal in = vowel(ms, 100, 3500);
  const std::vector<char> key = keyOver(ms, {{100, 700}, {1200, 1800}, {2300, 2900}});
  cv::BloomParams p;
  p.lengthMs = 1000.0f;
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
    Signal out(in.size());
    for (size_t i = 0; i < in.size(); i += static_cast<size_t>(c)) {
      const int n = static_cast<int>(in.size() - i < static_cast<size_t>(c) ? in.size() - i : static_cast<size_t>(c));
      gBloom.process(&in[i], &out[i], n, q, true);
    }
    if (ref.empty()) ref = out;
    else if (!sameBytes(ref, out)) chunked = false;
  }
  return report("determinism", sameBytes(a.out, b.out) && chunked, "two renders byte-identical: %.0f, calls of 1/7/64/1000 samples byte-identical: %.0f",
                sameBytes(a.out, b.out) ? 1.0 : 0.0, chunked ? 1.0 : 0.0);
}

// Renders `x` (280,000 samples) in calls of `call` samples with the gate key open from 56,000 to
// 224,000, AMOUNT up at 112,000 and LENGTH down at 168,000. Every change falls on a multiple of 56,000,
// which each call size divides, so it falls on a call boundary.
Signal renderInCalls(const Signal& x, int call) {
  Signal out(x.size());
  cv::BloomParams p;
  p.keyFromGate = true;
  gBloom.reset();
  for (size_t i = 0; i < x.size(); i += static_cast<size_t>(call)) {
    if (i == 112000) p.amount = 0.9f;
    if (i == 168000) p.lengthMs = 1500.0f;
    gBloom.process(&x[i], &out[i], call, p, i >= 56000 && i < 224000);
  }
  return out;
}

// The same key, knob and toggle history rendered in calls of 64, 128, 192 and 384 samples, every
// change on a multiple of 384: the chunks the engine cuts are the same, so the outputs match.
bool chunksAgree(const Signal& clips, int seed, bool gateKey) {
  const size_t groups = clips.size() / 384;
  Rng r(static_cast<uint64_t>(seed));
  std::vector<char> key(groups), on(groups);
  std::vector<float> amount(groups), length(groups);
  bool k = false, o = true;
  float a = 0.5f, l = 300.0f;
  for (size_t i = 0; i < groups; ++i) {
    if (r.uni() < 0.04) k = !k;
    if (r.uni() < 0.01) a = static_cast<float>(r.uni());
    if (r.uni() < 0.01) l = static_cast<float>(r.logUni(20, 3000));
    if (r.uni() < 0.005) o = !o;
    key[i] = k;
    on[i] = o;
    amount[i] = a;
    length[i] = l;
  }
  Signal ref;
  for (const int call : {64, 128, 192, 384}) {
    Signal out(groups * 384);
    cv::BloomParams p;
    p.keyFromGate = gateKey;
    gBloom.reset();
    for (size_t i = 0; i < out.size(); i += static_cast<size_t>(call)) {
      const size_t g = i / 384;
      p.amount = amount[g];
      p.lengthMs = length[g];
      p.on = on[g] != 0;
      gBloom.process(&clips[i], &out[i], call, p, key[g] != 0);
    }
    if (ref.empty()) ref = out;
    else if (!sameBytes(ref, out)) return false;
  }
  return true;
}

// Call sizes do not change the output while the key, AMOUNT and LENGTH move. Calls of 1, 7, 64 and 1,000
// samples agree through a key that rises at 56,000 samples and falls at 224,000 (up to the sample
// before the hold, which counts in chunks, could end the word). Calls of 64, 128, 192 and 384 agree
// through random key, knob and toggle histories on the clips, with the gate key and the own key,
// including the word ends.
bool testCallSizes() {
  Signal x = vowel(6000, 0, 5800);
  x.resize(280000);
  const Signal ref = renderInCalls(x, 64);
  bool sizes = true;
  for (const int call : {1, 7, 1000}) {
    const Signal out = renderInCalls(x, call);
    for (size_t i = 0; i < 226000; ++i) sizes = sizes && out[i] == ref[i];
  }
  const std::vector<Signal> clips = loadClips(0);
  if (clips.empty()) {
    const bool clipsOk = skipNoClips("call-sizes clips");
    const bool sizesOk = report("call-sizes", sizes, "calls of 1/7/64/1000 samples agree through a rising key and two knob moves: %.0f (clips skipped)", sizes ? 1.0 : 0.0);
    return sizesOk && clipsOk;
  }
  Signal all;
  for (const Signal& c : clips) all.insert(all.end(), c.begin(), c.end());
  bool blocks = true;
  for (int seed = 1; seed <= 4; ++seed) blocks = blocks && chunksAgree(all, seed, true) && chunksAgree(all, seed, false);
  return report("call-sizes", sizes && blocks, "calls of 1/7/64/1000 samples agree through a rising key and two knob moves: %.0f; calls of 64/128/192/384 agree through 8 random key, knob and toggle histories: %.0f",
                sizes ? 1.0 : 0.0, blocks ? 1.0 : 0.0);
}

// A clock value shifted by the run's start: the same sample of a run that starts at `off` reads `off`
// more than the same sample of a run that starts at 0. Both runs read 0 for a clock never set.
bool shifted(const std::vector<uint32_t>& zero, const std::vector<uint32_t>& moved, uint32_t off) {
  if (zero.size() != moved.size()) return false;
  for (size_t i = 0; i < zero.size(); ++i)
    if (!((zero[i] == 0 && moved[i] == 0) || moved[i] == zero[i] + off)) return false;
  return true;
}

bool shifted(const std::vector<int>& zero, const std::vector<int>& moved, uint32_t off) {
  std::vector<uint32_t> a(zero.begin(), zero.end()), b(moved.begin(), moved.end());
  return shifted(a, b, off);
}

// Whether a run that starts `off` samples after clock 0 matches the run that starts at 0, sample for
// sample: the output, every gain and flag, and the clocks of the arms, writes, cuts and splices
// shifted by `off`.
bool tracesMatch(const Trace& zero, const Trace& moved, uint32_t off) {
  bool ok = zero.out == moved.out && zero.total == moved.total && zero.pair == moved.pair;
  ok = ok && zero.word == moved.word && zero.loop == moved.loop && zero.taken == moved.taken && zero.skipped == moved.skipped;
  ok = ok && shifted(zero.arms, moved.arms, off) && shifted(zero.writes, moved.writes, off);
  for (size_t k = 0; k < static_cast<size_t>(kVoices); ++k) {
    ok = ok && zero.gain[k] == moved.gain[k] && zero.seq[k] == moved.seq[k] && zero.len[k] == moved.len[k] && zero.on[k] == moved.on[k];
    ok = ok && shifted(zero.cut[k], moved.cut[k], off) && shifted(zero.spliced[k], moved.spliced[k], off);
  }
  return ok;
}

// Renders in calls of `call` samples from a clock that starts at `start`, the gate key read at the first
// sample of each call.
Signal renderFrom(const Signal& in, const std::vector<char>& key, cv::BloomParams p, uint32_t start, int call) {
  p.keyFromGate = true;
  gBloom.reset();
  gBloom.startClockAt(start);
  Signal out(in.size());
  for (size_t i = 0; i < in.size(); i += static_cast<size_t>(call)) {
    const int n = static_cast<int>(in.size() - i < static_cast<size_t>(call) ? in.size() - i : static_cast<size_t>(call));
    gBloom.process(&in[i], &out[i], n, p, key[i] != 0);
  }
  return out;
}

// The 32-bit sample clock wraps after 24.8 hours. A run that starts shortly before the wrap behaves as a
// run that starts at 0: the schedule (arms, writes, cuts, splices, ends and fades) and the output match
// sample for sample, in calls of one sample and in calls of 64, with the wrap landing in a word, at its
// end, in the tail, under the next word, in a pile of quick words and on the first sample.
bool testClockWrap() {
  struct Program {
    Signal in;
    std::vector<char> key;
    cv::BloomParams p;
    std::vector<int> wrapAtSmp;  // samples from the start to the wrap
  };
  std::vector<Program> programs(2);
  {
    Program& g = programs[0];
    g.in = vowel(4200, 100, 4000);
    g.key = keyOver(4200, {{100, 1700}, {2300, 3000}});
    g.p.lengthMs = 1500.0f;
    g.wrapAtSmp = {1, 20 * kMs + 7, 640 * kMs + 37, 1720 * kMs + 5, 2000 * kMs + 11, 2650 * kMs + 29, 3300 * kMs + 3};
  }
  {
    Program& g = programs[1];
    g.in = vowel(3200, 50, 3100);
    g.key = keyOver(3200, {{100, 260}, {420, 560}, {720, 880}, {1040, 1200}, {1360, 1520}, {1700, 1850}});
    g.p.lengthMs = 3000.0f;
    g.p.amount = 1.0f;
    g.p.tuning.firstCaptureMs = 2.0f;
    g.wrapAtSmp = {50 * kMs + 9, 500 * kMs + 21, 1100 * kMs + 1, 1900 * kMs + 33};
  }
  int runs = 0;
  bool ok = true;
  for (Program& g : programs) {
    g.p.keyFromGate = true;
    gBloom.reset();
    const Trace zero = trace(gBloom, g.in, g.key, g.p);
    const Signal zero64 = renderFrom(g.in, g.key, g.p, 0, 64);
    for (const int wrap : g.wrapAtSmp) {
      const uint32_t start = 0u - static_cast<uint32_t>(wrap);
      gBloom.reset();
      gBloom.startClockAt(start);
      const Trace moved = trace(gBloom, g.in, g.key, g.p);
      ok = ok && tracesMatch(zero, moved, start) && sameBytes(zero64, renderFrom(g.in, g.key, g.p, start, 64));
      ++runs;
    }
  }
  return report("clock-wrap", ok, "%.0f runs that cross the 32-bit wrap match the run from clock 0, sample for sample, in calls of 1 and of 64: %.0f", runs, ok ? 1.0 : 0.0);
}

// Settled off is a plain copy, tested with ==, from reset and after a running stage switches off.
bool offIsCopy(const Signal& in, bool& wasWet) {
  cv::BloomParams off;
  off.on = false;
  off.keyFromGate = true;
  Signal out(in.size());
  gBloom.reset();
  gBloom.process(in.data(), out.data(), static_cast<int>(in.size()), off, true);
  const bool fromReset = out == in;
  cv::BloomParams on;
  on.keyFromGate = true;
  gBloom.reset();
  const size_t half = 1000 * kMs;
  gBloom.process(in.data(), out.data(), static_cast<int>(half), on, true);
  wasWet = gBloom.looping();
  gBloom.process(in.data() + half, out.data() + half, static_cast<int>(in.size() - half), off, true);
  bool tailCopy = true;
  for (size_t i = 2000 * kMs; i < in.size(); ++i)
    if (out[i] != in[i]) tailCopy = false;
  return fromReset && tailCopy;
}

// A tail fading under a new word and the new word itself, with a slice each, then switched off: both
// stop and the output is the input.
bool bothVoicesOff(const Signal& two, bool& bothRan) {
  cv::BloomParams a;
  a.keyFromGate = true;
  a.lengthMs = 2000.0f;
  Signal out(two.size());
  gBloom.reset();
  for (size_t i = 0; i < 900 * kMs; ++i) gBloom.process(&two[i], &out[i], 1, a, i < 400 * kMs || i >= 700 * kMs);
  bothRan = gBloom.voiceOn(kWord) && gBloom.voiceOn(kFade);
  a.on = false;
  gBloom.process(two.data() + 900 * kMs, out.data() + 900 * kMs, 2100 * kMs, a, true);
  bool copy = true;
  for (size_t i = 2800 * kMs; i < two.size(); ++i)
    if (out[i] != two[i]) copy = false;
  bool stopped = !gBloom.looping() && !gBloom.inWord();
  for (int k = 0; k < kVoices; ++k) stopped = stopped && !gBloom.voiceOn(k);
  return copy && stopped;
}

bool testSettledOff() {
  const int ms = 3000;
  const Signal in = vowel(ms, 0, 2000);
  bool wasWet = false, bothRan = false;
  const bool copies = offIsCopy(in, wasWet);
  const bool twoOff = bothVoicesOff(vowel(ms, 0, 2800), bothRan);
  cv::BloomParams zero;
  zero.keyFromGate = true;
  zero.amount = 0.0f;
  Signal out(in.size());
  gBloom.reset();
  gBloom.process(in.data(), out.data(), static_cast<int>(in.size()), zero, true);
  const bool knobZero = out == in;
  return report("settled-off", copies && wasWet && bothRan && twoOff && knobZero && !gBloom.looping(),
                "off from reset and after running == input, loop stopped: %.0f, word and fading tail off == input: %.0f, AMOUNT 0 == input: %.0f",
                copies && wasWet ? 1.0 : 0.0, bothRan && twoOff ? 1.0 : 0.0, knobZero ? 1.0 : 0.0);
}

// A quick off and on keeps the running word; extreme input stays finite and bounded.
bool testToggleAndRange() {
  const int ms = 1500;
  const Signal in = vowel(ms, 0, 1200);
  cv::BloomParams p;
  p.keyFromGate = true;
  gBloom.reset();
  Signal out(in.size());
  gBloom.process(in.data(), out.data(), 500 * kMs, p, true);
  const int takenBefore = gBloom.taken();
  p.on = false;
  gBloom.process(in.data() + 500 * kMs, out.data() + 500 * kMs, 20 * kMs, p, true);
  p.on = true;
  gBloom.process(in.data() + 520 * kMs, out.data() + 520 * kMs, 100 * kMs, p, true);
  const bool keeps = gBloom.looping() && gBloom.inWord() && gBloom.taken() >= takenBefore;

  Signal loud(static_cast<size_t>(ms) * kMs);
  for (size_t i = 0; i < loud.size(); ++i) loud[i] = (i / 37) % 2 ? 4.0f : -4.0f;
  gBloom.reset();
  Signal y(loud.size());
  gBloom.process(loud.data(), y.data(), static_cast<int>(loud.size()), p, true);
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

// A scripted history: key phrases and gaps, AMOUNT and LENGTH steps, and brief toggle-offs.
struct History {
  Rng r;
  cv::BloomParams p;
  bool keyOn = false;
  int mode = 0;
  long keyUntil = 0, paramUntil = 0, toggleUntil;
  bool off = false;

  explicit History(int seed) : r(static_cast<uint64_t>(seed)), toggleUntil(static_cast<long>(r.range(2, 20)) * cv::kSampleRate) { p.keyFromGate = true; }

  void step(long t) {
    if (t >= keyUntil) {
      if (r.uni() < 0.2) mode = r.range(0, 2);
      keyOn = !keyOn;
      double ms;
      if (mode == 0) ms = keyOn ? r.logUni(150, 1500) : r.logUni(5, 600);
      else if (mode == 1) ms = keyOn ? r.logUni(10, 200) : r.logUni(10, 120);
      else ms = keyOn ? r.logUni(1000, 8000) : r.logUni(100, 4000);
      keyUntil = t + static_cast<long>(ms * kMs);
    }
    if (t >= paramUntil) {
      const double a = r.uni(), l = r.uni();
      p.amount = a < 0.1 ? 0.0f : (a > 0.9 ? 1.0f : static_cast<float>(r.uni()));
      p.lengthMs = l < 0.08 ? 20.0f : (l > 0.92 ? 3000.0f : static_cast<float>(r.logUni(20, 3000)));
      paramUntil = t + static_cast<long>(r.logUni(100, 3000) * kMs);
    }
    if (t >= toggleUntil) {
      off = !off;
      toggleUntil = t + static_cast<long>(off ? r.logUni(1, 500) : r.logUni(500, 20000)) * kMs;
      p.on = !off;
    }
  }
};

// The first broken structural invariant of the voices, or null: gains finite in 0..0.8, an off voice
// carries no gain, no two voices play one slice, and looping() agrees with the voices.
const char* voiceFault() {
  uint32_t seen[kVoices];
  int playing = 0;
  for (int k = 0; k < kVoices; ++k) {
    const float g = gBloom.voiceGain(k);
    if (!std::isfinite(g) || g < 0.0f || g > 0.8f) return "voice gain out of 0..0.8";
    if (!gBloom.voiceOn(k) && g != 0.0f) return "off voice with a gain";
    const uint32_t seq = gBloom.playingSeq(k);
    if (seq == 0) continue;
    for (int j = 0; j < playing; ++j)
      if (seen[j] == seq) return "one slice played by two voices";
    seen[playing++] = seq;
  }
  return gBloom.looping() == (playing > 0) ? nullptr : "looping() differs from the voices";
}

struct FuzzStats {
  int faults = 0, jumps = 0, tailsLeft = 0;
  double worstStep = 0.0;
};

// One seeded history of `seconds` over the clips, the dry input silent so the output is the wet path
// alone; with `unit` one sample a call and each voice's gain checked for steps. Then the key stops and the
// knobs hold still, and every tail must end within 5.5 s.
void fuzzSeed(int seed, bool unit, int seconds, const Signal& clips, FuzzStats& st) {
  History h(seed);
  gBloom.reset();
  const Signal zero(256, 0.0f);
  Signal src(256), out(256);
  size_t srcPos = static_cast<size_t>(h.r.range(0, static_cast<int>(clips.size()) - 1));
  float last = 0.0f;
  std::array<float, kVoices> gain{};
  std::array<uint32_t, kVoices> seq{};
  const long total = static_cast<long>(seconds) * cv::kSampleRate;
  const long end = total + 11 * cv::kSampleRate / 2;
  for (long done = 0; done < end;) {
    if (done < total) h.step(done);
    else {
      h.keyOn = false;
      h.p.on = true;
    }
    const int n = unit ? 1 : (h.r.uni() < 0.2 ? h.r.range(1, 200) : 64);
    for (int i = 0; i < n; ++i) src[static_cast<size_t>(i)] = clips[(srcPos + static_cast<size_t>(i)) % clips.size()];
    srcPos = (srcPos + static_cast<size_t>(n)) % clips.size();
    gBloom.process(zero.data(), src.data(), out.data(), n, h.p, h.keyOn);
    done += n;
    if (voiceFault()) ++st.faults;
    for (int i = 0; i < n; ++i) {
      st.worstStep = std::fmax(st.worstStep, std::fabs(static_cast<double>(out[static_cast<size_t>(i)]) - last));
      last = out[static_cast<size_t>(i)];
    }
    for (int k = 0; k < kVoices; ++k) {
      const size_t j = static_cast<size_t>(k);
      const bool continuing = unit && seq[j] != 0 && gBloom.playingSeq(k) == seq[j];
      if (continuing && std::fabs(gBloom.voiceGain(k) - gain[j]) > 0.0035f) ++st.jumps;
      gain[j] = gBloom.voiceGain(k);
      seq[j] = gBloom.voiceOn(k) ? gBloom.playingSeq(k) : 0u;
    }
  }
  st.tailsLeft += gBloom.looping() || gBloom.inWord() ? 1 : 0;
}

// Random key, knob and toggle histories on the clips, in calls of 1 to 200 samples and one sample at a
// time. The voices keep their invariants, no voice's gain steps more than a 5 ms ramp step, the wet path
// never steps more than 0.045 (the loops' own peaks reach 0.030; a tail dropped from the mix for a block
// steps 0.068), and every tail ends.
bool testFuzz() {
  const std::vector<Signal> clips = loadClips(0);
  if (clips.empty()) return skipNoClips("fuzz");
  Signal all;
  for (const Signal& c : clips) all.insert(all.end(), c.begin(), c.end());
  FuzzStats st;
  for (int seed = 1; seed <= 12; ++seed) fuzzSeed(seed, false, 60, all, st);
  for (int seed = 101; seed <= 103; ++seed) fuzzSeed(seed, true, 20, all, st);
  return report("fuzz", st.faults == 0 && st.jumps == 0 && st.tailsLeft == 0 && st.worstStep < 0.045,
                "12 histories of 60 s and 3 of 20 s a sample at a time: invariant faults %.0f (0), voice gain steps over 0.0035 %.0f (0), tails still sounding 5.5 s after the key stops %.0f (0), largest wet step %.4f (< 0.045)",
                st.faults, st.jumps, st.tailsLeft, st.worstStep);
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
  ok &= testLeadingEdge();
  ok &= testRearmOnce();
  ok &= testTail();
  ok &= testShortTail();
  ok &= testTailSlice();
  ok &= testTailAfterStop();
  ok &= testTailOnClips();
  ok &= testStraddle();
  ok &= testClipGaps();
  ok &= testNextWord();
  ok &= testYield();
  ok &= testKeyBlip();
  ok &= testKeyHold();
  ok &= testSource();
  ok &= testAgeLimit();
  ok &= testLoopUnderWord();
  ok &= testOwnKey();
  ok &= testNoClicks();
  ok &= testDryUnity();
  ok &= testKnobMoves();
  ok &= testDeterminism();
  ok &= testCallSizes();
  ok &= testClockWrap();
  ok &= testSettledOff();
  ok &= testToggleAndRange();
  ok &= testStorage();
  ok &= testNoAlloc();
  ok &= testFuzz();
  return ok ? 0 : 1;
}
