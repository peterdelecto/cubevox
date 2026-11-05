// Autotune checks: passthrough, scale degrees, correction, out-of-scale notes,
// response time, Octave builds on the corrected note, hard tune, the toggle and
// its level, the halfway point, the tracker's range ends, the first note's glide,
// bleed, the plain copy after it goes off, call sizes, no-alloc. No window, no
// audio device.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "alloc_guard.h"
#include "pitch_series.h"
#include "ringed_tracker.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kA3 = 220.0;
constexpr double kSharp = 226.4;   // A3 + 50 cents
constexpr double kSharpLow = 100.87;  // G2 + 50 cents, in Adam's register
constexpr double kBb3 = 233.1;
constexpr double kB3 = 246.94;
constexpr double kC4 = 261.63;
constexpr int kSettle = cv::kSampleRate * 3 / 10;  // 300 ms
constexpr double kOctaveVibratoCents = 5.5;  // the voice reads 2.7 following the correction, 10.4 behind a 15 ms glide

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

// Arms the allocation guard for the process() call that follows and counts it, so the closing line
// reports how many calls ran under the guard.
int gGuardedCalls = 0;

void armGuard() {
  gInProcess = true;
  ++gGuardedCalls;
}

// Median f0 after 300 ms, measured by a fresh tracker.
double medianHz(const std::vector<float>& x) {
  RingedTracker t;
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
  bool voiced;
  float hz;
  float corr;
};

