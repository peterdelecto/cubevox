// The Formant engine's base and voice sitting on one lag, and its joining of Autotune's grain pass directly
// behind the Autotune card, which behind a settled-off Autotune equals the card alone bit for bit on Adam's
// clips. No window, no audio device.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "clip_util.h"
#include "engine/autotune_fx.h"
#include "engine/formant_follower.h"
#include "engine/formant_fx.h"
#include "ringed_tracker.h"

namespace {

using Signal = std::vector<float>;

constexpr double kPi = 3.14159265358979323846;

// Ten harmonics at 1/k amplitude, peak 0.5, with a 5 Hz vibrato of 20 cents.
Signal voice(double hz, int n) {
  Signal x(static_cast<size_t>(n));
  double phase = 0.0;
  double peak = 0.0;
  std::vector<double> y(x.size());
  for (int i = 0; i < n; ++i) {
    const double t = static_cast<double>(i) / cv::kSampleRate;
    double s = 0.0;
    for (int k = 1; k <= 10; ++k) s += std::sin(k * phase) / k;
    phase += 2.0 * kPi * hz * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0) / cv::kSampleRate;
    y[static_cast<size_t>(i)] = s;
    peak = std::max(peak, std::fabs(s));
  }
  for (size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(0.5 * y[i] / peak);
  return x;
}

template <typename F>
Signal run(const Signal& in, F&& block) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    block(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, i);
  }
  return out;
}

bool report(const char* name, bool ok, double a, double b = 0.0) {
  std::printf("%s %s  %.2f %.2f\n", ok ? "PASS" : "FAIL", name, a, b);
  return ok;
}

// Normalised correlation of a with b delayed by lag samples, over [from, to).
double corrAt(const Signal& a, const Signal& b, int lag, size_t from, size_t to) {
  double ab = 0.0;
  double aa = 0.0;
  double bb = 0.0;
  for (size_t i = from; i < to; ++i) {
    const double x = a[i];
    const double y = b[i - static_cast<size_t>(lag)];
    ab += x * y;
    aa += x * x;
    bb += y * y;
  }
  return ab / std::sqrt(aa * bb + 1e-30);
}

// At SHIFT 0 and MIX 50 % the voice rebuilds the base, so the output is the input delayed by the lag.
bool testBase() {
  bool ok = true;
  const double hz = 220.0;
  const Signal in = voice(hz, cv::kSampleRate * 2);
  auto eng = std::make_unique<cv::FormantFx>();
  const Signal out = run(in, [&](const float* i, float* o, int n, int) {
    cv::FormantParams p;
    p.on = true;
    p.shiftSt = 0;
    p.mix = 0.5f;
    eng->process(i, o, n, p);
  });
  const size_t from = static_cast<size_t>(cv::kSampleRate);
  const size_t to = out.size();
  const int want = static_cast<int>(eng->lagForTest() + 0.5f);
  double best = -1.0;
  int bestLag = 0;
  for (int lag = want - 8; lag <= want + 8; ++lag) {
    const double c = corrAt(out, in, lag, from, to);
    if (c > best) {
      best = c;
      bestLag = lag;
    }
  }
  ok &= report("formant base and voice sit on one lag at SHIFT 0", best > 0.98 && std::abs(bestLag - want) <= 3, best,
               static_cast<double>(bestLag - want));
  const double lagMs = eng->lagForTest() / cv::kSmpPerMs;
  ok &= report("formant on its own tracker lags the grain delay plus a period",
               std::fabs(lagMs - (1040.0 + 48000.0 / hz) / 48.0) < 0.5, lagMs);
  return ok;
}

