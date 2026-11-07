// Gate checks: passthrough, closing, attack and release travel times, hysteresis on held notes, the
// threshold edges, the on/off fade, bad samples, call sizes, the applied gain against libm, no-alloc,
// and the Input gate's pots, burst timing, noise stability and detector peak. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/chain_params.h"
#include "core/gate_floor.h"
#include "core/registry.h"
#include "engine/gate.h"

#include "alloc_guard.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMs = cv::kSampleRate / 1000;

float dbfs(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

// Feeds n samples one at a time. amp 0 is silence. Records the gain after each sample.
// Phase runs on across calls so a tone has no discontinuity.
class Rig {
 public:
  cv::Gate gate;
  long pos = 0;

  std::vector<float> feed(int n, double hz, float amp, const cv::GateParams& p,
                          std::vector<float>* in = nullptr, std::vector<float>* out = nullptr) {
    std::vector<float> g(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i, ++pos) {
      const float x = amp * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(pos) / cv::kSampleRate));
      float y = 0.0f;
      gInProcess = true;
      gate.process(&x, &y, 1, p);
      gInProcess = false;
      g[static_cast<size_t>(i)] = gate.gainDb();
      if (in) in->push_back(x);
      if (out) out->push_back(y);
    }
    return g;
  }
};

// The physics checks run on fixed tuning so they do not follow default changes.
cv::GateParams fixedParams() {
  cv::GateParams p;
  p.thresholdDb = -40.0f;
  p.tuning.holdMs = 60.0f;
  p.tuning.releaseMs = 120.0f;
  p.tuning.rangeDb = -80.0f;
  p.tuning.kneeDb = 6.0f;
  p.tuning.hysteresisDb = 3.0f;
  return p;
}

// Each card as the box sets it: the registry's pot mapping on top of the chain defaults.
cv::GateParams boxInputGate(float thresholdPos, float decayPos) {
  ChainParams chain;
  applyKnob(Card::InputGate, 0, thresholdPos, chain);
  applyKnob(Card::InputGate, 1, decayPos, chain);
  return chain.inputGate;
}

cv::GateParams boxGate(float thresholdPos, float decayPos) {
  ChainParams chain;
  applyKnob(Card::Gate, 0, thresholdPos, chain);
  applyKnob(Card::Gate, 1, decayPos, chain);
  cv::GateParams p = chain.gate;
  p.on = true;
  return p;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0,
            double d = 0.0, double e = 0.0) {
  char detail[220];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d, e);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// One gate fed a sample at a time.
class Probe {
 public:
  cv::Gate gate;

  float step(float x, const cv::GateParams& p) {
    float y = 0.0f;
    gInProcess = true;
    gate.process(&x, &y, 1, p);
    gInProcess = false;
    return y;
  }

  // Silence until the gain sits at the closed range.
  bool settleClosed(const cv::GateParams& p) {
    for (int i = 0; i < 40 * cv::kSampleRate; ++i) {
      if (gate.gainDb() <= p.tuning.rangeDb + 0.1f) return true;
      step(0.0f, p);
    }
    return false;
  }
};

bool testPassthrough() {
  cv::GateParams off;
  off.on = false;
  Rig a;
  a.gate.reset();
  std::vector<float> in, out;
  a.feed(cv::kSampleRate, 1000.0, dbfs(-60.0), off, &in, &out);
  float offDiff = 0.0f;
  for (size_t i = static_cast<size_t>(100 * kMs); i < in.size(); ++i)
    offDiff = std::fmax(offDiff, std::fabs(out[i] - in[i]));

  cv::GateParams on;
  on.thresholdDb = -40.0f;
  Rig b;
  b.gate.reset();
  in.clear();
  out.clear();
  b.feed(cv::kSampleRate, 1000.0, dbfs(-30.0), on, &in, &out);
  float onDiff = 0.0f;
  for (size_t i = static_cast<size_t>(100 * kMs); i < in.size(); ++i)
    onDiff = std::fmax(onDiff, std::fabs(out[i] - in[i]));
  return report("passthrough", offDiff == 0.0f && onDiff == 0.0f,
                "off max|out-in|=%.3g (0); on, +10 dB over threshold, max|out-in|=%.3g (0)", offDiff, onDiff);
}

bool testCloses() {
  cv::GateParams p = fixedParams();
  Rig r;
  r.gate.reset();
  const std::vector<float> g = r.feed(2 * cv::kSampleRate, 1000.0, dbfs(-60.0), p);
  const float last = g.back();
  return report("closes", last <= -70.0f, "1 kHz -60 dBFS, thr -40: steady gain=%.2f dB (<=-70)", last);
}

