// Distortion checks: passthrough, switch-off, reset, call sizes, non-finite and huge input, top boost,
// harmonics, level, asymmetry, DC, no-alloc. No window, no audio device.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include "engine/distortion.h"

#include "alloc_guard.h"
#include "distortion_rule.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kAmp = 0.251f;  // -12 dBFS
constexpr float kHz = 220.0f;
constexpr int kSecond = cv::kSampleRate;

std::vector<float> sine(int n) {
  std::vector<float> x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i)
    x[static_cast<size_t>(i)] =
        kAmp * static_cast<float>(std::sin(2.0 * kPi * kHz * i / cv::kSampleRate));
  return x;
}

// Runs the whole signal through process() in kBlock chunks.
std::vector<float> run(cv::Distortion& d, const std::vector<float>& in,
                       const cv::DistortionParams& p) {
  std::vector<float> out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = total - i < cv::kBlock ? total - i : cv::kBlock;
    gInProcess = true;
    d.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

cv::DistortionParams params(float drive) {
  cv::DistortionParams p;
  p.drive = drive;
  return p;
}

// Amplitude of one frequency over [from, to), a whole number of cycles.
double goertzel(const std::vector<float>& x, int from, int to, double hz) {
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  double re = 0.0, im = 0.0;
  for (int i = from; i < to; ++i) {
    const double v = x[static_cast<size_t>(i)];
    re += v * std::cos(w * (i - from));
    im += v * std::sin(w * (i - from));
  }
  return 2.0 * std::sqrt(re * re + im * im) / (to - from);
}

// Harmonic h over the fundamental, in dB, from second 1 to second 2.
double harmonicDb(const std::vector<float>& x, int h) {
  return 20.0 * std::log10(goertzel(x, kSecond, 2 * kSecond, kHz * h) /
                           goertzel(x, kSecond, 2 * kSecond, kHz));
}

double rmsOf(const std::vector<float>& x, int from, int to) {
  double s = 0.0;
  for (int i = from; i < to; ++i) s += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
  return std::sqrt(s / (to - from));
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0,
            double c = 0.0) {
  char detail[160];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

static cv::Distortion gDist;

std::vector<float> render(const cv::DistortionParams& p) {
  gDist.reset();
  return run(gDist, sine(2 * kSecond), p);
}

// DRIVE 0 and the toggle off give the input back bit for bit from the first sample.
bool testPassthrough() {
  const int n = kSecond;
  const std::vector<float> in = sine(n);
  float maxDiff = 0.0f;
  cv::DistortionParams zero = params(0.0f);
  cv::DistortionParams off = params(1.0f);
  off.on = false;
  for (const cv::DistortionParams& p : {zero, off}) {
    gDist.reset();
    const std::vector<float> out = run(gDist, in, p);
    for (int i = 0; i < n; ++i) {
      const float d = std::fabs(out[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
      if (d > maxDiff) maxDiff = d;
    }
  }
  return report("passthrough", maxDiff == 0.0f, "drive 0 and on=false max|out-in|=%.3g (==0)",
                maxDiff);
}

// The box resets an engine on the enable edge of a slot that sat off, and the engine may have been left
// engaged and dirty. After the reset it gives the output a fresh engine gives, bit for bit.
bool testResetFromEngaged() {
  const std::vector<float> in = sine(2 * kSecond);
  const std::vector<float> head(in.begin(), in.begin() + kSecond);
  const std::vector<float> rest(in.begin() + kSecond, in.end());
  cv::DistortionParams engaged = params(0.8f);
  engaged.tone = 0.9f;
  cv::DistortionParams then = params(kDistortionRuleDrive);
  then.tone = 0.2f;
  gDist.reset();
  run(gDist, head, engaged);
  gDist.reset();
  const std::vector<float> got = run(gDist, rest, then);
  cv::Distortion fresh;
  fresh.reset();
  const std::vector<float> want = run(fresh, rest, then);
  size_t differing = 0;
  for (size_t i = 0; i < got.size(); ++i)
    if (got[i] != want[i]) ++differing;
  return report("reset", differing == 0, "after DRIVE 80 %% and TONE 90 %%, a reset engine vs a fresh one: %.0f samples differ (==0)",
                static_cast<double>(differing));
}

// A stage that ran for a second and is then switched off, or turned down to 0, gives the input back
// bit for bit once its tail has rung down. The tail lasts under 0.3 s; the check starts at 0.5 s.
bool testSwitchOff() {
  constexpr int kEngaged = kSecond;
  constexpr int kTail = kSecond / 2;
  const std::vector<float> in = sine(3 * kSecond);
  const std::vector<float> head(in.begin(), in.begin() + kEngaged);
  const std::vector<float> rest(in.begin() + kEngaged, in.end());
  struct Case {
    float drive;
    bool offToggle;
    float afterDrive;
  };
  const Case cases[] = {{0.5f, true, 0.5f}, {1.0f, true, 1.0f}, {0.5f, false, 0.0f}, {1.0f, false, 0.0f}};
  size_t worst = 0;
  bool ok = true;
  for (const Case& c : cases) {
    gDist.reset();
    run(gDist, head, params(c.drive));
    cv::DistortionParams after = params(c.afterDrive);
    after.on = !c.offToggle;
    const std::vector<float> out = run(gDist, rest, after);
    size_t differing = 0;
    for (size_t i = kTail; i < out.size(); ++i)
      if (out[i] != rest[i]) ++differing;
    if (differing > worst) worst = differing;
    if (differing != 0) ok = false;
  }
  return report("switch-off", ok, "off or DRIVE 0 after DRIVE 0.5 and 1: %.0f samples differ from the input after 0.5 s (==0)",
                static_cast<double>(worst));
}

// The stage turns on when DRIVE reaches 5 % and off when it falls under 4 %, and between the two it keeps its
// last state. A stage that was off and sits at 4.5 % stays a plain copy; one that was on and comes down to
// 4.5 % keeps running; under 4 % it rings down to an exact copy. The knob never parks it inside the fade.
bool testEngage() {
  const std::vector<float> in = sine(2 * kSecond);
  auto count = [&](float drive, int from) {
    const std::vector<float> out = run(gDist, in, params(drive));
    size_t differing = 0;
    for (size_t i = static_cast<size_t>(from); i < out.size(); ++i)
      if (out[i] != in[i]) ++differing;
    return differing;
  };
  gDist.reset();
  const size_t offAt45 = count(0.045f, 0);              // fresh: off, so no sample differs
  const size_t on = count(0.06f, kSecond);              // on past 5 %: the second half differs
  const size_t keeps = count(0.045f, kSecond);          // down to 4.5 %: still on
  const size_t off = count(0.039f, kSecond);            // under 4 %: off after the ring-down
  const size_t staysOff = count(0.045f, 0);             // back to 4.5 %: still off
  const bool ok = offAt45 == 0 && on > 0 && keeps > 0 && off == 0 && staysOff == 0;
  char detail[200];
  std::snprintf(detail, sizeof detail,
                "samples differing from the input: 4.5 %% fresh %zu (0), 6 %% %zu (>0), 4.5 %% after 6 %% %zu (>0), 3.9 %% %zu (0), 4.5 %% after that %zu (0)",
                offAt45, on, keeps, off, staysOff);
  std::printf("%s engage  %s\n", ok ? "PASS" : "FAIL", detail);
  return ok;
}

// process() takes any call size. From the same settled state a call of 1, 17, 64, 200 or 1000 samples
// gives the output the 64-sample calls give, bit for bit.
bool testCallSizes() {
  constexpr int kWarm = 3 * kSecond / 2;
  const std::vector<float> in = sine(3 * kSecond);
  const std::vector<float> warm(in.begin(), in.begin() + kWarm);
  const std::vector<float> rest(in.begin() + kWarm, in.end());
  const cv::DistortionParams p = params(0.6f);
  gDist.reset();
  run(gDist, warm, p);
  cv::Distortion ref = gDist;
  const std::vector<float> want = run(ref, rest, p);
  size_t worst = 0;
  const int sizes[] = {1, 17, 64, 200, 1000};
  for (int size : sizes) {
    cv::Distortion d = gDist;
    std::vector<float> got(rest.size());
    const int total = static_cast<int>(rest.size());
    for (int i = 0; i < total; i += size) {
      const int n = total - i < size ? total - i : size;
      gInProcess = true;
      d.process(&rest[static_cast<size_t>(i)], &got[static_cast<size_t>(i)], n, p);
      gInProcess = false;
    }
    size_t differing = 0;
    for (size_t i = 0; i < got.size(); ++i)
      if (got[i] != want[i]) ++differing;
    if (differing > worst) worst = differing;
  }
  return report("call-sizes", worst == 0, "calls of 1/17/64/200/1000 samples vs 64 from a settled state: %.0f samples differ (==0)",
                static_cast<double>(worst));
}

// A NaN input sample, a NaN DRIVE or a NaN TONE does not stick: the output is finite again within
// three blocks of the NaN, and NaN knobs never put a non-finite sample out.
bool testNonFinite() {
  std::vector<float> in = sine(kSecond);
  const size_t bad = static_cast<size_t>(cv::kBlock * 3 + 10);
  in[bad] = std::nanf("");
  gDist.reset();
  const std::vector<float> out = run(gDist, in, params(0.5f));
  bool recovered = true;
  for (size_t i = bad + 3 * cv::kBlock; i < out.size(); ++i)
    if (!std::isfinite(out[i])) recovered = false;

  const std::vector<float> clean = sine(kSecond);
  bool knobs = true;
  for (int which = 0; which < 2; ++which) {
    cv::DistortionParams p = params(which == 0 ? std::nanf("") : 0.5f);
    p.tone = which == 1 ? std::nanf("") : 0.5f;
    gDist.reset();
    for (float v : run(gDist, clean, p))
      if (!std::isfinite(v)) knobs = false;
  }
  return report("non-finite", recovered && knobs, "NaN sample recovers: %.0f, NaN DRIVE and TONE stay finite: %.0f",
                recovered ? 1.0 : 0.0, knobs ? 1.0 : 0.0);
}

// One input sample of NaN, infinity or 1e37 into an engaged stage, at the level rule's DRIVE and at 100 %,
// never puts a non-finite sample out, and the stage is back at the clean run's level within 1.5 s. The
// rails bound the output while the filters ring down from a finite spike.
bool testHugeSample() {
  constexpr size_t kBad = static_cast<size_t>(kSecond / 2);
  const float spikes[] = {std::nanf(""), std::numeric_limits<float>::infinity(), 1e37f};
  const std::vector<float> clean = sine(3 * kSecond);
  bool finite = true;
  double worstDb = 0.0;
  for (const float drive : {kDistortionRuleDrive, 1.0f}) {
    gDist.reset();
    const std::vector<float> want = run(gDist, clean, params(drive));
    for (const float spike : spikes) {
      std::vector<float> in = clean;
      in[kBad] = spike;
      gDist.reset();
      const std::vector<float> got = run(gDist, in, params(drive));
      for (const float v : got)
        if (!std::isfinite(v)) finite = false;
      const double db = 20.0 * std::log10(rmsOf(got, 2 * kSecond, 3 * kSecond) / rmsOf(want, 2 * kSecond, 3 * kSecond));
      if (std::fabs(db) > std::fabs(worstDb)) worstDb = db;
    }
  }
  return report("huge-sample", finite && std::fabs(worstDb) <= 0.1,
                "NaN, inf and 1e37 at DRIVE 30 %% and 100 %%: all output finite: %.0f, level 1.5 s later vs clean worst %+.3f dB (+-0.1)",
                finite ? 1.0 : 0.0, worstDb);
}

// Stage 1 gain before the top boost: the plain exponential sweep, in the same float operations
// as the engine runs it (exp2Fast, which matches libm to 1e-7 relative).
float plainGain1(const cv::DistortionTuning& t, float drive) {
  return t.gain1Min * cv::exp2Fast(log2f(t.gain1Max / t.gain1Min) * drive);
}

// Applies p's tuning to d, so stage1GainAt() reads it.
void prime(cv::Distortion& d, const cv::DistortionParams& p) {
  const std::vector<float> in(static_cast<size_t>(cv::kBlock), 0.0f);
  std::vector<float> out(in.size());
  d.reset();
  d.process(in.data(), out.data(), cv::kBlock, p);
}

// DRIVE 0 to topBoostFrom (the loop's last step, 0.6): the boost term is exactly 0.0f, so the gain law and
// the whole output match the sweep without the boost, sample for sample.
bool testBelowBoost() {
  cv::DistortionParams p = params(0.5f);
  prime(gDist, p);
  const cv::DistortionTuning& t = p.tuning;
  bool law = true;
  for (int k = 0; k <= 60; ++k) {
    const float drive = static_cast<float>(k) / 100.0f;
    if (gDist.stage1GainAt(drive) != plainGain1(t, drive)) law = false;
  }

  const float drives[4] = {0.1f, 0.3f, 0.5f, 0.6f};
  bool audio = true;
  for (float drive : drives) {
    const std::vector<float> boosted = render(params(drive));
    cv::DistortionParams plain = params(drive);
    plain.tuning.topBoostDb = 0.0f;
    const std::vector<float> flat = render(plain);
    for (size_t i = 0; i < boosted.size(); ++i)
      if (boosted[i] != flat[i]) audio = false;
  }
  char detail[160];
  std::snprintf(detail, sizeof detail,
                "gain at DRIVE 0..0.6 equals the plain sweep (==): %s; output at 0.1/0.3/0.5/0.6 equals boost 0 dB (==): %s",
                law ? "yes" : "NO", audio ? "yes" : "NO");
  std::printf("%s below-boost  %s\n", law && audio ? "PASS" : "FAIL", detail);
  return law && audio;
}

// topBoostDb extra stage 1 gain at DRIVE 1, half of it halfway up the ramp, and none of it
// when topBoostDb is 0.
bool testTopBoost() {
  cv::DistortionParams p = params(0.5f);
  prime(gDist, p);
  const cv::DistortionTuning& t = p.tuning;
  const double want = std::pow(10.0, t.topBoostDb / 20.0);
  const double top = gDist.stage1GainAt(1.0f) / plainGain1(t, 1.0f);
  const double mid = gDist.stage1GainAt(0.8f) / plainGain1(t, 0.8f);
  const bool topOk = std::fabs(top / want - 1.0) < 1e-4 && std::fabs(top - 2.0) < 0.01;
  const bool midOk = std::fabs(mid / std::sqrt(want) - 1.0) < 1e-4;

  p.tuning.topBoostDb = 0.0f;
  prime(gDist, p);
  bool offOk = true;
  for (int k = 0; k <= 100; ++k) {
    const float drive = static_cast<float>(k) / 100.0f;
    if (gDist.stage1GainAt(drive) != plainGain1(p.tuning, drive)) offOk = false;
  }
  const bool ok = topOk && midOk && offOk;
  char detail[160];
  std::snprintf(detail, sizeof detail,
                "stage 1 gain vs the plain sweep: DRIVE 1 x%.5f = %.3f dB (x%.5f), DRIVE 0.8 x%.5f; boost 0 dB equals the sweep: %s",
                top, 20.0 * std::log10(top), want, mid, offOk ? "yes" : "NO");
  std::printf("%s top-boost  %s\n", ok ? "PASS" : "FAIL", detail);
  return ok;
}

// The stage 1 gain rises with DRIVE for the default tuning and for the widest boost.
bool testGainMonotonic() {
  bool ok = true;
  for (int wide = 0; wide < 2; ++wide) {
    cv::DistortionParams p = params(0.5f);
    if (wide) {
      p.tuning.topBoostDb = 12.0f;
      p.tuning.topBoostFrom = 0.0f;
    }
    prime(gDist, p);
    float prev = 0.0f;
    for (int k = 0; k <= 1000; ++k) {
      const float g = gDist.stage1GainAt(static_cast<float>(k) / 1000.0f);
      if (!(g > prev)) ok = false;
      prev = g;
    }
  }
  return report("gain-monotonic", ok,
                "stage 1 gain strictly rising over DRIVE 0..1 in %.0f steps, default and 12 dB from 0",
                1000.0);
}

// DRIVE taper (owner 2026-10-02: dead until 50-60 %, too saturated at 100 %). Each drive has a
// 3rd-harmonic target and a +-3 dB band. Drives 0.7 and 1 carry the top boost (owner 2026-10-30).
bool testHarmonics() {
  constexpr int kPoints = 5;
  const float drives[kPoints] = {0.1f, 0.3f, 0.5f, 0.7f, 1.0f};
  const double target[kPoints] = {-40.0, -28.0, -20.0, -14.0, -11.6};
  double h[kPoints];
  bool ok = true;
  for (int k = 0; k < kPoints; ++k) {
    h[k] = harmonicDb(render(params(drives[k])), 3);
    if (std::fabs(h[k] - target[k]) > 3.0) ok = false;
    if (k > 0 && h[k] <= h[k - 1]) ok = false;
  }
  char detail[160];
  std::snprintf(detail, sizeof detail,
                "3rd at drive 0.1/0.3/0.5/0.7/1 = %.1f / %.1f / %.1f / %.1f / %.1f dB (targets -40/-28/-20/-14/-11.6 +-3, rising)",
                h[0], h[1], h[2], h[3], h[4]);
  std::printf("%s harmonics  %s\n", ok ? "PASS" : "FAIL", detail);
  return ok;
}

// The sine's level against its input holds to the stage's readings, so a trim or makeup error of half a
// dB shows. The level rule itself is judged on Adam's clips in level_test.
bool testLevel() {
  const double inRms = rmsOf(sine(kSecond), 0, kSecond);
  const float drives[3] = {0.15f, 0.5f, 1.0f};
  const double want[3] = {3.33, 1.49, -3.76};
  double db[3];
  bool ok = true;
  for (int k = 0; k < 3; ++k) {
    db[k] = 20.0 * std::log10(rmsOf(render(params(drives[k])), kSecond, 2 * kSecond) / inRms);
    if (std::fabs(db[k] - want[k]) > 0.5) ok = false;
  }
  return report("level", ok, "output vs input RMS at drive 0.15/0.5/1 = %.2f / %.2f / %.2f dB (+3.33 / +1.49 / -3.76, +-0.5)",
                db[0], db[1], db[2]);
}

// Drive 0.2 sits just past the clipping onset (about 0.1), where one rail clips first.
bool testAsymmetry() {
  const double asym = harmonicDb(render(params(0.2f)), 2);
  cv::DistortionParams sym = params(0.2f);
  sym.tuning.railAsym = 0.0f;
  const double flat = harmonicDb(render(sym), 2);
  return report("asymmetry", asym >= -46.0 && flat <= -55.0,
                "2nd at drive 0.2: default %.1f dB (>=-46), railAsym 0 %.1f dB (<=-55)", asym, flat);
}

bool testDc() {
  const std::vector<float> out = render(params(1.0f));
  double mean = 0.0;
  for (int i = kSecond; i < 2 * kSecond; ++i) mean += out[static_cast<size_t>(i)];
  mean /= kSecond;
  return report("dc", std::fabs(mean) < 1e-3, "drive 1 mean=%.3g (|mean|<1e-3)", mean);
}

bool testNoAlloc() {
  const std::vector<float> in = sine(cv::kBlock * 8);
  gDist.reset();
  run(gDist, in, params(0.7f));  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

// Above the shelf's corner TONE 1 sits toneMaxDb over TONE 0 at toneMinDb, within 0.5 dB, and TONE at noon
// gives the output of a tuning with no TONE shelf bit for bit.
bool testTone() {
  std::vector<float> in(static_cast<size_t>(2 * kSecond));
  for (size_t i = 0; i < in.size(); ++i)
    in[i] = kAmp * static_cast<float>(std::sin(2.0 * kPi * 8000.0 * static_cast<double>(i) / cv::kSampleRate));
  auto wet = [&](float tone, bool flatTuning) {
    cv::DistortionParams p = params(0.5f);
    p.tone = tone;
    if (flatTuning) p.tuning.toneMinDb = p.tuning.toneMaxDb = 0.0f;
    gDist.reset();
    return run(gDist, in, p);
  };
  const std::vector<float> mid = wet(0.5f, false);
  const bool noonFlat = mid == wet(0.5f, true);
  const double spanDb = 20.0 * std::log10(rmsOf(wet(1.0f, false), kSecond, 2 * kSecond) /
                                          rmsOf(wet(0.0f, false), kSecond, 2 * kSecond));
  const cv::DistortionTuning t;
  const double wantDb = static_cast<double>(t.toneMaxDb - t.toneMinDb);
  return report("tone", noonFlat && std::fabs(spanDb - wantDb) <= 0.5,
                "8 kHz at drive 0.5: tone 1 vs 0 = %.2f dB (want %.1f +-0.5), tone 0.5 equals no TONE shelf (==): %.0f",
                spanDb, wantDb, noonFlat ? 1.0 : 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testPassthrough();
  ok &= testSwitchOff();
  ok &= testResetFromEngaged();
  ok &= testEngage();
  ok &= testCallSizes();
  ok &= testNonFinite();
  ok &= testHugeSample();
  ok &= testBelowBoost();
  ok &= testTopBoost();
  ok &= testGainMonotonic();
  ok &= testHarmonics();
  ok &= testLevel();
  ok &= testAsymmetry();
  ok &= testDc();
  ok &= testTone();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
