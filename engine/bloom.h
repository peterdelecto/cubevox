#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Chatter, the engine behind the Bloom card. While a word lasts, short slices of the voice loop
// under it. Each slice plays twice, then the next cut splices in at the slice boundary. When the word
// ends, its last good slice keeps repeating and fades over LINGER, gritty in the middle and clean at
// the end (spec 2026-11-01-bloom-design, sections 12 to 14).
//
// A word is a key. The Input gate hands its detector state in through BloomParams, or the engine
// listens to its own input. Three slices cover a word, its capture and the old tail fading under the
// next word. Slices are cut from the `src` signal, through the low-pass, and the dry path is `in`,
// untouched except for one level trim. Each voice's wet path runs drive, soft clip and its gain, then
// one high shelf runs over their sum.

namespace cv {

struct BloomTuning {
  // Level. The whole output scales by trimDb times the wet level over wetHi, so AMOUNT at the bottom
  // of its travel is exactly dry. The three sample clips spread 0.56 dB under one gain with the Input
  // gate key, wider than the 0.5 dB window, so this value centres them on it at the default.
  float trimDb = -5.62f;

  // Word timing.
  float firstCaptureMs = 40.0f;  // first slice, after the word starts
  float crossingWaitMs = 10.0f;  // longest wait for a zero crossing before a capture starts anyway
  float rampMs = 5.0f;           // wet gain rise to its level
  float fadeMs = 5.8f;           // pass fade in and out, and the splice crossfade
  float skipDb = -12.0f;         // a slice this far under the looping slice is skipped

  // AMOUNT laws. The wet level is wetHi * AMOUNT^wetExp, 0 at AMOUNT 0. The slice runs from
  // sliceLoMs at AMOUNT 0 to sliceHiMs at 100 %, and LINGER caps it at sliceTailFrac of its setting.
  float wetHi = 0.70f;
  float wetExp = 0.6f;
  float sliceLoMs = 80.0f;
  float sliceHiMs = 45.0f;
  float sliceMinMs = 35.0f;
  float sliceTailFrac = 0.8f;

  // Cut timing. Each cut's length drifts by up to this fraction either way, so the interval between
  // cuts, two plays of the slice, drifts with it.
  float drift = 0.03f;

  // While two loops sound together each plays this far down, so two loops carry the power of one.
  // The change rides the 20 ms smoother.
  float pairDb = -3.0f;

  // Age and fades. During a word no slice plays longer than maxAgeMs after it was cut, and closeMs
  // ends a loop that has nothing newer to play. A tail has no age limit and runs LINGER's law out.
  // nextWordMs is the fade a new word puts on the tail it finds, and yieldMs is the faster fade a
  // tail already fading takes when another word follows.
  float maxAgeMs = 2500.0f;
  float closeMs = 80.0f;
  float nextWordMs = 150.0f;
  float yieldMs = 20.0f;

  // LINGER laws.
  float tauScale = 1.5f;  // life time constant is max(LINGER, min(tauScale * LINGER, tauMaxMs))
  float tauMaxMs = 300.0f;
  float staticStart = 0.20f;  // life where the faster fade takes over
  float staticFadeMs = 80.0f;  // its time constant, or LINGER when that is shorter
  float closeGain = 0.001f;  // under this the wet gain writes 0 and the loop stops

  // Wet tone. The drive doubles through the word and swells by driveBell in the middle of the tail.
  float driveWord = 2.8f;
  float driveBell = 2.0f;
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
  float tailMs = 300.0f;  // panel LINGER, 20..3000
  float bloom = 0.5f;     // panel AMOUNT, 0..1
  bool keyOpen = false;   // the Input gate's detector state for this call
  bool keyFromGate = false;  // false makes the engine listen to its own input
  BloomTuning tuning;
};

class Bloom {
 public:
  static constexpr int kSlices = 3;                           // the loop, the capture, the fading tail
  static constexpr int kSliceMax = 80 * kSampleRate / 1000;   // 80 ms, 3,840 samples
  static constexpr int kPlaysPerSlice = 2;
  static constexpr int kVoices = 4;  // word, tail, and two fading tails
  static constexpr float kTailMinMs = 20.0f;
  static constexpr float kTailMaxMs = 3000.0f;