// First sample index in g at or past `from` where pred holds, else -1.
template <class F>
int firstWhere(const std::vector<float>& g, int from, F pred) {
  for (size_t i = static_cast<size_t>(from); i < g.size(); ++i)
    if (pred(g[i])) return static_cast<int>(i);
  return -1;
}

bool testTiming() {
  cv::GateParams p = fixedParams();
  const cv::GateTuning& t = p.tuning;
  Rig r;
  r.gate.reset();
  r.feed(2 * cv::kSampleRate, 1000.0, 0.0f, p);  // settle closed

  // Attack: tone at -20 dBFS.
  const std::vector<float> up = r.feed(500 * kMs, 1000.0, dbfs(-20.0), p);
  const int tAtt = firstWhere(up, 0, [](float g) { return g >= -1.0f; });
  const double attMs = tAtt < 0 ? -1.0 : static_cast<double>(tAtt) / kMs;
  const bool attOk = tAtt >= 0 && attMs <= static_cast<double>(t.attackMs) + 5.0;

  // Release: back to silence. The detector window holds the level for up to two segments, and then the
  // detector falls from -20 dBFS to the close level before the hold timer starts, so both are part of the
  // expected delay.
  const std::vector<float> dn = r.feed(2 * cv::kSampleRate, 1000.0, 0.0f, p);
  const int tLeave = firstWhere(dn, 0, [](float g) { return g < -1.0f; });
  const double holdMs = tLeave < 0 ? -1.0 : static_cast<double>(tLeave) / kMs;
  const double detFallMs = 20.0 * std::log(std::pow(10.0, ((p.thresholdDb - t.hysteresisDb) - -20.0) / -20.0));
  const double windowMs = 2.0 * 640.0 / kMs;
  const double lo = static_cast<double>(t.holdMs) - 10.0;
  const double hi = static_cast<double>(t.holdMs) + windowMs + detFallMs + 10.0;
  const bool holdOk = tLeave >= 0 && holdMs >= lo && holdMs <= hi;
  const int tRel = firstWhere(dn, 0, [&](float g) { return g <= t.rangeDb + 6.0f; });
  const double relMs = tRel < 0 ? -1.0 : static_cast<double>(tRel) / kMs - holdMs;
  const bool relOk = tRel >= 0 && relMs <= static_cast<double>(t.releaseMs);
  report("timing attack", attOk, "gain >= -1 dB after %.2f ms (<= attack+5 = %.1f)", attMs, t.attackMs + 5.0);
  report("timing hold", holdOk, "gain < -1 dB after %.1f ms (hold %.0f -10 .. +window %.1f +detector fall %.1f +10 = %.1f)",
         holdMs, t.holdMs, windowMs, detFallMs, hi);
  return report("timing release", relOk, "range+6 dB reached %.1f ms after the drop (<= release = %.0f)", relMs,
                static_cast<double>(t.releaseMs)) && attOk && holdOk;
}

bool testHysteresis() {
  cv::GateParams p = fixedParams();  // threshold -40, hysteresis 3
  Rig r;
  r.gate.reset();
  r.feed(500 * kMs, 1000.0, dbfs(-20.0), p);  // open
  const std::vector<float> stay = r.feed(1000 * kMs, 1000.0, dbfs(-41.0), p);
  float minStay = 0.0f;
  for (float g : stay) minStay = std::fmin(minStay, g);
  const std::vector<float> shut = r.feed(1500 * kMs, 1000.0, dbfs(-44.0), p);
  const float endShut = shut.back();
  return report("hysteresis", minStay > -1.0f && endShut < -10.0f,
                "-41 dBFS after open: min gain %.2f dB (> -1); -44 dBFS: gain %.2f dB (< -10)", minStay,
                endShut);
}

// The lowest gain over 3 s of a note held underDb under the threshold after the gate opened, with a
// 5 Hz vibratoDb amplitude wobble. The detector keeps the level between the pitch peaks, so the
// hysteresis holds the gate open for any note whose troughs stay above the close level.
float heldNoteLowestGain(const cv::GateParams& p, double hz, double underDb, double vibratoDb) {
  Probe r;
  r.gate.reset();
  const double thr = static_cast<double>(p.thresholdDb);
  long pos = 0;
  const auto tone = [&](double db) {
    const double t = static_cast<double>(pos++) / cv::kSampleRate;
    return static_cast<float>(std::pow(10.0, db / 20.0) * std::sin(2.0 * kPi * hz * t));
  };
  for (int n = 0; n < 100 * kMs; ++n) r.step(tone(thr + 10.0), p);
  float lowest = 0.0f;
  for (int n = 0; n < 3 * cv::kSampleRate; ++n) {
    const double wobble = vibratoDb * std::sin(2.0 * kPi * 5.0 * static_cast<double>(n) / cv::kSampleRate);
    r.step(tone(thr - underDb + wobble), p);
    lowest = std::fmin(lowest, r.gate.gainDb());
  }
  return lowest;
}

