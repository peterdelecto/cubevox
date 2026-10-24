#include "core/knob_filter.h"

#include <math.h>

namespace {

constexpr float kSmoothing = 0.3f;        // one-pole step per update
constexpr float kMoveThreshold = 0.015f;  // knob travel that counts as touched

}  // namespace

float knobPosition(float adcFraction) {
  const float n = (adcFraction - kKnobEndMargin) / (1.0f - 2.0f * kKnobEndMargin);
  if (!(n > 0.0f)) return 0.0f;  // also catches NaN
  return n > 1.0f ? 1.0f : n;
}

int steppedPosition(float norm, int positions, int current, float hysteresis) {
  if (positions <= 0) return 0;
  if (!(norm > 0.0f)) norm = 0.0f;  // also catches NaN
  if (norm > 1.0f) norm = 1.0f;

  const float band = 1.0f / static_cast<float>(positions);
  int pos = static_cast<int>(norm * static_cast<float>(positions));
  if (pos >= positions) pos = positions - 1;
  if (pos == current) return current;

  const float lo = static_cast<float>(current) * band - hysteresis * band;
  const float hi = static_cast<float>(current + 1) * band + hysteresis * band;
  return (norm >= lo && norm <= hi) ? current : pos;
}

void KnobFilter::reseed(int steps) {
  stepsSeeded_ = steps;
  step_ = steps > 0 ? steppedPosition(smoothed_, steps, 0, 0.0f) : 0;
  reported_ = smoothed_;
}

KnobEvent KnobFilter::restep(int steps) {
  if (!seeded_ || steps == stepsSeeded_) return KnobEvent::None;
  reseed(steps);
  return KnobEvent::Reseeded;
}

KnobEvent KnobFilter::update(float norm, int steps) {
  if (!seeded_) {
    smoothed_ = norm;
    seeded_ = true;
    reseed(steps);
    return KnobEvent::Seeded;
  }
  smoothed_ += kSmoothing * (norm - smoothed_);
  if (steps != stepsSeeded_) {
    reseed(steps);
    return KnobEvent::Reseeded;
  }
  if (steps > 0) {
    const int next = steppedPosition(smoothed_, steps, step_);
    if (next == step_) return KnobEvent::None;
    step_ = next;
    return KnobEvent::Moved;
  }
  if (fabsf(smoothed_ - reported_) > kMoveThreshold) {
    reported_ = smoothed_;
    return KnobEvent::Moved;
  }
  return KnobEvent::None;
}

void KnobBank::setSteps(const std::array<int, kPotCount>& steps) {
  for (size_t i = 0; i < steps.size(); ++i) {
    if (steps[i] == steps_[i]) continue;
    steps_[i] = steps[i];
    if (knob_[i].restep(steps[i]) == KnobEvent::Reseeded) changed_ = true;
  }
}

void KnobBank::update(int pot, float position, LastMoved owner) {
  if (pot < 0 || pot >= kPotCount) return;
  const size_t i = static_cast<size_t>(pot);
  switch (knob_[i].update(position, steps_[i])) {
    case KnobEvent::Seeded:
      if (++seededCount_ == kPotCount) changed_ = true;
      break;
    case KnobEvent::Reseeded:
      changed_ = true;
      break;
    case KnobEvent::Moved:
      if (owner.slot < 0) break;
      lastMoved_ = owner;
      touched_ = true;
      changed_ = true;
      break;
    case KnobEvent::None:
      break;
  }
}

bool KnobBank::takeChanged() {
  const bool changed = changed_;
  changed_ = false;
  return changed;
}

bool KnobBank::takeTouch(LastMoved& out) {
  if (!touched_) return false;
  touched_ = false;
  out = lastMoved_;
  return true;
}