// Runs in through fx in calls of callSize samples, recording a Hop after each call.
std::vector<float> run(PitchSeries& fx, const std::vector<float>& in, const cv::PitchFxParams& p,
                       std::vector<Hop>* hops = nullptr, int callSize = cv::kBlock) {
  std::vector<float> out(in.size());
  if (hops) hops->reserve(in.size() / static_cast<size_t>(callSize) + 1);
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += callSize) {
    const int n = std::min(callSize, total - i);
    armGuard();
    fx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
    if (hops) hops->push_back({i + n, fx.pitch().voiced, fx.pitch().hz, fx.correctionSemis()});
  }
  return out;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0, double d = 0.0) {
  char detail[400];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static PitchSeries gFx;

// Autotune alone in key C; Octave off.
cv::PitchFxParams autotuneOnly(float responseMs) {
  cv::PitchFxParams p;
  p.autotune.on = true;
  p.autotune.key = 0;
  p.autotune.responseMs = responseMs;
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

// 1. Off from the start is bit-exact. On with an in-tune A3, the output matches the input delayed by
// the voice's latency, the grain delay plus one period, and scaled by the level trim. That delay is not
// a whole number of samples and a steady tone matches at every period of lag, so the test takes the
// best whole-sample lag within 500 samples of kGrainDelay and prints only the residual.
bool testPassthrough() {
  const std::vector<float> in = tone(kA3, cv::kSampleRate);
  cv::PitchFxParams off = autotuneOnly(60.0f);
  off.autotune.on = false;
  gFx.reset();
  const int diff = countDiff(in, run(gFx, in, off), 0);
  bool ok = report("passthrough off", diff == 0, "samples differing=%.0f (0)", diff);

  gFx.reset();
  const std::vector<float> out = run(gFx, in, autotuneOnly(60.0f));
  const int from = kSettle;
  const int to = static_cast<int>(in.size());
  const double trim = std::pow(10.0, static_cast<double>(cv::AutotuneTuning{}.trimDb) / 20.0);
  const double ref = rms(in, from, to);
  double bestDb = 999.0;
  for (int lag = cv::PsolaVoice::kGrainDelay - 500; lag <= cv::PsolaVoice::kGrainDelay + 500; ++lag) {
    double s = 0.0;
    for (int i = from; i < to; ++i) {
      const double d = out[static_cast<size_t>(i)] - trim * in[static_cast<size_t>(i - lag)];
      s += d * d;
    }
    const double db = 20.0 * std::log10(std::sqrt(s / (to - from)) / ref + 1e-12);
    bestDb = std::min(bestDb, db);
  }
  ok &= report("passthrough in tune", bestDb <= -40.0,
               "|out - in| RMS=%.1f dB re input at the best lag within 500 samples of the grain delay (<= -40)",
               bestDb);
  return ok;
}

// 2. Scale notes: every semitone of C major from C4 to B4 in key C pulls to the scale note below or at
// it. A scale note is its own note; a note between two scale notes sits a tie away from both and snaps
// to the lower one.
bool testScaleNotes() {
  constexpr int kNoteOf[12] = {0, 0, 2, 2, 4, 5, 5, 7, 7, 9, 9, 11};  // semitones above C4
  constexpr float kDevOf[12] = {0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0};
  int bad = 0;
  for (int s = 0; s < 12; ++s) {
    const float midi = cv::midiOf(261.6256f * std::exp2(static_cast<float>(s) / 12.0f));
    const float dev = cv::scaleDeviation(midi, 0);
    if (std::fabs(midi - dev - (60.0f + static_cast<float>(kNoteOf[s]))) > 1e-3f || std::fabs(dev - kDevOf[s]) > 1e-3f)
      ++bad;
  }
  // G major (KEY position 1) holds F#, so F#4 is its own note there.
  const float fsDev = cv::scaleDeviation(cv::midiOf(369.994f), 1);
  return report("scale notes", bad == 0 && std::fabs(fsDev) < 1e-3f,
                "wrong notes in C=%.0f (0), F#4 in G off its note by %.4f st (0)", bad, fsDev);
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

// 6b. Octave +12 on a note that hard tune holds on A3 against a 5.5 Hz, +-30 cent vibrato comes out on
// A4. The correction is what cancels the vibrato, so the voice has to follow it as it moves: the
// output pitch's RMS distance from A4, read by a fresh tracker.
bool testOctaveFollowsVibrato() {
  constexpr double kVibratoHz = 5.5;
  constexpr double kA4 = 2.0 * kA3;
  const std::vector<float> in = tone(3 * cv::kSampleRate, [](int i) {
    return kA3 * std::exp2(0.3 / 12.0 * std::sin(2.0 * kPi * kVibratoHz * i / cv::kSampleRate));
  });
  cv::PitchFxParams p = autotuneOnly(cv::AutotuneParams{}.responseMs);
  p.octave.on = true;
  p.octave.mix = 1.0f;
  p.octave.semitones = 12;
  gFx.reset();
  const std::vector<float> out = run(gFx, in, p);
  RingedTracker t;
  double sum = 0.0;
  int n = 0;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    t.push(&out[static_cast<size_t>(i)], std::min(cv::kBlock, total - i));
    if (i > kSettle && t.result().voiced) {
      const double c = cents(t.result().rawHz, kA4);
      sum += c * c;
      ++n;
    }
  }
  const double rmsCents = n > 0 ? std::sqrt(sum / n) : 9999.0;
  return report("octave follows vibrato", rmsCents <= kOctaveVibratoCents,
                "octave +12 on hard-tuned A3 under 5.5 Hz vibrato: %.2f cents RMS from A4 (<= %.1f)", rmsCents,
                kOctaveVibratoCents);
}

// The process() call sizes every call-size case runs: odd, short, the box's block, and longer.
constexpr int kCallSizes[] = {17, 40, cv::kBlock, 200};

int countNonFinite(const std::vector<float>& x) {
  int n = 0;
  for (float v : x) n += std::isfinite(v) ? 0 : 1;
  return n;
}

// 7. Hard tune at the shipped defaults: a scale note with a 5 Hz, +-30 cent vibrato comes out held on
// the note, at every call size. Adam sings from 78 Hz, so the check also runs on the scale notes of his
// register, A2, G2 and E2, where a grain spans two periods of 9 to 12 ms each and the tracker's own
// reading is coarser. Each gate is about twice the note's worst reading over the call sizes (A3 1.07
// cents, A2 2.40, G2 2.59, E2 3.00). Adam's own box measures about 2 cents median off the note.
struct HardTuneNote {
  double hz;
  double maxCents;
};
constexpr HardTuneNote kHardTuneNotes[] = {{kA3, 2.0}, {110.0, 5.0}, {98.0, 5.0}, {82.41, 6.0}};

double hardTuneCents(double noteHz, int callSize, int& nonFinite) {
  const std::vector<float> in = tone(3 * cv::kSampleRate, [noteHz](int i) {
    return noteHz * std::exp2(0.3 / 12.0 * std::sin(2.0 * kPi * 5.0 * i / cv::kSampleRate));
  });
  cv::PitchFxParams p = autotuneOnly(cv::AutotuneParams{}.responseMs);
  gFx.reset();
  const std::vector<float> out = run(gFx, in, p, nullptr, callSize);
  nonFinite += countNonFinite(out);
  RingedTracker t;
  double sum = 0.0;
  int n = 0;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    t.push(&out[static_cast<size_t>(i)], std::min(cv::kBlock, total - i));
    if (i > kSettle && t.result().voiced) {
      const double c = cents(t.result().rawHz, noteHz);
      sum += c * c;
      ++n;
    }
  }
  return n > 0 ? std::sqrt(sum / n) : 9999.0;
}

bool testHardTune() {
  bool ok = true;
  for (const HardTuneNote& note : kHardTuneNotes) {
    for (int size : kCallSizes) {
      int nonFinite = 0;
      const double rmsCents = hardTuneCents(note.hz, size, nonFinite);
      char name[56];
      std::snprintf(name, sizeof name, "hard tune holds vibrato, %g Hz, calls of %d", note.hz, size);
      ok &= report(name, rmsCents <= note.maxCents && nonFinite == 0,
                   "+-30 cent vibrato in, %.2f cents RMS off the note out (<= %.0f), non-finite samples %.0f (0)",
                   rmsCents, note.maxCents, nonFinite);
    }
  }
  return ok;
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

// Largest |x| over [from, to).
double peakLevel(const std::vector<float>& x, int from, int to) {
  double worst = 0.0;
  for (int i = from; i < to; ++i) worst = std::max(worst, std::fabs(static_cast<double>(x[static_cast<size_t>(i)])));
  return worst;
}

// Autotune off for gapBlocks and on again over a held tone halfway to the next semitone (A3 + 50 cents,
// or G2 + 50 cents in Adam's register), in calls of callSize samples; a call that overlaps the gap runs
// off. The output crossfades between the corrected voice and the dry input, so for its level it may
// bend no harder than either does alone. The equal-power crossfade lifts the level where the two voices
// add in phase (the toggle level check bounds that rise), so a fade window that peaks above steady state
// reads against its own peak. Returns the peak curvature from the off edge to 100 ms past the on edge
// over the larger steady peak, so scaled.
double toggleRatio(double hz, int gapBlocks, int callSize, int& nonFinite) {
  const int offAt = cv::kSampleRate / cv::kBlock * cv::kBlock;
  const int onAt = offAt + gapBlocks * cv::kBlock;
  const int total = offAt + 2 * cv::kSampleRate;
  const std::vector<float> in = tone(hz, total);
  std::vector<float> out(in.size());
  const cv::PitchFxParams on = autotuneOnly(20.0f);
  cv::PitchFxParams off = on;
  off.autotune.on = false;
  gFx.reset();
  for (int i = 0; i < total; i += callSize) {
    const int n = std::min(callSize, total - i);
    const bool gap = i < onAt && i + n > offAt;
    armGuard();
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, gap ? off : on);
    gInProcess = false;
  }
  nonFinite += countNonFinite(out);
  const int offCall = offAt / callSize * callSize;                 // the first call that runs off
  const int onCall = (onAt + callSize - 1) / callSize * callSize;  // the first call back on
  const int to = onCall + cv::kSampleRate / 10;
  const int window = cv::kSampleRate / 2;
  const double steady = std::max({peakCurvature(out, offCall - window, offCall),
                                  peakCurvature(out, total - window, total), peakCurvature(in, 0, total)});
  const double steadyPeak = std::max({peakLevel(out, offCall - window, offCall), peakLevel(out, total - window, total),
                                      peakLevel(in, 0, total)});
  const double rise = std::max(1.0, peakLevel(out, offCall, to) / steadyPeak);
  return peakCurvature(out, offCall, to) / steady / rise;
}

// 8. A flick inside the crossfade keeps the running voice; a long gap lets the crossfade settle,
// and the voice then starts clean once the tracker has rebuilt, at every call size. The fade's own
// slope adds under 1 %; a voice that loses its period at the off switch reads 1.4 to 5.4 times
// steady state.
bool testToggle() {
  constexpr double kMaxRatio = 1.05;
  bool ok = true;
  for (double hz : {kSharp, kSharpLow}) {
    for (int size : kCallSizes) {
      double worst = 0.0;
      int worstGap = 0;
      int nonFinite = 0;
      for (int gap : {1, 4, 15, 60, 300}) {
        const double r = toggleRatio(hz, gap, size, nonFinite);
        if (r >= worst) {
          worst = r;
          worstGap = gap;
        }
      }
      char name[40];
      std::snprintf(name, sizeof name, "toggle, %g Hz, calls of %d", hz, size);
      ok &= report(name, worst <= kMaxRatio && nonFinite == 0,
                   "off for 1.3 to 400 ms: peak curvature x%.3f of steady state at %.0f blocks off (<= %.2f), "
                   "non-finite samples %.0f (0)",
                   worst, worstGap, kMaxRatio, nonFinite);
    }
  }
  return ok;
}

// The crossfade's level over steady tones: Autotune on at 0.5 s and off at 1.25 s while the Crowd card
// keeps the tracker running, in two-period windows over the 200 ms after each switch. The dip reads
// against the quieter of the dry input and the corrected voice, the rise against the louder.
struct ToggleLevel {
  double dip = 0.0;
  double rise = -99.0;
};

// With formant true the Formant card sits at SHIFT -3 st and MIX 50 %, and the two steady levels are the
// outputs with Autotune off and on, since the mix moves both.
void toggleLevelAt(double hz, bool formant, ToggleLevel& worst) {
  const int n = 2 * cv::kSampleRate;
  const std::vector<float> in = tone(hz, n);
  cv::PitchFxParams on = autotuneOnly(1.0f);
  gFx.readersOn = true;  // a Mob card keeps the tracker running through the toggle
  if (formant) {
    on.formant.on = true;
    on.formant.shiftSt = -3;
    on.formant.mix = 0.5f;
  }
  cv::PitchFxParams off = on;
  off.autotune.on = false;
  gFx.reset();
  const std::vector<float> wet = run(gFx, in, on);
  gFx.reset();
  const std::vector<float> plain = run(gFx, in, off);
  const double dry = rms(plain, cv::kSampleRate / 4, cv::kSampleRate / 2);
  const double corrected = rms(wet, cv::kSampleRate, cv::kSampleRate * 3 / 2);
  const int onAt = cv::kSampleRate / 2 / cv::kBlock * cv::kBlock;
  const int offAt = cv::kSampleRate * 5 / 4 / cv::kBlock * cv::kBlock;
  std::vector<float> out(in.size());
  gFx.reset();
  for (int i = 0; i < n; i += cv::kBlock) {
    armGuard();
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, i < onAt || i >= offAt ? off : on);
    gInProcess = false;
  }
  gFx.readersOn = false;
  const int win = static_cast<int>(std::lround(2.0 * cv::kSampleRate / hz));
  for (int edge : {onAt, offAt}) {
    for (int i = edge; i + win < edge + cv::kSampleRate / 5; i += 24) {
      const double r = rms(out, i, i + win);
      worst.dip = std::min(worst.dip, 20.0 * std::log10(r / std::min(dry, corrected)));
      worst.rise = std::max(worst.rise, 20.0 * std::log10(r / std::max(dry, corrected)));
    }
  }
}