bool testHeldNote() {
  const cv::GateParams input = boxInputGate(0.5f, 0.5f);  // -55 dBFS, 3 dB hysteresis
  cv::GateParams gate = boxGate(0.5f, 0.5f);               // -40 dBFS, 6 dB hysteresis
  gate.tuning.detectorHpHz = 0.0f;
  float worst = 0.0f;
  for (const double hz : {78.0, 100.0, 220.0, 440.0}) {
    worst = std::fmin(worst, heldNoteLowestGain(input, hz, 0.5, 1.0));  // troughs 1.5 dB under
    worst = std::fmin(worst, heldNoteLowestGain(input, hz, 2.0, 0.0));
    worst = std::fmin(worst, heldNoteLowestGain(gate, hz, 2.0, 1.0));   // troughs 3 dB under
  }
  return report("held note", worst > -1.0f,
                "notes 78 to 440 Hz hovering 0.5 to 2 dB under the threshold, 5 Hz +-1 dB vibrato: lowest gain %.2f dB (> -1)",
                static_cast<double>(worst));
}

bool testNoAlloc() {
  cv::GateParams p;
  Rig r;
  r.gate.reset();
  r.feed(cv::kBlock * 8, 500.0, dbfs(-30.0), p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// The gain runs through dbToGainFast. Through an open, a hold and a close to the -80 dB range,
// every output sample sits within 1e-6 relative of the input times libm's 10^(gainDb / 20).
bool testGainMatchesLibm() {
  cv::GateParams p = fixedParams();
  Rig r;
  r.gate.reset();
  std::vector<float> in, out;
  std::vector<float> g = r.feed(500 * kMs, 1000.0, dbfs(-20.0), p, &in, &out);
  const std::vector<float> shut = r.feed(2 * cv::kSampleRate, 1000.0, dbfs(-60.0), p, &in, &out);
  g.insert(g.end(), shut.begin(), shut.end());
  double worst = 0.0;
  for (size_t i = 0; i < in.size(); ++i) {
    const double want = static_cast<double>(in[i]) * std::pow(10.0, static_cast<double>(g[i]) / 20.0);
    if (want != 0.0) worst = std::fmax(worst, std::fabs(static_cast<double>(out[i]) - want) / std::fabs(want));
  }
  const float lowest = *std::min_element(g.begin(), g.end());
  return report("gain vs libm", worst <= 1e-6 && lowest <= -79.0f,
                "worst relative error %.3g (<= 1e-6) with the gain down to %.1f dB (<= -79)", worst, lowest);
}

// A tone burst program with a 1 kHz tone 20 dB over the threshold and gaps of silence, and a parameter
// change at every segment, so the gain moves, the hold runs and the law is worked out again.
std::vector<float> burstProgram(int seconds) {
  std::vector<float> x(static_cast<size_t>(seconds * cv::kSampleRate));
  for (size_t i = 0; i < x.size(); ++i) {
    const bool tone = (i / (150 * kMs)) % 3 != 2;
    x[i] = tone ? 0.1f * static_cast<float>(std::sin(2.0 * kPi * 330.0 * static_cast<double>(i) / cv::kSampleRate)) : 0.0f;
  }
  return x;
}

// process() gives the same samples for any call size, across parameter changes and toggles.
bool testCallSizes() {
  constexpr int kSegment = 1000;
  const std::vector<float> in = burstProgram(2);
  bool same = true;
  for (const bool inputCard : {true, false}) {
    std::vector<float> want;
    for (const int chunk : {1, 17, 64, 200, 1000}) {
      cv::Gate g;
      g.reset();
      std::vector<float> out(in.size());
      for (size_t at = 0; at < in.size(); at += kSegment) {
        const int seg = static_cast<int>(at / kSegment);
        cv::GateParams p = inputCard ? boxInputGate(0.2f + 0.01f * static_cast<float>(seg % 50), 0.5f)
                                     : boxGate(0.2f + 0.01f * static_cast<float>(seg % 50), 0.3f);
        p.on = (seg / 40) % 2 == 0;
        const int len = static_cast<int>(std::min<size_t>(kSegment, in.size() - at));
        for (int done = 0; done < len; done += chunk) {
          const int m = std::min(chunk, len - done);
          gInProcess = true;
          g.process(&in[at + static_cast<size_t>(done)], &out[at + static_cast<size_t>(done)], m, p);
          gInProcess = false;
        }
      }
      if (want.empty()) want = out;
      else same = same && out == want;
    }
  }
  return report("call sizes", same, "calls of 17, 64, 200 and 1000 samples equal one sample a call, both cards (%.0f = same)",
                same ? 1.0 : 0.0);
}

// The decision to open or close is the dB compare, at every level around each edge. The linear edges that
// short-cut it must never change the answer.
bool testLevelEdges() {
  cv::GateParams p;
  p.thresholdDb = -37.3f;
  p.tuning.kneeDb = 0.0f;
  p.tuning.detectorHpHz = 0.0f;
  p.tuning.detectorAttackMs = 0.0f;
  p.tuning.detectorReleaseMs = 0.0f;
  p.tuning.hysteresisDb = 3.0f;
  const float closeAt = p.thresholdDb - p.tuning.hysteresisDb;
  int wrong = 0;
  for (int k = -60; k <= 60; ++k) {
    const float amp = cv::dbToGain(p.thresholdDb + 0.0005f * static_cast<float>(k));
    Probe opening;
    opening.gate.reset();
    opening.step(amp, p);
    wrong += opening.gate.open() != (cv::dbFast(amp + 1e-9f) >= p.thresholdDb);

    const float ampClose = cv::dbToGain(closeAt + 0.0005f * static_cast<float>(k));
    Probe closing;
    closing.gate.reset();
    closing.step(0.5f, p);
    for (int n = 0; n < 1400; ++n) closing.step(ampClose, p);  // the window passes the 0.5
    wrong += closing.gate.open() != (cv::dbFast(ampClose + 1e-9f) >= closeAt);
  }
  return report("level edges", wrong == 0, "%.0f decisions differ from the dB compare around both edges (0)",
                static_cast<double>(wrong));
}

struct Travel {
  double riseMs, fallMs;
};

// The time the gain takes to rise from the range to within 1 dB of open, and to fall from open to within
// 1 dB of the range, on a hard range with an instant detector and no hold.
Travel measureTravel(float rangeDb, float attackMs, float releaseMs) {
  cv::GateParams p;
  p.thresholdDb = -40.0f;
  p.tuning.rangeDb = rangeDb;
  p.tuning.kneeDb = 0.0f;
  p.tuning.holdMs = 0.0f;
  p.tuning.detectorHpHz = 0.0f;
  p.tuning.detectorAttackMs = 0.0f;
  p.tuning.detectorReleaseMs = 0.0f;
  p.tuning.attackMs = attackMs;
  p.tuning.releaseMs = releaseMs;
  Probe r;
  r.gate.reset();
  r.settleClosed(p);
  int rise = -1;
  for (int n = 0; n < 4 * cv::kSampleRate && rise < 0; ++n) {
    r.step(0.5f, p);
    if (r.gate.gainDb() >= -1.0f) rise = n + 1;
  }
  for (int n = 0; n < 2000; ++n) r.step(0.5f, p);
  int start = -1;
  int fall = -1;
  for (int n = 0; n < 8 * cv::kSampleRate && fall < 0; ++n) {
    r.step(0.0f, p);
    if (start < 0 && r.gate.gainDb() < -1e-4f) start = n;
    if (r.gate.gainDb() <= rangeDb + 1.0f) fall = n + 1;
  }
  return {static_cast<double>(rise) / kMs, static_cast<double>(fall - start) / kMs};
}

// Attack and release are travel times for any range. Both cards share the law.
bool testTravelTimes() {
  bool ok = true;
  double worst = 0.0;
  for (const float range : {-80.0f, -40.0f}) {
    for (const float attack : {1.0f, 3.0f, 8.0f}) {
      for (const float release : {20.0f, 200.0f, 1000.0f}) {
        const Travel t = measureTravel(range, attack, release);
        const double errAttack = std::fabs(t.riseMs - static_cast<double>(attack)) / static_cast<double>(attack);
        const double errRelease = std::fabs(t.fallMs - static_cast<double>(release)) / static_cast<double>(release);
        worst = std::fmax(worst, std::fmax(errAttack, errRelease));
        ok = ok && errAttack < 0.03 && errRelease < 0.03;
      }
    }
  }
  return report("travel times", ok, "gain within 1 dB of open or closed after the time shown, ranges -80 and -40 dB: worst error %.2f %% (< 3)",
                100.0 * worst);
}

// The Gate opens from its full close inside 1.1 ms of a burst, as the Input gate does.
bool testGateAttack() {
  const cv::GateParams p = boxGate(0.5f, 0.5f);
  Rig r;
  r.gate.reset();
  r.feed(2 * cv::kSampleRate, 1000.0, 0.0f, p);
  const std::vector<float> up = r.feed(20 * kMs, 1000.0, dbfs(static_cast<double>(p.thresholdDb) + 10.0), p);
  const int t = firstWhere(up, 0, [](float g) { return g >= -1.0f; });
  const double ms = t < 0 ? 1e9 : static_cast<double>(t) / kMs;
  return report("gate attack", ms <= 1.1, "from a full close, within 1 dB of open after %.3f ms (<= 1.1)", ms);
}

// Block gains of a floor tone through a gate switched on or off while closed.
std::vector<double> fadeBlockGains(bool toOn) {
  cv::GateParams p = fixedParams();
  p.on = !toOn;
  Rig r;
  r.gate.reset();
  const float amp = dbfs(-60.0);  // 20 dB under the threshold: the gate sits closed
  r.feed(2 * cv::kSampleRate, 1000.0, amp, p);
  p.on = toOn;
  std::vector<float> in, out;
  r.feed(8 * cv::kBlock, 1000.0, amp, p, &in, &out);
  std::vector<double> gains;
  for (size_t b = 0; b + cv::kBlock <= in.size(); b += cv::kBlock) {
    double ei = 0.0, eo = 0.0;
    for (size_t i = b; i < b + cv::kBlock; ++i) {
      ei += static_cast<double>(in[i]) * static_cast<double>(in[i]);
      eo += static_cast<double>(out[i]) * static_cast<double>(out[i]);
    }
    gains.push_back(10.0 * std::log10(eo / ei));
  }
  return gains;
}

// Switching a closed gate off rises to unity in one steady climb that ends with the slot fade, and the
// output is the input from then on. Switching it on falls the same way. Neither passes unity.
bool testOnOffFade() {
  const std::vector<double> off = fadeBlockGains(false);
  const std::vector<double> on = fadeBlockGains(true);
  bool monotone = true;
  for (size_t b = 1; b < off.size(); ++b) monotone = monotone && off[b] >= off[b - 1] - 0.05 && off[b] <= 0.05;
  for (size_t b = 1; b < on.size(); ++b) monotone = monotone && on[b] <= on[b - 1] + 0.05 && on[b] <= 0.05;
  const int settledBlocks = static_cast<int>(cv::kGateFadeMs * cv::kSmpPerMs) / cv::kBlock;
  bool settled = true;
  for (size_t b = static_cast<size_t>(settledBlocks); b < off.size(); ++b) settled = settled && std::fabs(off[b]) < 0.001;
  return report("on/off fade", monotone && settled && off.front() < -20.0 && on.back() < -20.0,
                "off: block gains %.1f .. %.1f dB (steady rise, unity from block 6); on: %.1f .. %.1f dB (steady fall)",
                off.front(), off.back(), on.front(), on.back());
}

// A NaN, an infinity or a sample past +80 dBFS does not leave either gate open. The output keeps the
// bad sample so the chain's guard still sees it.
bool testBadSamples() {
  bool ok = true;
  for (const bool inputCard : {true, false}) {
    const cv::GateParams p = inputCard ? boxInputGate(0.5f, 0.0f) : boxGate(0.5f, 0.0f);
    for (const float bad : {NAN, INFINITY, -INFINITY, 3e38f, 1e30f, 1e5f}) {
      Probe r;
      r.gate.reset();
      for (int n = 0; n < 100 * kMs; ++n) r.step(0.1f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * n / cv::kSampleRate)), p);
      const float y = r.step(bad, p);
      ok = ok && (std::isfinite(bad) || !std::isfinite(y));
      for (int n = 0; n < 2 * cv::kSampleRate; ++n) r.step(0.0f, p);
      ok = ok && !r.gate.open() && r.gate.gainDb() <= p.tuning.rangeDb + 1.0f;
      const float peak = r.gate.takeDetectorPeak();
      ok = ok && std::isfinite(peak) && peak < 1e4f;
    }
  }
  const bool floorOk = peakDbfs(INFINITY) <= 80.0f && std::isfinite(peakDbfs(INFINITY)) && peakDbfs(1e4f) <= 80.001f &&
                       peakDbfs(1.0f) == 0.0f && peakDbfs(0.0f) == kFloorSilentDb && peakDbfs(NAN) == kFloorSilentDb;
  return report("bad samples", ok && floorOk,
                "NaN, +-inf and huge samples leave both gates shut after 2 s (%.0f), floor_dbfs stays finite (%.0f)",
                ok ? 1.0 : 0.0, floorOk ? 1.0 : 0.0);
}