// Directly behind Autotune the engine joins Autotune's grain pass: it reads Autotune's tracker, cuts
// Autotune's input at Autotune's delay, and at SHIFT 0 hands back Autotune's output with no lag of its own.
bool testGroup() {
  bool ok = true;
  const double hz = 220.0;
  const Signal in = voice(hz, cv::kSampleRate * 2);
  auto at = std::make_unique<cv::AutotuneFx>();
  auto fm = std::make_unique<cv::FormantFx>();
  Signal mid(in.size());
  const Signal out = run(in, [&](const float* i, float* o, int n, int pos) {
    cv::AutotuneParams ap;
    ap.on = true;
    ap.responseMs = 1.0f;
    cv::TrackerNeeds need;
    need.run = true;
    need.fine = true;
    need.outOfRange = true;
    fm->feed(i, n);
    at->process(i, &mid[static_cast<size_t>(pos)], n, ap, need, fm->gridMarks());
    cv::UpstreamPitch up;
    up.result = &at->pitch();
    up.shiftSemis = at->correctionSemis();
    up.latency = at->outputLatency();
    up.group = at.get();
    cv::FormantParams fp;
    fp.on = true;
    fp.shiftSt = 0;
    fp.mix = 1.0f;
    fm->process(&mid[static_cast<size_t>(pos)], o, n, fp, up);
  });
  const double period = 48000.0 / hz;
  const double grainLag = 1040.0 + period;
  ok &= report("formant behind autotune reads at autotune's delay", std::fabs(fm->lagForTest() - grainLag) < 3.0,
               fm->lagForTest(), grainLag);
  const size_t from = static_cast<size_t>(cv::kSampleRate);
  double best = -1.0;
  int bestLag = 0;
  for (int lag = -8; lag <= 8; ++lag) {
    const double c = lag >= 0 ? corrAt(out, mid, lag, from, out.size()) : corrAt(mid, out, -lag, from, out.size());
    if (c > best) {
      best = c;
      bestLag = lag;
    }
  }
  ok &= report("formant behind autotune lands with autotune's output", best > 0.98 && bestLag == 0, best,
               static_cast<double>(bestLag));
  ok &= report("formant behind autotune ran no tracker of its own", fm->ownPitch().period == 0.0f, fm->ownPitch().period);
  ok &= report("formant behind autotune adds none of its lag to the delay it hands on", fm->outputLatency() < 0.5f,
               fm->outputLatency());
  return ok;
}

// Without the Autotune engine the same call runs the engine on its own ring and its own tracker, and lags by
// the grain delay plus a period.
bool testNoGroupWithoutEngine() {
  bool ok = true;
  const double hz = 220.0;
  const Signal in = voice(hz, cv::kSampleRate * 2);
  auto at = std::make_unique<cv::AutotuneFx>();
  auto fm = std::make_unique<cv::FormantFx>();
  Signal mid(in.size());
  run(in, [&](const float* i, float* o, int n, int pos) {
    cv::AutotuneParams ap;
    ap.on = true;
    cv::TrackerNeeds need;
    need.run = true;
    at->process(i, &mid[static_cast<size_t>(pos)], n, ap, need);
    cv::UpstreamPitch up;
    up.result = &at->pitch();
    cv::FormantParams fp;
    fp.on = true;
    fp.mix = 1.0f;
    fm->process(&mid[static_cast<size_t>(pos)], o, n, fp, up);
  });
  ok &= report("formant given a tracker and no engine runs its own", fm->ownPitch().period > 0.0f && !fm->groupedForTest(),
               fm->ownPitch().period);
  return ok;
}

// Behind a settled-off Autotune the grouped engine runs its ratio 1 voice alone, on Autotune's tracker, and
// equals the engine on its own ring and its own tracker, bit for bit. The grouped run is the chain's: the
// Autotune slot feeds the card and runs its engine for the tracker, hands on the dry input, and its slot
// gains are 0.
bool testSettledOffGroupEqualsAlone() {
  bool whole = true;
  const std::vector<Signal> takes = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (!whole || takes.empty()) return report("settled-off group equals the card alone: Adam's clips missing", false, 0.0);
  bool ok = true;
  for (size_t c = 0; c < takes.size(); ++c) {
    for (int st : {-7, 0, 4}) {
      for (float mix : {0.5f, 1.0f}) {
        auto at = std::make_unique<cv::AutotuneFx>();
        auto grouped = std::make_unique<cv::FormantFx>();
        auto alone = std::make_unique<cv::FormantFx>();
        cv::FormantParams fp;
        fp.on = true;
        fp.shiftSt = st;
        fp.mix = mix;
        Signal scratch(cv::kBlock);
        Signal a(takes[c].size());
        const Signal g = run(takes[c], [&](const float* i, float* o, int n, int pos) {
          cv::TrackerNeeds need{true, true, true};
          grouped->feed(i, n);
          at->process(i, scratch.data(), n, cv::AutotuneParams{}, need, grouped->gridMarks());
          const cv::UpstreamPitch up{&at->pitch(), 0.0f, 0.0f, at.get(), 0.0f, 0.0f};
          grouped->process(i, o, n, fp, up);
          alone->process(i, &a[static_cast<size_t>(pos)], n, fp);
        });
        size_t differ = 0;
        for (size_t k = 0; k < g.size(); ++k) differ += g[k] != a[k] ? 1u : 0u;
        char name[96];
        std::snprintf(name, sizeof name, "settled-off group equals the card alone, clip %zu SHIFT %+d MIX %.0f %%", c + 1, st,
                      100.0 * mix);
        ok &= report(name, differ == 0 && grouped->groupedForTest(), static_cast<double>(differ));
      }
    }
  }
  return ok;
}

