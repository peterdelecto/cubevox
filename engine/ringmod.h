#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch.h"
#include "engine/scale.h"
#include "engine/smooth.h"

namespace cv {

// The Ringer card. A sine carrier at RATIO times a held note multiplies the voice, so the clang stays in
// tune and follows the melody in steps. The card tracks the pitch of its own input, the voice the slot
// ahead of it hands over, with a PitchTracker and a short ring of its own, so an Octave card ahead moves
// the carrier with the voice. The note is the one nearest the tracked pitch. It is a major-scale note of
// the Autotune card's key when that card is placed, and any semitone (A4 = 440 Hz) when not. The
// held note changes only when the tracked pitch sits closer to another note by more than 0.3 semitone, so
// vibrato leaves the carrier still, and only when that same note has won for 60 ms in a row, so a stray
// tracker frame at an onset leaves it still too. The count restarts when the candidate changes or the margin
// test fails, and unvoiced frames pause it without touching the held note. The first voiced note after a
// reset sets the carrier at once. A settled note moves the carrier in the same block, with no glide, and the
// phase counter carries on. The carrier is a phase counter read through sinTurns. Unvoiced frames hold the
// last note, and before the first voiced frame the carrier sits at 220 Hz times RATIO.
//
// A note start is the first voiced frame after the tracker has read unvoiced for gapMs. The wet ducks to the
// dry voice (8 ms) when that gap begins, and the carrier stays on the old note while the voice scoops into
// the new one. The note lands once the tracked pitch has stayed within landSemis for landMs (or after
// landMaxMs). The carrier then jumps to the nearest note in one step, and the wet fades back in over 20 ms.
// A voice that returns to the held note lands after landAgreeMs and leaves the carrier alone. The tracker's
// first voiced frame after a gap can still carry the old period, so no single frame lands a note. Moves
// with no gap before them (legato) follow the 60 ms rule above and keep the wet up.
// MIX is an equal-power crossfade, and the wet alone is trimmed to unity.

constexpr int kRingModRatioCount = 7;
constexpr std::array<float, kRingModRatioCount> kRingModRatio = {0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 4.0f};
constexpr std::array<const char*, kRingModRatioCount> kRingModRatioName = {"X0.5", "X1",   "X1.5", "X2",
                                                                            "X2.5", "X3",   "X4"};
constexpr uint8_t kRingModDefaultRatioStep = 2;

// The card's input ring holds what its tracker reads and no more, 5,040 B.
constexpr int kRingModRingLen = PitchTracker::kRingNeed;

// The floats of the ring one rest() call zeroes, so the ring is clear after three calls.
constexpr int kRingModRestFloats = 512;

struct RingModTuning {
  float holdSemis = 0.3f;   // the pitch must sit this much closer to another note before the held note changes
  float settleMs = 60.0f;   // a new note must win this long in a row before the carrier moves to it
  float gapMs = 20.0f;      // unvoiced this long ends a note: the wet ducks and the next voiced frame starts a note
  float landMs = 40.0f;     // a note start lands once the pitch has stayed within landSemis this long
  float landAgreeMs = 15.0f;  // the same, when the pitch sits on the held note
  float landSemis = 0.3f;   // the width of the band the pitch must stay in to count as landed
  float landMaxMs = 140.0f; // a note start lands after this long however the pitch moves
  float restHz = 220.0f;    // the pitch the carrier assumes before the first voiced frame
  float trimDb = 3.30f;     // the wet path's gain; a sine carrier alone leaves the voice 3.01 dB down
};

struct RingModParams {
  bool on = false;
  float ratio = 1.5f;  // knob A (RATIO), the carrier's frequency over the pitch
  float mix = 0.5f;    // knob B (MIX), 0..1
  bool keyed = false;  // an Autotune card is placed, so the notes are its key's major scale
  RingModTuning tuning;
};

namespace ringmod_detail {

// The carrier stays under this many hertz so its step never reaches half the sample rate.
constexpr float kMaxCarrierHz = 0.45f * kSampleRate;

// The dry-to-card crossfade takes 20 ms, in a straight line.
constexpr float kFadeStep = 1.0f / (20.0f * kSmpPerMs);

// The note-start gate fades the wet in over 20 ms and out over 8 ms.
constexpr float kGateInStep = 1.0f / (20.0f * kSmpPerMs);
constexpr float kGateOutStep = 1.0f / (8.0f * kSmpPerMs);

// One quarter turn in the phase units of sinTurns, the equal-power law's span.
constexpr uint32_t kQuarterTurn = 1u << 30;

}  // namespace ringmod_detail

class RingMod {
 public:
  RingMod() {
    tracker_.readOutOfRange(false);
    reset();
  }

