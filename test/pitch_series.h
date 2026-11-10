#pragma once

// The Autotune, Formant and Octave engines in series, in that order, each on the output of the one before,
// as the chain runs them in three adjacent slots. The tests that drive the pitch cards together use it. As the
// chain does when the Formant card sits directly behind the Autotune card, it feeds the Formant engine the
// audio that enters Autotune and hands it Autotune's tracker and engine, and it gathers the shift and the
// delay the cards put on the pitch for the Growl card.
// `autotunePlaced` false models a layout with no Autotune card: the Formant engine runs its own tracker and
// the Growl and Crowd cards run theirs. While the Formant card is off with its fade down it idles and passes
// its input, as a stopped slot does, and Autotune's tracker runs for it only while it can be heard.

#include <algorithm>
#include <array>

#include "engine/autotune_fx.h"
#include "engine/crowd.h"
#include "engine/formant_fx.h"
#include "engine/octave_fx.h"
#include "engine/pitch_params.h"

class PitchSeries {
 public:
  explicit PitchSeries(bool autotunePlaced = true) : placed_(autotunePlaced) {}

  void reset() {
    autotune.reset();
    formant.reset();
    octave.reset();
    up_ = cv::UpstreamPitch{};
  }

  // n samples of any length, in chunks of at most kBlock. `readers` (or the member readersOn) says a Crowd
  // or Growl card is on, so Autotune's tracker runs for it. in and out may be the same buffer.
  void process(const float* in, float* out, int n, const cv::PitchCardParams& p, bool readers = false) {
    for (int c = 0; c < n; c += cv::kBlock)
      chunk(in + c, out + c, n - c < cv::kBlock ? n - c : cv::kBlock, p, readers || readersOn);
  }

  bool readersOn = false;

  // Autotune's tracker and correction, and the shift and delay of the whole series' output.
  const cv::PitchResult& pitch() const { return placed_ ? autotune.pitch() : formant.ownPitch(); }
  float correctionSemis() const { return placed_ ? autotune.correctionSemis() : 0.0f; }
  float outputShiftSemis() const { return up_.shiftSemis; }
  float outputLatency() const { return up_.latency; }
  float shiftSemis() const { return formant.shiftSemis(); }
  float formantFade() const { return formant.fade(); }

  // What the chain hands a Growl card behind the series, and a Crowd card.
  cv::UpstreamPitch upstream() const { return placed_ ? up_ : cv::UpstreamPitch{}; }
  cv::CrowdPitch crowdPitch() const {
    return placed_ ? cv::CrowdPitch{&autotune.pitch(), autotune.correctionSemis()} : cv::CrowdPitch{};
  }

  cv::AutotuneFx autotune;
  cv::FormantFx formant;
  cv::OctaveFx octave;

 private:
  void chunk(const float* in, float* out, int n, const cv::PitchCardParams& p, bool readers) {
    const bool formantLive = p.formant.on || formant.fade() > 0.0f;
    const bool formantReads = formantLive && formant.needsTracker(p.formant);
    cv::UpstreamPitch up;
    const float* src = in;
    if (placed_) {
      cv::TrackerNeeds need;
      need.run = readers || formantReads;
      need.fine = formantReads;
      need.outOfRange = readers || formantReads;
      formant.feed(src, n);
      autotune.process(src, a_.data(), n, p.autotune, need, formantLive ? formant.gridMarks() : cv::GridMarks{});
      up = {&autotune.pitch(), autotune.correctionSemis(), autotune.outputLatency()};
      up.group = &autotune;
      src = a_.data();
    }
    if (formantLive) {
      formant.process(src, b_.data(), n, p.formant, up);
    } else {
      formant.idle(src, n);
      std::copy(src, src + n, b_.begin());
    }
    if (up.result != nullptr) up.latency += formant.outputLatency();
    octave.process(b_.data(), out, n, p.octave);
    if (up.result != nullptr) {
      if (octave.leads()) up.shiftSemis += octave.intervalSemis();
      up.latency += octave.outputLatency();
    }
    up_ = up;
  }

  bool placed_;
  cv::UpstreamPitch up_;
  std::array<float, cv::kBlock> a_{};
  std::array<float, cv::kBlock> b_{};
};