// A hold past the accepted maximum behaves as the maximum, and a longer one is no undefined cast.
bool testHoldClamp() {
  const std::vector<float> in = burstProgram(2);
  std::vector<float> a(in.size()), b(in.size());
  for (int which = 0; which < 2; ++which) {
    cv::GateParams p = boxGate(0.5f, 0.5f);
    p.tuning.holdMs = which == 0 ? cv::kGateHoldMaxMs : 1e9f;
    cv::Gate g;
    g.reset();
    g.process(in.data(), (which == 0 ? a : b).data(), static_cast<int>(in.size()), p);
  }
  return report("hold clamp", a == b, "a hold of 1e9 ms plays as the %.0f ms maximum (%.0f = same)", static_cast<double>(cv::kGateHoldMaxMs),
                a == b ? 1.0 : 0.0);
}

// ---- The slot-1 Input gate ------------------------------------------------------------------------

const char* kPotName[] = {"DECAY 0", "DECAY 0.5", "DECAY 1"};

// The DECAY pot: 20 ms to 2 s log.
bool testInputGateDecayPot() {
  const KnobDesc& dec = *knobDesc(Card::InputGate, 1);
  const double lo = cv::kInputGateDecayMinMs;
  const double hi = cv::kInputGateDecayMaxMs;
  bool ok = true;
  double mapped[3] = {};
  const float pos[3] = {0.0f, 0.5f, 1.0f};
  for (int i = 0; i < 3; ++i) {
    ChainParams chain;
    applyKnob(Card::InputGate, 1, pos[i], chain);
    mapped[i] = static_cast<double>(chain.inputGate.tuning.releaseMs);
    const double want = lo * std::pow(hi / lo, static_cast<double>(pos[i]));
    ok = ok && std::fabs(mapped[i] - want) <= 1e-3 * want &&
         std::fabs(static_cast<double>(knobValue(dec, pos[i])) - want) <= 1e-3 * want;
  }
  return report("input gate DECAY", ok, "DECAY at 0, 0.5, 1 maps to %.1f, %.1f, %.1f ms (20, 200, 2000 log)", mapped[0],
                mapped[1], mapped[2]);
}