// 9. Switching Autotune on or off mid-note: the corrected voice lags the dry input by 2 to 5 periods
// and sits up to a semitone away, so the equal-power crossfade dips where the two cancel and rises
// where they add. The dip reads 3.86 dB against a gate of 4.4, the rise 2.34 dB against 3.0. The same
// scan runs with the Formant card at part MIX behind Autotune. The Formant engine cuts from Autotune's
// ring and plays a voice for Autotune's input and one for its corrected voice, both at Autotune's delay,
// so Autotune's crossfade moves it from one voice to the other with no change of delay and no cancellation
// between the two. The dip reads 1.03 dB and the rise 0.31 dB, and the Formant row's gates sit 0.1 dB above
// each (1.13 and 0.41), so a return of the cancellation fails it.
bool testToggleLevel() {
  bool ok = true;
  for (bool formant : {false, true}) {
    const double kMaxDipDb = formant ? 1.13 : 4.4;
    const double kMaxRiseDb = formant ? 0.41 : 3.0;
    ToggleLevel worst;
    for (double hz : {97.0, 98.0, 99.0, 100.0, 101.0, 102.0, 103.0, 104.0, 105.0, 106.0, 110.0, 130.81, 146.83, 220.0})
      toggleLevelAt(hz, formant, worst);
    ok &= report(formant ? "toggle level, Formant -3 st at MIX 50 %" : "toggle level",
                 -worst.dip <= kMaxDipDb && worst.rise <= kMaxRiseDb,
                 "tones 97 to 220 Hz, two-period windows: largest dip %.2f dB (<= %.2f), largest rise %.2f dB (<= %.2f)",
                 -worst.dip, kMaxDipDb, worst.rise, kMaxRiseDb);
  }
  return ok;
}