  void reset() {
    closeAll();
    for (Slice& s : slice_) s = Slice{};
    wl_ = 0.0f;
    prevSrc_ = 0.0f;
    lcg_ = kSeed;
    clock_ = 0;
    armedAt_ = 0;
    writtenAt_ = 0;
    taken_ = 0;
    skipped_ = 0;
    commitSeq_ = 0;
    trimWl_ = -1.0f;
    wetAmount_ = -1.0f;
    prepared_ = false;
  }

  // Cuts its slices from `in`, as a Bloom with nothing ahead of it in the chain does.
  void process(const float* in, float* out, int n, const BloomParams& p) { process(in, in, out, n, p); }

  // `in` is the dry path and the own key's input. `src` is the signal the slices are cut from, the
  // same block time-aligned with `in`. Any n; `out` may be `in`.
  void process(const float* in, const float* src, float* out, int n, const BloomParams& p) {
    prepare(p);
    for (int done = 0; done < n; done += kBlock) {
      const int m = n - done < kBlock ? n - done : kBlock;
      runChunk(in + done, src + done, out + done, m, p);
    }
  }

  // Sum of the voices' wet gains before the trim.
  float wetGain() const {
    float sum = 0.0f;
    for (const Voice& v : v_) sum += v.gain;
    return sum;
  }

  // True while a word is open (the key plus its hold).
  bool inWord() const { return v_[kWord].on; }

  // The gain on the summed loops: 1 with one loop sounding, pairDb with more.
  float pairScale() const { return pairScale_; }

  // True while any voice has a slice looping.
  bool looping() const {
    for (const Voice& v : v_)
      if (v.on && v.slice >= 0) return true;
    return false;
  }

  // Samples seen since reset(), and the clock at the latest capture arm and write start.
  uint32_t clock() const { return clock_; }
  uint32_t armedAt() const { return armedAt_; }
  uint32_t writeStartedAt() const { return writtenAt_; }

  // Slices committed, and slices skipped for sitting too far under the looping one, since reset().
  int taken() const { return taken_; }
  int skipped() const { return skipped_; }

  // Per voice (0 the word, 1 the tail, 2 and 3 the tails fading under a new word): whether it
  // runs, its gain, the commit number, cut clock and length of the slice it plays (0, 0 and 0 when
  // none), and the clock of its latest splice.
  bool voiceOn(int k) const { return v_[static_cast<size_t>(k)].on; }
  float voiceGain(int k) const { return v_[static_cast<size_t>(k)].gain; }
  uint32_t playingSeq(int k) const { return playing(k).seq; }
  uint32_t playingCutAt(int k) const { return playing(k).cutAt; }
  int playingLen(int k) const { return playing(k).len; }
  uint32_t spliceAt(int k) const { return v_[static_cast<size_t>(k)].splicedAt; }

  // The slices: whether one holds a finished cut, and its commit number, cut clock, length and samples.
  bool sliceValid(int slot) const { return slice_[static_cast<size_t>(slot)].valid; }
  uint32_t sliceSeq(int slot) const { return slice_[static_cast<size_t>(slot)].seq; }
  uint32_t sliceCutAt(int slot) const { return slice_[static_cast<size_t>(slot)].cutAt; }
  int sliceLen(int slot) const { return slice_[static_cast<size_t>(slot)].len; }
  const float* sliceData(int slot) const { return data_[static_cast<size_t>(slot)].data(); }

 private:
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;
  static constexpr uint32_t kSeed = 0x2545F491u;
  static constexpr int kWord = 0;
  static constexpr int kTail = 1;
  static constexpr int kFade = 2;  // first of the fading voices

  enum class Cap : uint8_t { Idle, Waiting, Writing };

  // One finished cut. `seq` numbers commits from 1, so a bigger number was cut later.
  struct Slice {
    bool valid = false;
    int len = 0;
    uint32_t seq = 0;
    uint32_t cutAt = 0;  // clock at the first sample written
    float peak = 0.0f;
  };

