// Sympath (Strings engine) checks: off is a bit-exact copy, the output is finite at every knob corner and in
// silence, each of the eight strings rings within 3 cents of its note, a held 220 Hz tone puts the strings on A
// and E over four octaves, harmonic tones and a vibrato find their pitch class, a tone that moves every 100 ms
// never retunes while a step to a new held note retunes once, the wet is silent before the first held note and
// a pause keeps the tuning, DECAY's T60 lands within 20 % of the knob at 0.5, 2 and 8 s, a retune leaves no
// click, and the level at the defaults (DECAY 2 s, MIX 40 %) on Adam's clips. The notes the strings should
// ring on are worked out here, apart from the engine. `strings_test --bench` prints the default card's cost.
// The clip rows skip when samples/ is absent. No window, no audio device.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "engine/strings.h"

#include "clip_util.h"
#include "dft_util.h"
#include "signal_util.h"

namespace {

using testutil::Signal;
constexpr int kRate = cv::kSampleRate;

bool report(const char* name, bool ok, const char* detail) {
  std::printf("%s %-28s %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

cv::StringsParams stringsAt(float decayMs, float mix, bool on = true) {
  cv::StringsParams p;
  p.on = on;
  p.decayMs = decayMs;
  p.mix = mix;
  return p;
}

Signal runStrings(cv::Strings& s, const Signal& in, const cv::StringsParams& p, size_t chunk = cv::kBlock) {
  Signal out(in.size());
  for (size_t i = 0; i < in.size(); i += chunk) {
    const int n = static_cast<int>(std::min(chunk, in.size() - i));
    s.process(&in[i], &out[i], n, p);
  }
  return out;
}

// The eight notes' frequencies for the held pitch class `pc` (0 is C): the root at or above 110 Hz and its
// fifth, then the same pair on each of the three octaves above.
std::vector<double> expectedHz(int pc) {
  int midi = 45;
  while (midi % 12 != pc) ++midi;
  std::vector<double> hz;
  for (int i = 0; i < 8; ++i) hz.push_back(440.0 * std::pow(2.0, (midi + 12 * (i / 2) + 7 * (i % 2) - 69) / 12.0));
  return hz;
}

double midiHz(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

void append(Signal& to, const Signal& more) { to.insert(to.end(), more.begin(), more.end()); }

Signal tone(double hz, double seconds, float peak = 0.3f) {
  return testutil::sine(static_cast<size_t>(seconds * kRate), hz, peak);
}

Signal silence(double seconds) { return Signal(static_cast<size_t>(seconds * kRate), 0.0f); }

// A voice-like tone: six harmonics at 1/k, scaled to `peak`.
Signal buzz(double hz, double seconds, float peak = 0.3f, double vibratoSemis = 0.0) {
  Signal x(static_cast<size_t>(seconds * kRate));
  double phase = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double f = hz * std::pow(2.0, vibratoSemis * std::sin(2.0 * testutil::kPi * 5.5 * t) / 12.0);
    phase += 2.0 * testutil::kPi * f / kRate;
    double v = 0.0;
    for (int k = 1; k <= 6; ++k) v += std::sin(k * phase) / k;
    x[i] = peak * static_cast<float>(v / 1.8);
  }
  return x;
}

// Silence for kLead, which the card needs for nothing, then a 5 ms noise burst, then silence to `seconds`
// after the burst.
constexpr double kLead = 0.1;
Signal burst(double seconds) {
  Signal x(static_cast<size_t>((kLead + seconds) * kRate), 0.0f);
  const Signal n = testutil::noise(static_cast<size_t>(kRate / 200), 0.5f);
  std::copy(n.begin(), n.end(), x.begin() + static_cast<long>(kLead * kRate));
  return x;
}

bool checkOff() {
  const Signal in = testutil::noise(static_cast<size_t>(kRate), 0.3f);
  cv::Strings never;
  const bool exact = runStrings(never, in, stringsAt(2000.0f, 0.4f, false)) == in;

  cv::Strings flip;
  Signal out(in.size() * 2);
  Signal both = in;
  both.insert(both.end(), in.begin(), in.end());
  const size_t offAt = in.size();
  for (size_t i = 0; i < both.size(); i += cv::kBlock) {
    flip.process(&both[i], &out[i], cv::kBlock, stringsAt(8000.0f, 1.0f, i < offAt));
  }
  size_t differ = 0;
  for (size_t i = offAt + static_cast<size_t>(kRate / 10); i < both.size(); ++i) differ += out[i] != both[i] ? 1 : 0;
  char detail[96];
  std::snprintf(detail, sizeof detail, "never on and 100 ms after a switch-off: %zu samples differ (0)", differ);
  return report("off is a copy", exact && differ == 0, detail);
}

bool allFinite(const Signal& x) {
  for (const float v : x) {
    if (!std::isfinite(v)) return false;
  }
  return true;
}

bool checkFinite() {
  const Signal noise = testutil::noise(static_cast<size_t>(kRate), 1.0f);
  Signal stepsBuilt = tone(220.0, 0.4, 1.0f);
  append(stepsBuilt, tone(261.63, 0.4, 1.0f));
  append(stepsBuilt, silence(0.2));
  const Signal& steps = stepsBuilt;
  bool ok = true;
  float peak = 0.0f;
  for (const float decay : {500.0f, 8000.0f}) {
    for (const float mix : {0.0f, 1.0f}) {
      for (const Signal* in : {&noise, &steps}) {
        cv::Strings s;
        const Signal out = runStrings(s, *in, stringsAt(decay, mix));
        ok = ok && allFinite(out);
        peak = std::fmax(peak, testutil::peakOf(out));
      }
      cv::Strings quiet;
      const Signal out = runStrings(quiet, silence(1.0), stringsAt(decay, mix));
      ok = ok && allFinite(out);
    }
  }
  char detail[96];
  std::snprintf(detail, sizeof detail, "DECAY 0.5 and 8 s, MIX 0 and 100 %%, noise, tone steps, silence; peak %.2f (< 20)",
                static_cast<double>(peak));
  return report("finite at the corners", ok && peak < 20.0f, detail);
}

// Each string alone rings within 3 cents of its note. The burst enters one string, and the ring after 0.3 s
// is read over 1 s. The card's own report of what it tuned is held against the notes above within 0.5 cent.
bool checkTuning() {
  bool ok = true;
  double worstCents = 0.0;
  int measured = 0;
  const Signal in = burst(1.5);
  for (const int pc : {9, 0, 1, 8}) {
    const std::vector<double> want = expectedHz(pc);
    for (int i = 0; i < 8; ++i) {
      cv::Strings s;
      s.soloForTest(i);
      s.tuneForTest(pc);
      const Signal out = runStrings(s, in, stringsAt(8000.0f, 1.0f));
      const double hz = want[static_cast<size_t>(i)];
      const double heard = dft::peakHz(out, static_cast<size_t>((kLead + 0.3) * kRate), static_cast<size_t>((kLead + 1.3) * kRate), hz * 0.985,
                                       hz * 1.015, hz * 0.001);
      const double cents = 1200.0 * std::log2(heard / hz);
      const double reported = 1200.0 * std::log2(static_cast<double>(s.noteHz(i)) / hz);
      worstCents = std::fmax(worstCents, std::fabs(cents));
      ok = ok && std::fabs(cents) <= 3.0 && std::fabs(reported) <= 0.5;
      ++measured;
    }
  }
  char detail[96];
  std::snprintf(detail, sizeof detail, "%d strings over 4 held notes, worst %.2f cents (3)", measured, worstCents);
  return report("strings are in tune", ok, detail);
}

// Feeds `in` in blocks and returns the time in ms at which the strings were first tuned to `pc`, or -1.
double tunedAtMs(cv::Strings& s, const Signal& in, int pc, const cv::StringsParams& p) {
  Signal out(in.size());
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    const int n = static_cast<int>(std::min<size_t>(cv::kBlock, in.size() - i));
    s.process(&in[i], &out[i], n, p);
    if (s.tunedKey() == pc) return 1000.0 * static_cast<double>(i + static_cast<size_t>(n)) / kRate;
  }
  return -1.0;
}

// A held 220 Hz tone puts the eight strings on A and E over four octaves, each within 0.5 cent, and the
// tuning is in place within 350 ms of the onset (150 ms of hold, the tracker's window, eight builds).
bool checkHeldTone() {
  cv::Strings s;
  const double at = tunedAtMs(s, tone(220.0, 0.6), 9, stringsAt(2000.0f, 0.4f));
  const std::vector<double> want = expectedHz(9);
  double worst = 0.0;
  for (int i = 0; i < 8; ++i) {
    worst = std::fmax(worst, std::fabs(1200.0 * std::log2(static_cast<double>(s.noteHz(i)) / want[static_cast<size_t>(i)])));
  }
  char detail[112];
  std::snprintf(detail, sizeof detail, "tuned to A at %.0f ms (150..350), 8 strings worst %.3f cents (0.5), %d build", at,
                worst, s.retuneCount());
  return report("held tone sets the strings", at >= 150.0 && at <= 350.0 && worst <= 0.5 && s.retuneCount() == 1, detail);
}

// Harmonic tones from G2 to C5 each find their pitch class, and so does a vibrato of 0.3 semitone either side
// of A3 at 5.5 Hz.
bool checkPitchClasses() {
  int right = 0;
  int total = 0;
  for (int midi = 43; midi <= 72; ++midi) {
    cv::Strings s;
    tunedAtMs(s, buzz(midiHz(midi), 0.6), midi % 12, stringsAt(2000.0f, 0.4f));
    ++total;
    right += s.tunedKey() == midi % 12 ? 1 : 0;
  }
  cv::Strings vib;
  tunedAtMs(vib, buzz(midiHz(57), 0.8, 0.3f, 0.3), 9, stringsAt(2000.0f, 0.4f));
  const bool vibOk = vib.tunedKey() == 9;
  char detail[96];
  std::snprintf(detail, sizeof detail, "%d of %d buzz notes G2..C5 right, vibrato on A3: %s", right, total,
                vibOk ? "right" : "wrong");
  return report("pitch classes are found", right == total && vibOk, detail);
}

// A tone that moves every 100 ms never retunes. A step to C held 150 ms or more retunes once, and the same
// note held again after a pause does not.
bool checkHold() {
  cv::Strings s;
  Signal moving;
  const int steps[] = {57, 59, 60, 62, 64, 65, 67, 69, 71, 72, 70, 68, 66, 64, 62, 60, 58, 56, 54, 52};
  for (const int midi : steps) append(moving, tone(midiHz(midi), 0.1));
  runStrings(s, moving, stringsAt(2000.0f, 0.4f));
  const int afterMoving = s.retuneCount();
  const bool untuned = s.tunedKey() == cv::strings_detail::kUntuned;

  runStrings(s, tone(midiHz(60), 0.4), stringsAt(2000.0f, 0.4f));
  const bool onePlusC = s.retuneCount() == 1 && s.tunedKey() == 0;
  const int atC = s.retuneCount();

  Signal again = silence(0.5);
  append(again, tone(midiHz(60), 0.5));
  append(again, silence(0.3));
  append(again, tone(midiHz(72), 0.5));
  runStrings(s, again, stringsAt(2000.0f, 0.4f));
  const bool noRetune = s.retuneCount() == atC && s.tunedKey() == 0;

  char detail[128];
  std::snprintf(detail, sizeof detail, "moving: %d builds (0); C held: %d build, on C: %s; C again an octave up: %s",
                afterMoving, atC, onePlusC ? "yes" : "no", noRetune ? "no build" : "built");
  return report("hold and retune", afterMoving == 0 && untuned && onePlusC && noRetune, detail);
}

// Before any held note the wet is silent, and a pause keeps the tuning and the strings' ring.
bool checkSilentUntilHeld() {
  cv::Strings s;
  Signal early = testutil::noise(static_cast<size_t>(kRate / 10), 0.3f);
  append(early, tone(220.0, 0.1));
  append(early, silence(0.1));
  append(early, tone(262.0, 0.1));
  const Signal out = runStrings(s, early, stringsAt(2000.0f, 1.0f));
  float peak = 0.0f;
  for (size_t i = static_cast<size_t>(kRate / 10); i < out.size(); ++i) peak = std::fmax(peak, std::fabs(out[i]));
  const bool quiet = peak < 1e-9f && s.tunedKey() == cv::strings_detail::kUntuned;

  cv::Strings t;
  Signal pause = tone(220.0, 0.5);
  append(pause, silence(1.5));
  const Signal outP = runStrings(t, pause, stringsAt(8000.0f, 1.0f));
  const float ring = testutil::peakOf(Signal(outP.begin() + static_cast<long>(1.0 * kRate), outP.end()));
  const bool kept = t.tunedKey() == 9 && t.retuneCount() == 1 && ring > 1e-3f;

  char detail[112];
  std::snprintf(detail, sizeof detail, "before a held note the wet peaks at %.1e (< 1e-9); after a pause tuned to A: %s, ring %.3f",
                static_cast<double>(peak), kept ? "yes" : "no", static_cast<double>(ring));
  return report("silent until held", quiet && kept, detail);
}

// DECAY's T60 from the drop of one string's fundamental between two windows, extended to 60 dB.
bool checkDecay() {
  bool ok = true;
  char detail[160];
  int len = 0;
  detail[0] = '\0';
  for (const float decayMs : {500.0f, 2000.0f, 8000.0f}) {
    const double t60 = decayMs / 1000.0;
    const double hz = expectedHz(9)[2];
    cv::Strings s;
    s.soloForTest(2);
    s.tuneForTest(9);
    const Signal out = runStrings(s, burst(t60 + 1.0), stringsAt(decayMs, 1.0f));
    const double window = std::fmax(0.05, 0.04 * t60);
    const auto at = [&](double centre) {
      const size_t from = static_cast<size_t>((kLead + centre - 0.5 * window) * kRate);
      return dft::magnitude(out, from, from + static_cast<size_t>(window * kRate), hz);
    };
    const double t1 = 0.1 * t60 + 0.5 * window;
    const double t2 = 0.6 * t60 + 0.5 * window;
    const double dropDb = 20.0 * std::log10(at(t1) / at(t2));
    const double measured = 60.0 * (t2 - t1) / dropDb;
    ok = ok && std::fabs(measured / t60 - 1.0) <= 0.2;
    len += std::snprintf(detail + len, sizeof detail - static_cast<size_t>(len), "%s%.2f s reads %.2f s", len ? ", " : "",
                         t60, measured);
  }
  return report("decay is T60 within 20 %", ok, detail);
}

// A retune mid-ring joins smoothly. The sample-to-sample step of the wet signal across the change stays near
// the step of the steady ring before it, and the old bank is gone 200 ms after the swap.
bool checkRetuneClick() {
  Signal in = tone(220.0, 1.0, 0.1f);
  const size_t changeAt = in.size();
  append(in, tone(261.63, 1.0, 0.1f));
  append(in, silence(0.5));
  cv::Strings s;
  Signal out(in.size());
  bool retiringSeen = false;
  size_t goneAt = 0;
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    s.process(&in[i], &out[i], cv::kBlock, stringsAt(8000.0f, 1.0f));
    if (i >= changeAt && s.retiring()) retiringSeen = true;
    if (retiringSeen && !s.retiring() && goneAt == 0) goneAt = i;
  }
  const auto stepOf = [&](size_t from, size_t to) {
    float step = 0.0f;
    for (size_t i = from + 1; i < to; ++i) step = std::fmax(step, std::fabs(out[i] - out[i - 1]));
    return step;
  };
  const float before = stepOf(changeAt - 9600, changeAt);
  const float across = stepOf(changeAt, changeAt + 24000);
  const bool settled = !s.retiring() && s.tunedKey() == 0;
  char detail[128];
  std::snprintf(detail, sizeof detail, "step across the change %.5f against %.5f before it; old bank gone: %s",
                static_cast<double>(across), static_cast<double>(before), settled ? "yes" : "no");
  return report("retune has no click", retiringSeen && settled && across <= before * 1.5f, detail);
}

// The level on Adam's clips. The wet path alone (MIX 100 %) and the default (MIX 40 %), both at DECAY 2 s, are
// held to 0 to +0.5 dB on the mean of the clips, and the default to -1.5 to +1.0 dB on every clip.
bool checkLevel() {
  bool whole = true;
  const std::vector<Signal> sets = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (sets.empty()) {
    std::printf("SKIP level rows  samples/ is not in this checkout\n");
    return true;
  }
  bool ok = whole;
  double wetSum = 0.0;
  double midSum = 0.0;
  for (const Signal& padded : sets) {
    cv::Strings wet;
    const Signal w = runStrings(wet, padded, stringsAt(2000.0f, 1.0f));
    cv::Strings mid;
    const Signal m = runStrings(mid, padded, stringsAt(2000.0f, 0.4f));
    const double in = testutil::rmsOf(padded, clips::kPad, padded.size());
    const double wetDb = 20.0 * std::log10(testutil::rmsOf(w, clips::kPad, w.size()) / in);
    const double midDb = 20.0 * std::log10(testutil::rmsOf(m, clips::kPad, m.size()) / in);
    wetSum += wetDb;
    midSum += midDb;
    ok = ok && midDb >= -1.5 && midDb <= 1.0;
    std::printf("NOTE strings level clip: wet only %+.2f dB, default %+.2f dB (default held to -1.5..+1.0)\n", wetDb, midDb);
  }
  const double wetMean = wetSum / static_cast<double>(sets.size());
  const double midMean = midSum / static_cast<double>(sets.size());
  const bool wetOk = wetMean >= 0.0 && wetMean <= 0.5;
  const bool midOk = midMean >= 0.0 && midMean <= 0.5;
  ok = ok && wetOk && midOk;
  std::printf("%s strings level means: wet only %+.2f dB, default %+.2f dB (both 0..+0.5)\n", wetOk && midOk ? "PASS" : "FAIL",
              wetMean, midMean);
  return report("level at the defaults", ok, "see the NOTE rows");
}

int bench() {
  bool whole = true;
  const std::vector<Signal> sets = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (sets.empty()) return 1;
  double best = 1e30;
  double worst = 1e30;
  for (int pass = 0; pass < 9; ++pass) {
    for (const Signal& padded : sets) {
      auto s = std::make_unique<cv::Strings>();
      Signal out(padded.size());
      const cv::StringsParams p = stringsAt(2000.0f, 0.4f);
      double spent = 0.0;
      double slowest = 0.0;
      for (size_t i = 0; i + cv::kBlock <= padded.size(); i += cv::kBlock) {
        const auto t0 = std::chrono::steady_clock::now();
        s->process(&padded[i], &out[i], cv::kBlock, p);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        spent += ns;
        slowest = std::fmax(slowest, ns);
      }
      best = std::fmin(best, spent / static_cast<double>(padded.size()));
      worst = std::fmin(worst, slowest);
    }
  }
  std::printf("strings bench: %.2f ns a sample, %.0f ns the costliest block (a retune, both banks running) at DECAY 2 s, "
              "MIX 40 %% (minimum of 9 passes per clip); %zu B of state\n",
              best, worst, sizeof(cv::Strings));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) return bench();
  bool ok = true;
  ok &= checkOff();
  ok &= checkFinite();
  ok &= checkTuning();
  ok &= checkHeldTone();
  ok &= checkPitchClasses();
  ok &= checkHold();
  ok &= checkSilentUntilHeld();
  ok &= checkDecay();
  ok &= checkRetuneClick();
  ok &= checkLevel();
  return ok ? 0 : 1;
}
