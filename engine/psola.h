#pragma once

#include <array>
#include <cmath>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/pitch.h"

// One pitch-shifted voice by pitch-synchronous overlap-add (Lent 1989 form).
// Grains two periods long are cut on a grid spaced one input period apart
// and laid down one output period apart, so the formants stay put.

namespace cv {

// The shared input ring holds the Crowd card's longest lag, the timing scatter its working set rides
// on top of that lag, and the reach of its longest grain (two periods at the tracker's longest, and
// a pad) behind both. The longest lag is a raised copy's, 75 ms behind a tuned copy at the tuned
// copies' 150 ms ceiling. 12,756 samples, 265.75 ms. Autotune reads under 2,100 samples back and
// Octave under 3,850.
constexpr int kCrowdMaxLagMs = 225;
constexpr int kCrowdScatterMs = 12;
constexpr int kCrowdGrainReach = 2 * PitchTracker::kMaxPeriod + 8;
constexpr int kVoiceRingLen = (kCrowdMaxLagMs + kCrowdScatterMs) * (kSampleRate / 1000) + kCrowdGrainReach;
using VoiceRing = std::array<float, kVoiceRingLen>;

// The pitch ratio a PSOLA voice clamps to, and the delays a grain reads at. A grain never reads
// closer to the head than the near bound or further back than the ring holds. The periods a voice
// plays are the tracker's, PitchTracker::kMinPeriod to kMaxPeriod.
constexpr float kMinPitchRatio = 0.5f;
constexpr float kMaxPitchRatio = 2.0f;
constexpr float kMinReadDelay = 3.0f;
constexpr float kMaxReadDelay = kVoiceRingLen - 4.0f;

// Where a voice's marks and launch countdown stand. A voice that starts beside another takes its
// grid and launches on the same samples. Both PSOLA voices keep the newest mark in
// [grainDelay, grainDelay + period) behind the head.
struct VoiceGrid {
  float markDelay;
  float countdown;
};

class PsolaVoice {
 public:
  // The default grain delay lines the voice up with the tracker's frame. A shorter delay lets the
  // measurement trail the audio and loses Adam's 2-cent hard tune (commit ee527fb). The voice's
  // latency is this delay plus one period, 22.7 ms at 1 kHz to 36.0 ms at 70 Hz.
  static constexpr int kGrainDelay = 1040;

  // The pool of 6 holds the clamp's worst case, ceil(2r) + 1 = 5 live grains at ratio 2, and one
  // spare; Autotune pulls under 1.2 st, so it keeps at most 4 live.
  static constexpr int kMaxGrains = 6;

  void reset() {
    grains_.fill(Grain{});
    markDelay_ = grainDelay_;
    countdown_ = 0.0f;
  }

  // The grain delay in samples, up to kGrainDelay. Each grain reads the input at this fixed delay.
  void setGrainDelay(float samples) { grainDelay_ = clampf(samples, kMinReadDelay + 1.0f, kGrainDelay); }

  // The gain of the grains that launch from now on, 1 for none. It rides on their tap weights, so
  // a trim costs nothing per sample.
  void setTrim(float gain) { trim_ = gain; }

  // The grain delay in force, after the clamp.
  float grainDelay() const { return grainDelay_; }

  VoiceGrid grid() const { return {markDelay_, countdown_}; }
  void adoptGrid(const VoiceGrid& g) {
    markDelay_ = g.markDelay;
    countdown_ = g.countdown;
  }

  // How far the voice lags the input at this period, in samples: the grain delay plus the period
  // the voice plays at. The Formant card reads it to put its voice on the same sample.
  float latency(float period) const { return grainDelay_ + PitchTracker::clampPeriod(period); }

