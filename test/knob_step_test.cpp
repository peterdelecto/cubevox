// Knob-step clicks: every card's knobs move by one knob-filter touch (1.5 %) or one detent while a
// tone plays. Right after the step the output may bend no harder than it does in steady state at
// either setting, so a jump in a gain, a delay tap or a filter state shows as a spike in the second
// difference. Settings come from the registry, as the box applies them. No window, no audio device.

#include <algorithm>
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
Signal tone() {
  Signal x(static_cast<size_t>(kTotal));
  for (int i = 0; i < kTotal; ++i)
    x[static_cast<size_t>(i)] = static_cast<float>(kToneAmp * std::sin(2.0 * kPi * kToneHz * i / cv::kSampleRate));
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

// The engine a card runs, mapped as runStage in board/audio.cpp maps it.
Signal runCard(Card c, const Signal& in, const ChainParams& a, const ChainParams& b) {
  switch (engineOf(c)) {
    case Engine::InputGate: return runFx<cv::Gate>(in, a, b, &ChainParams::inputGate);
    case Engine::Pitch: return runFx<cv::PitchFx>(in, a, b, &ChainParams::pitchFx);
    case Engine::Unison: return runFx<cv::Unison>(in, a, b, &ChainParams::unison);
    case Engine::Slapback: return runFx<cv::Slapback>(in, a, b, &ChainParams::slapback);
    case Engine::Distortion: return runFx<cv::Distortion>(in, a, b, &ChainParams::distortion);
    case Engine::Gate: return runFx<cv::Gate>(in, a, b, &ChainParams::gate);
    case Engine::Reverb: return runFx<cv::Reverb>(in, a, b, &ChainParams::reverb);
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

// Continuous knobs step at a low, a middle and a high setting; stepped knobs move one detent at a
// quarter and at three quarters of their travel.
std::vector<Step> stepsFor(Card c, int knob) {
  const int detents = knobSteps(c, knob);
  if (detents == 0) {
    std::vector<Step> s;
    for (float p : {0.2f, 0.5f, 0.8f}) s.push_back({p, p + kContinuousStep});
    return s;
  }
  const float lo = static_cast<float>(detents / 4);
  const float hi = static_cast<float>(3 * detents / 4);
  return {{lo, lo + 1.0f}, {hi, hi + 1.0f}};
}

// The registry's defaults with the card placed and switched on.
ChainParams background(Card c) {
  ChainParams p;
  applyPlacement(c, p);
  applyToggle(c, true, p);
  return p;
}

bool checkKnob(Card c, int knob, const Signal& in) {
  const ChainParams base = background(c);
  double worst = 0.0;
  Step at{};
  for (const Step& s : stepsFor(c, knob)) {
    ChainParams a = base;
    ChainParams b = base;
    applyKnob(c, knob, s.from, a);
    applyKnob(c, knob, s.to, b);
    const double r = stepRatio(runCard(c, in, a, b));
    if (r >= worst) {
      worst = r;
      at = s;
    }
  }
  const bool ok = worst <= kMaxRatio;
  std::printf("%s %-14s %-10s worst step %.3g to %.3g: peak curvature x%.5f of steady state (<= %.2f)\n",
              ok ? "PASS" : "FAIL", cardDesc(c).name, cardDesc(c).knob[knob].name, static_cast<double>(at.from),
              static_cast<double>(at.to), worst, kMaxRatio);
  return ok;
}

}  // namespace

int main() {
  const Signal in = tone();
  bool ok = true;
  for (int i = 0; i < kCardCount; ++i) {
    const Card c = static_cast<Card>(i);
    for (int k = 0; k < cardDesc(c).knobCount; ++k) ok &= checkKnob(c, k, in);
  }
  std::printf("%s knob step\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
