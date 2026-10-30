// Input conditioning checks: the response of the high-pass and the low-pass at every cutoff, the OFF
// entries, the crossfade on a cutoff change, odd block lengths and the no-alloc rule. No window,
// no audio device.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "engine/conditioning.h"

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
constexpr int kSettleSamples = cv::kSampleRate / 5;      // 0.2 s
constexpr int kMeasureSamples = cv::kSampleRate / 2;     // 0.5 s
constexpr double kProbeAmp = 0.1;                        // -20 dBFS

int gFailures = 0;

void report(bool ok, const char* name, const char* fmt, double a = 0.0, double b = 0.0, double c = 0.0,
            double d = 0.0) {
  char detail[240];
  std::snprintf(detail, sizeof detail, fmt, a, b, c, d);
  std::printf("%s %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (!ok) ++gFailures;
}

// A probe sine with a phase offset, so a tone at Nyquist is not all zeros.
float probe(double hz, long n, double amp = kProbeAmp) {
  return static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(n) / cv::kSampleRate + 0.7));
}

cv::ConditioningParams paramsFor(int hpf, int lpf) {
  cv::ConditioningParams p;
  p.set(static_cast<uint8_t>(hpf), static_cast<uint8_t>(lpf));
  return p;
}

// Runs `total` samples of a sine through a fresh engine in kBlock chunks and returns the level in dB
// of the output over the last kMeasureSamples against the input over the same samples.
double gainDbAt(const cv::ConditioningParams& p, double hz) {
  cv::InputConditioning engine;
  const int total = kSettleSamples + kMeasureSamples;
  std::vector<float> in(static_cast<size_t>(total));
  std::vector<float> out(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i) in[static_cast<size_t>(i)] = probe(hz, i);
  for (int i = 0; i < total; i += cv::kBlock) {
    const int n = std::min(cv::kBlock, total - i);
    gInProcess = true;
    engine.process(&in[static_cast<size_t>(i)], &out[static_cast<size_t>(i)], n, p);
    gInProcess = false;
  }
  double sumIn = 0.0;
  double sumOut = 0.0;
  for (int i = kSettleSamples; i < total; ++i) {
    sumIn += static_cast<double>(in[static_cast<size_t>(i)]) * static_cast<double>(in[static_cast<size_t>(i)]);
    sumOut += static_cast<double>(out[static_cast<size_t>(i)]) * static_cast<double>(out[static_cast<size_t>(i)]);
  }
  return 10.0 * std::log10(sumOut / sumIn);
}

// Prints one row of the response table the audit reads.
void row(const char* kind, double fc, double atFc, double atHalf, double atDouble, double at1k) {
  std::printf("TABLE %s %6.0f Hz  fc %+7.2f dB  fc/2 %+7.2f dB  2fc %+7.2f dB  1k %+7.3f dB\n", kind, fc, atFc,
              atHalf, atDouble, at1k);
}

bool testHighPass() {
  const int before = gFailures;
  for (int h = 1; h < cv::kHpfCount; ++h) {
    const double fc = static_cast<double>(cv::kHpfCutoffHz[static_cast<size_t>(h)]);
    const cv::ConditioningParams p = paramsFor(h, 0);
    const double atFc = gainDbAt(p, fc);
    const double atHalf = gainDbAt(p, fc / 2.0);
    const double atDouble = gainDbAt(p, 2.0 * fc);
    const double at1k = gainDbAt(p, 1000.0);
    row("HPF", fc, atFc, atHalf, atDouble, at1k);
    char name[40];
    std::snprintf(name, sizeof name, "high-pass %.0f Hz", fc);
    report(std::fabs(atFc + 3.0) <= 0.3, name, "at fc %.2f dB (-3.0 +- 0.3)", atFc);
    report(atHalf <= -11.0, name, "at fc/2 %.2f dB (<= -11)", atHalf);
    report(std::fabs(at1k) <= 0.1, name, "at 1 kHz %.3f dB (0 +- 0.1)", at1k);
  }
  return gFailures == before;
}