  // n samples of the voice into out. first is the ring index of the first sample, which the ring
  // already holds, and the voice reads nothing newer. period is the sung period in samples (last
  // voiced; 0 silences the call and holds the voice where it is), inv[i] the inverse of the pitch
  // ratio at sample i, between 0.5 and 2.0 as the ratio is. The marks and the launch countdown step
  // sample by sample; the grains between two launches then play their stretch from locals, two at a
  // time so their window chains overlap.
  void render(const VoiceRing& ring, int first, int n, float period, const float* inv, float* out) {
    for (int i = 0; i < n; ++i) out[i] = 0.0f;
    if (period <= 0.0f) return;
    const float p = PitchTracker::clampPeriod(period);
    float markDelay = markDelay_;
    float countdown = countdown_;
    int from = 0;
    for (int i = 0; i < n; ++i) {
      markDelay += 1.0f;
      while (markDelay - p >= grainDelay_) markDelay -= p;
      const float outPeriod = p * inv[i];
      if (countdown > outPeriod) countdown = outPeriod;
      countdown -= 1.0f;
      if (countdown > 0.0f) continue;
      markDelay_ = markDelay;
      playGrains(ring, out + from, i - from);
      from = i;
      launch(wrap(first + i), p, -countdown);
      countdown += outPeriod;
    }
    markDelay_ = markDelay;
    countdown_ = countdown;
    playGrains(ring, out + from, n - from);
    for (int i = 0; i < n; ++i) out[i] *= inv[i];
  }

 private:
  struct Grain {
    int idx = 0;            // ring index of the next read, advances one per sample
    CatmullRomWeights w{};  // the taps for the fixed fractional offset, trim included
    float age = 0.0f;       // samples since the window's start
    float len = 0.0f;       // the window's length
    float c = 1.0f;         // the Hann window's cosine
    float s = 0.0f;         // the last step of c
    float eps = 0.0f;       // 2 - 2 cos(step): c steps by s -= eps * c, c += s
    bool active = false;
  };

  static constexpr float kMaxPeriod = static_cast<float>(PitchTracker::kMaxPeriod);

  // The delay that lines the voice up with the tracker's frame (see kGrainDelay) stays over 1.5
  // longest periods and a pad.
  static_assert(kGrainDelay >= 1.5f * kMaxPeriod + 4.0f,
                "the grain delay that lines up with the tracker's frame fell under 1.5 longest periods and a pad");

  // lead is how far past the ideal launch instant this tick already is. Marks are tracked as delays
  // behind the write head, so they stay bounded floats on a box that runs for hours. markDelay_ is
  // the newest mark at or before grainDelay_.
  void launch(int head, float p, float lead) {
    float centre = markDelay_;
    if (centre - grainDelay_ > 0.5f * p) centre -= p;
    Grain* slot = &grains_[0];
    for (Grain& g : grains_) {
      if (!g.active) {
        slot = &g;
        break;
      }
      if (g.age > slot->age) slot = &g;
    }
    // The delay stays constant over the grain, so the fraction is fixed.
    const float delay = clampf(centre + p - lead, kMinReadDelay, kMaxReadDelay);
    const float pos = static_cast<float>(head) - delay;
    const float fl = floorf(pos);
    const CatmullRomWeights w = catmullRomWeights(pos - fl);
    slot->idx = wrap(static_cast<int>(fl));
    slot->w = {trim_ * w.m, trim_ * w.z, trim_ * w.p, trim_ * w.q};
    slot->age = lead;
    slot->len = 2.0f * p;
    // The window is 0.5 * (1 - c) with c = cos(2 pi age / len). The difference form of the cosine
    // recurrence holds float rounding where the plain two-term form with 2 cos(step) does not.
    float s0, c0, sd, cd;
    sinCosFast(kTwoPi * lead / slot->len, s0, c0);
    sinCosFast(kTwoPi / slot->len, sd, cd);
    slot->eps = 2.0f * sd * sd / (1.0f + cd);
    slot->c = c0;
    slot->s = 0.5f * slot->eps * c0 - s0 * sd;
    slot->active = true;
  }

  // The live grains over count samples, added into acc.
  void playGrains(const VoiceRing& ring, float* acc, int count) {
    if (count <= 0) return;
    Grain* live[kMaxGrains];
    int nLive = 0;
    for (Grain& g : grains_) {
      if (g.active) live[nLive++] = &g;
    }
    int k = 0;
    for (; k + 1 < nLive; k += 2) playPair(ring, *live[k], *live[k + 1], count, acc);
    if (k < nLive) playSolo(ring, *live[k], soundsIn(*live[k], count), acc);
    for (int j = 0; j < nLive; ++j) retire(*live[j], count);
  }

  // How many of count samples a grain sounds: until its length runs out.
  static int soundsIn(const Grain& g, int count) {
    const float left = g.len - g.age;
    const int sounds = left > 0.0f ? static_cast<int>(ceilf(left)) : 0;
    return sounds < count ? sounds : count;
  }

