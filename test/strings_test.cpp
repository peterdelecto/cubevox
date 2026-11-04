// Strings card checks: off is a bit-exact copy, the output is finite at every knob corner and in silence, each
// of the twelve strings rings within 3 cents of its note (chromatic from A2, and the major scale of three
// keys from the root at or above 110 Hz), DECAY's T60 lands within 20 % of the knob at 0.5, 2 and 8 s, a key
// change leaves no click, and the level at the defaults (DECAY 2 s, MIX 40 %) on Adam's clips. The notes the
// strings should ring on are worked out here, apart from the engine. `strings_test --bench` prints the default
// card's cost. The clip rows skip when samples/ is absent. No window, no audio device.

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

// `key` is an Autotune KEY index, or -1 for the chromatic scale.
Signal runStrings(cv::Strings& s, const Signal& in, const cv::StringsParams& p, int key, size_t chunk = 256) {
  Signal out(in.size());
  for (size_t i = 0; i < in.size(); i += chunk) {
    const int n = static_cast<int>(std::min(chunk, in.size() - i));
    s.process(&in[i], &out[i], n, p, key);
  }
  return out;
}

// The twelve notes' frequencies for `key`, by walking up the semitones from the first note at or above
// 110 Hz whose pitch class is the key's root (A2 for the chromatic scale), keeping the scale's pitch classes.
std::vector<double> expectedHz(int key) {
  std::vector<double> hz;
  const int root = key < 0 ? 9 : cv::kKeyRoot[static_cast<size_t>(key)];
  int midi = 45;
  while (midi % 12 != root) ++midi;
  for (; hz.size() < 12; ++midi) {
    const int degree = (midi - root + 120) % 12;
    const bool inScale = key < 0 || std::find(cv::kMajor.begin(), cv::kMajor.end(), degree) != cv::kMajor.end();
    if (inScale) hz.push_back(440.0 * std::pow(2.0, (midi - 69) / 12.0));
  }
  return hz;
}

// Silence for kLead, which the strings take to build, then a 5 ms noise burst, then silence to `seconds`
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
  const bool exact = runStrings(never, in, stringsAt(2000.0f, 0.4f, false), -1) == in;

  cv::Strings flip;
  Signal out(in.size() * 2);
  Signal both = in;
  both.insert(both.end(), in.begin(), in.end());
  const size_t offAt = in.size();
  for (size_t i = 0; i < both.size(); i += cv::kBlock) {
    flip.process(&both[i], &out[i], cv::kBlock, stringsAt(8000.0f, 1.0f, i < offAt), 3);
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
  const Signal silence(static_cast<size_t>(kRate), 0.0f);
  bool ok = true;
  float peak = 0.0f;
  for (const float decay : {500.0f, 8000.0f}) {
    for (const float mix : {0.0f, 1.0f}) {
      for (const int key : {-1, 0, 7}) {
        for (const Signal* in : {&noise, &silence}) {
          cv::Strings s;
          const Signal out = runStrings(s, *in, stringsAt(decay, mix), key);
          ok = ok && allFinite(out);
          peak = std::fmax(peak, testutil::peakOf(out));
        }
      }
    }
  }
  char detail[96];
  std::snprintf(detail, sizeof detail, "DECAY 0.5 and 8 s, MIX 0 and 100 %%, 3 scales, noise, silence; peak %.2f (< 8)",
                static_cast<double>(peak));
  return report("finite at the corners", ok && peak < 8.0f, detail);
}

