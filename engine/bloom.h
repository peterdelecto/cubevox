#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Bloom: while a word lasts, short slices of the voice loop under it, a fresh slice every 55 to 180
// ms. When the word ends, the last good slice keeps looping and fades over TAIL, gritty in the
// middle and clean at the end (spec 2026-11-01-bloom-design).
//
// A word is a key. The Input gate hands its detector state in through BloomParams, or the engine
// listens to its own input. Two grain buffers alternate, one looping while the other takes the next
// capture. The dry path is untouched except for one level trim; the wet path runs
// low-pass, drive, soft clip, high shelf, then the wet gain.

namespace cv {

struct BloomTuning {
  // Level. The whole output scales by trimDb times the wet level over wetHi, so BLOOM at the bottom
  // of its travel is exactly dry. The default lands 0 to +0.5 dB RMS on the three sample clips.
  float trimDb = -2.33f;

  // Word timing.
  float firstCaptureMs = 40.0f;  // first slice, after the word starts
  float crossingWaitMs = 10.0f;  // longest wait for a zero crossing before a capture starts anyway
  float rampMs = 5.0f;           // wet gain rise to its level
  float fadeMs = 5.8f;           // pass fade in and out, and the splice crossfade
  float jitter = 0.05f;          // re-capture interval, plus or minus
  float skipDb = -12.0f;         // a slice this far under the looping grain is skipped

  // BLOOM laws. Below bottomZone the wet level ramps up from 0; above it, every law is linear in
  // x = (BLOOM - bottomZone) / (1 - bottomZone).
  float bottomZone = 0.05f;
  float wetLo = 0.25f;
  float wetHi = 0.70f;
  float grainLoMs = 80.0f;  // at x = 0
  float grainHiMs = 45.0f;  // at x = 1
  float intervalLoMs = 180.0f;
  float intervalHiMs = 55.0f;
  float grainMinMs = 35.0f;
  float grainTailFrac = 0.8f;  // the grain stays under this fraction of TAIL

  // TAIL laws.
  float tauScale = 1.5f;  // life time constant is max(TAIL, clamp(tauScale * TAIL, tauMinMs, tauMaxMs))
  float tauMinMs = 120.0f;
  float tauMaxMs = 300.0f;
  float staticStart = 0.20f;  // life where the faster fade takes over
  float staticFadeMs = 80.0f;
  float closeGain = 0.001f;  // under this the wet gain writes 0 and the loop stops

  // Wet tone.
  float driveWord = 1.4f;
  float driveBell = 1.6f;  // extra drive at the middle of the tail
  float lowpassHz = 11000.0f;
  float lowpassQ = 0.70f;
  float shelfHz = 3000.0f;
  float shelfDb = 2.2f;
  float shelfSlope = 0.7f;

  // Word key from the engine's own input, and the hold both key sources share.
  float ownOpenDb = -45.0f;
  float ownHysteresisDb = 3.0f;
  float keyHoldMs = 50.0f;

  // True plays the loop only after the word ends; false plays it under the word.
  bool tailOnly = false;
};

struct BloomParams {
  bool on = true;
  float tailMs = 300.0f;  // panel TAIL, 60..1000
  float bloom = 0.5f;     // panel BLOOM, 0..1
  bool keyOpen = false;   // the Input gate's detector state for this call
  bool keyFromGate = false;  // false makes the engine listen to its own input
  BloomTuning tuning;
};

class Bloom {
 public:
  static constexpr int kGrainMax = 80 * kSampleRate / 1000;  // 3,840 samples
  static constexpr float kTailMinMs = 60.0f;
  static constexpr float kTailMaxMs = 1000.0f;

  void reset() {
    for (std::array<float, kGrainMax>& g : grain_) g.fill(0.0f);
    lp_.clear();
    shelf_.clear();
    wl_ = 0.0f;
    prevIn_ = 0.0f;
    ownOpen_ = false;
    keyHold_ = 0;
    lcg_ = kSeed;
    clock_ = 0;
    armedAt_ = 0;
    writtenAt_ = 0;
    taken_ = 0;
    skipped_ = 0;
    trimWl_ = -1.0f;
    prepared_ = false;
    closeEngine();
  }

