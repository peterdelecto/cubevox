#pragma once

#include <array>
#include <cstring>

#include "engine/autotune.h"
#include "engine/common.h"
#include "engine/fastmath.h"
#include "engine/input_ring.h"
#include "engine/pitch.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// The Autotune card's engine. It keeps a ring of its own input and a pitch tracker on that input, and
// crossfades from the dry voice to the corrected one (equal-power) once the tracker has a period. The
// tracker also serves the readers behind the card (Formant or Apraxia directly behind it, Mob and
// Deepthroat), so the chain asks for it through TrackerNeeds, and the engine keeps it warm while a reader
// is on, whatever Autotune's own toggle does. Settled off with no reader, the output is the input bit for
// bit.
//
// The Formant engine directly behind the card cuts its grains from this card's input, on this grid, so the
// formant-shifted voice waits for the corrected voice once. The chain feeds it the input, and groupFeed()
// hands it what it needs to cut: the tracked period, the pitch ratio, the crossfade's gains and the grain
// grid.
//
// A voice resets on its enable edge only once its crossfade has settled at zero, so a quick off and on
// keeps the running voice instead of cutting to fresh grains. The tracker's frame is warm only while the
// engine runs. A stopped engine calls rest() each block, and the next call after it starts clean, so the
// first voiced frame comes about 32 ms in where a running engine gives it in 8 ms. At a note that starts
// after a rest, the period the voice reads moves to the new note from the tracker's onset estimate, ahead
// of its first voiced frame (pitch.h). The correction still moves only on voiced frames.

namespace cv {

// The ring holds the deepest grain read (the grain delay, one and a half longest periods and a tap), a
// block and a pad. The tracker's replay needs less.
constexpr int kAutotuneRingLen = 2176;
static_assert(kAutotuneRingLen >= PsolaVoice::kGrainDelay + 3 * PitchTracker::kMaxPeriod / 2 + 1 + kBlock + 4,
              "the ring holds the deepest grain read");
static_assert(PitchTracker::kReplaySpan < kAutotuneRingLen, "the ring holds a tracker replay's history");

// What the readers behind the card ask of its tracker.
struct TrackerNeeds {
  bool run = false;         // a reader is on: the tracker runs whatever Autotune's toggle does
  bool fine = false;        // the reader needs the 48 kHz fine pass
  bool outOfRange = false;  // the reader reads PitchResult::outOfRange
};

// What the Formant engine directly behind the card reads for the chunk the engine just ran, n samples. The
// audio that leaves the card is `tuned` times the corrected voice plus `dry` times the card's input, and
// `fade` is the crossfade's linear position, 0 dry to 1 corrected, at the chunk's first and last sample.
struct GroupFeed {
  int n = 0;
  float period = 0.0f;  // the tracker's period for the chunk
  float delay = 0.0f;   // the corrected voice's delay behind the input at that period
  float ratio = 1.0f;   // the pitch ratio of the correction at the middle of the chunk
  float trim = 1.0f;    // the gain the corrected voice's grains carry
  float fade0 = 0.0f;
  float fade1 = 0.0f;
  float tuned0 = 0.0f;
  float tuned1 = 0.0f;
  float dry0 = 1.0f;
  float dry1 = 1.0f;
  GridMarks marks;      // the corrected voice's grain grid as the chunk's render started; valid while it renders
};

class AutotuneFx {
 public:
  AutotuneFx() { reset(); }

  // Leaves the ring zero and the tracker cold. A card that rested since it last ran (see rest) has
  // cleared most of the ring already, and this clears what is left.
  void reset() {
    ring_.clear();
    tracker_.reset();
    voice_.reset();
    fade_ = 0.0f;
    gains_ = Gains{};
    feed_ = GroupFeed{};
    was_ = false;
    fresh_ = true;
    rested_ = false;
  }

  // A call for each block the card sits stopped (the chain does not run it). It zeroes the next
  // InputRing::kRestClearFloats floats of the ring, so the reset that starts the card again finds the ring
  // clear and the first block after a switch-on carries none of the clear.
  void rest() {
    ring_.rest();
    rested_ = true;
  }

  // Runs n samples of any call length, in chunks of at most kBlock. in and out may be the same buffer.
  // `grid` is the grain grid of the Formant voice directly behind the card when one is running. A corrected
  // voice that starts takes its newest mark from it, so the two lay their grains on one grid from the start.
  void process(const float* in, float* out, int n, const AutotuneParams& p, const TrackerNeeds& need = TrackerNeeds{},
               const GridMarks& grid = GridMarks{}) {
    if (rested_) reset();
    for (int c = 0; c < n; c += kBlock) chunk(in + c, out + c, n - c < kBlock ? n - c : kBlock, p, need, grid);
  }

  // The tracker's reading of the audio that entered the card.
  const PitchResult& pitch() const { return tracker_.result(); }

  // Autotune's smoothed correction in semitones. It is the live value while Autotune is on, runs down
  // with the crossfade after Autotune goes off, and is exactly 0 once the crossfade settles. The
  // crossfade takes about 280 ms to settle, so the scaling lets a reader follow the dry voice back
  // instead of holding the old correction until then.
  float correctionSemis() const {
    const float live = voice_.correctionSemis();
    return was_ ? live : live * fade_;
  }