// Each string alone rings within 3 cents of its note. The burst enters one string, and the ring after 0.3 s
// is read over 1 s. The scale is the card's own report of what it tuned, held against the notes above.
bool checkTuning() {
  bool ok = true;
  double worstCents = 0.0;
  int measured = 0;
  const Signal in = burst(1.5);
  for (const int key : {-1, 0, 1, 6}) {
    const std::vector<double> want = expectedHz(key);
    for (int i = 0; i < 12; ++i) {
      cv::Strings s;
      s.soloForTest(i);
      const Signal out = runStrings(s, in, stringsAt(8000.0f, 1.0f), key);
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
  std::snprintf(detail, sizeof detail, "%d strings over 4 scales, worst %.2f cents (3)", measured, worstCents);
  return report("strings are in tune", ok, detail);
}

// DECAY's T60 from the drop of one string's fundamental between two windows, extended to 60 dB.
bool checkDecay() {
  bool ok = true;
  char detail[160];
  int len = 0;
  detail[0] = '\0';
  for (const float decayMs : {500.0f, 2000.0f, 8000.0f}) {
    const double t60 = decayMs / 1000.0;
    const double hz = expectedHz(-1)[4];
    cv::Strings s;
    s.soloForTest(4);
    const Signal out = runStrings(s, burst(t60 + 1.0), stringsAt(decayMs, 1.0f), -1);
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

// A key change mid-ring joins smoothly. The sample-to-sample step of the wet signal in the 50 ms after the
// change stays under the step of the steady ring before it, and the old bank is gone 60 ms later.
bool checkRetune() {
  const Signal excite = testutil::noise(static_cast<size_t>(kRate / 2), 0.4f);
  Signal in = excite;
  in.resize(static_cast<size_t>(2 * kRate), 0.0f);
  cv::Strings s;
  Signal out(in.size());
  const size_t changeAt = static_cast<size_t>(kRate);
  bool retiringSeen = false;
  for (size_t i = 0; i < in.size(); i += cv::kBlock) {
    s.process(&in[i], &out[i], cv::kBlock, stringsAt(8000.0f, 1.0f), i < changeAt ? 0 : 5);
    if (i >= changeAt && s.retiring()) retiringSeen = true;
  }
  const auto stepOf = [&](size_t from, size_t to) {
    float step = 0.0f;
    for (size_t i = from + 1; i < to; ++i) step = std::fmax(step, std::fabs(out[i] - out[i - 1]));
    return step;
  };
  const float before = stepOf(changeAt - 4800, changeAt);
  const float across = stepOf(changeAt - 64, changeAt + 2400);
  const bool settled = !s.retiring() && s.tunedKey() == 5;
  char detail[112];
  std::snprintf(detail, sizeof detail, "step across the change %.5f against %.5f before it; old bank gone: %s",
                static_cast<double>(across), static_cast<double>(before), settled ? "yes" : "no");
  return report("key change has no click", retiringSeen && settled && across <= before * 1.05f, detail);
}

// The level on Adam's clips. The wet path alone (MIX 100 %) at DECAY 2 s is held to the rule over the mean of
// the clips and the default (MIX 40 %) to the rule on every clip, for the chromatic scale and for key C.
bool checkLevel() {
  bool whole = true;
  const std::vector<Signal> sets = clips::load(CUBEVOX_SAMPLES_DIR, whole);
  if (sets.empty()) {
    std::printf("SKIP level rows  samples/ is not in this checkout\n");
    return true;
  }
  bool ok = whole;
  for (const int key : {-1, 0}) {
    double wetSum = 0.0;
    double midSum = 0.0;
    for (const Signal& padded : sets) {
      cv::Strings wet;
      const Signal w = runStrings(wet, padded, stringsAt(2000.0f, 1.0f), key);
      cv::Strings mid;
      const Signal m = runStrings(mid, padded, stringsAt(2000.0f, 0.4f), key);
      const double in = testutil::rmsOf(padded, clips::kPad, padded.size());
      const double wetDb = 20.0 * std::log10(testutil::rmsOf(w, clips::kPad, w.size()) / in);
      const double midDb = 20.0 * std::log10(testutil::rmsOf(m, clips::kPad, m.size()) / in);
      wetSum += wetDb;
      midSum += midDb;
      ok = ok && midDb >= -1.5 && midDb <= 1.0;
      std::printf("NOTE strings level (%s) clip: wet only %+.2f dB, default %+.2f dB (default held to -1.5..+1.0)\n",
                  key < 0 ? "chromatic" : "key C", wetDb, midDb);
    }
    const double wetMean = wetSum / static_cast<double>(sets.size());
    const double midMean = midSum / static_cast<double>(sets.size());
    const bool wetOk = key < 0 ? wetMean >= -1.5 && wetMean <= 1.0 : wetMean >= 0.0 && wetMean <= 0.5;
    const bool midOk = midMean >= -1.5 && midMean <= 1.0;
    ok = ok && wetOk && midOk;
    std::printf("%s strings level (%s) means: wet only %+.2f dB (%s), default %+.2f dB (-1.5..+1.0)\n",
                wetOk && midOk ? "PASS" : "FAIL", key < 0 ? "chromatic" : "key C", wetMean,
                key < 0 ? "-1.5..+1.0" : "0..+0.5, the default layout", midMean);
  }
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
        const int key = (i / static_cast<size_t>(kRate)) % 2 == 0 ? 0 : 5;  // a key change every second
        const auto t0 = std::chrono::steady_clock::now();
        s->process(&padded[i], &out[i], cv::kBlock, p, key);
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        spent += ns;
        slowest = std::fmax(slowest, ns);
      }
      best = std::fmin(best, spent / static_cast<double>(padded.size()));
      worst = std::fmin(worst, slowest);
    }
  }
  std::printf("strings bench: %.2f ns a sample, %.0f ns the costliest block (a key change, both banks running) at DECAY 2 s, "
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
  ok &= checkDecay();
  ok &= checkRetune();
  ok &= checkLevel();
  return ok ? 0 : 1;
}