  void process(const float* in, float* out, int n, const BloomParams& p) {
    prepare(p);
    for (int done = 0; done < n; done += kBlock) {
      const int m = n - done < kBlock ? n - done : kBlock;
      runChunk(in + done, out + done, m, p);
    }
  }

  // Wet gain before the trim: wet level times life.
  float wetGain() const { return gain_; }

  // True while a word is open (the key plus its hold).
  bool inWord() const { return phase_ == Phase::Word; }

  // True while a grain loops.
  bool looping() const { return curLen_ > 0; }

  // Samples seen since reset(), and the clock at the latest capture arm and write start.
  uint32_t clock() const { return clock_; }
  uint32_t armedAt() const { return armedAt_; }
  uint32_t writeStartedAt() const { return writtenAt_; }

  // Grains spliced in, and grains skipped for sitting too far under the looping one, since reset().
  int taken() const { return taken_; }
  int skipped() const { return skipped_; }

 private:
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr uint32_t kSeed = 0x2545F491u;

  enum class Phase : uint8_t { Idle, Word, Tail };
  enum class Cap : uint8_t { Idle, Waiting, Writing };

  // Everything BLOOM and TAIL set for one call.
  struct Law {
    float wet;
    int grainSmp;
    float intervalSmp;
  };

  static float lerp(float a, float b, float t) { return a + (b - a) * t; }
  static float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

  static Law lawFor(const BloomParams& p, int minGrainSmp) {
    const BloomTuning& t = p.tuning;
    const float k = smooth::clamp01(p.bloom);
    const float bottom = clampf(t.bottomZone, 0.0f, 0.5f);
    float x = 0.0f;
    float ramp = 1.0f;
    if (k < bottom) ramp = k / bottom;
    else x = (k - bottom) / (1.0f - bottom);
    const float tailMs = clampf(p.tailMs, kTailMinMs, kTailMaxMs);
    float grainMs = lerp(t.grainLoMs, t.grainHiMs, x);
    grainMs = grainMs < t.grainTailFrac * tailMs ? grainMs : t.grainTailFrac * tailMs;
    grainMs = grainMs > t.grainMinMs ? grainMs : t.grainMinMs;
    int grain = static_cast<int>(grainMs * kSmpPerMs);
    grain = grain < minGrainSmp ? minGrainSmp : (grain > kGrainMax ? kGrainMax : grain);
    return {ramp * lerp(t.wetLo, t.wetHi, x), grain, lerp(t.intervalLoMs, t.intervalHiMs, x) * kSmpPerMs};
  }

  // Coefficients that follow the tuning. The cheap ones run every call; the ones that need libm run
  // when their inputs change.
  void prepare(const BloomParams& p) {
    const BloomTuning& t = p.tuning;
    fadeSmp_ = static_cast<int>(clampf(t.fadeMs, 0.5f, 10.0f) * kSmpPerMs + 0.5f);
    invFade_ = 1.0f / static_cast<float>(fadeSmp_);
    firstSmp_ = static_cast<int>(clampf(t.firstCaptureMs, 1.0f, 500.0f) * kSmpPerMs);
    waitSmp_ = static_cast<int>(clampf(t.crossingWaitMs, 0.0f, 50.0f) * kSmpPerMs);
    holdSmp_ = static_cast<int>(clampf(t.keyHoldMs, 0.0f, 1000.0f) * kSmpPerMs);
    invRampSmp_ = 1.0f / (clampf(t.rampMs, 0.1f, 100.0f) * kSmpPerMs);
    jitter_ = clampf(t.jitter, 0.0f, 0.5f);
    openLin_ = dbToGainFast(t.ownOpenDb);
    closeLin_ = dbToGainFast(t.ownOpenDb - (t.ownHysteresisDb > 0.0f ? t.ownHysteresisDb : 0.0f));
    skipLin_ = dbToGainFast(t.skipDb);
    minGrainSmp_ = 2 * fadeSmp_ + 2;
    prepareFilters(t);
    prepareTail(p);
    prepared_ = true;
  }