double midiOf(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

// The output's note per hop, read by a fresh tracker from 500 ms on.
struct Notes {
  double changesPerSec;  // changes of the nearest semitone
  double lo;             // lowest and highest pitch, MIDI
  double hi;
};

Notes outputNotes(const std::vector<float>& out) {
  RingedTracker t;
  int changes = 0, prev = -999;
  double lo = 1e9, hi = -1e9;
  const int total = static_cast<int>(out.size());
  for (int i = 0; i + cv::kBlock <= total; i += cv::kBlock) {
    t.push(&out[static_cast<size_t>(i)], cv::kBlock);
    const int end = i + cv::kBlock;
    if (end < cv::kSampleRate / 2 || end % cv::PitchTracker::kHop != 0 || !t.result().voiced) continue;
    const double m = midiOf(t.result().rawHz);
    lo = std::min(lo, m);
    hi = std::max(hi, m);
    const int note = static_cast<int>(std::lround(m));
    if (prev != -999 && note != prev) ++changes;
    prev = note;
  }
  return {changes / ((total - cv::kSampleRate / 2) / static_cast<double>(cv::kSampleRate)), lo, hi};
}

// A tone at `midi` under a 5 Hz, +-30 cent vibrato.
std::vector<float> vibratoAt(double midi) {
  const double hz = 440.0 * std::exp2((midi - 69.0) / 12.0);
  return tone(3 * cv::kSampleRate, [hz](int i) {
    return hz * std::exp2(0.3 / 12.0 * std::sin(2.0 * kPi * 5.0 * i / cv::kSampleRate));
  });
}

// 10. A pitch near the halfway point between two scale notes holds one note at MECHANICAL, at every
// call size. C#4 15 cents past halfway in KEY C under a 5 Hz, +-30 cent vibrato crosses the halfway
// point ten times a second, which would flip the output between C4 and D4 as often, and so does F#2 in
// Adam's register between F2 and G2. A steady A3 exactly halfway in KEY Eb holds Ab3.
bool testHalfway() {
  const std::vector<float> vib = vibratoAt(61.15);
  const std::vector<float> vibLow = vibratoAt(42.15);
  const std::vector<float> steady = tone(kA3, 3 * cv::kSampleRate);
  bool ok = true;
  for (int size : kCallSizes) {
    cv::PitchFxParams p = autotuneOnly(1.0f);
    gFx.reset();
    const Notes a = outputNotes(run(gFx, vib, p, nullptr, size));
    gFx.reset();
    const Notes low = outputNotes(run(gFx, vibLow, p, nullptr, size));
    char lowName[56];
    std::snprintf(lowName, sizeof lowName, "halfway holds at 93 Hz, calls of %d", size);
    ok &= report(lowName, low.changesPerSec == 0.0, "F#2 +15 c vibrato in C: %.1f note changes a second (0)",
                 low.changesPerSec);
    p.autotune.key = 9;  // Eb / Cm
    gFx.reset();
    const Notes b = outputNotes(run(gFx, steady, p, nullptr, size));
    const bool held = b.changesPerSec == 0.0 && std::fabs(b.lo - 56.0) <= 0.05 && std::fabs(b.hi - 56.0) <= 0.05;
    char name[48];
    std::snprintf(name, sizeof name, "halfway holds, calls of %d", size);
    ok &= report(name, a.changesPerSec == 0.0 && held,
                 "C#4 +15 c vibrato in C: %.1f note changes a second (0); A3 in Eb: %.1f a second (0), out MIDI "
                 "%.2f..%.2f (56 +-0.05)",
                 a.changesPerSec, b.changesPerSec, b.lo, b.hi);
  }
  return ok;
}

// Normalised autocorrelation of x over [from, from + w) against x lag samples ahead.
double ncorr(const std::vector<float>& x, int from, int w, double lag) {
  const int li = static_cast<int>(std::floor(lag));
  const double fr = lag - li;
  double sxy = 0.0, sxx = 0.0, syy = 0.0;
  for (int i = from; i < from + w; ++i) {
    const size_t j = static_cast<size_t>(i + li);
    const double a = x[static_cast<size_t>(i)];
    const double b = (1.0 - fr) * x[j] + fr * x[j + 1];
    sxy += a * b;
    sxx += a * a;
    syy += b * b;
  }
  return sxy / std::sqrt(std::max(sxx * syy, 1e-30));
}

// The pitch of x near nearHz over one second from 600 ms, for pitches the tracker cannot read: the
// best normalised autocorrelation at m periods, m chosen so the lag spans 400 samples or more, in
// quarter samples within half a period of m periods (3 semitones when m is 1).
double periodicHz(const std::vector<float>& x, double nearHz) {
  const double p = cv::kSampleRate / nearHz;
  const int m = std::max(1, static_cast<int>(400.0 / p));
  const double span = m == 1 ? std::exp2(3.0 / 12.0) : 1.0 + 0.45 / m;
  double best = -2.0, bestLag = m * p;
  for (double lag = std::floor(m * p / span); lag <= m * p * span; lag += 0.25) {
    const double r = ncorr(x, cv::kSampleRate * 6 / 10, cv::kSampleRate, lag);
    if (r > best) {
      best = r;
      bestLag = lag;
    }
  }
  return m * cv::kSampleRate / bestLag;
}

// 11. At the ends of the tracker's range the note passes uncorrected, at every call size. 68 Hz reads
// at the slow end of the search and 1,030 Hz at the fast end, both flagged clamped, and the output
// stays on the sung note; the edge reading would pull them 85 cents sharp and 21 cents flat. 1,200 Hz
// reads an octave down, unclamped, and still lands D6. The readings are within 0.4 cents.
bool testRangeEnds() {
  constexpr double kD6 = 1174.66;
  const std::vector<float> lowIn = tone(68.0, 2 * cv::kSampleRate), highIn = tone(1030.0, 2 * cv::kSampleRate),
                           aboveIn = tone(1200.0, 2 * cv::kSampleRate);
  const cv::PitchFxParams p = autotuneOnly(1.0f);
  bool ok = true;
  for (int size : kCallSizes) {
    gFx.reset();
    const double low = cents(periodicHz(run(gFx, lowIn, p, nullptr, size), 68.0), 68.0);
    gFx.reset();
    const double high = cents(periodicHz(run(gFx, highIn, p, nullptr, size), 1030.0), 1030.0);
    gFx.reset();
    const double above = cents(periodicHz(run(gFx, aboveIn, p, nullptr, size), kD6), kD6);
    char name[48];
    std::snprintf(name, sizeof name, "range ends, calls of %d", size);
    ok &= report(name, std::fabs(low) <= 2.0 && std::fabs(high) <= 2.0 && std::fabs(above) <= 2.0,
                 "68 Hz out %+.1f c off the sung note, 1,030 Hz %+.1f c; 1,200 Hz out %+.1f c off D6 (all +-2)", low,
                 high, above);
  }
  return ok;
}

// 12. The first note after a reset glides at RESPONSE like every other note. At 500 ms, A3 + 40 cents
// from a reset and D4 - 30 cents after 250 ms of silence each move the same share of their way in the
// 50 ms after the tracker first reads them voiced, within 10 % (both read 9.6 %). The correction moves
// only on voiced frames, so the second note counts from its voiced frame and not from the onset estimate.
double shareIn50Ms(const std::vector<Hop>& hops, size_t first, double want) {
  const double start = first > 0 ? hops[first - 1].corr : 0.0;
  const size_t at = first + static_cast<size_t>(cv::kSampleRate / 20 / cv::kBlock);
  return (hops[at].corr - start) / (want - start);
}

bool testFirstNote() {
  const int n1 = cv::kSampleRate * 6 / 10, gap = cv::kSampleRate / 4, n2 = cv::kSampleRate * 6 / 10;
  std::vector<float> in = tone(440.0 * std::exp2((57.4 - 69.0) / 12.0), n1);
  in.insert(in.end(), static_cast<size_t>(gap), 0.0f);
  const std::vector<float> second = tone(440.0 * std::exp2((61.7 - 69.0) / 12.0), n2);
  in.insert(in.end(), second.begin(), second.end());
  std::vector<Hop> hops;
  gFx.reset();
  run(gFx, in, autotuneOnly(500.0f), &hops);
  size_t f1 = 0, f2 = 0;
  while (f1 < hops.size() && hops[f1].hz <= 0.0f) ++f1;
  while (f2 < hops.size() && !(hops[f2].end > n1 + gap && hops[f2].voiced && hops[f2].hz > 280.0f)) ++f2;
  if (f2 + cv::kSampleRate / 20 / cv::kBlock >= hops.size()) return report("first note glides", false, "no second note", 0.0);
  const double s1 = shareIn50Ms(hops, f1, -0.4), s2 = shareIn50Ms(hops, f2, 0.3);
  return report("first note glides", std::fabs(s1 - s2) <= 0.1 * s2,
                "RESPONSE 500 ms, share of the way in 50 ms: first note %.1f %%, next note after a gap %.1f %% "
                "(within 10 %%)",
                100.0 * s1, 100.0 * s2);
}

// A3 held for 3 s at hard tune or the given RESPONSE, KEY 4 (E, which holds A) until keyAt and KEY 5
// (B, which does not: A3 pulls down a semitone to G#) from then on, with the sung pitch stepping up
// `cents` at pitchAt. A keyAt or pitchAt of 3 s or more never comes. Returns the correction per call
// of 64 samples.
std::vector<Hop> keyRun(float responseMs, int keyAt, int pitchAt, double cents) {
  const int total = 3 * cv::kSampleRate;
  const std::vector<float> in = tone(total, [=](int i) { return i < pitchAt ? kA3 : kA3 * std::exp2(cents / 1200.0); });
  cv::PitchFxParams p = autotuneOnly(responseMs);
  std::vector<float> out(in.size());
  std::vector<Hop> hops;
  gFx.reset();
  for (int i = 0; i < total; i += cv::kBlock) {
    p.autotune.key = i < keyAt ? 4 : 5;
    armGuard();
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);
    gInProcess = false;
    hops.push_back({i + cv::kBlock, gFx.pitch().voiced, gFx.pitch().hz, gFx.correctionSemis()});
  }
  return hops;
}