  // Leaves the ring and the tracker clear. A card that rested since it last ran (see rest) has cleared most
  // of the ring already, and this clears what is left.
  void reset() {
    clearRing(kRingModRingLen);
    head_ = 0;
    tracker_.reset();
    phase_ = 0;
    hasNote_ = false;
    noteMidi_ = 0.0f;
    noteHz_ = 0.0f;
    pendingMidi_ = 0.0f;
    pendingSamples_ = 0;
    carrierHz_ = 0.0f;
    unvoicedSamples_ = 0;
    ducked_ = false;
    starting_ = false;
    gate_ = 1.0f;
    wetLevel_ = 0.0f;
    mix_ = 0.0f;
    wet_ = 0.0f;
    fresh_ = true;
  }

  // `key` is the Autotune KEY index whose major scale holds the notes, or a negative number for every
  // semitone. The tracker follows `in` while the card is on or its wet is fading out, and rests otherwise.
  // Off and settled, out equals in bit for bit. in and out may be the same buffer.
  void process(const float* in, float* out, int n, const RingModParams& p, int key) {
    if (n <= 0) return;
    const bool tracking = p.on || wet_ > 0.0f;
    for (int done = 0; done < n;) {
      const int m = n - done < kBlock ? n - done : kBlock;
      pushTracker(in + done, m, tracking);
      done += m;
    }
    trackNote(tracker_.result(), key, p.tuning, n);
    const float wetTarget = p.on ? 1.0f : 0.0f;
    if (wet_ == 0.0f && wetTarget == 0.0f) {
      fresh_ = true;
      gate_ = ducked_ ? 0.0f : 1.0f;
      wetLevel_ = 0.0f;
      if (out != in) std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
      return;
    }

    // The carrier sits on RATIO times the held note for the whole block.
    const float baseHz = hasNote_ ? noteHz_ : p.tuning.restHz;
    carrierHz_ = clampf(p.ratio * baseHz, 1.0f, ringmod_detail::kMaxCarrierHz);
    const uint32_t step = stepOf(carrierHz_);

    // MIX glides, and the wet fade and the note-start gate scale it, so a switch-on starts at the dry input
    // and a ducked wet leaves the dry voice at unity.
    const float mixKnob = smooth::clamp01(p.mix);
    if (fresh_) mix_ = mixKnob;
    const float e0 = mix_ * wet_ * gate_;
    mix_ = smooth::step(mix_, mixKnob, smooth::blockCoef(smooth::kSmoothSec, n));
    wet_ = smooth::toward(wet_, wetTarget, ringmod_detail::kFadeStep * static_cast<float>(n));
    const float gateStep = ducked_ ? ringmod_detail::kGateOutStep : ringmod_detail::kGateInStep;
    gate_ = smooth::toward(gate_, ducked_ ? 0.0f : 1.0f, gateStep * static_cast<float>(n));
    fresh_ = false;
    const float trim = dbToGainFast(clampf(p.tuning.trimDb, -24.0f, 24.0f));
    float dry = dryGain(e0);
    float wet = wetGain(e0) * trim;
    const float inv = 1.0f / static_cast<float>(n);
    const float e1 = mix_ * wet_ * gate_;
    wetLevel_ = wetGain(e1);
    const float dDry = (dryGain(e1) - dry) * inv;
    const float dWet = (wetGain(e1) * trim - wet) * inv;

    uint32_t phase = phase_;
    for (int i = 0; i < n; ++i) {
      phase += step;
      dry += dDry;
      wet += dWet;
      const float x = in[i];
      out[i] = x * (dry + wet * sinTurns(phase));
    }
    phase_ = phase;
  }

