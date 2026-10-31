#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/psola.h"

// The glottal pulses of the voice, found once and shared by the Gang card's four copies. Every copy
// cuts its grains from the same ring, so the pulses are a property of the voice and not of a copy's
// lag. The search runs on PitchFx's low-passed shadow ring, where each pulse is one broad peak.
//
// A pulse is expected one period after the last one. The track takes the tallest sample of the
// shadow within +-search periods of that spot, places the pulse at the centre of the lobe around it,
// and expects the next one a period later. A copy asks for the pulse nearest its own grid mark and
// cuts its grain there.

namespace cv {
namespace gang_detail {

class EpochTrack {
 public:
  // Marks kept. A 200 ms lag at the tracker's highest pitch (a 48 sample period) spans 200 of them.
  static constexpr int kMax = 256;

  // The widest search the track takes, in periods either side of the expected spot.
  static constexpr float kMaxSearch = 0.3f;

  void reset() {
    count_ = 0;
    newest_ = 0;
    clock_ = 0;
  }

  // The clock counts samples since reset. It is the time base of the marks and of snap().
  uint32_t clock() const { return clock_; }

  // Per process call. head is the ring index the next sample writes to, so the call's newest sample
  // sits at head - 1. period 0 advances the clock and finds nothing.
  void advance(const VoiceRing& shadow, int head, int n, float period, float search) {
    clock_ += static_cast<uint32_t>(n);
    if (!(period > 0.0f)) return;
    const float half = clampf(search, 0.0f, kMaxSearch) * period;
    float lastAge = count_ > 0 ? ageOf(newest_) : kNone;
    if (lastAge > kReseedPeriods * period) {
      count_ = 0;
      lastAge = push(shadow, head, 1.5f * period + 2.0f, 0.5f * period, true);
    }
    for (int k = 0; k < kMaxPerCall; ++k) {
      const float expect = lastAge - period;
      if (static_cast<int>(ceilf(expect - half)) < kMinAge) break;
      lastAge = push(shadow, head, expect, half);
    }
  }

  // The delay in samples of the mark nearest `centre`, measured back from the sample whose clock is
  // nowAbs. A mark more than half a period from centre is no help, so centre comes back unchanged.
  float snap(uint32_t nowAbs, float centre, float period) const {
    float best = centre;
    float bestErr = 0.5f * period + 1.0f;
    size_t i = newest_;
    for (int k = 0; k < count_; ++k) {
      const float d = static_cast<float>(static_cast<int32_t>(nowAbs - t_[i])) + off_[i];
      if (d >= 0.0f) {
        const float err = d > centre ? d - centre : centre - d;
        if (err < bestErr) {
          bestErr = err;
          best = d;
        } else if (d > centre) {
          break;
        }
      }
      i = i == 0 ? static_cast<size_t>(kMax - 1) : i - 1;
    }
    return best;
  }

 private:
  // A mark's age is its delay behind the newest sample plus one, so the newest sample has age 1. A
  // search stays off the newest sample, which has no neighbour yet to show it is a peak.
  static constexpr int kMinAge = 2;
  static constexpr float kNone = 1e9f;

  // A track idle this long (in periods) starts over from a fresh search of a whole period.
  static constexpr float kReseedPeriods = 8.0f;

  // A call finds at most this many marks. A call of 1024 samples at the highest pitch needs 22.
  static constexpr int kMaxPerCall = 64;

  // A lobe's centroid counts the samples above this fraction of the peak, out to this fraction of
  // the search half-width (plus two samples) either side.
  static constexpr float kLobeCut = 0.5f;
  static constexpr float kReach = 0.5f;

  float ageOf(size_t i) const { return static_cast<float>(static_cast<int32_t>(clock_ - t_[i])) + off_[i]; }

  // Finds the shadow peak within +-half of the age `expect`, records it as a mark and returns its
  // age. A seed takes the largest absolute sample and keeps its sign. Every later search takes the
  // largest sample of that sign, so a ringing voice, whose positive and negative lobes are nearly
  // as tall, does not flip between them.
  float push(const VoiceRing& shadow, int head, float expect, float half, bool seed = false) {
    int lo = static_cast<int>(ceilf(expect - half));
    lo = lo < kMinAge ? kMinAge : lo;
    const int hi = static_cast<int>(floorf(expect + half));
    int idx = wrapOnce<kVoiceRingLen>(head - lo);
    int bestAge = lo;
    int bestIdx = idx;
    float bestScore = -1e30f;
    for (int age = lo; age <= hi || age == lo; ++age) {
      const float x = shadow[static_cast<size_t>(idx)];
      const float score = seed ? fabsf(x) : sign_ * x;
      if (score > bestScore) {
        bestScore = score;
        bestAge = age;
        bestIdx = idx;
      }
      idx = idx == 0 ? kVoiceRingLen - 1 : idx - 1;
    }
    if (seed) sign_ = shadow[static_cast<size_t>(bestIdx)] < 0.0f ? -1.0f : 1.0f;
    return record(bestAge, lobeCentre(shadow, bestIdx, bestAge, static_cast<int>(half * kReach) + 2));
  }

  // The centre of the lobe around the peak at bestIdx, whose age is bestAge, in samples toward older
  // than the peak. It is the centroid of the part of the lobe above half its height, out to `reach`
  // samples either side and never past the newest sample. A broad or noisy lobe moves its tallest
  // sample from period to period, and its centroid far less.
  float lobeCentre(const VoiceRing& shadow, int bestIdx, int bestAge, int reach) const {
    const float peak = sign_ * shadow[static_cast<size_t>(bestIdx)];
    if (!(peak > 0.0f)) return 0.0f;
    const float cut = kLobeCut * peak;
    float weight = peak - cut;
    float moment = 0.0f;
    walkLobe(shadow, bestIdx, 1, reach < bestAge - 1 ? reach : bestAge - 1, cut, weight, moment);
    walkLobe(shadow, bestIdx, -1, reach, cut, weight, moment);
    return moment / weight;
  }

  // Adds the samples above `cut` on one side of the peak, stepping dir (+1 newer, -1 older) through
  // the ring until one falls below it.
  void walkLobe(const VoiceRing& shadow, int from, int dir, int reach, float cut, float& weight, float& moment) const {
    int idx = from;
    for (int k = 1; k <= reach; ++k) {
      idx = idx + dir;
      idx = idx < 0 ? kVoiceRingLen - 1 : (idx >= kVoiceRingLen ? 0 : idx);
      const float w = sign_ * shadow[static_cast<size_t>(idx)] - cut;
      if (w <= 0.0f) return;
      weight += w;
      moment -= w * static_cast<float>(dir * k);
    }
  }

  float record(int bestAge, float off) {
    newest_ = count_ == 0 ? 0 : (newest_ + 1 == static_cast<size_t>(kMax) ? 0 : newest_ + 1);
    t_[newest_] = clock_ - static_cast<uint32_t>(bestAge);
    off_[newest_] = off;
    if (count_ < kMax) ++count_;
    return static_cast<float>(bestAge) + off;
  }

  std::array<uint32_t, kMax> t_{};  // clock of each mark's sample
  std::array<float, kMax> off_{};   // the mark's offset from that sample in samples, + toward older
  size_t newest_ = 0;
  int count_ = 0;
  uint32_t clock_ = 0;
  float sign_ = 1.0f;  // the sign of the lobe the track follows
};

}  // namespace gang_detail
}  // namespace cv