  void prepareFilters(const BloomTuning& t) {
    if (!prepared_ || t.lowpassHz != lpHz_ || t.lowpassQ != lpQ_) {
      lp_.setLowPass(t.lowpassHz, t.lowpassQ);
      lpHz_ = t.lowpassHz;
      lpQ_ = t.lowpassQ;
    }
    if (!prepared_ || t.shelfHz != shHz_ || t.shelfDb != shDb_ || t.shelfSlope != shSlope_) {
      shelf_.setCoefsFrom(highShelf(t.shelfHz, t.shelfDb, t.shelfSlope));
      shHz_ = t.shelfHz;
      shDb_ = t.shelfDb;
      shSlope_ = t.shelfSlope;
    }
  }

  // The tail's decay steps, per sample.
  void prepareTail(const BloomParams& p) {
    const BloomTuning& t = p.tuning;
    const float tail = clampf(p.tailMs, kTailMinMs, kTailMaxMs);
    if (!prepared_ || tail != tailMs_ || t.tauScale != tauScale_ || t.tauMinMs != tauMin_ ||
        t.tauMaxMs != tauMax_ || t.staticFadeMs != staticMs_) {
      const float scaled = clampf(t.tauScale * tail, t.tauMinMs, t.tauMaxMs);
      const float tauMs = tail > scaled ? tail : scaled;
      aTail_ = expf(-1.0f / (tauMs * kSmpPerMs));
      aStatic_ = expf(-1.0f / (clampf(t.staticFadeMs, 1.0f, 2000.0f) * kSmpPerMs));
      tailMs_ = tail;
      tauScale_ = t.tauScale;
      tauMin_ = t.tauMinMs;
      tauMax_ = t.tauMaxMs;
      staticMs_ = t.staticFadeMs;
    }
  }