// Milliseconds from `from` to the first call whose correction, less `base`, has covered `share` of
// `move`; -1 if none does.
double msToCover(const std::vector<Hop>& hops, const std::vector<Hop>* base, int from, double move, double share) {
  for (size_t h = 0; h < hops.size(); ++h) {
    if (hops[h].end <= from) continue;
    const double c = hops[h].corr - (base ? base->at(h).corr : 0.0);
    if (std::fabs(c) >= share * std::fabs(move)) return (hops[h].end - from) * 1000.0 / cv::kSampleRate;
  }
  return -1.0;
}

// 13. A KEY change while a note sounds glides the note to the new key, and hard tune holds through
// the glide. At hard tune the change from KEY 4 to 5 moves a held A3 down a semitone, covering 99 % of
// it in 150 to 350 ms (it reads 232). A sung step up of 30 cents 62 ms into the glide is followed as
// quickly as one with no KEY change at all, within 3 ms of it (both read 63 % of the step at the same
// call). At RESPONSE 500 ms the glide runs at RESPONSE: 63 % of the move in 450 to 550 ms.
bool testKeyGlide() {
  constexpr int kKeyAt = cv::kSampleRate * 3 / 2, kPitchAt = kKeyAt + 3000, kNever = 3 * cv::kSampleRate;
  const std::vector<Hop> move = keyRun(1.0f, kKeyAt, kNever, 0.0);
  const double covered = msToCover(move, nullptr, kKeyAt, -1.0, 0.99);
  const std::vector<Hop> plain = keyRun(1.0f, kNever, kPitchAt, 30.0);
  const std::vector<Hop> during = keyRun(1.0f, kKeyAt, kPitchAt, 30.0);
  const double followPlain = msToCover(plain, nullptr, kPitchAt, -0.3, 0.63);
  const double followDuring = msToCover(during, &move, kPitchAt, -0.3, 0.63);
  const std::vector<Hop> slow = keyRun(500.0f, kKeyAt, kNever, 0.0);
  const double slowMs = msToCover(slow, nullptr, kKeyAt, -1.0, 0.63);
  const bool ok = covered >= 150.0 && covered <= 350.0 && followPlain > 0.0 && followDuring > 0.0 &&
                  std::fabs(followDuring - followPlain) <= 3.0 && slowMs >= 450.0 && slowMs <= 550.0;
  return report("key glide", ok,
                "KEY 4 to 5 on a held A3: 99%% of the semitone in %.0f ms (150..350); a +30 cent step in the glide "
                "reaches 63%% in %.1f ms against %.1f ms with no KEY change (within 3); at RESPONSE 500 ms 63%% of "
                "the move in %.0f ms (450..550)",
                covered, followDuring, followPlain, slowMs);
}