  // A grain that had run out before the stretch ended goes inactive; the rest age by the stretch.
  static void retire(Grain& g, int count) {
    const float left = g.len - g.age;
    const int sounds = left > 0.0f ? static_cast<int>(ceilf(left)) : 0;
    g.age += static_cast<float>(sounds < count ? sounds : count);
    if (count > sounds) g.active = false;
  }

  // Samples from read index idx before a four-tap read leaves the ring's contiguous part.
  static int plainRun(int idx) {
    return static_cast<unsigned>(idx - 1) < static_cast<unsigned>(kVoiceRingLen - 3) ? kVoiceRingLen - 2 - idx : 0;
  }

  // One sample of a grain by the wrapped path, for the reads that touch the ring's seam.
  static float tapWrapped(const VoiceRing& ring, Grain& g) {
    const float win = 0.5f * (1.0f - g.c);
    g.s -= g.eps * g.c;
    g.c += g.s;
    const int i = g.idx;
    g.idx = i + 1 == kVoiceRingLen ? 0 : i + 1;
    return win * (g.w.m * ring[wrap(i - 1)] + g.w.z * ring[static_cast<size_t>(i)] + g.w.p * ring[wrap(i + 1)] +
                  g.w.q * ring[wrap(i + 2)]);
  }

  // The grain's next `sounds` samples, added into acc.
  static void playSolo(const VoiceRing& ring, Grain& g, int sounds, float* acc) {
    for (int j = 0; j < sounds;) {
      const int room = plainRun(g.idx);
      if (room == 0) {
        acc[j++] += tapWrapped(ring, g);
        continue;
      }
      const int run = sounds - j < room ? sounds - j : room;
      const float* r = ring.data() + g.idx;
      float c = g.c, s = g.s;
      for (int k = 0; k < run; ++k) {
        const float win = 0.5f * (1.0f - c);
        s -= g.eps * c;
        c += s;
        acc[j + k] += win * (g.w.m * r[k - 1] + g.w.z * r[k] + g.w.p * r[k + 1] + g.w.q * r[k + 2]);
      }
      g.c = c;
      g.s = s;
      g.idx += run;
      j += run;
    }
  }

  // Two grains over count samples, their window chains side by side; the longer one finishes alone.
  static void playPair(const VoiceRing& ring, Grain& a, Grain& b, int count, float* acc) {
    const int soundsA = soundsIn(a, count), soundsB = soundsIn(b, count);
    const int both = soundsA < soundsB ? soundsA : soundsB;
    int j = 0;
    while (j < both) {
      const int roomA = plainRun(a.idx), roomB = plainRun(b.idx);
      if (roomA == 0 || roomB == 0) {
        acc[j] += tapWrapped(ring, a) + tapWrapped(ring, b);
        ++j;
        continue;
      }
      int run = both - j;
      run = run < roomA ? run : roomA;
      run = run < roomB ? run : roomB;
      pairRun(ring.data() + a.idx, ring.data() + b.idx, a, b, run, acc + j);
      a.idx += run;
      b.idx += run;
      j += run;
    }
    playSolo(ring, a, soundsA - both, acc + both);
    playSolo(ring, b, soundsB - both, acc + both);
  }

  // run samples of two grains whose reads all sit inside the ring.
  static void pairRun(const float* ra, const float* rb, Grain& a, Grain& b, int run, float* acc) {
    float ca = a.c, sa = a.s, cb = b.c, sb = b.s;
    for (int k = 0; k < run; ++k) {
      const float winA = 0.5f * (1.0f - ca);
      const float winB = 0.5f * (1.0f - cb);
      sa -= a.eps * ca;
      sb -= b.eps * cb;
      ca += sa;
      cb += sb;
      const float ya = a.w.m * ra[k - 1] + a.w.z * ra[k] + a.w.p * ra[k + 1] + a.w.q * ra[k + 2];
      const float yb = b.w.m * rb[k - 1] + b.w.z * rb[k] + b.w.p * rb[k + 1] + b.w.q * rb[k + 2];
      acc[k] += winA * ya + winB * yb;
    }
    a.c = ca;
    a.s = sa;
    b.c = cb;
    b.s = sb;
  }

  static int wrap(int i) { return wrapOnce<kVoiceRingLen>(i); }

  std::array<Grain, kMaxGrains> grains_{};
  float grainDelay_ = static_cast<float>(kGrainDelay);
  float markDelay_ = static_cast<float>(kGrainDelay);
  float countdown_ = 0.0f;
  float trim_ = 1.0f;
};

}  // namespace cv