  // RBJ high shelf with a slope other than the shared Biquad's fixed 1.
  static Biquad highShelf(float hz, float db, float slope) {
    const float w0 = kTwoPi * clampf(hz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
    const float amp = powf(10.0f, db / 40.0f);
    const float cw = cosf(w0);
    const float s = clampf(slope, 0.1f, 1.0f);
    const float alpha = 0.5f * sinf(w0) * sqrtf((amp + 1.0f / amp) * (1.0f / s - 1.0f) + 2.0f);
    const float beta = 2.0f * sqrtf(amp) * alpha;
    const float ap = amp + 1.0f, am = amp - 1.0f;
    const float a0 = ap - am * cw + beta;
    Biquad b;
    b.b0 = amp * (ap + am * cw + beta) / a0;
    b.b1 = -2.0f * amp * (am + ap * cw) / a0;
    b.b2 = amp * (ap + am * cw - beta) / a0;
    b.a1 = 2.0f * (am - ap * cw) / a0;
    b.a2 = (ap - am * cw - beta) / a0;
    return b;
  }

  void runChunk(const float* in, float* out, int m, const BloomParams& p) {
    const BloomTuning& t = p.tuning;
    const Law law = lawFor(p, minGrainSmp_);
    const float wetTarget = p.on ? law.wet : 0.0f;
    if (wetTarget == 0.0f && wl_ == 0.0f) {
      for (int i = 0; i < m; ++i) out[i] = in[i];
      if (running_) closeEngine();
      prevIn_ = in[m - 1];
      clock_ += static_cast<uint32_t>(m);
      return;
    }
    running_ = true;
    stepKey(in, m, p);
    if (keyHeld_ && phase_ != Phase::Word) startWord();
    else if (!keyHeld_ && phase_ == Phase::Word) endWord();
    for (int i = 0; i < m; ++i) out[i] = sample(in[i], law, wetTarget, t);
  }

  // The key for this chunk: the gate's state or the engine's own detector, then the hold.
  void stepKey(const float* in, int m, const BloomParams& p) {
    bool key = p.keyOpen;
    if (!p.keyFromGate) {
      float peak = 0.0f;
      for (int i = 0; i < m; ++i) {
        const float a = fabsf(in[i]);
        peak = a > peak ? a : peak;
      }
      if (peak >= openLin_) ownOpen_ = true;
      else if (peak < closeLin_) ownOpen_ = false;
      key = ownOpen_;
    }
    if (key) {
      keyHold_ = holdSmp_;
      keyHeld_ = true;
    } else {
      keyHold_ = keyHold_ > m ? keyHold_ - m : 0;
      keyHeld_ = keyHold_ > 0;
    }
  }

  void startWord() {
    phase_ = Phase::Word;
    life_ = 1.0f;
    armIn_ = firstSmp_;
    refPeak_ = 0.0f;
  }

  // A capture still open at the word's end is dropped, and a word that never took a grain has no tail.
  void endWord() {
    phase_ = Phase::Tail;
    cap_ = Cap::Idle;
    if (curLen_ == 0) closeEngine();
  }

  // Stops the loop and returns to the state reset() leaves, except the fixed-seed generator.
  void closeEngine() {
    running_ = false;
    phase_ = Phase::Idle;
    cap_ = Cap::Idle;
    curBuf_ = 0;
    curLen_ = 0;
    curPos_ = 0;
    prevOn_ = false;
    life_ = 0.0f;
    gain_ = 0.0f;
    refPeak_ = 0.0f;
    armIn_ = 0;
    keyHeld_ = false;
    keyHold_ = 0;
    ownOpen_ = false;
    lp_.clear();
    shelf_.clear();
  }

  float sample(float x, const Law& law, float wetTarget, const BloomTuning& t) {
    const bool crossing = (prevIn_ < 0.0f) != (x < 0.0f);
    prevIn_ = x;
    stepCapture(x, crossing, law);
    wl_ = smooth::step(wl_, wetTarget, smooth::kSmoothCoef);
    stepLife(t);
    const float wet = wetPath(t);
    ++clock_;
    return (x + wet) * trimGain(t);
  }

  // Arms captures on the timer, waits for a zero crossing, and writes the free buffer.
  void stepCapture(float x, bool crossing, const Law& law) {
    if (phase_ == Phase::Word) {
      if (armIn_ > 0) --armIn_;
      else if (cap_ == Cap::Idle && !prevOn_) arm(law);
    }
    if (cap_ == Cap::Waiting) {
      if (crossing || waitLeft_ <= 0) {
        cap_ = Cap::Writing;
        capPos_ = 0;
        capPeak_ = 0.0f;
        writtenAt_ = clock_;
      } else {
        --waitLeft_;
      }
    }
    if (cap_ == Cap::Writing) {
      grain_[static_cast<size_t>(1 - curBuf_)][static_cast<size_t>(capPos_++)] = x;
      const float a = fabsf(x);
      capPeak_ = a > capPeak_ ? a : capPeak_;
      if (capPos_ >= capLen_) finishCapture();
    }
  }

  void arm(const Law& law) {
    cap_ = Cap::Waiting;
    waitLeft_ = waitSmp_;
    capLen_ = law.grainSmp;
    armedAt_ = clock_;
    lcg_ = lcg_ * 1664525u + 1013904223u;
    const float r = static_cast<float>(static_cast<int32_t>(lcg_)) * (1.0f / 2147483648.0f);
    const int gap = static_cast<int>(law.intervalSmp * (1.0f + jitter_ * r));
    armIn_ = gap > 1 ? gap : 1;
  }

  // The new grain loops from its first sample, and the old one fades out under it.
  void finishCapture() {
    cap_ = Cap::Idle;
    if (curLen_ > 0 && refPeak_ > 0.0f && capPeak_ < refPeak_ * skipLin_) {
      ++skipped_;
      return;
    }
    prevOn_ = curLen_ > 0;
    prevLen_ = curLen_;
    prevPos_ = curPos_;
    curBuf_ = 1 - curBuf_;
    curLen_ = capLen_;
    curPos_ = 0;
    refPeak_ = capPeak_;
    ++taken_;
  }

  // Life stays 1 through the word, decays over the tail, and closes the loop when the wet gain is gone.
  void stepLife(const BloomTuning& t) {
    float target = 0.0f;
    if (phase_ == Phase::Word) {
      target = t.tailOnly ? 0.0f : wl_;
    } else if (phase_ == Phase::Tail) {
      life_ *= life_ > t.staticStart ? aTail_ : aStatic_;
      target = wl_ * life_;
      if (target < t.closeGain) {
        closeEngine();
        return;
      }
    }
    if (target > gain_) {
      gain_ += wl_ * invRampSmp_;
      gain_ = gain_ < target ? gain_ : target;
    } else {
      gain_ = target;
    }
  }

  // The loop through the wet tone, scaled by the wet gain.
  float wetPath(const BloomTuning& t) {
    float y = lp_.run(readLoop());
    const float bell = 4.0f * life_ * (1.0f - life_);
    y *= t.driveWord + t.driveBell * bell;
    y = y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);
    y -= y * y * y * (1.0f / 3.0f);
    return shelf_.run(y) * gain_;
  }