// The THRESHOLD pot: -80 to -30 dBFS linear, shown in whole dB.
bool testInputGateThresholdPot() {
  const KnobDesc& thr = *knobDesc(Card::InputGate, 0);
  const bool shown = knobShown(thr, 0.0f) == -80 && knobShown(thr, 1.0f) == -30 && knobShown(thr, 0.5f) == -55 &&
                     thr.curve == Curve::Linear;
  ChainParams chain;
  applyKnob(Card::InputGate, 0, 0.0f, chain);
  const double atMin = static_cast<double>(chain.inputGate.thresholdDb);
  applyKnob(Card::InputGate, 0, 1.0f, chain);
  const double atMax = static_cast<double>(chain.inputGate.thresholdDb);
  return report("input gate THRESHOLD", shown && std::fabs(atMin + 80.0) < 1e-4 && std::fabs(atMax + 30.0) < 1e-4,
                "THRESHOLD maps %.1f to %.1f dBFS, linear, shown in whole dB", atMin, atMax);
}

// A 1 kHz burst 10 dB over the threshold: the gain is within 1 dB of open inside 1 ms of the onset,
// the output peak of the last half cycle inside that millisecond is within 1 dB of the input's, and
// after the burst the gain is under -60 dB within hold + release + 20 ms.
struct BurstResult {
  double openMs, outDb, closeMs, limitMs;
  bool ok;
};

