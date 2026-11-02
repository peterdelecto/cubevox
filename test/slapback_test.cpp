// Echo checks: settled off, Time and its clamps, low-pass, feedback and its cap, level clamps, the
// dry level and the echo trim, chunk sizes, reset, no-alloc. No window, no audio device.

#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "engine/slapback.h"

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

constexpr float kAmp = 0.5f;
constexpr int kSecond = cv::kSampleRate;

using Signal = std::vector<float>;

Signal sine(float hz, int n) {
  Signal x(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = kAmp * sinf(2.0f * cv::kPi * hz * i / cv::kSampleRate);
  return x;
}

// Deterministic white noise in [-amp, amp).
Signal noise(int n, float amp) {
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  Signal x(static_cast<size_t>(n));
  for (float& v : x) {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    v = amp * static_cast<float>(static_cast<double>(s >> 11) / 4503599627370496.0 - 1.0);
  }
  return x;
}

// Runs the whole signal through process() in chunks of `chunk` samples.
Signal run(cv::Slapback& s, const Signal& in, const cv::SlapbackParams& p, int chunk = cv::kBlock) {
  Signal out(in.size());
  const int total = static_cast<int>(in.size());
  for (int i = 0; i < total; i += chunk) {
    const int n = total - i < chunk ? total - i : chunk;
    gInProcess = true;
    s.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  return out;
}

cv::SlapbackParams params(float amount) {
  cv::SlapbackParams p;
  p.amount = amount;
  p.tuning.trimDb = 0.0f;  // the checks on the echo's timing and level read it with no trim
  return p;
}

Signal wetOf(const Signal& out, const Signal& in) {
  Signal wet(in.size());
  for (size_t i = 0; i < in.size(); ++i) wet[i] = out[i] - in[i];
  return wet;
}

float rms(const Signal& x, size_t from) {
  double s = 0.0;
  for (size_t i = from; i < x.size(); ++i) s += static_cast<double>(x[i]) * x[i];
  return static_cast<float>(std::sqrt(s / static_cast<double>(x.size() - from)));
}

// Index of the largest |x| in [from, to).
int peakIndex(const Signal& x, int from, int to) {
  int best = from;
  for (int i = from; i < to; ++i)
    if (std::fabs(x[static_cast<size_t>(i)]) > std::fabs(x[static_cast<size_t>(best)])) best = i;
  return best;
}

// Samples in [from, to) where a and b differ in any bit.
size_t countDiffer(const Signal& a, const Signal& b, size_t from, size_t to) {
  size_t n = 0;
  for (size_t i = from; i < to; ++i) n += a[i] != b[i];
  return n;
}

bool report(const char* name, bool ok, const char* fmt, double a, double b = 0.0, double c = 0.0) {
  char detail[200];
  std::snprintf(detail, sizeof detail, fmt, a, b, c);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

// An engine that has recorded two seconds of loud noise at the longest Time and been reset, so the
// whole line holds stale samples.
std::unique_ptr<cv::Slapback> dirtyReset() {
  auto s = std::make_unique<cv::Slapback>();
  s->reset();
  cv::SlapbackParams p = params(1.0f);
  p.tuning.timeMs = cv::Slapback::kTimeMaxMs;
  run(*s, noise(2 * kSecond, 0.5f), p);
  s->reset();
  return s;
}

constexpr int kImpulseAt = cv::kSampleRate / 2;

Signal impulse(int n) {
  Signal x(static_cast<size_t>(n), 0.0f);
  x[static_cast<size_t>(kImpulseAt)] = 1.0f;
  return x;
}

// Where the echo of an impulse lands for a Time setting, in samples past the impulse.
int echoLag(float timeMs) {
  cv::SlapbackParams p = params(1.0f);
  p.tuning.timeMs = timeMs;
  p.tuning.feedback = 0.0f;
  p.tuning.lowpassHz = 12000.0f;
  auto s = std::make_unique<cv::Slapback>();
  s->reset();
  const Signal in = impulse(kSecond);
  const Signal wet = wetOf(run(*s, in, p), in);
  return peakIndex(wet, kImpulseAt + 1, kSecond) - kImpulseAt;
}

// A stage that is off ends as a plain copy: never on, and on for 0.5 s then off.
bool testSettledOff() {
  const Signal in = noise(2 * kSecond, 0.4f);
  auto s = std::make_unique<cv::Slapback>();

  s->reset();
  const Signal zero = run(*s, in, params(0.0f));
  cv::SlapbackParams off = params(1.0f);
  off.on = false;
  s->reset();
  const Signal never = run(*s, in, off);
  const size_t differNever = countDiffer(zero, in, 0, in.size()) + countDiffer(never, in, 0, in.size());

  // On for 0.5 s, then off. The 20 ms smoother snaps 277 ms after the edge, so the output is the
  // input from 300 ms on.
  cv::SlapbackParams on;
  on.amount = 1.0f;
  cv::SlapbackParams after = on;
  after.on = false;
  s->reset();
  const size_t edge = kSecond / 2;
  run(*s, Signal(in.begin(), in.begin() + static_cast<long>(edge)), on);
  const Signal rest = run(*s, Signal(in.begin() + static_cast<long>(edge), in.end()), after);
  const Signal tail(in.begin() + static_cast<long>(edge), in.end());
  const size_t differEarly = countDiffer(rest, tail, 0, kSecond * 3 / 10);
  const size_t differAfter = countDiffer(rest, tail, kSecond * 3 / 10, rest.size());
  return report("settled off", differNever == 0 && differAfter == 0 && differEarly > 0,
                "never on: %.0f samples differ (0); 300 ms after the off edge: %.0f differ (0)",
                static_cast<double>(differNever), static_cast<double>(differAfter));
}

bool testTime() {
  const float times[4] = {cv::Slapback::kTimeMinMs, 100.0f, 180.0f, cv::Slapback::kTimeMaxMs};
  bool ok = true;
  int worst = 0;
  for (float ms : times) {
    const int expect = static_cast<int>(ms * cv::kSmpPerMs);
    const int err = std::abs(echoLag(ms) - expect);
    if (err > worst) worst = err;
    if (err > 1) ok = false;
  }
  return report("time", ok, "worst peak error over 60/100/180/250 ms=%.0f samples (<=1)", worst);
}

// A Time outside the knob's range lands on the nearest end.
bool testTimeClamps() {
  const int lo = std::abs(echoLag(1.0f) - static_cast<int>(cv::Slapback::kTimeMinMs * cv::kSmpPerMs));
  const int hi = std::abs(echoLag(1.0e6f) - static_cast<int>(cv::Slapback::kTimeMaxMs * cv::kSmpPerMs));
  return report("time clamps", lo <= 1 && hi <= 1, "1 ms lands %.0f samples off 60 ms; 1e6 ms lands %.0f off 250 ms (<=1)",
                lo, hi);
}

// A glide ends exactly on its target: an engine that moved from 100 ms to 200 ms answers an impulse
// bit for bit like one that started at 200 ms.
bool testTimeLands() {
  const Signal silence(kSecond, 0.0f);
  cv::SlapbackParams from = params(1.0f);
  from.tuning.timeMs = 100.0f;
  cv::SlapbackParams to = from;
  to.tuning.timeMs = 200.0f;
  auto moved = std::make_unique<cv::Slapback>();
  auto direct = std::make_unique<cv::Slapback>();
  moved->reset();
  direct->reset();
  run(*moved, silence, from);
  run(*moved, Signal(2 * kSecond, 0.0f), to);
  run(*direct, Signal(3 * kSecond, 0.0f), to);
  const Signal hit = impulse(kSecond);
  const size_t differ = countDiffer(run(*moved, hit, to), run(*direct, hit, to), 0, hit.size());
  return report("time lands", differ == 0, "after a 100 to 200 ms glide: %.0f samples differ from a fresh 200 ms (0)",
                static_cast<double>(differ));
}

// Amount at 0 copies the input and keeps recording, so the repeats are there when Amount lifts.
bool testIdleRecords() {
  const Signal in = noise(kSecond, 0.4f);
  cv::SlapbackParams idle = params(0.0f);
  cv::SlapbackParams lifted = params(1.0f);
  idle.tuning.timeMs = lifted.tuning.timeMs = 100.0f;
  auto s = std::make_unique<cv::Slapback>();
  s->reset();
  run(*s, in, idle);
  const Signal out = run(*s, Signal(kSecond / 10, 0.0f), lifted);
  const float level = rms(out, static_cast<size_t>(kSecond / 50)) / rms(in, 0);
  return report("idle records", level > 0.3f, "echo level in the 100 ms after Amount lifts, against the input: %.2f (>0.3)",
                static_cast<double>(level));
}

// The 2-pole Butterworth at 4 kHz reads -13.53 dB at 8 kHz after the bilinear warp.
bool testLowpass() {
  const int n = 2 * kSecond;
  cv::SlapbackParams p = params(1.0f);
  p.tuning.lowpassHz = 4000.0f;
  p.tuning.feedback = 0.0f;
  const size_t skip = static_cast<size_t>(kSecond / 2);
  auto s = std::make_unique<cv::Slapback>();

  const Signal lo = sine(500.0f, n);
  s->reset();
  const float loRms = rms(wetOf(run(*s, lo, p), lo), skip);

  const Signal hi = sine(8000.0f, n);
  s->reset();
  const float hiRms = rms(wetOf(run(*s, hi, p), hi), skip);

  const float db = 20.0f * log10f(hiRms / loRms);
  return report("lowpass", db <= -13.0f && db >= -14.0f, "8 kHz wet vs 500 Hz wet=%.2f dB (-14..-13)", db);
}

// Amount drives the repeats: tuned 0.35 at 25 %, sqrt growth, capped at 0.5. The 12 kHz low-pass
// leaves the second peak at 0.8284 of the first after its two passes, so the full-Amount ratio is
// 0.5 x 0.8284 = 0.414. The law is checked as ratios to the full-Amount repeat.
bool testFeedback() {
  const float amount[3] = {0.0625f, 0.25f, 1.0f};
  std::array<float, 3> ratio{};
  auto s = std::make_unique<cv::Slapback>();
  for (int k = 0; k < 3; ++k) {
    const Signal in = impulse(kSecond);
    cv::SlapbackParams p = params(amount[k]);
    p.tuning.lowpassHz = 12000.0f;
    p.tuning.timeMs = 80.0f;
    p.tuning.feedback = 0.35f;
    s->reset();
    const Signal wet = wetOf(run(*s, in, p), in);
    const int first = kImpulseAt + 80 * kSecond / 1000;
    const int second = kImpulseAt + 160 * kSecond / 1000;
    const float p1 = std::fabs(wet[static_cast<size_t>(peakIndex(wet, first - 10, first + 10))]);
    const float p2 = std::fabs(wet[static_cast<size_t>(peakIndex(wet, second - 10, second + 10))]);
    ratio[static_cast<size_t>(k)] = p2 / p1;
  }
  const float low = ratio[0] / ratio[2];  // expect 0.175 / 0.5 = 0.35
  const float mid = ratio[1] / ratio[2];  // expect 0.35 / 0.5 = 0.70
  const bool ok = ratio[2] >= 0.39f && ratio[2] <= 0.43f && std::fabs(low - 0.35f) <= 0.03f &&
                  std::fabs(mid - 0.70f) <= 0.03f;
  return report("feedback", ok,
                "full-Amount repeat ratio=%.3f (0.39..0.43); 6 %% and 25 %% vs full=%.3f, %.3f (0.35, 0.70 +-0.03)",
                ratio[2], low, mid);
}

// A tuned feedback above kFeedbackMax does what the cap does, bit for bit.
bool testFeedbackCap() {
  const Signal in = noise(kSecond, 0.4f);
  auto s = std::make_unique<cv::Slapback>();
  size_t differ = 0;
  for (float amount : {0.25f, 1.0f}) {
    cv::SlapbackParams capped = params(amount);
    capped.tuning.feedback = 0.5f;
    cv::SlapbackParams over = capped;
    over.tuning.feedback = 0.9f;
    s->reset();
    const Signal a = run(*s, in, capped);
    s->reset();
    const Signal b = run(*s, in, over);
    differ += countDiffer(a, b, 0, a.size());
  }
  return report("feedback cap", differ == 0, "feedback 0.9 against 0.5: %.0f samples differ (0)",
                static_cast<double>(differ));
}

// wetMaxDb and trimDb clamp to +-24 dB, and a stray value cannot make the output blow up or turn NaN.
bool testLevelClamps() {
  const Signal in = noise(kSecond, 0.4f);
  auto s = std::make_unique<cv::Slapback>();
  size_t differ = 0;
  bool finite = true;
  const float asked[4] = {100.0f, -100.0f, 800.0f, -800.0f};
  const float limit[4] = {24.0f, -24.0f, 24.0f, -24.0f};
  for (int k = 0; k < 4; ++k) {
    for (int field = 0; field < 2; ++field) {
      cv::SlapbackParams over = params(1.0f);
      cv::SlapbackParams clamped = over;
      (field == 0 ? over.tuning.wetMaxDb : over.tuning.trimDb) = asked[k];
      (field == 0 ? clamped.tuning.wetMaxDb : clamped.tuning.trimDb) = limit[k];
      s->reset();
      const Signal a = run(*s, in, over);
      s->reset();
      const Signal b = run(*s, in, clamped);
      differ += countDiffer(a, b, 0, a.size());
      for (float v : a) finite = finite && std::isfinite(v);
    }
  }
  return report("level clamps", differ == 0 && finite, "wetMaxDb and trimDb past +-24 dB: %.0f samples differ from the clamp (0)",
                static_cast<double>(differ));
}

// The dry voice passes at unity at every Amount, trim included. A lone impulse after a silent half
// second reads the dry gain before any echo returns.
bool testDry() {
  std::array<double, 3> db{};
  bool ok = true;
  const float amounts[3] = {0.25f, 0.5f, 1.0f};
  auto s = std::make_unique<cv::Slapback>();
  for (size_t k = 0; k < 3; ++k) {
    cv::SlapbackParams p;  // the shipped tuning, trim and all
    p.amount = amounts[k];
    s->reset();
    const Signal out = run(*s, impulse(kSecond), p);
    const float dry = out[static_cast<size_t>(kImpulseAt)];
    db[k] = 20.0 * std::log10(static_cast<double>(dry));
    ok = ok && dry == 1.0f;
  }
  return report("dry", ok, "dry gain at Amount 25/50/100 %%: %.2f, %.2f, %.2f dB (0.00)", db[0], db[1], db[2]);
}

// The trim scales the echo alone: 6.02 dB less trim halves the echo and leaves the repeats' timing
// and the feedback as they were.
bool testTrim() {
  const Signal in = noise(kSecond, 0.4f);
  cv::SlapbackParams full = params(1.0f);
  cv::SlapbackParams half = full;
  half.tuning.trimDb = -6.0206f;
  auto s = std::make_unique<cv::Slapback>();
  s->reset();
  const Signal a = wetOf(run(*s, in, full), in);
  s->reset();
  const Signal b = wetOf(run(*s, in, half), in);
  const float ratio = rms(b, 0) / rms(a, 0);
  return report("trim", std::fabs(ratio - 0.5f) < 1e-3f, "echo at trimDb -6.02 against 0 dB: x%.4f (0.5000 +-1e-3)",
                static_cast<double>(ratio));
}

// With settled parameters the output does not depend on how the call is cut into chunks.
bool testChunks() {
  const Signal in = noise(kSecond + kSecond / 2, 0.3f);
  const Signal warm(in.begin(), in.begin() + kSecond / 2);
  const Signal rest(in.begin() + kSecond / 2, in.end());
  cv::SlapbackParams p;
  p.amount = 0.6f;
  p.tuning.timeMs = 137.3f;
  auto ref = std::make_unique<cv::Slapback>();
  ref->reset();
  run(*ref, warm, p);
  const Signal want = run(*ref, rest, p);
  size_t differ = 0;
  for (int chunk : {1, 7, 37, 100, 128, 480, 4800}) {
    auto s = std::make_unique<cv::Slapback>();
    s->reset();
    run(*s, warm, p);
    differ += countDiffer(run(*s, rest, p, chunk), want, 0, want.size());
  }
  return report("chunks", differ == 0, "chunks of 1/7/37/100/128/480/4800 against 64: %.0f samples differ (0)",
                static_cast<double>(differ));
}

// reset() leaves the line alone, so it must act as a clear: a used engine, once reset, gives silence
// for silence and the same output as a fresh engine for any input.
bool testReset() {
  auto dirty = dirtyReset();
  cv::SlapbackParams p = params(1.0f);
  p.tuning.timeMs = cv::Slapback::kTimeMaxMs;
  const Signal silent = run(*dirty, Signal(kSecond, 0.0f), p);
  size_t nonzero = 0;
  for (float v : silent) nonzero += v != 0.0f;

  const Signal in = noise(kSecond, 0.4f);
  cv::SlapbackParams q = params(0.6f);
  q.tuning.timeMs = 90.0f;
  auto used = dirtyReset();
  auto fresh = std::make_unique<cv::Slapback>();
  fresh->reset();
  const size_t differ = countDiffer(run(*used, in, q), run(*fresh, in, q), 0, in.size());
  return report("reset", nonzero == 0 && differ == 0,
                "silence after reset: %.0f nonzero samples (0); a reset engine against a fresh one: %.0f differ (0)",
                static_cast<double>(nonzero), static_cast<double>(differ));
}

bool testNoAlloc() {
  const Signal in = sine(440.0f, cv::kBlock * 8);
  cv::SlapbackParams p = params(0.7f);
  p.tuning.feedback = 0.3f;
  auto s = std::make_unique<cv::Slapback>();
  s->reset();
  run(*s, in, p);  // aborts on any new/delete inside process()
  return report("no-alloc", true, "%.0f allocations in process()", 0.0);
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testSettledOff();
  ok &= testTime();
  ok &= testTimeClamps();
  ok &= testTimeLands();
  ok &= testIdleRecords();
  ok &= testLowpass();
  ok &= testFeedback();
  ok &= testFeedbackCap();
  ok &= testLevelClamps();
  ok &= testDry();
  ok &= testTrim();
  ok &= testChunks();
  ok &= testReset();
  ok &= testNoAlloc();
  return ok ? 0 : 1;
}
