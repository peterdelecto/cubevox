// Lo-fi checks: settled off, silence, band corners, honk, crush law, chunking, enable edge, level, no-alloc.
// No window, no audio device. The level check reads Adam's clips from samples/ and skips when they are absent.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "engine/lofi.h"

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
constexpr int kSecond = cv::kSampleRate;

using Signal = std::vector<float>;

bool report(const char* name, bool ok, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

class Lcg {
 public:
  double next() {
    s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(s_ >> 11) / 4503599627370496.0 - 1.0;
  }

 private:
  uint64_t s_ = 0x9E3779B97F4A7C15ULL;
};

Signal noise(int n, float amp) {
  Lcg rng;
  Signal x(static_cast<size_t>(n));
  for (float& v : x) v = amp * static_cast<float>(rng.next());
  return x;
}

Signal sine(int n, double hz, float amp) {
  Signal x(static_cast<size_t>(n));
  for (size_t i = 0; i < x.size(); ++i)
    x[i] = amp * static_cast<float>(std::sin(2.0 * kPi * hz * static_cast<double>(i) / cv::kSampleRate));
  return x;
}

// Runs the signal through process() in chunks of `chunk` samples.
Signal run(cv::Lofi& l, const Signal& in, const cv::LofiParams& p, int chunk = cv::kBlock) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += chunk) {
    const int n = total - i < chunk ? total - i : chunk;
    gInProcess = true;
    l.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

double rmsOf(const Signal& x, size_t from, size_t to) {
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(to - from));
}