BurstResult runBurst(float thrPos, float decayPos) {
  const cv::GateParams p = boxInputGate(thrPos, decayPos);
  Probe r;
  r.gate.reset();
  BurstResult b{1e9, 0.0, 1e9, 0.0, r.settleClosed(p)};
  const double amp = static_cast<double>(dbfs(static_cast<double>(p.thresholdDb) + 10.0));
  int open = -1;
  double outPeak = 0.0;
  float gainEnd = -80.0f;
  for (int n = 0; n < 300 * kMs; ++n) {
    const float x = static_cast<float>(amp * std::sin(2.0 * kPi * 1000.0 * n / cv::kSampleRate));
    const float y = r.step(x, p);
    if (open < 0 && r.gate.gainDb() >= -1.0f) open = n;
    if (n >= kMs / 2 && n < kMs) outPeak = std::fmax(outPeak, std::fabs(static_cast<double>(y)));
    gainEnd = r.gate.gainDb();
  }
  b.limitMs = static_cast<double>(p.tuning.holdMs) + static_cast<double>(p.tuning.releaseMs) + 20.0;
  int below = -1;
  for (int n = 0; n < static_cast<int>((b.limitMs + 3000.0) * kMs) && below < 0; ++n) {
    r.step(0.0f, p);
    if (r.gate.gainDb() <= -60.0f) below = n;
  }
  b.openMs = open < 0 ? 1e9 : static_cast<double>(open) / kMs;
  b.outDb = 20.0 * std::log10(outPeak / amp);
  b.closeMs = below < 0 ? 1e9 : static_cast<double>(below) / kMs;
  b.ok = b.ok && open >= 0 && b.openMs <= 1.0 && b.outDb >= -1.0 && gainEnd > -0.01f && below >= 0 && b.closeMs <= b.limitMs;
  return b;
}

