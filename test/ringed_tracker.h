#pragma once

// A PitchTracker with the ring it reads, for the tests that run a tracker without a card's engine. The
// tracker's fine pass reads the caller's ring, so each push writes the call into the ring first, as
// the engines do.

#include <vector>

#include "engine/pitch.h"
#include "engine/psola.h"

class RingedTracker : public cv::PitchTracker {
 public:
  using cv::PitchTracker::push;

  RingedTracker() : ring_(static_cast<size_t>(cv::kVoiceRingLen), 0.0f) {}

  void push(const float* in, int n, bool active = true, bool fine = true) {
    const int first = head_;
    for (int i = 0; i < n; ++i) {
      ring_[static_cast<size_t>(head_)] = in[i];
      head_ = head_ + 1 == cv::kVoiceRingLen ? 0 : head_ + 1;
    }
    cv::PitchTracker::push(in, n, active, fine, History{ring_.data(), cv::kVoiceRingLen, first});
  }

 private:
  std::vector<float> ring_;
  int head_ = 0;
};
