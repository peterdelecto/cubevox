#include "core/knob_filter.h"

#include <math.h>

namespace {

constexpr float kSmoothing = 0.3f;        // one-pole step per update
constexpr float kMoveThreshold = 0.015f;  // knob travel that counts as touched

}  // namespace

bool DebouncedInput::update(bool raw, uint32_t nowMs, uint32_t holdMs) {
  if (raw != candidate_) {
    candidate_ = raw;
    candidateSinceMs_ = nowMs;
    return false;
  }
  if (candidate_ == stable_ || nowMs - candidateSinceMs_ < holdMs) return false;
  stable_ = candidate_;
  return true;
}

int steppedPosition(float norm, int positions, int current, float hysteresis) {
  const float band = 1.0f / static_cast<float>(positions);
  int pos = static_cast<int>(norm * static_cast<float>(positions));
  if (pos >= positions) pos = positions - 1;
  if (pos < 0) pos = 0;
  if (pos == current) return current;

  const float lo = static_cast<float>(current) * band - hysteresis * band;
  const float hi = static_cast<float>(current + 1) * band + hysteresis * band;
  return (norm >= lo && norm <= hi) ? current : pos;
}

void KnobFilter::reseed(int steps) {
  stepsSeeded_ = steps;
  step_ = steps > 0 ? steppedPosition(smoothed_, steps, 0, 0.0f) : 0;
}

KnobEvent KnobFilter::update(float norm, int steps) {
  if (!seeded_) {
    smoothed_ = norm;
    reported_ = norm;
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