  // One loop. The word, the tail and the fading tails are all voices; the array index says which.
  // `slice` is the slice that loops (-1 for none) and `prev` the one fading out under a splice. A
  // fade is a straight fall of the gain to 0 by fadeStep each sample.
  struct Voice {
    bool on = false;
    int slice = -1;
    int len = 0;
    int pos = 0;
    bool prevOn = false;
    int prev = 0;
    int prevLen = 0;
    int prevPos = 0;
    uint32_t splicedAt = 0;
    uint32_t staleAt = 0;  // clock where the word's slice has used its age and the close fade starts
    float refPeak = 0.0f;  // peak of the looping slice
    float life = 1.0f;
    float gain = 0.0f;
    float fadeStep = 0.0f;
  };

  // Everything AMOUNT and LINGER set for one call: the wet level, the slice length AMOUNT asks for in
  // ms, and the longest slice LINGER allows.
  struct Law {
    float wet;
    float sliceMs;
    float capMs;
  };

  static float lerp(float a, float b, float t) { return a + (b - a) * t; }
  static float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

  Slice playing(int k) const {
    const Voice& v = v_[static_cast<size_t>(k)];
    return v.on && v.slice >= 0 ? slice_[static_cast<size_t>(v.slice)] : Slice{};
  }

  Law lawFor(const BloomParams& p) {
    const BloomTuning& t = p.tuning;
    const float amount = smooth::clamp01(p.bloom);
    if (amount != wetAmount_ || t.wetHi != wetHiKey_ || t.wetExp != wetExpKey_) {
      wet_ = amount > 0.0f ? t.wetHi * powf(amount, t.wetExp) : 0.0f;
      wetAmount_ = amount;
      wetHiKey_ = t.wetHi;
      wetExpKey_ = t.wetExp;
    }
    const float tailMs = clampf(p.tailMs, kTailMinMs, kTailMaxMs);
    return {wet_, lerp(t.sliceLoMs, t.sliceHiMs, amount), t.sliceTailFrac * tailMs};
  }

  // The length of the next cut in samples. AMOUNT's slice drifts by up to `drift` either way, from
  // the fixed-seed generator, so renders repeat. LINGER's cap and the floor apply after the drift.
  int nextSlice(const Law& law) {
    lcg_ = lcg_ * 1664525u + 1013904223u;
    const float r = static_cast<float>(static_cast<int32_t>(lcg_)) * (1.0f / 2147483648.0f);
    float ms = law.sliceMs * (1.0f + drift_ * r);
    ms = ms < law.capMs ? ms : law.capMs;
    ms = ms > sliceMinMs_ ? ms : sliceMinMs_;
    const int smp = static_cast<int>(ms * kSmpPerMs);
    return smp < minSliceSmp_ ? minSliceSmp_ : (smp > kSliceMax ? kSliceMax : smp);
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
    limitSmp_ = static_cast<uint32_t>(clampf(t.maxAgeMs, 100.0f, 60000.0f) * kSmpPerMs);
    closeSmp_ = static_cast<int>(clampf(t.closeMs, 1.0f, 2000.0f) * kSmpPerMs);
    nextWordSmp_ = static_cast<int>(clampf(t.nextWordMs, 1.0f, 2000.0f) * kSmpPerMs);
    yieldSmp_ = static_cast<int>(clampf(t.yieldMs, 1.0f, 500.0f) * kSmpPerMs);
    drift_ = clampf(t.drift, 0.0f, 0.2f);
    sliceMinMs_ = t.sliceMinMs;
    openLin_ = dbToGainFast(t.ownOpenDb);
    closeLin_ = dbToGainFast(t.ownOpenDb - (t.ownHysteresisDb > 0.0f ? t.ownHysteresisDb : 0.0f));
    skipLin_ = dbToGainFast(t.skipDb);
    pairLin_ = dbToGainFast(t.pairDb);
    minSliceSmp_ = 2 * fadeSmp_ + 2;
    if (t.trimDb != trimDb_ || t.wetHi != trimHi_) {
      trimDb_ = t.trimDb;
      trimHi_ = t.wetHi;
      trimWl_ = -1.0f;
    }
    prepareFilters(t);
    prepareTail(p);
    prepared_ = true;
  }