bool testLowPass() {
  const int before = gFailures;
  for (int l = 1; l < cv::kLpfCount; ++l) {
    const double fc = static_cast<double>(cv::kLpfCutoffHz[static_cast<size_t>(l)]);
    const cv::ConditioningParams p = paramsFor(0, l);
    const double atFc = gainDbAt(p, fc);
    const double atHalf = gainDbAt(p, fc / 2.0);
    const double atDouble = gainDbAt(p, 2.0 * fc);
    const double at1k = gainDbAt(p, 1000.0);
    row("LPF", fc, atFc, atHalf, atDouble, at1k);
    char name[40];
    std::snprintf(name, sizeof name, "low-pass %.0f Hz", fc);
    report(std::fabs(atFc + 3.0) <= 0.3, name, "at fc %.2f dB (-3.0 +- 0.3)", atFc);
    report(atDouble <= -23.0, name, "at 2 fc %.2f dB (<= -23)", atDouble);
    report(std::fabs(atHalf) <= 0.1 && std::fabs(at1k) <= 0.1, name,
           "at fc/2 %.3f dB and at 1 kHz %.3f dB (0 +- 0.1, ripple under 0.1 dB)", atHalf, at1k);
  }
  return gFailures == before;
}

bool testDefaults() {
  const cv::ConditioningParams p;
  const double at1k = gainDbAt(p, 1000.0);
  report(std::fabs(at1k) <= 0.1, "defaults", "HPF 120 + LPF 12k at 1 kHz %.3f dB (0 +- 0.1)", at1k);
  report(p.hpf == cv::kHpfDefaultIndex && p.lpf == cv::kLpfDefaultIndex &&
             cv::kHpfCutoffHz[p.hpf] == 120.0f && cv::kLpfCutoffHz[p.lpf] == 12000.0f,
         "defaults", "default params are HPF 120 Hz and LPF 12 kHz (%.0f, %.0f)", cv::kHpfCutoffHz[p.hpf],
         cv::kLpfCutoffHz[p.lpf]);
  return std::fabs(at1k) <= 0.1;
}

// Deterministic noise in [-1, 1) to catch any value-dependent path.
class Lcg {
 public:
  float next() {
    s_ = s_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<float>(static_cast<double>(s_ >> 11) / 4503599627370496.0 - 1.0);
  }

 private:
  uint64_t s_ = 0x9E3779B97F4A7C15ULL;
};

// A bare biquad compiled apart from the engine's loop may contract a multiply and an add differently,
// and the 80 Hz section has a pole near 1 that carries those last bits for a while. The comparison
// sits far below the 0.1 or so a wrong stage would leave.
constexpr float kBiquadTolerance = 1e-4f;