  // The delay of the card's output behind its input in samples, the mean over the voices heard. The dry
  // input has none. The corrected voice lags by the grain delay plus a period, and its share follows the
  // crossfade's power.
  float outputLatency() const {
    return gains_.corrected * gains_.corrected * voice_.voiceDelay(tracker_.result().period);
  }

  // What the Formant engine directly behind the card cuts from, for the last chunk the engine ran. A call
  // longer than kBlock runs several chunks and leaves the last one's.
  const GroupFeed& groupFeed() const { return feed_; }

  // Test hooks: the ring, and the floats the ring clears have zeroed.
  const InputRing<kAutotuneRingLen>& ringForTest() const { return ring_; }
  uint32_t ringClearedForTest() const { return ring_.clearedForTest(); }

 private:
  // The crossfade's gains (equal-power): the corrected voice's share and the dry input's.
  struct Gains {
    float corrected = 0.0f;
    float dry = 1.0f;
  };

  Gains gainsNow() const {
    Gains g;
    quarterTurn(fade_, g.corrected, g.dry);
    return g;
  }

  void chunk(const float* in, float* out, int n, const AutotuneParams& p, const TrackerNeeds& need,
             const GridMarks& grid) {
    // The tracker runs while Autotune is on or still fading, so the voice keeps its period through the
    // crossfade, and while a reader behind is on. The 48 kHz fine pass runs for Autotune's own voice,
    // which must sit on the sung pitch to the cent, and for a reader that asks.
    const bool tuning = p.on || fade_ > 0.0f;
    const int first = ring_.write(in, n);
    tracker_.readOutOfRange(need.outOfRange);
    if (tuning || need.run) {
      tracker_.push(in, n, true, tuning || need.fine, PitchTracker::History{ring_.data().data(), kAutotuneRingLen, first});
    } else {
      tracker_.idle(n);
    }
    const PitchResult& pr = tracker_.result();
    if (p.on && !was_ && fade_ == 0.0f) {
      voice_.reset();
      voice_.adoptMarks(grid);
    }
    was_ = p.on;
    voice_.prepare(pr, p, n);

    // The period stays set while the crossfade is up (the tracker runs until it settles), so the grains
    // never lose their pitch mid-fade.
    const float target = p.on && pr.period > 0.0f ? 1.0f : 0.0f;
    if (fresh_) {
      fade_ = target;
      gains_ = gainsNow();
      fresh_ = false;
    }
    if (fade_ == 0.0f && target == 0.0f) {
      gains_ = Gains{};
      feed_ = GroupFeed{};
      feed_.n = n;
      feed_.period = pr.period;
      feed_.delay = voice_.voiceDelay(pr.period);
      feed_.trim = voice_.trimGain();
      if (out != in) std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
      return;
    }
    const Gains g0 = gains_;
    const float fade0 = fade_;
    if (fade_ != target) {
      fade_ = smooth::step(fade_, target, coef_.at(n));
      gains_ = gainsNow();
    }
    const Gains g1 = gains_;
    noteFeed(n, pr.period, fade0, g0, g1);
    mix(in, out, n, first, pr.period, g0, g1);
  }

  // Records what the Formant engine behind the card reads for this chunk. It runs before the voice
  // renders, so the pitch ratio and the grid are the chunk's own, not the end of it.
  void noteFeed(int n, float period, float fade0, const Gains& g0, const Gains& g1) {
    feed_.n = n;
    feed_.period = period;
    feed_.delay = voice_.voiceDelay(period);
    feed_.ratio = exp2Fast(voice_.correctionAfter(0.5f * static_cast<float>(n)) * (1.0f / 12.0f));
    feed_.trim = voice_.trimGain();
    feed_.fade0 = fade0;
    feed_.fade1 = fade_;
    feed_.tuned0 = g0.corrected;
    feed_.tuned1 = g1.corrected;
    feed_.dry0 = g0.dry;
    feed_.dry1 = g1.dry;
    feed_.marks = voice_.marks();
    feed_.marks.valid = true;
  }

  // The corrected voice for the chunk, mixed against the input. The caller returns early while the
  // crossfade sits at 0 at both edges, so the voice always renders here.
  void mix(const float* in, float* out, int n, int first, float period, const Gains& g0, const Gains& g1) {
    voice_.render(ring_.data(), first, n, period, buf_.data());
    const float inv = 1.0f / static_cast<float>(n);
    const float dCor = (g1.corrected - g0.corrected) * inv;
    const float dDry = (g1.dry - g0.dry) * inv;
    float cor = g0.corrected;
    float dry = g0.dry;
    for (int j = 0; j < n; ++j) {
      cor += dCor;
      dry += dDry;
      float y = in[j];
      if (cor > 0.0f) y = cor * buf_[static_cast<size_t>(j)] + dry * y;
      out[j] = y;
    }
  }

  InputRing<kAutotuneRingLen> ring_;
  PitchTracker tracker_;
  AutotuneVoiceT<kAutotuneRingLen> voice_;
  std::array<float, kBlock> buf_{};  // the corrected voice's chunk
  GroupFeed feed_;                   // what the Formant engine behind the card reads for the last chunk
  float fade_ = 0.0f;                // 0 dry .. 1 corrected
  Gains gains_;                      // the gains at the end of the last chunk
  smooth::BlockCoef coef_;  // the 20 ms one-pole's step over the last chunk length
  bool was_ = false;     // Autotune was on last chunk
  bool fresh_ = true;    // the next chunk takes its target outright
  bool rested_ = false;  // the engine sat stopped since it last ran
};

}  // namespace cv