// 14. A KEY change made in a rest has no note to protect and takes effect at once. F4 + 20 cents,
// 0.3 s of silence with the KEY changing from C to G in the middle of it, then the same note: the
// correction reaches within 0.1 st of the new key's +0.80 st at hard tune within 10 ms of the tracker's
// first report of the second note. The same change made while the note sounds takes over 100 ms.
bool testKeyInRest() {
  const double kHz = 440.0 * std::exp2((65.2 - 69.0) / 12.0);
  const int noteLen = cv::kSampleRate * 6 / 10, gap = cv::kSampleRate * 3 / 10;
  std::vector<float> in = tone(kHz, noteLen);
  in.insert(in.end(), static_cast<size_t>(gap), 0.0f);
  const std::vector<float> second = tone(kHz, noteLen);
  in.insert(in.end(), second.begin(), second.end());
  const int keyAt = noteLen + gap / 2;
  cv::PitchFxParams p = autotuneOnly(1.0f);
  std::vector<float> out(in.size());
  std::vector<Hop> hops;
  gFx.reset();
  for (int i = 0; i + cv::kBlock <= static_cast<int>(in.size()); i += cv::kBlock) {
    p.autotune.key = i < keyAt ? 0 : 1;
    armGuard();
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);
    gInProcess = false;
    hops.push_back({i + cv::kBlock, gFx.pitch().voiced, gFx.pitch().hz, gFx.correctionSemis()});
  }
  size_t first = 0;
  while (first < hops.size() && !(hops[first].end > noteLen + gap && hops[first].voiced)) ++first;
  size_t settled = first;
  while (settled < hops.size() && std::fabs(hops[settled].corr - 0.8f) > 0.1f) ++settled;
  if (settled == hops.size()) return report("key change in a rest", false, "the correction never reached +0.8 st", 0.0);
  const double ms = (hops[settled].end - hops[first].end) * 1000.0 / cv::kSampleRate;
  const std::vector<Hop> mid = keyRun(1.0f, cv::kSampleRate * 3 / 2, 3 * cv::kSampleRate, 0.0);
  const double midMs = msToCover(mid, nullptr, cv::kSampleRate * 3 / 2, -1.0, 0.9);
  return report("key change in a rest", ms <= 10.0 && midMs > 100.0,
                "second note tuned %.1f ms after the tracker's first report (<= 10); the same change mid-note "
                "takes %.0f ms to cover 90%% (> 100)",
                ms, midMs);
}

