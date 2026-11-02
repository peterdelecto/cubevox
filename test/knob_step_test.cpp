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
Signal tone(double hz) {
  Signal x(static_cast<size_t>(kTotal));
  for (int i = 0; i < kTotal; ++i)
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

// The registry's defaults with the card placed and switched on.
ChainParams background(Card c) {
  ChainParams p;
  applyPlacement(c, p);
  applyToggle(c, true, p);
  if (c == Card::Slapback) applyKnob(c, 1, kEchoBackgroundAmount, p);
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

// KEY moves the target note, and a grain is two periods, so a low voice is the harder case for the
// glide. Adam sings from 78 Hz, and the KEY gate also runs on sines at 98 Hz, 82 Hz and 74.5 Hz.
constexpr double kLowTones[] = {98.0, 82.0, 74.5};

}  // namespace

int main() {
  bool ok = true;
  for (int i = 0; i < kCardCount; ++i) {
    const Card c = static_cast<Card>(i);
    const Signal in = tone(kToneHz);
    for (int k = 0; k < cardDesc(c).knobCount; ++k) ok &= checkKnob(c, k, in);
    if (c == Card::Autotune) {
      for (double hz : kLowTones) ok &= checkKnob(c, 0, tone(hz), hz);
    }
  }
  std::printf("%s knob step\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