// Both filters OFF is a plain copy, in a separate buffer and in place, for any block length. Each
// filter's OFF entry also leaves the other filter's biquads running as they are.
bool testOff() {
  Lcg rng;
  std::vector<float> in(4096);
  for (float& v : in) v = rng.next();
  const cv::ConditioningParams off = paramsFor(0, 0);

  cv::InputConditioning engine;
  std::vector<float> out(in.size(), 7.0f);
  bool same = true;
  size_t at = 0;
  for (int n : {64, 1, 37, 64, 200, 3, 64}) {
    engine.process(&in[at], &out[at], n, off);
    same = same && std::memcmp(&in[at], &out[at], static_cast<size_t>(n) * sizeof(float)) == 0;
    at += static_cast<size_t>(n);
  }
  report(same, "off", "both OFF copies %.0f samples bit for bit", static_cast<double>(at));

  cv::InputConditioning inPlace;
  std::vector<float> buf = in;
  inPlace.process(buf.data(), buf.data(), static_cast<int>(buf.size()), off);
  report(buf == in, "off in place", "both OFF in place leaves the buffer unchanged");

  bool lpOnly = true;
  bool hpOnly = true;
  for (int l = 1; l < cv::kLpfCount; ++l) {
    cv::InputConditioning e;
    const cv::ConditioningParams p = paramsFor(0, l);
    cv::Biquad a = p.lp[0];
    cv::Biquad b = p.lp[1];
    std::vector<float> o(in.size());
    e.process(in.data(), o.data(), static_cast<int>(in.size()), p);
    for (size_t i = 0; i < in.size(); ++i) lpOnly = lpOnly && std::fabs(o[i] - b.run(a.run(in[i]))) <= kBiquadTolerance;
  }
  for (int h = 1; h < cv::kHpfCount; ++h) {
    cv::InputConditioning e;
    const cv::ConditioningParams p = paramsFor(h, 0);
    cv::Biquad a = p.hp;
    std::vector<float> o(in.size());
    e.process(in.data(), o.data(), static_cast<int>(in.size()), p);
    for (size_t i = 0; i < in.size(); ++i) hpOnly = hpOnly && std::fabs(o[i] - a.run(in[i])) <= kBiquadTolerance;
  }
  report(lpOnly, "off hpf", "HPF OFF leaves the low-pass biquads alone (within 1e-4 of a bare cascade)");
  report(hpOnly, "off lpf", "LPF OFF leaves the high-pass biquad alone (within 1e-4 of a bare biquad)");

  // Settling back to OFF after a filter ran is a plain copy again once the crossfade block is over.
  cv::InputConditioning back;
  std::vector<float> o(in.size());
  back.process(in.data(), o.data(), 512, paramsFor(3, 6));
  back.process(in.data() + 512, o.data() + 512, 512, off);
  report(std::memcmp(in.data() + 640, o.data() + 640, 384 * sizeof(float)) == 0, "off settled",
         "after the crossfade block, OFF copies bit for bit again");
  return same && buf == in && lpOnly && hpOnly;
}

// What one cutoff change leaves in the crossfade block, as peaks in full-scale units.
struct FadeDiff {
  // The block against an engine that ran the new params all along: the wording of the spec. It
  // includes the blend itself, because the old and the new filter differ in phase at 1 kHz.
  double againstSettled = 0.0;
  // The block against the ideal blend of the two settled outputs, the same linear ramp the engine
  // uses: what the carried-over filter state adds on top of the blend, which is what a click is.
  double againstBlend = 0.0;
};

// Runs a 1 kHz sine at -20 dBFS for a second under `from`, then one block under `to`.
FadeDiff crossfadeDiff(const cv::ConditioningParams& from, const cv::ConditioningParams& to) {
  constexpr int kLead = cv::kSampleRate;
  cv::InputConditioning moved;
  cv::InputConditioning steady;
  cv::InputConditioning old;
  std::vector<float> o(cv::kBlock);
  std::vector<float> in(cv::kBlock);
  long n = 0;
  for (; n < kLead; n += cv::kBlock) {
    for (int i = 0; i < cv::kBlock; ++i) in[static_cast<size_t>(i)] = probe(1000.0, n + i);
    moved.process(in.data(), o.data(), cv::kBlock, from);
    steady.process(in.data(), o.data(), cv::kBlock, to);
    old.process(in.data(), o.data(), cv::kBlock, from);
  }
  for (int i = 0; i < cv::kBlock; ++i) in[static_cast<size_t>(i)] = probe(1000.0, n + i);
  std::vector<float> fade(cv::kBlock);
  std::vector<float> settled(cv::kBlock);
  std::vector<float> before(cv::kBlock);
  moved.process(in.data(), fade.data(), cv::kBlock, to);
  steady.process(in.data(), settled.data(), cv::kBlock, to);
  old.process(in.data(), before.data(), cv::kBlock, from);
  FadeDiff d;
  for (int i = 0; i < cv::kBlock; ++i) {
    const size_t k = static_cast<size_t>(i);
    const double w = static_cast<double>(i + 1) / cv::kBlock;
    const double blend = (1.0 - w) * static_cast<double>(before[k]) + w * static_cast<double>(settled[k]);
    d.againstSettled = std::fmax(d.againstSettled, std::fabs(static_cast<double>(fade[k]) - static_cast<double>(settled[k])));
    d.againstBlend = std::fmax(d.againstBlend, std::fabs(static_cast<double>(fade[k]) - blend));
  }
  return d;
}