bool testInputGateBurst() {
  bool ok = true;
  double worstOpenMs = 0.0;
  double worstOutDb = 0.0;
  double worstCloseOverMs = -1e9;
  for (const float thrPos : {0.1f, 0.52f, 0.9f}) {
    for (int d = 0; d < 3; ++d) {
      const float decayPos[3] = {0.0f, 0.5f, 1.0f};
      const BurstResult b = runBurst(thrPos, decayPos[d]);
      worstOpenMs = std::fmax(worstOpenMs, b.openMs);
      worstOutDb = std::fmin(worstOutDb, b.outDb);
      worstCloseOverMs = std::fmax(worstCloseOverMs, b.closeMs - b.limitMs);
      ok = ok && b.ok;
      std::printf("TABLE burst thr %+5.0f dB %-9s within 1 dB of open at %.3f ms, last half cycle %.2f dB, under -60 dB %.1f ms after the burst (limit %.0f)\n",
                  static_cast<double>(boxInputGate(thrPos, decayPos[d]).thresholdDb), kPotName[d], b.openMs, b.outDb, b.closeMs,
                  b.limitMs);
    }
  }
  return report("input gate burst", ok,
                "worst: within 1 dB at %.3f ms (<= 1), output %.2f dB (>= -1), under -60 dB %.1f ms inside the limit",
                worstOpenMs, worstOutDb, -worstCloseOverMs);
}

// Binary white noise (every sample +-1) read by a peak detector shows its amplitude exactly, so a
// level in dB is the level the gate sees. A slow wobble of +-1.5 dB sits on it, from three sines that
// do not repeat inside the run.
class WobbleNoise {
 public:
  explicit WobbleNoise(double centerDb) : centerDb_(centerDb) {}

  float next() {
    s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
    const double t = static_cast<double>(n_++) / cv::kSampleRate;
    const double w = (std::sin(2.0 * kPi * 3.1 * t) + std::sin(2.0 * kPi * 7.3 * t + 1.0) + std::sin(2.0 * kPi * 11.7 * t + 2.0)) / 3.0;
    const double db = centerDb_ + 1.5 * w;
    return static_cast<float>(((s_ >> 40) & 1ULL ? 1.0 : -1.0) * std::pow(10.0, db / 20.0));
  }

 private:
  double centerDb_;
  unsigned long long s_ = 0x9E3779B97F4A7C15ULL;
  long n_ = 0;
};

struct Excursion {
  float minGain = 0.0f;
  float maxGain = -80.0f;
  int risesToOpen = 0;  // crossings of -40 dB upward
  int fallsToClosed = 0;
};

// Feeds 10 s of wobbling noise and records how the gain moved.
Excursion runNoise(Probe& r, const cv::GateParams& p, double centerDb) {
  WobbleNoise noise(centerDb);
  Excursion e;
  e.minGain = r.gate.gainDb();
  e.maxGain = r.gate.gainDb();
  bool wasOpen = r.gate.gainDb() > -40.0f;
  for (int n = 0; n < 10 * cv::kSampleRate; ++n) {
    r.step(noise.next(), p);
    const float g = r.gate.gainDb();
    e.minGain = std::fmin(e.minGain, g);
    e.maxGain = std::fmax(e.maxGain, g);
    const bool open = g > -40.0f;
    if (open && !wasOpen) ++e.risesToOpen;
    if (!open && wasOpen) ++e.fallsToClosed;
    wasOpen = open;
  }
  return e;
}

