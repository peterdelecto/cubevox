// Knob-step clicks: every card's knobs move by one knob-filter touch (1.5 %) or one detent while a
// tone plays. Right after the step the output may bend no harder than it does in steady state at
// either setting, so a jump in a gain, a delay tap or a filter state shows as a spike in the second
// difference. Settings come from the registry, as the box applies them. No window, no audio device.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

#include "core/chain_params.h"
#include "core/registry.h"

#include "distortion_rule.h"

namespace {

using Signal = std::vector<float>;

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 220.0;
constexpr double kToneAmp = 0.25;

// The travel the knob filter counts as a touch (kMoveThreshold in core/knob_filter.cpp), the
// first step a slow turn publishes.
constexpr float kContinuousStep = 0.015f;

constexpr int kStepAt = 2 * cv::kSampleRate;
constexpr int kTotal = 2 * kStepAt;
constexpr int kSteadyWindow = cv::kSampleRate / 2;
constexpr int kStepWindow = cv::kSampleRate / 10;

// A steady output reaches its own peak in every window, so a clean step reads 1.0 give or take the
// grain engines' window-to-window wobble (about 0.02 %). The smallest click the engine audit
// measured, an octave voice reset at 10 % MIX, read 1.6.
constexpr double kMaxRatio = 1.05;
static_assert(kStepAt % cv::kBlock == 0, "the step lands on a block edge");

// A pure sine: its second difference is small and steady, so any cut stands out.
Signal tone(double hz, int total = kTotal) {
  Signal x(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i)
    x[static_cast<size_t>(i)] = static_cast<float>(kToneAmp * std::sin(2.0 * kPi * hz * i / cv::kSampleRate));
  return x;
}

// Largest |second difference| of x over [from, to).
double peakCurvature(const Signal& x, int from, int to) {
  double worst = 0.0;
  for (int i = std::max(from, 2); i < to; ++i) {
    const size_t k = static_cast<size_t>(i);
    worst = std::max(worst, std::fabs(static_cast<double>(x[k]) - 2.0 * x[k - 1] + x[k - 2]));
  }
  return worst;
}

// Runs `in` through a fresh engine with `a` before kStepAt and `b` from there on.
template <typename Fx, typename P>
Signal runFx(const Signal& in, const ChainParams& a, const ChainParams& b, P ChainParams::*field) {
  auto fx = std::make_unique<Fx>();
  fx->reset();
  Signal out(in.size());
  for (int i = 0; i < kTotal; i += cv::kBlock) {
    const size_t k = static_cast<size_t>(i);
    fx->process(&in[k], &out[k], cv::kBlock, (i < kStepAt ? a : b).*field);
  }
  return out;
}

// Runs `in` through the Crowd card as the chain does when it sits alone: the pitch stage first, which
// writes the ring and feeds the tracker, then the Crowd card on that stage's output.
Signal runCrowd(const Signal& in, const ChainParams& a, const ChainParams& b) {
  auto pitch = std::make_unique<cv::PitchFx>();
  auto fx = std::make_unique<cv::Crowd>();
  pitch->reset();
  fx->reset();
  Signal out(in.size());
  std::array<float, cv::kBlock> lead{};
  for (int i = 0; i < kTotal; i += cv::kBlock) {
    const size_t k = static_cast<size_t>(i);
    const ChainParams& p = i < kStepAt ? a : b;
    pitch->process(&in[k], lead.data(), cv::kBlock, p.pitchFx);
    fx->process(lead.data(), &out[k], cv::kBlock, p.crowd, *pitch);
  }
  return out;
}

// The engine a card runs, mapped as runStage in core/chain_run.cpp maps it.
Signal runCard(Card c, const Signal& in, const ChainParams& a, const ChainParams& b) {
  switch (engineOf(c)) {
    case Engine::InputGate: return runFx<cv::Gate>(in, a, b, &ChainParams::inputGate);
    case Engine::Pitch: return runFx<cv::PitchFx>(in, a, b, &ChainParams::pitchFx);
    case Engine::Unison: return runFx<cv::Unison>(in, a, b, &ChainParams::unison);
    case Engine::Slapback: return runFx<cv::Slapback>(in, a, b, &ChainParams::slapback);
    case Engine::Distortion: return runFx<cv::Distortion>(in, a, b, &ChainParams::distortion);
    case Engine::Gate: return runFx<cv::Gate>(in, a, b, &ChainParams::gate);
    case Engine::Reverb: return runFx<cv::Reverb>(in, a, b, &ChainParams::reverb);
    case Engine::Crowd: return runCrowd(in, a, b);
    case Engine::Lofi: return runFx<cv::Lofi>(in, a, b, &ChainParams::lofi);
    case Engine::Bloom: return runFx<cv::Bloom>(in, a, b, &ChainParams::bloom);
    case Engine::Growl:  // the audition cards have no knob-step rows yet
    case Engine::RingMod:
    case Engine::Strings:
    case Engine::Count: break;
  }
  return in;
}

// Peak curvature just after the step over the larger steady-state peak of the two settings.
double stepRatio(const Signal& out) {
  const double steady = std::max(peakCurvature(out, kStepAt - kSteadyWindow, kStepAt),
                                 peakCurvature(out, kTotal - kSteadyWindow, kTotal));
  const double step = peakCurvature(out, kStepAt, kStepAt + kStepWindow);
  if (steady > 0.0) return step / steady;
  return step > 0.0 ? std::numeric_limits<double>::infinity() : 0.0;
}

struct Step {
  float from;
  float to;
};

// Continuous knobs step at a low, a middle and a high setting; both Chatter knobs also step at the last
// touch below the top of their travel (LENGTH 3 s, AMOUNT 100 %), and LENGTH at the first touch above
// its bottom (20 ms). Both Spring knobs also move their whole travel in one step, up and down. Chatter's
// AMOUNT sets the slice length as well as the loudness, so it walks its whole travel in tenths, from the
// bottom, where the stage is a plain copy. A stepped knob of up to fifteen detents (the Mob card's VOICES,
// the Formant card's SHIFT) moves across every detent in turn, and a longer one (Octave's SEMITONES) moves
// one detent at a quarter and at three quarters of its travel.
constexpr int kWalkedDetents = 15;

std::vector<Step> stepsFor(Card c, int knob) {
  const int detents = knobSteps(c, knob);
  if (c == Card::Autotune && knob == 0) {
    // KEY moves the target note, so each of its twelve steps goes up and back, the wrap from the last
    // key to the first included.
    std::vector<Step> s;
    for (int d = 0; d < detents; ++d) {
      const int next = (d + 1) % detents;
      s.push_back({static_cast<float>(d), static_cast<float>(next)});
      s.push_back({static_cast<float>(next), static_cast<float>(d)});
    }
    return s;
  }
  if (detents == 0) {
    std::vector<Step> s;
    if (c == Card::Bloom && knob == 1) {
      for (int tenth = 0; tenth < 10; ++tenth) s.push_back({0.1f * static_cast<float>(tenth), 0.1f * static_cast<float>(tenth) + kContinuousStep});
    } else {
      for (float p : {0.2f, 0.5f, 0.8f}) s.push_back({p, p + kContinuousStep});
    }
    if (c == Card::Bloom) s.push_back({1.0f - kContinuousStep, 1.0f});
    // The Spring card's knobs also move the whole travel in one step each way, as a fast turn would.
    if (c == Card::ReverbSpring) {
      s.push_back({0.0f, 1.0f});
      s.push_back({1.0f, 0.0f});
    }
    if (c == Card::Bloom && knob == 0) s.push_back({0.0f, kContinuousStep});
    // Lo-fi's TONE and AMOUNT also step at the last touch below the top of their travel.
    if (c == Card::Lofi) s.push_back({1.0f - kContinuousStep, 1.0f});
    return s;
  }
  if (detents <= kWalkedDetents) {
    std::vector<Step> s;
    for (int d = 0; d + 1 < detents; ++d) s.push_back({static_cast<float>(d), static_cast<float>(d + 1)});
    return s;
  }
  const float lo = static_cast<float>(detents / 4);
  const float hi = static_cast<float>(3 * detents / 4);
  return {{lo, lo + 1.0f}, {hi, hi + 1.0f}};
}

// Echo's Amount starts at 0, where the echo has no gain and its Time never reaches the output, so its
// background holds Amount at half travel.
constexpr float kEchoBackgroundAmount = 0.5f;

// The registry's defaults with the card placed and switched on. Distortion's DRIVE starts at 0, where the
// stage is a plain copy and its TONE never reaches the output, so its background holds DRIVE at the level
// rule's setting.
ChainParams background(Card c) {
  ChainParams p;
  applyPlacement(c, p);
  applyToggle(c, true, p);
  if (c == Card::Slapback) applyKnob(c, 1, kEchoBackgroundAmount, p);
  if (c == Card::Distortion) applyKnob(c, 1, kDistortionRuleDrive, p);
  return p;
}

// Echo's Time bends the repeat while the delay glides, and a pure tone through a comb shows the dry
// and the echo beating as their phase turns, which is not a click. The Time row reads the echo
// alone (the output less the input, with the trim off), whose curvature stays at its steady value
// through the glide and jumps at a click in the delay. The steps are upward touches, as the knob
// filter delivers them, so the repeat's pitch falls and its curvature stays below the steady value.
bool readsEchoAlone(Card c, int knob) { return c == Card::Slapback && knob == 0; }

bool checkKnob(Card c, int knob, const Signal& in, double toneHz = kToneHz) {
  const ChainParams base = background(c);
  const bool echoAlone = readsEchoAlone(c, knob);
  double worst = 0.0;
  Step at{};
  for (const Step& s : stepsFor(c, knob)) {
    ChainParams a = base;
    ChainParams b = base;
    applyKnob(c, knob, s.from, a);
    applyKnob(c, knob, s.to, b);
    if (echoAlone) a.slapback.tuning.trimDb = b.slapback.tuning.trimDb = 0.0f;
    Signal out = runCard(c, in, a, b);
    if (echoAlone) {
      for (size_t i = 0; i < out.size(); ++i) out[i] -= in[i];
    }
    const double r = stepRatio(out);
    if (r >= worst) {
      worst = r;
      at = s;
    }
  }
  const bool ok = worst <= kMaxRatio;
  char pitch[24] = "";
  if (toneHz != kToneHz) std::snprintf(pitch, sizeof pitch, " at %g Hz", toneHz);
  std::printf("%s %-14s %-10s worst step %.3g to %.3g: peak curvature x%.5f of steady state (<= %.2f)%s\n",
              ok ? "PASS" : "FAIL", cardDesc(c).name, cardDesc(c).knob[knob].name, static_cast<double>(at.from),
              static_cast<double>(at.to), worst, kMaxRatio, pitch);
  return ok;
}

// Distortion's DRIVE moves, the ones that start it included. The box's enable edge resets an engine
// and then moves nothing but the toggle, so distortion_test holds that reset to a fresh engine's output.
Signal runDistortionStep(const Signal& in, const cv::DistortionParams& a, const cv::DistortionParams& b) {
  auto fx = std::make_unique<cv::Distortion>();
  fx->reset();
  Signal out(in.size());
  for (int i = 0; i < kTotal; i += cv::kBlock) {
    const size_t k = static_cast<size_t>(i);
    fx->process(&in[k], &out[k], cv::kBlock, i < kStepAt ? a : b);
  }
  return out;
}

cv::DistortionParams distortionAt(float drive, float tone) {
  cv::DistortionParams p = background(Card::Distortion).distortion;
  p.drive = drive;
  p.tone = tone;
  return p;
}

bool distortionRow(const char* label, const Signal& in, const cv::DistortionParams& a, const cv::DistortionParams& b) {
  const double r = stepRatio(runDistortionStep(in, a, b));
  const bool ok = r <= kMaxRatio;
  std::printf("%s %-14s %-40s peak curvature x%.5f of steady state (<= %.2f)\n", ok ? "PASS" : "FAIL", "Distortion",
              label, r, kMaxRatio);
  return ok;
}

// DRIVE up from 0, at DRIVE 30, 60 and 100 % and TONE 0, 50 and 100 %. TONE follows the knob while the
// stage is dry, so both start from where the knob sits, and the dry-to-wet fade takes 16 ms at the least.
// DRIVE crossing 5 % turns the stage on, and falling under 4 % turns it off; the last rows are turns
// across both thresholds, the last a fast one from 6 % down.
bool distortionEdgeRows(const Signal& in) {
  bool ok = true;
  char label[64];
  for (float drive : {kDistortionRuleDrive, 0.6f, 1.0f}) {
    for (float tone : {0.0f, 0.5f, 1.0f}) {
      std::snprintf(label, sizeof label, "DRIVE 0 to %.0f %%, TONE %.0f %%", static_cast<double>(drive * 100.0f),
                    static_cast<double>(tone * 100.0f));
      ok &= distortionRow(label, in, distortionAt(0.0f, tone), distortionAt(drive, tone));
    }
  }
  struct Cross {
    float from;
    float to;
  };
  for (const Cross& c : {Cross{0.045f, 0.06f}, Cross{0.035f, 0.05f}, Cross{0.05f, 0.035f}, Cross{0.06f, 0.045f},
                         Cross{0.06f, 0.03f}}) {
    std::snprintf(label, sizeof label, "DRIVE %.1f to %.1f %%", static_cast<double>(c.from * 100.0f),
                  static_cast<double>(c.to * 100.0f));
    ok &= distortionRow(label, in, distortionAt(c.from, 0.5f), distortionAt(c.to, 0.5f));
  }
  return ok;
}

// Unison runs at DEPTH 50 % in the background, where the chorus is audible, and judges each step against
// steady windows that span a full period of the slowest LFO the steps start from (RATE 20 %, 0.31 Hz and
// 3.2 s) and against the same run with no step. The swing and the LFO speeds glide on 150 ms, so the
// second steady window starts 1.5 s after the step.
constexpr float kUnisonBackgroundDepth = 0.5f;
constexpr int kUnisonStepAt = 6 * cv::kSampleRate;
constexpr int kUnisonSteadyWindow = 4 * cv::kSampleRate;
constexpr int kUnisonSettle = 3 * cv::kSampleRate / 2;
constexpr int kUnisonTotal = kUnisonStepAt + kUnisonSettle + kUnisonSteadyWindow;
static_assert(kUnisonStepAt % cv::kBlock == 0, "the step lands on a block edge");

Signal runUnison(const Signal& in, const cv::UnisonParams& a, const cv::UnisonParams& b) {
  auto fx = std::make_unique<cv::Unison>();
  fx->reset();
  Signal out(in.size());
  for (int i = 0; i < kUnisonTotal; i += cv::kBlock) {
    const size_t k = static_cast<size_t>(i);
    fx->process(&in[k], &out[k], cv::kBlock, i < kUnisonStepAt ? a : b);
  }
  return out;
}

// Peak curvature just after the step over the larger steady-state peak of the two settings.
double unisonStepRatio(const Signal& out) {
  const double steady = std::max(peakCurvature(out, kUnisonStepAt - kUnisonSteadyWindow, kUnisonStepAt),
                                 peakCurvature(out, kUnisonTotal - kUnisonSteadyWindow, kUnisonTotal));
  return peakCurvature(out, kUnisonStepAt, kUnisonStepAt + kStepWindow) / steady;
}

bool checkUnisonKnob(int knob, const Signal& in) {
  ChainParams base = background(Card::Unison);
  applyKnob(Card::Unison, 1, kUnisonBackgroundDepth, base);
  double worst = 0.0;
  double worstStill = 0.0;
  Step at{};
  std::vector<Step> steps;
  for (float p : {0.2f, 0.5f, 0.8f}) steps.push_back({p, p + kContinuousStep});
  if (knob == 0) {
    // RATE also moves its whole travel in one step each way, as a fast turn would. DEPTH's whole-travel
    // jump fades the wet itself, which bends the output by the fade's own slope and is no click.
    steps.push_back({0.0f, 1.0f});
    steps.push_back({1.0f, 0.0f});
  }
  for (const Step& s : steps) {
    ChainParams a = base;
    ChainParams b = base;
    applyKnob(Card::Unison, knob, s.from, a);
    applyKnob(Card::Unison, knob, s.to, b);
    const Signal out = runUnison(in, a.unison, b.unison);
    const double r = unisonStepRatio(out);
    if (r >= worst) {
      worst = r;
      at = s;
    }
    // A touch is read against the same run with no step; a whole-travel jump moves the chorus legitimately.
    if (std::fabs(s.to - s.from) > kContinuousStep * 1.01f) continue;
    const Signal still = runUnison(in, a.unison, a.unison);
    const double stillStep = peakCurvature(still, kUnisonStepAt, kUnisonStepAt + kStepWindow);
    worstStill = std::max(worstStill, peakCurvature(out, kUnisonStepAt, kUnisonStepAt + kStepWindow) / stillStep);
  }
  const bool ok = worst <= kMaxRatio && worstStill <= kMaxRatio;
  std::printf("%s %-14s %-10s worst step %.3g to %.3g: peak curvature x%.5f of steady state, x%.5f of no step (<= %.2f)\n",
              ok ? "PASS" : "FAIL", cardDesc(Card::Unison).name, cardDesc(Card::Unison).knob[knob].name,
              static_cast<double>(at.from), static_cast<double>(at.to), worst, worstStill, kMaxRatio);
  return ok;
}

bool unisonRows() {
  const Signal in = tone(kToneHz, kUnisonTotal);
  bool ok = checkUnisonKnob(0, in);
  ok &= checkUnisonKnob(1, in);
  return ok;
}

// KEY moves the target note, and a grain is two periods, so a low voice is the harder case for the
// glide. Adam sings from 78 Hz, and the KEY gate also runs on sines at 98 Hz, 82 Hz and 74.5 Hz.
constexpr double kLowTones[] = {98.0, 82.0, 74.5};

}  // namespace

int main() {
  bool ok = true;
  for (int i = 0; i < kCardCount; ++i) {
    const Card c = static_cast<Card>(i);
    if (engineOf(c) == Engine::Growl || engineOf(c) == Engine::RingMod || engineOf(c) == Engine::Strings) continue;
    if (c == Card::Unison) {
      ok &= unisonRows();
      continue;
    }
    const Signal in = tone(kToneHz);
    for (int k = 0; k < cardDesc(c).knobCount; ++k) ok &= checkKnob(c, k, in);
    if (c == Card::Autotune) {
      for (double hz : kLowTones) ok &= checkKnob(c, 0, tone(hz), hz);
    }
    if (c == Card::Distortion) ok &= distortionEdgeRows(in);
  }
  std::printf("%s knob step\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