// The pair, Autotune with Formant or Apraxia directly behind it, against the two cards run apart: Autotune,
// then the card on Autotune's output with its own ring and tracker. Both hard-tuned to C major, MIX 100 %.
struct PairRun {
  Signal pair;
  Signal apart;
  Signal tuned;  // Autotune's own output, the card apart's input
};

PairRun runPair(const Signal& in, int shiftSt, bool apraxia) {
  auto at = std::make_unique<cv::AutotuneFx>();
  auto fm = std::make_unique<cv::FormantFx>();
  auto atApart = std::make_unique<cv::AutotuneFx>();
  auto fmApart = std::make_unique<cv::FormantFx>();
  cv::FormantFollower follow;
  cv::FormantFollower followApart;
  follow.reset();
  followApart.reset();
  cv::AutotuneParams ap;
  ap.on = true;
  cv::FormantParams fp;
  fp.on = true;
  fp.shiftSt = shiftSt;
  fp.mix = 1.0f;
  cv::FormantFollowerParams ff;
  ff.placed = apraxia;
  ff.rangeSt = static_cast<float>(shiftSt);
  Signal mid(cv::kBlock);
  PairRun r{Signal(in.size()), Signal(in.size()), Signal(in.size())};
  for (size_t i = 0; i + cv::kBlock <= in.size(); i += cv::kBlock) {
    const float* x = &in[i];
    fm->feed(x, cv::kBlock);
    at->process(x, mid.data(), cv::kBlock, ap, cv::TrackerNeeds{true, true, true}, fm->gridMarks());
    const cv::UpstreamPitch up{&at->pitch(), at->correctionSemis(), at->outputLatency(), at.get(), 1.0f, 1.0f};
    fm->process(mid.data(), &r.pair[i], cv::kBlock, follow.follow(mid.data(), cv::kBlock, fp, ff), up);
    float* tuned = &r.tuned[i];
    atApart->process(x, tuned, cv::kBlock, ap);
    fmApart->process(tuned, &r.apart[i], cv::kBlock, followApart.follow(tuned, cv::kBlock, fp, ff));
  }
  return r;
}

// The tracked pitch of x at every block, 0 where the frame is unvoiced.
std::vector<double> pitchTrack(const Signal& x) {
  auto t = std::make_unique<RingedTracker>();
  std::vector<double> hz;
  for (size_t i = 0; i + cv::kBlock <= x.size(); i += cv::kBlock) {
    t->push(&x[i], cv::kBlock);
    const cv::PitchResult& r = t->result();
    hz.push_back(r.voiced && r.period > 0.0f ? cv::kSampleRate / static_cast<double>(r.period) : 0.0);
  }
  return hz;
}

// How far the two tracks sit apart over the frames both call voiced, the second shifted by the block offset
// that lines them up best (the card apart plays its own lag later): the median and the 90th percentile of
// |cents|.
struct PitchGap {
  double median = 1e9;
  double p90 = 1e9;
};

PitchGap pitchGap(const std::vector<double>& a, const std::vector<double>& b) {
  PitchGap best;
  for (int off = 0; off <= 40; ++off) {
    std::vector<double> c;
    for (size_t k = 0; k + static_cast<size_t>(off) < b.size() && k < a.size(); ++k) {
      const double x = a[k];
      const double y = b[k + static_cast<size_t>(off)];
      if (x > 0.0 && y > 0.0) c.push_back(std::fabs(1200.0 * std::log2(x / y)));
    }
    if (c.size() < 100) continue;
    std::sort(c.begin(), c.end());
    const PitchGap g{c[c.size() / 2], c[c.size() * 9 / 10]};
    if (g.median < best.median) best = g;
  }
  return best;
}

// The spectral envelope as the level in dB of each third-octave band from 200 Hz to 5 kHz, of the Welch
// power spectrum past the first half second, less the mean over the bands. The bands are wider than the
// harmonic spacing over most of the range, so they read the envelope a formant move shifts, and taking the
// mean out leaves level aside.
std::vector<double> bandEnvelope(const Signal& x) {
  constexpr size_t kSize = 4096;
  const std::vector<double> p = testutil::spectrum(x, static_cast<size_t>(cv::kSampleRate / 2), x.size(), kSize);
  std::vector<double> bands;
  for (double lo = 200.0; lo * 1.26 <= 5000.0; lo *= 1.26) {
    double e = 1e-30;
    for (size_t k = 0; k < p.size(); ++k) {
      const double hz = static_cast<double>(k) * cv::kSampleRate / static_cast<double>(kSize);
      if (hz >= lo && hz < lo * 1.26) e += p[k];
    }
    bands.push_back(10.0 * std::log10(e));
  }
  double mean = 0.0;
  for (double v : bands) mean += v;
  mean /= static_cast<double>(bands.size());
  for (double& v : bands) v -= mean;
  return bands;
}