// 15. reset() ends a KEY glide. The KEY 4 to 5 change of a held A3 is reset 100 ms into its glide, with
// most of the semitone still to go. The correction then settles on the new key's -1 st within 10 ms
// of the tracker's first report, as it does for any first note, with nothing of the old note's side
// left over.
bool testKeyReset() {
  constexpr int kKeyAt = cv::kSampleRate * 3 / 2, kResetAt = kKeyAt + cv::kSampleRate / 10 / cv::kBlock * cv::kBlock;
  const int total = 3 * cv::kSampleRate;
  const std::vector<float> in = tone(kA3, total);
  cv::PitchFxParams p = autotuneOnly(1.0f);
  std::vector<float> out(in.size());
  std::vector<Hop> hops;
  gFx.reset();
  for (int i = 0; i < total; i += cv::kBlock) {
    p.autotune.key = i < kKeyAt ? 4 : 5;
    if (i == kResetAt) gFx.reset();
    armGuard();
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], cv::kBlock, p);
    gInProcess = false;
    hops.push_back({i + cv::kBlock, gFx.pitch().voiced, gFx.pitch().hz, gFx.correctionSemis()});
  }
  size_t first = 0;
  while (first < hops.size() && !(hops[first].end > kResetAt && hops[first].voiced)) ++first;
  size_t settled = first;
  while (settled < hops.size() && std::fabs(hops[settled].corr + 1.0f) > 0.1f) ++settled;
  if (settled == hops.size()) return report("key glide reset", false, "the correction never reached -1 st", 0.0);
  const double ms = (hops[settled].end - hops[first].end) * 1000.0 / cv::kSampleRate;
  return report("key glide reset", ms <= 10.0,
                "after a reset in the glide the correction is within 0.1 st of -1 st %.1f ms after the tracker's "
                "first report (<= 10)",
                ms);
}