// Noise whose level wobbles +-1.5 dB, which is the width of the 3 dB hysteresis, leaves the gate
// where it was for 10 s, open or closed. The same noise 2 dB over the threshold opens it once and it
// stays open.
bool testInputGateNoise() {
  const cv::GateParams p = boxInputGate(0.52f, 0.0f);  // -54 dBFS, the shortest release
  const double thr = static_cast<double>(p.thresholdDb);
  bool ok = true;

  Probe open;
  open.gate.reset();
  for (int n = 0; n < cv::kSampleRate / 2; ++n) open.step(static_cast<float>(dbfs(thr + 6.0)), p);  // 6 dB over: open
  const Excursion held = runNoise(open, p, thr);
  report("input gate noise open", held.minGain >= -0.001f && held.fallsToClosed == 0,
         "open, noise at threshold +-1.5 dB for 10 s: gain min %.4f dB, %.0f closings (0)", static_cast<double>(held.minGain), static_cast<double>(held.fallsToClosed));
  ok = ok && held.minGain >= -0.001f && held.fallsToClosed == 0;

  Probe closed;
  closed.gate.reset();
  ok = ok && closed.settleClosed(p);
  const Excursion shut = runNoise(closed, p, thr - 2.0);
  report("input gate noise closed", shut.maxGain <= -79.9f && shut.risesToOpen == 0,
         "closed, noise at threshold -2 dB +-1.5 dB for 10 s: gain max %.3f dB, %.0f openings (0); a hard range never lets a partial gain through",
         static_cast<double>(shut.maxGain), static_cast<double>(shut.risesToOpen));
  ok = ok && shut.maxGain <= -79.9f && shut.risesToOpen == 0;

  Probe over;
  over.gate.reset();
  ok = ok && over.settleClosed(p);
  const Excursion up = runNoise(over, p, thr + 2.0);
  report("input gate noise over", up.risesToOpen == 1 && up.fallsToClosed == 0 && over.gate.gainDb() > -0.001f,
         "noise at threshold +2 dB +-1.5 dB: %.0f opening, %.0f closings, final gain %.4f dB (1, 0, 0)",
         static_cast<double>(up.risesToOpen), static_cast<double>(up.fallsToClosed), static_cast<double>(over.gate.gainDb()));
  ok = ok && up.risesToOpen == 1 && up.fallsToClosed == 0 && over.gate.gainDb() > -0.001f;
  return ok;
}

// The detector peak is a plain linear value the interrupt can keep: no division, restarted on a take.
// The detector holds a level for up to two window segments, so the second take waits them out.
bool testDetectorPeakWindow() {
  Probe r;
  r.gate.reset();
  cv::GateParams p = boxInputGate(0.5f, 0.0f);
  for (int n = 0; n < 480; ++n) r.step(static_cast<float>(0.25 * std::sin(2.0 * kPi * 1000.0 * n / cv::kSampleRate)), p);
  const float first = r.gate.takeDetectorPeak();
  for (int n = 0; n < 3000; ++n) r.step(0.0f, p);
  r.gate.takeDetectorPeak();
  for (int n = 0; n < 480; ++n) r.step(0.0f, p);
  const float second = r.gate.takeDetectorPeak();
  const bool ok = first > 0.2f && first <= 0.25f && second < first && second > 0.0f && r.gate.takeDetectorPeak() == 0.0f;
  return report("detector peak", ok, "a 0.25 sine reads a peak of %.3f, the later window %.3f, then 0", static_cast<double>(first),
                static_cast<double>(second));
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testCloses();
  ok &= testTiming();
  ok &= testHysteresis();
  ok &= testHeldNote();
  ok &= testGainMatchesLibm();
  ok &= testNoAlloc();
  ok &= testCallSizes();
  ok &= testLevelEdges();
  ok &= testTravelTimes();
  ok &= testGateAttack();
  ok &= testOnOffFade();
  ok &= testBadSamples();
  ok &= testHoldClamp();
  ok &= testInputGateDecayPot();
  ok &= testInputGateThresholdPot();
  ok &= testInputGateBurst();
  ok &= testInputGateNoise();
  ok &= testDetectorPeakWindow();
  return ok ? 0 : 1;
}