  // A call for each block the card sits stopped (the chain does not run it). It zeroes the next
  // kRingModRestFloats floats of the ring, so the reset that starts the card again finds the ring clear
  // and no block carries the whole clear. A card that restarts before the ring is clear pays for the rest
  // in that reset.
  void rest() { clearRing(kRingModRestFloats); }

  // Test hook: the tracker's reading of the card's input.
  const PitchResult& pitch() const { return tracker_.result(); }

  // Test hooks: the ring, and the floats the ring clears have zeroed since construction (it wraps).
  const std::array<float, kRingModRingLen>& ringForTest() const { return ring_; }
  uint32_t ringClearedForTest() const { return ringCleared_; }

  // Test hook: the carrier's frequency in the last block, in Hz.
  float carrierHz() const { return carrierHz_; }

  // Test hook: the held note's frequency in Hz, or 0 before the first voiced frame.
  float heldHz() const { return hasNote_ ? noteHz_ : 0.0f; }

  // Test hooks: the candidate note that is winning (MIDI number) and how long it has won, in samples.
  float pendingMidi() const { return pendingMidi_; }
  int pendingSamples() const { return pendingSamples_; }

  // Test hooks: the wet path's gain at the end of the last block (the equal-power wet gain before the trim,
  // 1 at MIX 100 % with the gate open), and whether the wet is ducked for a note start.
  float wetLevel() const { return wetLevel_; }
  bool ducked() const { return ducked_; }

  // Runs once a block. Moves the held note to the one nearest a voiced pitch once the pitch has sat more than `hold` semitones
  // closer to it than to the held note for `settleMs` in a row. `n` is the block's length in samples. The
  // first voiced note, and a held note that is not in the scale (the key changed), take effect at once.
  // Unvoiced frames pause the count and leave the note alone. After `gapMs` of unvoiced frames the wet ducks
  // and the next voiced stretch is a note start (see the header comment): the held note stays until the pitch
  // lands, and then it moves in one step.
  void trackNote(const PitchResult& pr, int key, const RingModTuning& t, int n) {
    if (!pr.voiced || pr.period <= 0.0f) {
      unvoicedSamples_ += n;
      if (static_cast<float>(unvoicedSamples_) >= t.gapMs * kSmpPerMs) {
        ducked_ = true;
        starting_ = false;
      }
      return;
    }
    unvoicedSamples_ = 0;
    const float midi = midiOf(kSampleRate / PitchTracker::clampPeriod(pr.period));
    const bool keyed = key >= 0;
    const int k = keyed ? key % kKeyCount : 0;
    const float nearest = keyed ? roundf(midi - scaleDeviation(midi, k)) : roundf(midi);
    const bool inScale = !keyed || fabsf(scaleDeviation(noteMidi_, k)) < 1e-3f;
    const bool atHeld = hasNote_ && inScale && fabsf(midi - noteMidi_) - fabsf(midi - nearest) <= t.holdSemis;
    if (ducked_ && !starting_) {
      starting_ = true;
      bandLo_ = bandHi_ = midi;
      stillSamples_ = 0;
      startSamples_ = 0;
    }
    if (starting_) {
      bandLo_ = fminf(bandLo_, midi);
      bandHi_ = fmaxf(bandHi_, midi);
      if (bandHi_ - bandLo_ > t.landSemis) {
        bandLo_ = bandHi_ = midi;
        stillSamples_ = 0;
      }
      stillSamples_ += n;
      startSamples_ += n;

      // A pitch that sits on the held note lands sooner, since the carrier does not move.
      const float needMs = atHeld ? t.landAgreeMs : t.landMs;
      if (static_cast<float>(stillSamples_) < needMs * kSmpPerMs && static_cast<float>(startSamples_) < t.landMaxMs * kSmpPerMs) {
        return;
      }
      starting_ = false;
      ducked_ = false;
      pendingSamples_ = 0;
      if (atHeld) return;
    } else if (hasNote_ && inScale) {
      if (fabsf(midi - noteMidi_) - fabsf(midi - nearest) <= t.holdSemis) {
        pendingSamples_ = 0;
        return;
      }
      if (pendingSamples_ == 0 || nearest != pendingMidi_) {
        pendingMidi_ = nearest;
        pendingSamples_ = 0;
      }
      pendingSamples_ += n;
      if (static_cast<float>(pendingSamples_) < t.settleMs * kSmpPerMs) return;
    }
    hasNote_ = true;
    noteMidi_ = nearest;
    noteHz_ = 440.0f * exp2f((nearest - 69.0f) / 12.0f);
    pendingSamples_ = 0;
  }