// Pink noise from a seeded LCG through Kellet's three-pole filter, as crowd_test's program makes it,
// scaled to rmsDb dBFS.
std::vector<float> pinkNoise(int n, double rmsDb) {
  std::vector<double> x(static_cast<size_t>(n));
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  double b0 = 0.0, b1 = 0.0, b2 = 0.0, sum = 0.0;
  for (double& v : x) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const double w = static_cast<double>(s >> 11) / 4503599627370496.0 - 1.0;
    b0 = 0.99765 * b0 + w * 0.0990460;
    b1 = 0.96300 * b1 + w * 0.2965164;
    b2 = 0.57000 * b2 + w * 1.0526913;
    v = b0 + b1 + b2 + w * 0.1848;
    sum += v * v;
  }
  const double g = std::pow(10.0, rmsDb / 20.0) / std::sqrt(sum / n);
  std::vector<float> out(x.size());
  for (size_t i = 0; i < x.size(); ++i) out[i] = static_cast<float>(x[i] * g);
  return out;
}

// 13. A wrong tracker frame stays out of the target, at every call size. A3 at -20 dBFS with pink
// noise 5 dB under reads a period three times too long on some hops, which in KEY E lands halfway
// between two notes; the target takes the median there instead. Voiced hops pulled 0.2 st or more off
// the in-tune A3 over 3 s read 16 against a gate of 18.
bool testBleed() {
  const int n = 3 * cv::kSampleRate;
  std::vector<float> x = tone(kA3, n);
  const float toneGain = static_cast<float>(std::pow(10.0, -20.0 / 20.0) / rms(x, 0, n));
  const std::vector<float> noise = pinkNoise(n, -25.0);
  for (size_t i = 0; i < x.size(); ++i) x[i] = x[i] * toneGain + noise[i];
  cv::PitchFxParams p = autotuneOnly(1.0f);
  p.autotune.key = 4;  // E / C#m
  bool ok = true;
  for (int size : kCallSizes) {
    std::vector<Hop> hops;
    gFx.reset();
    run(gFx, x, p, &hops, size);
    // Each hop is read at the first call that ends on or after its edge.
    int pulled = 0;
    int edge = (kSettle + cv::PitchTracker::kHop - 1) / cv::PitchTracker::kHop * cv::PitchTracker::kHop;
    for (const Hop& h : hops) {
      if (h.end < edge) continue;
      edge = (h.end / cv::PitchTracker::kHop + 1) * cv::PitchTracker::kHop;
      if (h.voiced && std::fabs(h.corr) >= 0.2f) ++pulled;
    }
    char name[32];
    std::snprintf(name, sizeof name, "bleed, calls of %d", size);
    ok &= report(name, pulled <= 18,
                 "A3 with pink noise 5 dB under, KEY E: %.0f voiced hops pulled 0.2 st or more (<= 18)", pulled);
  }
  return ok;
}

// 14. A stage switched off ends as a plain copy of its input (engine/CLAUDE.md). Autotune runs on a held
// A3 + 50 cents for 1 s, so the corrected voice differs from the input, then goes off. From 300 ms
// after the off edge every output sample equals its input, with no tolerance.
bool testSwitchedOff() {
  const int offAt = cv::kSampleRate / cv::kBlock * cv::kBlock;
  const int total = offAt + cv::kSampleRate;
  const std::vector<float> in = tone(kSharp, total);
  const cv::PitchFxParams on = autotuneOnly(1.0f);
  cv::PitchFxParams off = on;
  off.autotune.on = false;
  std::vector<float> out(in.size());
  gFx.reset();
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    armGuard();
    gFx.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, i < offAt ? on : off);
    gInProcess = false;
  }
  int differingOn = 0, differingOff = 0;
  for (int i = offAt - kSettle; i < offAt; ++i)
    differingOn += out[static_cast<size_t>(i)] != in[static_cast<size_t>(i)] ? 1 : 0;
  for (int i = offAt + kSettle; i < total; ++i)
    differingOff += out[static_cast<size_t>(i)] != in[static_cast<size_t>(i)] ? 1 : 0;
  return report("switched off is a plain copy", differingOn > 0 && differingOff == 0,
                "samples differing in the 300 ms while on=%.0f (> 0), from 300 ms after off=%.0f (0)", differingOn,
                differingOff);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testScaleNotes();
  ok &= testCorrection();
  ok &= testScale();
  ok &= testResponse();
  ok &= testOctaveFollows();
  ok &= testOctaveFollowsVibrato();
  ok &= testHardTune();
  ok &= testToggle();
  ok &= testToggleLevel();
  ok &= testHalfway();
  ok &= testRangeEnds();
  ok &= testFirstNote();
  ok &= testKeyGlide();
  ok &= testKeyInRest();
  ok &= testKeyReset();
  ok &= testBleed();
  ok &= testSwitchedOff();
  // An allocation inside a guarded process() aborts the run, so reaching this line with calls counted
  // means none happened.
  ok &= report("no allocation", gGuardedCalls > 0, "%.0f process() calls ran under the new/delete guard (> 0)",
               gGuardedCalls);
  std::printf("%s autotune\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