// The mean |dB| between two band envelopes.
double envelopeGap(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0.0;
  for (size_t k = 0; k < a.size(); ++k) s += std::fabs(a[k] - b[k]);
  return s / static_cast<double>(a.size());
}

// Owner rule (root CLAUDE.md, chain order): the pair's output is what the two cards in slot order would give,
// cut once instead of twice. On Adam's three clips and a voice 37 cents sharp of D3, at SHIFT (Apraxia: RANGE)
// -7, 0 and +4, MIX 100 % (the throat on the clips only):
// 1. Same note: the pair's tracked pitch sits within 7 cents of the cards apart at the median of the frames
//    both call voiced, and within 35 cents at the 90th percentile. A tracker reads a voice whose formants
//    moved less exactly: the cards apart read 0.4 to 20 cents (median) and 6 to 40 cents (90th) off
//    Autotune's own output, and the pair reads within a few cents of that, so a tighter bar would test the
//    tracker. SHIFT 0 reads within 0.7 / 6 cents.
// 2. Same throat: the pair's third-octave envelope sits within 1 dB of the cards apart on average over the
//    bands, while a Formant SHIFT of -7 or +4 st moves it by more than twice that, so the measure sees a
//    formant move.
bool testPairMatchesCardsApart() {
  bool whole = true;
  std::vector<Signal> takes = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (!whole || takes.empty()) return report("pair matches the cards apart: Adam's clips missing", false, 0.0);
  // Adam sings near the key's notes, so his clips ask little of the correction. A voice 37 cents sharp of D3
  // makes the pair carry Autotune's correction plainly.
  const size_t kVoiceTake = takes.size();
  takes.push_back(voice(150.0, 3 * cv::kSampleRate));
  bool ok = true;
  for (bool apraxia : {false, true}) {
    for (size_t c = 0; c < takes.size(); ++c) {
      std::vector<double> flat;
      for (int st : {0, -7, 4}) {
        const PairRun r = runPair(takes[c], st, apraxia);
        const std::vector<double> tracked = pitchTrack(r.tuned);
        const PitchGap g = pitchGap(pitchTrack(r.pair), pitchTrack(r.apart));
        const PitchGap gp = pitchGap(tracked, pitchTrack(r.pair));
        const PitchGap ga = pitchGap(tracked, pitchTrack(r.apart));
        std::printf("NOTE %s %+d clip %zu, cents off Autotune's own output (median / 90th): pair %.2f / %.2f, apart %.2f / %.2f\n",
                    apraxia ? "Apraxia RANGE" : "Formant SHIFT", st, c + 1, gp.median, gp.p90, ga.median, ga.p90);
        const std::vector<double> envPair = bandEnvelope(r.pair);
        const double gap = envelopeGap(envPair, bandEnvelope(r.apart));
        if (st == 0) flat = envPair;
        const double moved = st == 0 ? 0.0 : envelopeGap(envPair, flat);
        char name[120];
        std::snprintf(name, sizeof name, "%s pair, clip %zu, %s %+d: same note as the cards apart (median, p90 cents)",
                      apraxia ? "Apraxia" : "Formant", c + 1, apraxia ? "RANGE" : "SHIFT", st);
        ok &= report(name, g.median <= 7.0 && g.p90 <= 35.0, g.median, g.p90);
        if (c == kVoiceTake) continue;  // ten harmonics to 1.5 kHz leave the bands above empty: no envelope to read
        std::snprintf(name, sizeof name, "%s pair, clip %zu, %s %+d: same throat (envelope gap dB, moved from 0)",
                      apraxia ? "Apraxia" : "Formant", c + 1, apraxia ? "RANGE" : "SHIFT", st);
        ok &= report(name, gap <= 1.0 && (st == 0 || apraxia || moved > 2.0 * gap), gap, moved);
      }
    }
  }
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testBase();
  ok &= testGroup();
  ok &= testNoGroupWithoutEngine();
  ok &= testSettledOffGroupEqualsAlone();
  ok &= testPairMatchesCardsApart();
  return ok ? 0 : 1;
}