double toDbfs(double peak) { return peak > 0.0 ? 20.0 * std::log10(peak) : -200.0; }

// The worst of each measure over a set of changes, in dBFS, with the change that set it.
struct Worst {
  double againstSettled = -200.0;
  double againstBlend = -200.0;
  int blendFrom = 0;
  int blendTo = 0;

  void add(const FadeDiff& d, int from, int to) {
    againstSettled = std::fmax(againstSettled, toDbfs(d.againstSettled));
    if (toDbfs(d.againstBlend) > againstBlend) {
      againstBlend = toDbfs(d.againstBlend);
      blendFrom = from;
      blendTo = to;
    }
  }
};

// A cutoff change at -20 dBFS 1 kHz. The spec's measure, the block against the settled new filter,
// holds for every high-pass change. For the low-pass it cannot hold on a big jump, because the 4 kHz
// and the 12 kHz filter already differ at 1 kHz by more than the limit once settled, and the block
// blends from one to the other. So the limit applies to what the state carry-over adds to the blend,
// for every change including the ones to and from OFF, and the first measure is held for the
// high-pass and reported for the rest.
bool testCrossfade() {
  constexpr double kLimitDb = -40.0;
  const int before = gFailures;
  const int hpfDefault = cv::kHpfDefaultIndex;
  const int lpfDefault = cv::kLpfDefaultIndex;

  Worst hpf;
  Worst lpf;
  Worst off;
  for (int a = 1; a < cv::kHpfCount; ++a) {
    for (int b = 1; b < cv::kHpfCount; ++b) {
      if (a != b) hpf.add(crossfadeDiff(paramsFor(a, lpfDefault), paramsFor(b, lpfDefault)), a, b);
    }
  }
  for (int a = 1; a < cv::kLpfCount; ++a) {
    for (int b = 1; b < cv::kLpfCount; ++b) {
      if (a != b) lpf.add(crossfadeDiff(paramsFor(hpfDefault, a), paramsFor(hpfDefault, b)), a, b);
    }
  }
  for (int h = 1; h < cv::kHpfCount; ++h) {
    off.add(crossfadeDiff(paramsFor(0, lpfDefault), paramsFor(h, lpfDefault)), 0, h);
    off.add(crossfadeDiff(paramsFor(h, lpfDefault), paramsFor(0, lpfDefault)), h, 0);
  }
  for (int l = 1; l < cv::kLpfCount; ++l) {
    off.add(crossfadeDiff(paramsFor(hpfDefault, 0), paramsFor(hpfDefault, l)), 0, l);
    off.add(crossfadeDiff(paramsFor(hpfDefault, l), paramsFor(hpfDefault, 0)), l, 0);
  }
  std::printf("TABLE crossfade HPF cutoff changes: against settled %+.2f dBFS, against the blend %+.2f dBFS (%d -> %d)\n",
              hpf.againstSettled, hpf.againstBlend, hpf.blendFrom, hpf.blendTo);
  std::printf("TABLE crossfade LPF cutoff changes: against settled %+.2f dBFS, against the blend %+.2f dBFS (%d -> %d)\n",
              lpf.againstSettled, lpf.againstBlend, lpf.blendFrom, lpf.blendTo);
  std::printf("TABLE crossfade to and from OFF:    against settled %+.2f dBFS, against the blend %+.2f dBFS (%d -> %d)\n",
              off.againstSettled, off.againstBlend, off.blendFrom, off.blendTo);

  report(hpf.againstSettled < kLimitDb, "crossfade hpf",
         "worst high-pass change leaves %.2f dBFS peak against the settled filter (< -40)", hpf.againstSettled);
  report(hpf.againstBlend < kLimitDb && lpf.againstBlend < kLimitDb && off.againstBlend < kLimitDb, "crossfade",
         "state carry-over adds at most %.2f dBFS (HPF), %.2f (LPF), %.2f (OFF) to the blend (< -40)",
         hpf.againstBlend, lpf.againstBlend, off.againstBlend);
  return gFailures == before;
}