 private:
  // Writes m samples (at most kBlock) into the ring, then feeds them to the tracker, which reads its fine
  // frame and its replay from the ring. A card that is not tracking leaves the tracker idle; its next
  // push replays what it missed. The tracker runs the coarse search only, which a semitone grid with a
  // 0.3 semitone hold tolerates.
  void pushTracker(const float* in, int m, bool tracking) {
    ringZero_ = 0;
    const int first = head_;
    for (int i = 0; i < m; ++i) {
      ring_[static_cast<size_t>(head_)] = in[i];
      if (++head_ == kRingModRingLen) head_ = 0;
    }
    if (tracking) {
      tracker_.push(in, m, true, false, PitchTracker::History{ring_.data(), kRingModRingLen, first});
    } else {
      tracker_.idle(m);
    }
  }

  // Zeroes up to `most` more floats of the ring, in index order from ringZero_.
  void clearRing(int most) {
    const int n = most < kRingModRingLen - ringZero_ ? most : kRingModRingLen - ringZero_;
    zeroFloats(&ring_[static_cast<size_t>(ringZero_)], static_cast<size_t>(n));
    ringZero_ += n;
    ringCleared_ += static_cast<uint32_t>(n);
  }

  static uint32_t stepOf(float hz) { return static_cast<uint32_t>(hz * (4294967296.0f / kSampleRate)); }

  static float dryGain(float e) { return sinTurns(ringmod_detail::kQuarterTurn - phaseOf(e)); }
  static float wetGain(float e) { return sinTurns(phaseOf(e)); }
  static uint32_t phaseOf(float e) {
    return static_cast<uint32_t>(smooth::clamp01(e) * static_cast<float>(ringmod_detail::kQuarterTurn));
  }

  std::array<float, kRingModRingLen> ring_{};  // the card's input, newest at head_ - 1
  PitchTracker tracker_;
  int head_ = 0;            // the ring index the next sample takes
  int ringZero_ = 0;        // the clear has zeroed the ring below this index since the last write
  uint32_t ringCleared_ = 0;
  uint32_t phase_ = 0;
  bool hasNote_ = false;    // a voiced frame has set the note
  float noteMidi_ = 0.0f;   // the held note, a whole MIDI number
  float noteHz_ = 0.0f;
  float pendingMidi_ = 0.0f;  // the candidate note that is winning
  int pendingSamples_ = 0;    // how long it has won in a row, in samples; 0 when no candidate is winning
  float carrierHz_ = 0.0f;  // RATIO times the note, in the last block
  int unvoicedSamples_ = 0;   // how long the tracker has read unvoiced in a row, in samples
  bool ducked_ = false;       // the wet is down: a gap has passed and the new note has not landed
  bool starting_ = false;     // a note start is waiting for the pitch to land
  float bandLo_ = 0.0f;       // the lowest and highest pitch (MIDI) since the pitch last left its band
  float bandHi_ = 0.0f;
  int stillSamples_ = 0;      // how long the pitch has stayed in that band, in samples
  int startSamples_ = 0;      // how long the note start has waited, in samples
  float gate_ = 1.0f;         // the note-start fade, 0 ducked .. 1 open
  float wetLevel_ = 0.0f;
  float mix_ = 0.0f;
  float wet_ = 0.0f;        // the dry-to-card fade, 0 dry .. 1 card
  bool fresh_ = true;       // the next call takes the knobs outright
};

}  // namespace cv