  // The looping grain with its pass window, plus the previous grain fading out for one fade.
  float readLoop() {
    if (curLen_ == 0) return 0.0f;
    float s = grain_[static_cast<size_t>(curBuf_)][static_cast<size_t>(curPos_)] * passWindow(curPos_, curLen_);
    if (prevOn_) {
      if (curPos_ >= fadeSmp_) {
        prevOn_ = false;
      } else {
        const float down = 1.0f - smoothstep(static_cast<float>(curPos_) * invFade_);
        s += grain_[static_cast<size_t>(1 - curBuf_)][static_cast<size_t>(prevPos_)] * passWindow(prevPos_, prevLen_) * down;
        if (++prevPos_ >= prevLen_) prevPos_ = 0;
      }
    }
    if (++curPos_ >= curLen_) curPos_ = 0;
    return s;
  }

  // Fades in over the first fadeSmp_ samples and out over the last.
  float passWindow(int pos, int len) const {
    const int toEnd = len - 1 - pos;
    const int edge = pos < toEnd ? pos : toEnd;
    return edge >= fadeSmp_ ? 1.0f : smoothstep(static_cast<float>(edge) * invFade_);
  }

  // Whole-output trim, scaled by the wet level so the bottom of BLOOM is unity.
  float trimGain(const BloomTuning& t) {
    if (wl_ != trimWl_ || t.trimDb != trimDb_ || t.wetHi != trimHi_) {
      trim_ = dbToGainFast(t.trimDb * (t.wetHi > 0.0f ? wl_ / t.wetHi : 0.0f));
      trimWl_ = wl_;
      trimDb_ = t.trimDb;
      trimHi_ = t.wetHi;
    }
    return trim_;
  }

  std::array<std::array<float, kGrainMax>, 2> grain_{};
  Biquad lp_;
  Biquad shelf_;

  // Word and key.
  Phase phase_ = Phase::Idle;
  bool running_ = false;
  bool ownOpen_ = false;
  bool keyHeld_ = false;
  int keyHold_ = 0;
  float life_ = 0.0f;
  float gain_ = 0.0f;
  float wl_ = 0.0f;
  float prevIn_ = 0.0f;
  float refPeak_ = 0.0f;

  // Capture into buffer 1 - curBuf_.
  Cap cap_ = Cap::Idle;
  int armIn_ = 0;
  int waitLeft_ = 0;
  int capLen_ = 0;
  int capPos_ = 0;
  float capPeak_ = 0.0f;

  // Loop playback: the current grain, and the previous one while it fades.
  int curBuf_ = 0;
  int curLen_ = 0;
  int curPos_ = 0;
  bool prevOn_ = false;
  int prevLen_ = 0;
  int prevPos_ = 0;

  // Event log for tests and the wiring.
  uint32_t lcg_ = kSeed;
  uint32_t clock_ = 0;
  uint32_t armedAt_ = 0;
  uint32_t writtenAt_ = 0;
  int taken_ = 0;
  int skipped_ = 0;

  // Derived from the tuning in prepare().
  bool prepared_ = false;
  int fadeSmp_ = 278;
  int firstSmp_ = 1920;
  int waitSmp_ = 480;
  int holdSmp_ = 2400;
  int minGrainSmp_ = 558;
  float invFade_ = 1.0f / 278.0f;
  float invRampSmp_ = 1.0f / 240.0f;
  float jitter_ = 0.05f;
  float openLin_ = 0.0f;
  float closeLin_ = 0.0f;
  float skipLin_ = 0.0f;
  float aTail_ = 0.0f;
  float aStatic_ = 0.0f;
  float lpHz_ = 0.0f, lpQ_ = 0.0f;
  float shHz_ = 0.0f, shDb_ = 0.0f, shSlope_ = 0.0f;
  float tailMs_ = 0.0f, tauScale_ = 0.0f, tauMin_ = 0.0f, tauMax_ = 0.0f, staticMs_ = 0.0f;
  float trim_ = 1.0f;
  float trimWl_ = -1.0f;
  float trimDb_ = 0.0f;
  float trimHi_ = 0.0f;
};

}  // namespace cv