// Amplitude of one frequency over [from, to).
double goertzel(const Signal& x, size_t from, size_t to, double hz) {
  const double w = 2.0 * kPi * hz / cv::kSampleRate;
  double re = 0.0, im = 0.0;
  for (size_t i = from; i < to; ++i) {
    const double v = x[i];
    const double ph = w * static_cast<double>(i - from);
    re += v * std::cos(ph);
    im += v * std::sin(ph);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

static cv::Lofi gLofi;

// The tuning with the drive, the honk and the makeup out, so the sweep sees the band alone.
cv::LofiParams bandOnly(float tone, float amount) {
  cv::LofiParams p;
  p.tone = tone;
  p.amount = amount;
  p.tuning.honkDb = 0.0f;
  p.tuning.driveDb = 0.0f;
  for (auto& row : p.tuning.makeupDb) row.fill(0.0f);
  p.tuning.trimDb = 0.0f;
  return p;
}

// Gain of the engine at one frequency, from a -40 dBFS sine, so the tanh stays linear.
double gainAt(const cv::LofiParams& p, double hz) {
  const int n = kSecond / 3;
  const Signal in = sine(n, hz, 0.01f);
  gLofi.reset();
  const Signal out = run(gLofi, in, p);
  const size_t from = static_cast<size_t>(n / 2);
  return goertzel(out, from, in.size(), hz) / goertzel(in, from, in.size(), hz);
}

constexpr double kMinus3Db = 0.70794578;

// The frequency in [lo, hi] where the gain crosses -3 dB, by bisection on a log scale. rising is true
// for the high-pass flank.
double cornerHz(const cv::LofiParams& p, double lo, double hi, bool rising) {
  for (int i = 0; i < 16; ++i) {
    const double mid = std::sqrt(lo * hi);
    const bool above = gainAt(p, mid) > kMinus3Db;
    if (above == rising) hi = mid;
    else lo = mid;
  }
  return std::sqrt(lo * hi);
}

bool testSettledOff() {
  const Signal in = noise(2 * kSecond, 0.4f);
  cv::LofiParams p;
  p.on = false;
  gLofi.reset();
  const Signal never = run(gLofi, in, p);
  size_t differNever = 0;
  for (size_t i = 0; i < in.size(); ++i) differNever += never[i] != in[i];

  // On for 0.5 s, then off: from 0.5 s after the off edge the output is the input.
  cv::LofiParams on;
  gLofi.reset();
  run(gLofi, Signal(in.begin(), in.begin() + kSecond / 2), on);
  const Signal rest = run(gLofi, Signal(in.begin() + kSecond / 2, in.end()), p);
  size_t differOn = 0, differAfter = 0;
  for (size_t i = 0; i < rest.size(); ++i) {
    const bool differs = rest[i] != in[i + static_cast<size_t>(kSecond / 2)];
    if (i < static_cast<size_t>(kSecond / 2)) differOn += differs;
    else differAfter += differs;
  }
  return report("settled off", differNever == 0 && differAfter == 0 && differOn > 0,
                "never on: %.0f samples differ (0); 0.5 s after the off edge: %.0f differ (0)",
                static_cast<double>(differNever), static_cast<double>(differAfter));
}

bool testSilence() {
  const Signal zero(static_cast<size_t>(kSecond / 2), 0.0f);
  const float settings[][2] = {{0.5f, 0.5f}, {0.0f, 1.0f}, {1.0f, 1.0f}, {0.5f, 0.0f}, {0.2f, 0.75f}};
  size_t nonzero = 0;
  for (const auto& s : settings) {
    cv::LofiParams p;
    p.tone = s[0];
    p.amount = s[1];
    gLofi.reset();
    const Signal out = run(gLofi, zero, p);
    for (float v : out) nonzero += v != 0.0f;
  }
  return report("silence", nonzero == 0, "zeros in give %.0f nonzero samples out over 5 settings (0)",
                static_cast<double>(nonzero));
}

bool testBandCorners() {
  bool ok = true;
  double worst = 0.0;
  for (const float tone : {0.0f, 0.5f, 1.0f}) {
    const cv::LofiParams p = bandOnly(tone, 0.5f);
    const double fc = 600.0 * std::pow(4.0, tone);
    const double hp = cornerHz(p, fc / 4.0, fc, true);
    const double lp = cornerHz(p, fc, fc * 4.0, false);
    const double hpErr = hp / (fc / 2.0) - 1.0, lpErr = lp / (fc * 2.0) - 1.0;
    worst = std::max({worst, std::fabs(hpErr), std::fabs(lpErr)});
    ok &= std::fabs(hpErr) <= 0.05 && std::fabs(lpErr) <= 0.05;
    std::printf("     tone %.1f: high-pass -3 dB at %.0f Hz (law %.0f), low-pass at %.0f Hz (law %.0f)\n", tone, hp,
                fc / 2.0, lp, fc * 2.0);
  }
  return report("band corners", ok, "AMOUNT 50 %%: worst corner error %.1f %% (<= 5)", 100.0 * worst);
}

bool testHonk() {
  cv::LofiParams withHonk = bandOnly(0.5f, 0.5f);
  withHonk.tuning.honkDb = 6.0f;
  const cv::LofiParams without = bandOnly(0.5f, 0.5f);
  const double fc = 1200.0;
  const double db = 20.0 * std::log10(gainAt(withHonk, fc) / gainAt(without, fc));
  cv::LofiParams low = withHonk;
  low.amount = 0.0f;
  cv::LofiParams lowWithout = without;
  lowWithout.amount = 0.0f;
  const double dbLow = 20.0 * std::log10(gainAt(low, fc) / gainAt(lowWithout, fc));
  return report("honk", std::fabs(db - 6.0) <= 0.3 && std::fabs(dbLow) <= 0.1,
                "honk at the centre: AMOUNT 50 %% %.2f dB (6 +-0.3), AMOUNT 0 %.2f dB (0 +-0.1)", db, dbLow);
}

bool testCrushLaw() {
  const cv::LofiTuning t;
  const cv::Lofi::Law full = cv::Lofi::lawAt(0.5f, 1.0f, t);
  const cv::Lofi::Law mid = cv::Lofi::lawAt(0.5f, 0.75f, t);
  const cv::Lofi::Law half = cv::Lofi::lawAt(0.5f, 0.5f, t);
  const double fullHz = full.holdRatio * cv::kSampleRate, midHz = mid.holdRatio * cv::kSampleRate;
  const bool ok = std::fabs(fullHz - 6000.0) < 0.5 && std::fabs(full.bits - 5.0f) < 1e-4f &&
                  std::fabs(midHz - 48000.0 * std::sqrt(0.125)) < 1.0 && std::fabs(mid.bits - 10.5f) < 1e-4f &&
                  half.holdRatio == 1.0f && !half.crush && std::fabs(half.bits - 16.0f) < 1e-4f &&
                  std::fabs(full.step - 0.0625f) < 1e-7f;
  return report("crush law", ok, "AMOUNT 100 %%: %.0f Hz, %.2f bits; 75 %%: %.0f Hz (6000, 5; 16971)", fullHz, full.bits,
                midHz);
}

// At AMOUNT 100 % the output holds each value for 8 samples, and the values are multiples of one step.
bool testCrush() {
  const cv::LofiParams p = [] {
    cv::LofiParams q;
    q.amount = 1.0f;
    return q;
  }();
  const Signal in = noise(kSecond, 0.5f);
  gLofi.reset();
  const Signal out = run(gLofi, in, p);
  const size_t from = static_cast<size_t>(kSecond / 2);

  size_t first = from + 1;
  while (first < out.size() && out[first] == out[first - 1]) ++first;
  const size_t offset = first % 8;
  size_t offBoundary = 0, boundaries = 0, changed = 0;
  for (size_t i = from + 1; i < out.size(); ++i) {
    if (i % 8 == offset) {
      ++boundaries;
      changed += out[i] != out[i - 1];
    } else {
      offBoundary += out[i] != out[i - 1];
    }
  }
  const double changedFrac = static_cast<double>(changed) / static_cast<double>(boundaries);

  const cv::Lofi::Law law = cv::Lofi::lawAt(p.tone, p.amount, p.tuning);
  const double unit = static_cast<double>(law.makeup / law.drive) * law.step;
  std::vector<float> values(out.begin() + static_cast<std::ptrdiff_t>(from), out.end());
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  double minGap = 1e9;
  for (size_t i = 1; i < values.size(); ++i) minGap = std::min(minGap, static_cast<double>(values[i]) - values[i - 1]);
  const double gapRatio = minGap / unit;
  const size_t distinct = values.size();

  const bool ok = offBoundary == 0 && changedFrac >= 0.85 && std::fabs(gapRatio - 1.0) < 1e-3 && distinct <= 33 &&
                  distinct >= 25;
  std::printf("     held runs: %.0f values change off the 8-sample grid (0), %.0f %% of grid points change (>= 85)\n",
              static_cast<double>(offBoundary), 100.0 * changedFrac);
  return report("crush", ok, "AMOUNT 100 %%: smallest output gap %.4f of one 5-bit step (1), %.0f distinct values (25..33)",
                gapRatio, static_cast<double>(distinct));
}

// Settled knobs give the same samples for any call size, since the voice never sees the chunking.
bool testChunking() {
  const Signal in = noise(20000, 0.3f);
  cv::LofiParams p;
  p.tone = 0.3f;
  p.amount = 0.8f;
  gLofi.reset();
  const Signal ref = run(gLofi, in, p);
  size_t differ = 0;
  for (const int chunk : {1, 7, 64, 100, 20000}) {
    gLofi.reset();
    const Signal out = run(gLofi, in, p, chunk);
    for (size_t i = 0; i < in.size(); ++i) differ += out[i] != ref[i];
  }
  return report("chunking", differ == 0, "call sizes 1, 7, 64, 100, 20000 differ from 64 in %.0f samples (0)",
                static_cast<double>(differ));
}

// Off for 4 ms and on again keeps the running voice; off long enough to settle restarts it.
bool testEnableEdge() {
  const int total = kSecond;
  const Signal in = noise(total, 0.5f);
  cv::LofiParams on;
  on.amount = 1.0f;
  cv::LofiParams off = on;
  off.on = false;

  gLofi.reset();
  const Signal ref = run(gLofi, in, on);

  auto withGap = [&](int gapSamples) {
    const size_t start = static_cast<size_t>(kSecond / 4);
    const size_t end = start + static_cast<size_t>(gapSamples);
    gLofi.reset();
    Signal out(in.size());
    auto part = [&](size_t a, size_t b, const cv::LofiParams& q) {
      const Signal seg(in.begin() + static_cast<std::ptrdiff_t>(a), in.begin() + static_cast<std::ptrdiff_t>(b));
      const Signal o = run(gLofi, seg, q, 1);
      std::copy(o.begin(), o.end(), out.begin() + static_cast<std::ptrdiff_t>(a));
    };
    part(0, start, on);
    part(start, end, off);
    part(end, in.size(), on);
    return out;
  };
  auto tailDiffers = [&](const Signal& out) {
    size_t d = 0;
    for (size_t i = static_cast<size_t>(3 * total / 4); i < in.size(); ++i) d += out[i] != ref[i];
    return d;
  };
  const size_t quick = tailDiffers(withGap(195));
  const size_t settled = tailDiffers(withGap(24003));
  return report("enable edge", quick == 0 && settled > 0,
                "off 195 samples then on: %.0f samples differ from the uninterrupted run (0); off 24003 samples: %.0f (> 0)",
                static_cast<double>(quick), static_cast<double>(settled));
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

// RMS out over RMS in, in dB, of the clip. 0.4 s of silence ahead of it lets the enable fade settle.
double levelDb(const Signal& clip, float tone, float amount) {
  constexpr size_t kPad = kSecond * 2 / 5;
  Signal in(kPad, 0.0f);
  in.insert(in.end(), clip.begin(), clip.end());
  cv::LofiParams p;
  p.tone = tone;
  p.amount = amount;
  gLofi.reset();
  const Signal out = run(gLofi, in, p);
  return 20.0 * std::log10(rmsOf(out, kPad, out.size()) / rmsOf(in, kPad, in.size()));
}

// Vocal-like program: ten harmonics at 1/k on a 180 Hz tone with vibrato and a 2 Hz envelope.
Signal program() {
  const int n = 3 * kSecond;
  Signal x(static_cast<size_t>(n));
  double phase = 0.0;
  Lcg rng;
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / cv::kSampleRate;
    phase += 2.0 * kPi * 180.0 * std::pow(2.0, 20.0 * std::sin(2.0 * kPi * 5.0 * t) / 1200.0) / cv::kSampleRate;
    double v = 0.0;
    for (int k = 1; k <= 10; ++k) v += std::sin(k * phase) / k;
    x[i] = static_cast<float>(v * 0.5 * (1.0 - std::cos(2.0 * kPi * 2.0 * t)) + 0.02 * rng.next());
  }
  const double g = std::pow(10.0, -18.0 / 20.0) / rmsOf(x, 0, x.size());
  for (float& v : x) v = static_cast<float>(v * g);
  return x;
}

bool testLevel() {
  // Above 50 % the crush and the drive settle, so a program holds one level over the top half.
  // (The table is fitted on the clips, so the synthetic program only has to stay flat there.)
  const Signal prog = program();
  double lo = 1e9, hi = -1e9;
  for (int k = 10; k <= 20; ++k) {
    const double db = levelDb(prog, 0.5f, static_cast<float>(k) / 20.0f);
    lo = std::min(lo, db);
    hi = std::max(hi, db);
  }
  bool ok = report("level, program", hi - lo <= 0.3, "AMOUNT 50..100 %% on the synthetic program: %.2f dB spread (<= 0.3)",
                   hi - lo);

  const char* names[] = {"adam_vox_sample_3s", "adam_vox_sample2_4s", "adam_vox_sample3_5s"};
  for (const char* name : names) {
    const Signal clip = readWav(std::string(CUBEVOX_SAMPLES_DIR) + "/" + name + ".wav");
    if (clip.empty()) {
      std::printf("SKIP level, %s  samples/ is not in this checkout\n", name);
      continue;
    }
    double worst = 0.0;
    auto check = [&](float tone, float amount) { worst = std::max(worst, std::fabs(levelDb(clip, tone, amount))); };
    for (int k = 0; k <= 20; ++k) {
      const float x = static_cast<float>(k) / 20.0f;
      check(0.5f, x);
      check(x, 0.5f);
      check(x, 1.0f);
    }
    const double def = levelDb(clip, 0.5f, 0.5f);
    char label[96];
    std::snprintf(label, sizeof label, "level, %s", name);
    ok &= report(label, worst <= 1.0,
                 "AMOUNT sweep at TONE 50 %%, TONE sweep at AMOUNT 50 and 100 %%, 5 %% steps: worst %.2f dB from unity (<= 1), "
                 "default %+.2f dB",
                 worst, def);
  }
  return ok;
}

bool testNoAlloc() {
  const Signal in = noise(kSecond / 2, 0.3f);
  cv::LofiParams p;
  p.amount = 0.9f;
  gLofi.reset();
  run(gLofi, in, p);
  p.tone = 0.1f;
  p.amount = 0.2f;
  run(gLofi, in, p, 17);
  return report("no allocation", true, "process() ran with knob moves and odd call sizes without allocating");
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testSettledOff();
  ok &= testSilence();
  ok &= testBandCorners();
  ok &= testHonk();
  ok &= testCrushLaw();
  ok &= testCrush();
  ok &= testChunking();
  ok &= testEnableEdge();
  ok &= testLevel();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