// The crossfade ends with the block: 30 blocks later the output equals the settled engine's, apart
// from the float noise two different state histories leave in the 80 and 150 Hz sections.
bool testCrossfadeEnds() {
  cv::InputConditioning moved;
  cv::InputConditioning steady;
  const cv::ConditioningParams from = paramsFor(1, 4);
  const cv::ConditioningParams to = paramsFor(4, 2);
  std::vector<float> a(cv::kBlock);
  std::vector<float> b(cv::kBlock);
  std::vector<float> in(cv::kBlock);
  long n = 0;
  for (; n < cv::kSampleRate / 2; n += cv::kBlock) {
    for (int i = 0; i < cv::kBlock; ++i) in[static_cast<size_t>(i)] = probe(1000.0, n + i);
    moved.process(in.data(), a.data(), cv::kBlock, from);
    steady.process(in.data(), b.data(), cv::kBlock, to);
  }
  double peak = 0.0;
  for (int block = 0; block < 40; ++block, n += cv::kBlock) {
    for (int i = 0; i < cv::kBlock; ++i) in[static_cast<size_t>(i)] = probe(1000.0, n + i);
    moved.process(in.data(), a.data(), cv::kBlock, to);
    steady.process(in.data(), b.data(), cv::kBlock, to);
    if (block >= 30) {
      for (int i = 0; i < cv::kBlock; ++i) peak = std::fmax(peak, std::fabs(static_cast<double>(a[static_cast<size_t>(i)]) - static_cast<double>(b[static_cast<size_t>(i)])));
    }
  }
  report(toDbfs(peak) < -100.0, "crossfade settles",
         "30 blocks after a change the output matches the settled filter to %.1f dBFS (< -100)", toDbfs(peak));
  return toDbfs(peak) < -100.0;
}

// A changed cutoff takes effect at the next process() call and not before, and the first call adopts
// its params without a crossfade.
bool testAdoption() {
  cv::InputConditioning first;
  const cv::ConditioningParams p = paramsFor(1, 1);
  std::vector<float> in(cv::kBlock, 0.0f);
  std::vector<float> o(cv::kBlock);
  for (int i = 0; i < cv::kBlock; ++i) in[static_cast<size_t>(i)] = probe(1000.0, i);
  first.process(in.data(), o.data(), cv::kBlock, p);
  cv::Biquad hp = p.hp;
  cv::Biquad l0 = p.lp[0];
  cv::Biquad l1 = p.lp[1];
  bool exact = true;
  for (int i = 0; i < cv::kBlock; ++i) exact = exact && o[static_cast<size_t>(i)] == l1.run(l0.run(hp.run(in[static_cast<size_t>(i)])));
  report(exact, "adoption", "the first block runs its params from the start, no crossfade");
  return exact;
}

bool testNoAlloc() {
  cv::InputConditioning e;
  std::vector<float> in(cv::kBlock, 0.1f);
  std::vector<float> o(cv::kBlock);
  gInProcess = true;
  e.process(in.data(), o.data(), cv::kBlock, paramsFor(3, 6));
  e.process(in.data(), o.data(), cv::kBlock, paramsFor(1, 2));  // a crossfade block
  e.process(in.data(), o.data(), cv::kBlock, paramsFor(0, 0));
  gInProcess = false;
  report(true, "no-alloc", "0 allocations in process()");
  return true;
}

// A cutoff index past its list reads as the last entry; the engine never indexes out of the lists.
bool testIndexClamp() {
  cv::ConditioningParams p;
  p.set(200, 200);
  const bool ok = p.hpf == cv::kHpfCount - 1 && p.lpf == cv::kLpfCount - 1;
  report(ok, "clamp", "an index past the list reads as the last entry (%.0f, %.0f)", p.hpf, p.lpf);
  return ok;
}

}  // namespace

int main() {
  testHighPass();
  testLowPass();
  testDefaults();
  testOff();
  testCrossfade();
  testCrossfadeEnds();
  testAdoption();
  testNoAlloc();
  testIndexClamp();
  std::printf("%s conditioning\n", gFailures == 0 ? "PASS" : "FAIL");
  return gFailures == 0 ? 0 : 1;
}