  void prepareFilters(const BloomTuning& t) {
    if (!prepared_ || t.lowpassHz != lpHz_ || t.lowpassQ != lpQ_) {
      capLp_.setLowPass(t.lowpassHz, t.lowpassQ);
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
    if (!prepared_ || tail != tailMs_ || t.tauScale != tauScale_ || t.tauMaxMs != tauMax_ ||
        t.staticFadeMs != staticMs_) {
      const float scaled = t.tauScale * tail < t.tauMaxMs ? t.tauScale * tail : t.tauMaxMs;
      const float tauMs = tail > scaled ? tail : scaled;
      const float staticMs = t.staticFadeMs < tail ? t.staticFadeMs : tail;
      aTail_ = expf(-1.0f / (tauMs * kSmpPerMs));
      aStatic_ = expf(-1.0f / (clampf(staticMs, 1.0f, 2000.0f) * kSmpPerMs));
      tailMs_ = tail;
      tauScale_ = t.tauScale;
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

  void runChunk(const float* in, const float* src, float* out, int m, const BloomParams& p) {
    const BloomTuning& t = p.tuning;
    const Law law = lawFor(p);
    const float wetTarget = p.on ? law.wet : 0.0f;
    if (wetTarget == 0.0f && wl_ == 0.0f) {
      for (int i = 0; i < m; ++i) out[i] = in[i];
      if (running_) closeAll();
      prevSrc_ = src[m - 1];
      clock_ += static_cast<uint32_t>(m);
      return;
    }
    running_ = true;
    stepKey(in, m, p);
    if (keyHeld_ && !v_[kWord].on) startWord();
    else if (!keyHeld_ && v_[kWord].on) endWord();
    for (int i = 0; i < m; ++i) out[i] = sample(in[i], src[i], law, wetTarget, t);
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

  // A tail still sounding moves to a fading voice and falls to 0 over the next-word fade, while the
  // word counts down to its first capture. A tail already fading takes the faster yield fade.
  void startWord() {
    if (v_[kTail].on) {
      int dst = kFade;
      for (int k = kFade; k < kVoices; ++k) {
        if (!v_[k].on) {
          dst = k;
          break;
        }
      }
      for (int k = kFade; k < kVoices; ++k)
        if (k != dst && v_[k].on) beginFade(v_[k], yieldSmp_);
      v_[dst] = v_[kTail];
      beginFade(v_[dst], nextWordSmp_);
      v_[kTail] = Voice{};
    }
    v_[kWord] = Voice{};
    v_[kWord].on = true;
    cap_ = Cap::Idle;
    fresh_ = -1;
    spliceSet_ = false;
    armSet_ = true;
    armAt_ = clock_ + static_cast<uint32_t>(firstSmp_);
  }

  // A capture still open at the word's end is dropped. A finished cut that waits for its splice plays
  // now, so the tail repeats the newest slice. A word that never played a slice has no tail.
  void endWord() {
    Voice& w = v_[kWord];
    cap_ = Cap::Idle;
    armSet_ = false;
    spliceSet_ = false;
    if (fresh_ >= 0) {
      play(w, fresh_);
      fresh_ = -1;
    }
    if (w.slice >= 0) v_[kTail] = w;
    w = Voice{};
  }

  // Closes every voice and the key.
  void closeAll() {
    for (Voice& v : v_) v = Voice{};
    for (Slice& s : slice_) s.valid = false;
    cap_ = Cap::Idle;
    fresh_ = -1;
    armSet_ = false;
    spliceSet_ = false;
    running_ = false;
    keyHeld_ = false;
    keyHold_ = 0;
    ownOpen_ = false;
    shelf_.clear();
    shelfLive_ = false;
    pairScale_ = 1.0f;
  }

  // Forgets the slice and the loop but not the word: the voice goes on capturing.
  static void clearPlayback(Voice& v) {
    v.slice = -1;
    v.pos = 0;
    v.prevOn = false;
    v.refPeak = 0.0f;
    v.life = 1.0f;
    v.gain = 0.0f;
    v.fadeStep = 0.0f;
  }

  // The gain falls straight to 0 over `samples`, or faster if a fade is already under way.
  static void beginFade(Voice& v, int samples) {
    float step = v.gain / static_cast<float>(samples);
    step = step > 1e-7f ? step : 1e-7f;
    v.fadeStep = step > v.fadeStep ? step : v.fadeStep;
  }

  float sample(float x, float s, const Law& law, float wetTarget, const BloomTuning& t) {
    const bool crossing = (prevSrc_ < 0.0f) != (s < 0.0f);
    prevSrc_ = s;
    if (wl_ != wetTarget) wl_ = smooth::step(wl_, wetTarget, smooth::kSmoothCoef);
    if (v_[kWord].on) {
      stepCapture(s, crossing, law);
      stepSplice(law);
    }
    float wet = 0.0f;
    int loops = 0;
    for (int k = 0; k < kVoices; ++k) {
      Voice& v = v_[static_cast<size_t>(k)];
      if (!v.on || !stepGain(k, t)) continue;
      wet += wetOf(v, t);
      loops += v.slice >= 0 ? 1 : 0;
    }
    const bool looped = loops > 0;
    const float pairTarget = loops > 1 ? pairLin_ : 1.0f;
    if (pairScale_ != pairTarget) pairScale_ = smooth::step(pairScale_, pairTarget, smooth::kSmoothCoef);
    wet *= pairScale_;

    // The shelf runs while a slice loops, and its state clears when none does.
    if (looped) {
      wet = shelf_.run(wet);
      shelfLive_ = true;
    } else if (shelfLive_) {
      shelf_.clear();
      shelfLive_ = false;
    }
    ++clock_;
    return (x + wet) * trimGain();
  }

  // Arms a cut on the plan, waits for a zero crossing, and writes a free slice.
  void stepCapture(float s, bool crossing, const Law& law) {
    if (armSet_ && cap_ == Cap::Idle && due(armAt_)) arm(law);
    if (cap_ == Cap::Waiting && (crossing || due(waitUntil_))) {
      const int slot = freeSlice();
      if (slot >= 0) {
        cap_ = Cap::Writing;
        capPos_ = 0;
        capPeak_ = 0.0f;
        capCut_ = clock_;
        capSlot_ = slot;
        slice_[static_cast<size_t>(slot)].valid = false;
        capLp_.clear();
        writtenAt_ = clock_;
      }
    }
    if (cap_ == Cap::Writing) {
      data_[static_cast<size_t>(capSlot_)][static_cast<size_t>(capPos_++)] = capLp_.run(s);
      const float a = fabsf(s);
      capPeak_ = a > capPeak_ ? a : capPeak_;
      if (capPos_ >= capLen_) finishCapture();
    }
  }

  // The word's first cut is armed 40 ms in and splices after the longest wait and its length. Every
  // later cut takes the length its plan fixed.
  void arm(const Law& law) {
    cap_ = Cap::Waiting;
    armSet_ = false;
    waitUntil_ = clock_ + static_cast<uint32_t>(waitSmp_);
    armedAt_ = clock_;
    if (spliceSet_) {
      capLen_ = planLen_;
    } else {
      capLen_ = nextSlice(law);
      spliceAt_ = clock_ + static_cast<uint32_t>(waitSmp_ + capLen_);
      spliceSet_ = true;
    }
  }

  // The next splice falls `interval` after `base`, two plays of the slice that loops. The cut it will
  // play takes the next drifting length and is armed late enough to be the newest sound before the
  // splice, so it is ready by then at the latest.
  void planNext(uint32_t base, int interval, const Law& law) {
    spliceAt_ = base + static_cast<uint32_t>(interval);
    planLen_ = nextSlice(law);
    armAt_ = spliceAt_ - static_cast<uint32_t>(waitSmp_ + planLen_);
    if (static_cast<int32_t>(armAt_ - clock_) < 0) armAt_ = clock_;
    armSet_ = true;
  }

  // A free slice, which is not looping, not fading out under a splice and not a finished cut waiting
  // to play. An empty one goes first, else the oldest cut, so the slices turn over in rotation.
  int freeSlice() const {
    int best = -1;
    for (int j = 0; j < kSlices; ++j) {
      bool used = j == fresh_;
      for (const Voice& v : v_) used = used || (v.on && (v.slice == j || (v.prevOn && v.prev == j)));
      if (used) continue;
      const Slice& s = slice_[static_cast<size_t>(j)];
      if (!s.valid) return j;
      if (best < 0 || s.seq < slice_[static_cast<size_t>(best)].seq) best = j;
    }
    return best;
  }

  // True once the clock has reached `at`, across the clock's wrap.
  bool due(uint32_t at) const { return static_cast<int32_t>(clock_ - at) >= 0; }

  // Whether the slice that loops may play on until `ahead` samples from now plus one more interval
  // and still close within the age limit.
  bool mayPlayOn(const Voice& v, uint32_t ahead, int interval) const {
    if (v.slice < 0) return true;
    const uint32_t age = clock_ + ahead - slice_[static_cast<size_t>(v.slice)].cutAt;
    return age + static_cast<uint32_t>(interval + closeSmp_) <= limitSmp_;
  }

  // The new slice waits for its splice. A slice more than skipDb under the looping one is skipped,
  // unless the looping one cannot play another interval, which it replaces even when quieter.
  void finishCapture() {
    cap_ = Cap::Idle;
    const Voice& w = v_[kWord];
    const bool quiet = w.slice >= 0 && w.refPeak > 0.0f && capPeak_ < w.refPeak * skipLin_;
    const uint32_t ahead = due(spliceAt_) ? 0u : spliceAt_ - clock_;
    if (quiet && mayPlayOn(w, ahead, kPlaysPerSlice * capLen_)) {
      ++skipped_;
      return;
    }
    Slice& s = slice_[static_cast<size_t>(capSlot_)];
    s.valid = true;
    s.len = capLen_;
    s.seq = ++commitSeq_;
    s.cutAt = capCut_;
    s.peak = capPeak_;
    fresh_ = capSlot_;
    ++taken_;
  }

  // At the splice the finished cut plays and the next one is planned. With the cut still under way
  // the loop plays one more pass; with it skipped the loop keeps its slice and the grid moves on two plays.
  void stepSplice(const Law& law) {
    if (!spliceSet_ || !due(spliceAt_)) return;
    Voice& w = v_[kWord];
    if (fresh_ >= 0) {
      play(w, fresh_);
      fresh_ = -1;
      planNext(clock_, kPlaysPerSlice * w.len, law);
    } else if (cap_ != Cap::Idle) {
      spliceAt_ += static_cast<uint32_t>(w.slice >= 0 ? w.len : capLen_);
    } else {
      planNext(spliceAt_, kPlaysPerSlice * (w.slice >= 0 ? w.len : capLen_), law);
    }
  }

  // The slice loops from its first sample and the old one fades out under it. A fade that had begun
  // for want of a slice ends.
  void play(Voice& v, int slot) {
    if (v.slice >= 0 && v.slice != slot) {
      v.prevOn = true;
      v.prev = v.slice;
      v.prevLen = v.len;
      v.prevPos = v.pos;
    }
    const Slice& s = slice_[static_cast<size_t>(slot)];
    v.slice = slot;
    v.len = s.len;
    v.pos = 0;
    v.refPeak = s.peak;
    v.splicedAt = clock_;
    v.staleAt = s.cutAt + limitSmp_ - static_cast<uint32_t>(closeSmp_);
    v.fadeStep = 0.0f;
  }

  // Life stays 1 through the word and decays over the tail. The wet gain rides toward its target, or
  // falls straight to 0 in a fade. During a word, a slice that has used its age starts the close fade;
  // a tail has no age limit. Returns false when the voice closed or lost its slice.
  bool stepGain(int k, const BloomTuning& t) {
    Voice& v = v_[static_cast<size_t>(k)];
    if (k == kWord && v.slice >= 0 && v.fadeStep == 0.0f && due(v.staleAt)) beginFade(v, closeSmp_);
    if (v.fadeStep > 0.0f) {
      v.gain -= v.fadeStep;
      if (v.gain > 0.0f) return true;
      if (k == kWord) {
        clearPlayback(v);
      } else {
        v = Voice{};
      }
      return false;
    }
    float target = 0.0f;
    if (k == kWord) {
      target = t.tailOnly ? 0.0f : wl_;
    } else {
      v.life *= v.life > t.staticStart ? aTail_ : aStatic_;
      target = wl_ * v.life;
      if (target < t.closeGain) {
        v = Voice{};
        return false;
      }
    }
    if (target > v.gain) {
      v.gain += wl_ * invRampSmp_;
      v.gain = v.gain < target ? v.gain : target;
    } else {
      v.gain = target;
    }
    return true;
  }

  // The loop through the wet tone, scaled by the voice's gain.
  float wetOf(Voice& v, const BloomTuning& t) {
    if (v.slice < 0) return 0.0f;
    float y = readLoop(v);
    const float bell = 4.0f * v.life * (1.0f - v.life);
    y *= t.driveWord + t.driveBell * bell;
    y = y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);
    y -= y * y * y * (1.0f / 3.0f);
    return y * v.gain;
  }

  // The looping slice with its pass window, plus the previous slice fading out for one fade.
  float readLoop(Voice& v) {
    float s = data_[static_cast<size_t>(v.slice)][static_cast<size_t>(v.pos)] * passWindow(v.pos, v.len);
    if (v.prevOn) {
      if (v.pos >= fadeSmp_) {
        v.prevOn = false;
      } else {
        const float down = 1.0f - smoothstep(static_cast<float>(v.pos) * invFade_);
        s += data_[static_cast<size_t>(v.prev)][static_cast<size_t>(v.prevPos)] * passWindow(v.prevPos, v.prevLen) * down;
        if (++v.prevPos >= v.prevLen) v.prevPos = 0;
      }
    }
    if (++v.pos >= v.len) v.pos = 0;
    return s;
  }

  // Fades in over the first fadeSmp_ samples and out over the last.
  float passWindow(int pos, int len) const {
    const int toEnd = len - 1 - pos;
    const int edge = pos < toEnd ? pos : toEnd;
    return edge >= fadeSmp_ ? 1.0f : smoothstep(static_cast<float>(edge) * invFade_);
  }

  // Whole-output trim, scaled by the wet level so the bottom of AMOUNT is unity.
  float trimGain() {
    if (wl_ != trimWl_) {
      trim_ = dbToGainFast(trimDb_ * (trimHi_ > 0.0f ? wl_ / trimHi_ : 0.0f));
      trimWl_ = wl_;
    }
    return trim_;
  }

  std::array<std::array<float, kSliceMax>, kSlices> data_{};
  std::array<Slice, kSlices> slice_{};
  std::array<Voice, kVoices> v_{};
  Biquad shelf_;
  Biquad capLp_;  // the low-pass a slice passes through as it is cut
  bool shelfLive_ = false;

  // Word and key.
  bool running_ = false;
  bool ownOpen_ = false;
  bool keyHeld_ = false;
  int keyHold_ = 0;
  float wl_ = 0.0f;
  float pairScale_ = 1.0f;
  float prevSrc_ = 0.0f;
  uint32_t commitSeq_ = 0;

  // The cut schedule. `armAt_` and `spliceAt_` are clock values; `fresh_` is a finished cut that waits
  // for its splice (-1 for none).
  bool armSet_ = false;
  bool spliceSet_ = false;
  uint32_t armAt_ = 0;
  uint32_t spliceAt_ = 0;
  int planLen_ = 0;
  int fresh_ = -1;

  // The cut under way.
  Cap cap_ = Cap::Idle;
  uint32_t waitUntil_ = 0;
  int capLen_ = 0;
  int capPos_ = 0;
  int capSlot_ = 0;
  float capPeak_ = 0.0f;
  uint32_t capCut_ = 0;

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
  int minSliceSmp_ = 558;
  int closeSmp_ = 3840;
  int nextWordSmp_ = 7200;
  int yieldSmp_ = 960;
  uint32_t limitSmp_ = 120000;
  float drift_ = 0.03f;
  float sliceMinMs_ = 35.0f;
  float invFade_ = 1.0f / 278.0f;
  float invRampSmp_ = 1.0f / 240.0f;
  float openLin_ = 0.0f;
  float closeLin_ = 0.0f;
  float skipLin_ = 0.0f;
  float pairLin_ = 0.7079f;
  float aTail_ = 0.0f;
  float aStatic_ = 0.0f;
  float lpHz_ = 0.0f, lpQ_ = 0.0f;
  float shHz_ = 0.0f, shDb_ = 0.0f, shSlope_ = 0.0f;
  float tailMs_ = 0.0f, tauScale_ = 0.0f, tauMax_ = 0.0f, staticMs_ = 0.0f;
  float wet_ = 0.0f, wetAmount_ = -1.0f, wetHiKey_ = 0.0f, wetExpKey_ = 0.0f;
  float trim_ = 1.0f;
  float trimWl_ = -1.0f;
  float trimDb_ = 0.0f;
  float trimHi_ = 0.0f;
};

}  // namespace cv
