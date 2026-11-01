#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Bloom: while a word lasts, 62.5 ms slices of the voice loop under it, a fresh slice every 125 ms.
// When the word ends, its last four slices rotate in the order they were sung and fade over LINGER,
// gritty in the middle and clean at the end (spec 2026-11-01-bloom-design, sections 12 and 13).
//
// A word is a key. The Input gate hands its detector state in through BloomParams, or the engine
// listens to its own input. Two players run at once. Each owns four slices, so a new word starts
// its own loop while the old tail fades on its own, and a third word fades the oldest tail out.
// Slices are cut from the `src` signal, through the low-pass, and the dry path is `in`, untouched
// except for one level trim. Each player's wet path runs drive, soft clip and its gain, then one
// high shelf.

namespace cv {

struct BloomTuning {
  // Level. The whole output scales by trimDb times the wet level over wetHi, so AMOUNT at the bottom
  // of its travel is exactly dry. The three sample clips spread over 0.59 dB under one gain with the
  // Input gate key, wider than the 0.5 dB window, so this value centres them on it.
  float trimDb = -2.05f;

  // Word timing.
  float firstCaptureMs = 40.0f;  // first slice, after the word starts
  float crossingWaitMs = 10.0f;  // longest wait for a zero crossing before a capture starts anyway
  float rampMs = 5.0f;           // wet gain rise to its level
  float fadeMs = 5.8f;           // pass fade in and out, and the splice crossfade
  float skipDb = -12.0f;         // a slice this far under the looping slice is skipped

  // AMOUNT law: the wet level is wetHi * AMOUNT^wetExp, 0 at AMOUNT 0.
  float wetHi = 0.70f;
  float wetExp = 0.6f;

  // While two loops sound together each plays this far down, so two loops carry the power of one
  // and a tail under the next word does not stack louder. The change rides the 20 ms smoother.
  float pairDb = -3.0f;

  // Slices. The length is fixed and LINGER caps it. A fresh slice is cut every two slice lengths,
  // so each slice plays twice and the splices land on that grid.
  float grainMs = 62.5f;
  float grainMinMs = 35.0f;
  float grainTailFrac = 0.8f;  // the slice stays under this fraction of LINGER

  // Age and fades. No slice plays longer than maxAgeMs after it was cut, closeMs ends a loop that
  // has nothing newer to play, and yieldMs is the fade a third word puts on the oldest tail.
  float maxAgeMs = 2500.0f;
  float closeMs = 80.0f;
  float yieldMs = 20.0f;

  // LINGER laws.
  float tauScale = 1.5f;  // life time constant is max(LINGER, clamp(tauScale * LINGER, tauMinMs, tauMaxMs))
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
  float tailMs = 300.0f;  // panel LINGER, 60..2000
  float bloom = 0.5f;     // panel AMOUNT, 0..1
  bool keyOpen = false;   // the Input gate's detector state for this call
  bool keyFromGate = false;  // false makes the engine listen to its own input
  BloomTuning tuning;
};

class Bloom {
 public:
  static constexpr int kPlayers = 2;
  static constexpr int kKept = 4;                                // slices one player keeps
  static constexpr int kSliceMax = 125 * kSampleRate / 2000;     // 62.5 ms, 3,000 samples
  static constexpr int kPlaysPerSlice = 2;
  static constexpr float kTailMinMs = 60.0f;
  static constexpr float kTailMaxMs = 2000.0f;

  void reset() {
    closeAll();
    wl_ = 0.0f;
    prevSrc_ = 0.0f;
    clock_ = 0;
    armedAt_ = 0;
    writtenAt_ = 0;
    taken_ = 0;
    skipped_ = 0;
    wordSeq_ = 0;
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

  // Sum of the players' wet gains before the trim.
  float wetGain() const {
    float sum = 0.0f;
    for (const Player& pl : pl_) sum += pl.gain;
    return sum;
  }

  // True while a word is open (the key plus its hold).
  bool inWord() const { return wordPlayer_ >= 0; }

  // The gain on the summed loops: 1 with one loop sounding, pairDb with two.
  float pairScale() const { return pairScale_; }

  // True while any player has a slice looping.
  bool looping() const {
    for (const Player& pl : pl_)
      if (pl.phase != Phase::Idle && pl.cur >= 0) return true;
    return false;
  }

  // Samples seen since reset(), and the clock at the latest capture arm and write start.
  uint32_t clock() const { return clock_; }
  uint32_t armedAt() const { return armedAt_; }
  uint32_t writeStartedAt() const { return writtenAt_; }

  // Slices committed, and slices skipped for sitting too far under the looping one, since reset().
  int taken() const { return taken_; }
  int skipped() const { return skipped_; }

  // Per player: its phase, gain, the commit number and cut clock of the slice it plays (0 and 0 when
  // none), and the clock of its latest splice.
  bool playerActive(int k) const { return pl_[static_cast<size_t>(k)].phase != Phase::Idle; }
  bool playerInTail(int k) const { return pl_[static_cast<size_t>(k)].phase == Phase::Tail; }
  float playerGain(int k) const { return pl_[static_cast<size_t>(k)].gain; }
  uint32_t playingSeq(int k) const { return playing(k).seq; }
  uint32_t playingCutAt(int k) const { return playing(k).cutAt; }
  uint32_t spliceAt(int k) const { return pl_[static_cast<size_t>(k)].splicedAt; }

  // The slices a player holds: how many are valid, and one slice's commit number, cut clock, length
  // and samples.
  int keptCount(int k) const {
    int c = 0;
    for (const Slice& s : pl_[static_cast<size_t>(k)].slice) c += s.valid ? 1 : 0;
    return c;
  }
  bool sliceValid(int k, int slot) const { return pl_[static_cast<size_t>(k)].slice[static_cast<size_t>(slot)].valid; }
  uint32_t sliceSeq(int k, int slot) const { return pl_[static_cast<size_t>(k)].slice[static_cast<size_t>(slot)].seq; }
  uint32_t sliceCutAt(int k, int slot) const { return pl_[static_cast<size_t>(k)].slice[static_cast<size_t>(slot)].cutAt; }
  int sliceLen(int k, int slot) const { return pl_[static_cast<size_t>(k)].slice[static_cast<size_t>(slot)].len; }
  const float* sliceData(int k, int slot) const { return data_[static_cast<size_t>(k * kKept + slot)].data(); }

 private:
  static constexpr float kSmpPerMs = kSampleRate / 1000.0f;

  enum class Phase : uint8_t { Idle, Word, Tail };
  enum class Cap : uint8_t { Idle, Waiting, Writing };

  // One kept slice. `seq` numbers commits from 1, so a bigger number was sung later.
  struct Slice {
    bool valid = false;
    int len = 0;
    uint32_t seq = 0;
    uint32_t cutAt = 0;  // clock at the first sample written
    float peak = 0.0f;
  };

  // One loop with its four slices: the word it follows, the slice it plays, its gain and its capture.
  struct Player {
    Phase phase = Phase::Idle;
    uint32_t born = 0;  // word order; the oldest tail has the smallest
    std::array<Slice, kKept> slice;

    // Playback: the slice that loops, and the previous one while it fades out under a splice. The
    // index and length of each are copied out of `slice` and `data_` for the per-sample read.
    int cur = -1;
    int curIdx = 0;
    int curLen = 0;
    int pos = 0;
    bool prevOn = false;
    int prev = 0;
    int prevIdx = 0;
    int prevLen = 0;
    int prevPos = 0;
    uint32_t splicedAt = 0;
    float refPeak = 0.0f;  // peak of the looping slice

    // The next splice, as a clock value: in a word, the capture's slot on the grid; in a tail, the
    // next slice's turn.
    bool nextDue = false;
    uint32_t nextAt = 0;
    bool fresh = false;  // a committed slice waits for its splice

    // Gain. A fade is a straight fall to 0 over fadeStep each sample; `yield` marks the fade a third
    // word puts on a tail whose player it takes over.
    float life = 0.0f;
    float gain = 0.0f;
    float fadeStep = 0.0f;
    bool yield = false;

    // Capture into slice `capSlot`. `armAt` is the clock of the next arm and `waitUntil` the clock
    // where a capture that has seen no zero crossing starts anyway.
    Cap cap = Cap::Idle;
    uint32_t armAt = 0;
    uint32_t waitUntil = 0;
    int capLen = 0;
    int capPos = 0;
    int capSlot = 0;
    float capPeak = 0.0f;
    uint32_t capCut = 0;
  };

  // Everything AMOUNT and LINGER set for one call.
  struct Law {
    float wet;
    int grainSmp;
    int intervalSmp;
  };

  static float lerp(float a, float b, float t) { return a + (b - a) * t; }
  static float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

  Slice playing(int k) const {
    const Player& pl = pl_[static_cast<size_t>(k)];
    return pl.phase != Phase::Idle && pl.cur >= 0 ? pl.slice[static_cast<size_t>(pl.cur)] : Slice{};
  }

  float* data(int k, int slot) { return data_[static_cast<size_t>(k * kKept + slot)].data(); }

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
    float grainMs = clampf(t.grainMs, 1.0f, static_cast<float>(kSliceMax) / kSmpPerMs);
    grainMs = grainMs < t.grainTailFrac * tailMs ? grainMs : t.grainTailFrac * tailMs;
    grainMs = grainMs > t.grainMinMs ? grainMs : t.grainMinMs;
    int grain = static_cast<int>(grainMs * kSmpPerMs);
    grain = grain < minGrainSmp_ ? minGrainSmp_ : (grain > kSliceMax ? kSliceMax : grain);
    return {wet_, grain, kPlaysPerSlice * grain};
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
    yieldSmp_ = static_cast<int>(clampf(t.yieldMs, 1.0f, 500.0f) * kSmpPerMs);
    openLin_ = dbToGainFast(t.ownOpenDb);
    closeLin_ = dbToGainFast(t.ownOpenDb - (t.ownHysteresisDb > 0.0f ? t.ownHysteresisDb : 0.0f));
    skipLin_ = dbToGainFast(t.skipDb);
    pairLin_ = dbToGainFast(t.pairDb);
    minGrainSmp_ = 2 * fadeSmp_ + 2;
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
    if (keyHeld_ && wordPlayer_ < 0) startWord();
    else if (!keyHeld_ && wordPlayer_ >= 0) endWord();
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

  // A word takes an idle player. With none idle, the oldest tail gives way: it fades out over the
  // yield time while the word counts down to its first capture, and the player opens when it is gone.
  void startWord() {
    int k = -1;
    for (int j = 0; j < kPlayers; ++j)
      if (pl_[static_cast<size_t>(j)].phase == Phase::Idle) {
        k = j;
        break;
      }
    bool yielding = false;
    if (k < 0) {
      k = 0;
      for (int j = 1; j < kPlayers; ++j)
        if (pl_[static_cast<size_t>(j)].born < pl_[static_cast<size_t>(k)].born) k = j;
      yielding = true;
    }
    Player& pl = pl_[static_cast<size_t>(k)];
    pl.phase = Phase::Word;
    pl.born = ++wordSeq_;
    pl.cap = Cap::Idle;
    pl.armAt = clock_ + static_cast<uint32_t>(firstSmp_);
    pl.nextDue = false;
    pl.fresh = false;
    pl.refPeak = 0.0f;
    if (yielding) {
      beginFade(pl, yieldSmp_);
      pl.yield = true;
    } else {
      pl.life = 1.0f;
    }
    wordPlayer_ = k;
  }

  // A capture still open at the word's end is dropped. The rotation keeps the word's grid: the next
  // slice takes its turn when the next capture would have been spliced in. A word that kept no slice
  // has no tail.
  void endWord() {
    Player& pl = pl_[static_cast<size_t>(wordPlayer_)];
    const int k = wordPlayer_;
    wordPlayer_ = -1;
    pl.phase = Phase::Tail;
    pl.cap = Cap::Idle;
    pl.fresh = false;
    if (pl.yield) {
      pl.yield = false;
      return;
    }
    pl.life = 1.0f;
    if (!pl.nextDue) {
      pl.nextAt = pl.armAt + static_cast<uint32_t>(waitSmp_ + pl.capLen);
      pl.nextDue = true;
    }
    if (keptCount(k) == 0) closePlayer(k);
  }

  // Returns player `k` to the state reset() leaves.
  void closePlayer(int k) { pl_[static_cast<size_t>(k)] = Player{}; }

  // Closes every player and the key.
  void closeAll() {
    for (int k = 0; k < kPlayers; ++k) closePlayer(k);
    wordPlayer_ = -1;
    running_ = false;
    keyHeld_ = false;
    keyHold_ = 0;
    ownOpen_ = false;
    shelf_.clear();
    shelfLive_ = false;
    pairScale_ = 1.0f;
  }

  // Forgets the slices and the loop but not the word: a player in a word goes on capturing.
  void clearPlayback(Player& pl) {
    for (Slice& s : pl.slice) s.valid = false;
    pl.cur = -1;
    pl.pos = 0;
    pl.prevOn = false;
    pl.refPeak = 0.0f;
    pl.life = 1.0f;
    pl.gain = 0.0f;
    pl.fadeStep = 0.0f;
    pl.yield = false;
  }

  // The gain falls straight to 0 over `samples`.
  static void beginFade(Player& pl, int samples) {
    const float step = pl.gain / static_cast<float>(samples);
    pl.fadeStep = step > 1e-7f ? step : 1e-7f;
  }

  float sample(float x, float s, const Law& law, float wetTarget, const BloomTuning& t) {
    const bool crossing = (prevSrc_ < 0.0f) != (s < 0.0f);
    prevSrc_ = s;
    if (wl_ != wetTarget) wl_ = smooth::step(wl_, wetTarget, smooth::kSmoothCoef);
    float wet = 0.0f;
    int loops = 0;
    for (int k = 0; k < kPlayers; ++k) {
      Player& pl = pl_[static_cast<size_t>(k)];
      if (pl.phase == Phase::Idle) continue;
      wet += stepPlayer(k, s, crossing, law, t);
      loops += pl.cur >= 0 ? 1 : 0;
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

  // One player's sample: capture on a word, the splice clock, the gain, then the wet path.
  float stepPlayer(int k, float s, bool crossing, const Law& law, const BloomTuning& t) {
    Player& pl = pl_[static_cast<size_t>(k)];
    if (pl.phase == Phase::Word) stepCapture(k, s, crossing, law);
    if (!pl.yield) stepSplice(k, law);
    if (!stepGain(k, t)) return 0.0f;
    return wetOf(k, t);
  }

  // Arms captures on the grid, waits for a zero crossing, and writes a free slice.
  void stepCapture(int k, float s, bool crossing, const Law& law) {
    Player& pl = pl_[static_cast<size_t>(k)];
    if (pl.cap == Cap::Idle && due(pl.armAt) && !pl.yield) arm(pl, law);
    if (pl.cap == Cap::Waiting && (crossing || due(pl.waitUntil))) {
      pl.cap = Cap::Writing;
      pl.capPos = 0;
      pl.capPeak = 0.0f;
      pl.capCut = clock_;
      pl.capSlot = writeSlot(pl);
      pl.slice[static_cast<size_t>(pl.capSlot)].valid = false;
      capLp_.clear();
      writtenAt_ = clock_;
    }
    if (pl.cap == Cap::Writing) {
      data(k, pl.capSlot)[pl.capPos++] = capLp_.run(s);
      const float a = fabsf(s);
      pl.capPeak = a > pl.capPeak ? a : pl.capPeak;
      if (pl.capPos >= pl.capLen) finishCapture(pl);
    }
  }

  // The next arm falls one interval on. The capture's splice falls after the longest wait and the
  // slice, so it lands on the grid whenever the write began.
  void arm(Player& pl, const Law& law) {
    pl.cap = Cap::Waiting;
    pl.waitUntil = clock_ + static_cast<uint32_t>(waitSmp_);
    pl.capLen = law.grainSmp;
    pl.armAt = clock_ + static_cast<uint32_t>(law.intervalSmp);
    pl.nextAt = clock_ + static_cast<uint32_t>(waitSmp_ + pl.capLen);
    pl.nextDue = true;
    armedAt_ = clock_;
  }

  // An invalid slice, else the oldest, never the one that plays or fades out.
  static int writeSlot(const Player& pl) {
    int best = -1;
    for (int j = 0; j < kKept; ++j) {
      if (j == pl.cur || (pl.prevOn && j == pl.prev)) continue;
      const Slice& s = pl.slice[static_cast<size_t>(j)];
      if (!s.valid) return j;
      if (best < 0 || s.seq < pl.slice[static_cast<size_t>(best)].seq) best = j;
    }
    return best;
  }

  // True once the clock has reached `at`, across the clock's wrap.
  bool due(uint32_t at) const { return static_cast<int32_t>(clock_ - at) >= 0; }

  // Age of a slice now, and whether the slice that plays may play another interval and still close
  // within the limit.
  uint32_t ageOf(const Slice& s) const { return clock_ - s.cutAt; }
  bool mayPlayOn(const Player& pl, uint32_t ahead, int interval) const {
    if (pl.cur < 0) return true;
    const uint32_t age = ageOf(pl.slice[static_cast<size_t>(pl.cur)]) + ahead;
    return age + static_cast<uint32_t>(interval + closeSmp_) <= limitSmp_;
  }

  // The new slice waits for its splice. A slice more than skipDb under the looping one is skipped,
  // unless the looping one cannot play another interval, which it replaces even when quieter.
  void finishCapture(Player& pl) {
    pl.cap = Cap::Idle;
    const bool quiet = pl.cur >= 0 && pl.refPeak > 0.0f && pl.capPeak < pl.refPeak * skipLin_;
    if (quiet && mayPlayOn(pl, pl.nextAt - clock_, kPlaysPerSlice * pl.capLen)) {
      ++skipped_;
      return;
    }
    Slice& s = pl.slice[static_cast<size_t>(pl.capSlot)];
    s.valid = true;
    s.len = pl.capLen;
    s.seq = ++commitSeq_;
    s.cutAt = pl.capCut;
    s.peak = pl.capPeak;
    pl.fresh = true;
    ++taken_;
  }

  void stepSplice(int k, const Law& law) {
    Player& pl = pl_[static_cast<size_t>(k)];
    if (!pl.nextDue || !due(pl.nextAt)) return;
    pl.nextDue = false;
    if (pl.phase == Phase::Word) spliceWord(pl, law);
    else spliceTail(pl);
  }

  // Plays the newest slice. With none new, the looping one stays unless it has used its age.
  void spliceWord(Player& pl, const Law& law) {
    if (pl.fresh) {
      pl.fresh = false;
      const int next = newestSlot(pl);
      if (next >= 0) play(pl, next);
    } else if (!mayPlayOn(pl, 0, law.intervalSmp)) {
      beginFade(pl, closeSmp_);
    }
  }

  // Plays the next kept slice in the order sung, wrapping from the newest to the oldest. Slices that
  // cannot play out a turn leave the rotation, and with none left the loop closes.
  void spliceTail(Player& pl) {
    for (Slice& s : pl.slice)
      if (s.valid && ageOf(s) + static_cast<uint32_t>(kPlaysPerSlice * s.len + closeSmp_) > limitSmp_) s.valid = false;
    const int next = successor(pl);
    if (next < 0) {
      beginFade(pl, closeSmp_);
      return;
    }
    if (next != pl.cur) play(pl, next);
    pl.nextAt = clock_ + static_cast<uint32_t>(kPlaysPerSlice * pl.slice[static_cast<size_t>(next)].len);
    pl.nextDue = true;
  }

  // The slice loops from its first sample and the old one fades out under it. A fade that had begun
  // for want of a slice ends.
  void play(Player& pl, int slot) {
    if (pl.cur >= 0 && pl.cur != slot) {
      pl.prevOn = true;
      pl.prev = pl.cur;
      pl.prevIdx = pl.curIdx;
      pl.prevLen = pl.curLen;
      pl.prevPos = pl.pos;
    }
    pl.cur = slot;
    pl.curIdx = static_cast<int>(&pl - pl_.data()) * kKept + slot;
    pl.curLen = pl.slice[static_cast<size_t>(slot)].len;
    pl.pos = 0;
    pl.refPeak = pl.slice[static_cast<size_t>(slot)].peak;
    pl.splicedAt = clock_;
    if (pl.phase == Phase::Word && !pl.yield) pl.fadeStep = 0.0f;
  }

  static int newestSlot(const Player& pl) {
    int best = -1;
    for (int j = 0; j < kKept; ++j) {
      const Slice& s = pl.slice[static_cast<size_t>(j)];
      if (s.valid && (best < 0 || s.seq > pl.slice[static_cast<size_t>(best)].seq)) best = j;
    }
    return best;
  }

  // The valid slice sung just after the one that plays, else the first sung. -1 when none is valid.
  static int successor(const Player& pl) {
    const uint32_t after = pl.cur >= 0 ? pl.slice[static_cast<size_t>(pl.cur)].seq : 0u;
    int next = -1;
    int first = -1;
    for (int j = 0; j < kKept; ++j) {
      const Slice& s = pl.slice[static_cast<size_t>(j)];
      if (!s.valid) continue;
      if (first < 0 || s.seq < pl.slice[static_cast<size_t>(first)].seq) first = j;
      if (s.seq > after && (next < 0 || s.seq < pl.slice[static_cast<size_t>(next)].seq)) next = j;
    }
    return next >= 0 ? next : first;
  }

  // Life stays 1 through the word and decays over the tail. The wet gain rides toward its target,
  // or falls straight to 0 in a fade. Returns false when the player closed or reopened.
  bool stepGain(int k, const BloomTuning& t) {
    Player& pl = pl_[static_cast<size_t>(k)];
    if (pl.fadeStep > 0.0f) {
      pl.gain -= pl.fadeStep;
      if (pl.gain > 0.0f) return true;
      if (pl.phase == Phase::Word) {
        clearPlayback(pl);
      } else {
        closePlayer(k);
      }
      return false;
    }
    float target = 0.0f;
    if (pl.phase == Phase::Word) {
      target = t.tailOnly ? 0.0f : wl_;
    } else {
      pl.life *= pl.life > t.staticStart ? aTail_ : aStatic_;
      target = wl_ * pl.life;
      if (target < t.closeGain) {
        closePlayer(k);
        return false;
      }
    }
    if (target > pl.gain) {
      pl.gain += wl_ * invRampSmp_;
      pl.gain = pl.gain < target ? pl.gain : target;
    } else {
      pl.gain = target;
    }
    return true;
  }

  // The loop through the wet tone, scaled by the player's gain.
  float wetOf(int k, const BloomTuning& t) {
    Player& pl = pl_[static_cast<size_t>(k)];
    if (pl.cur < 0) return 0.0f;
    float y = readLoop(pl);
    const float bell = 4.0f * pl.life * (1.0f - pl.life);
    y *= t.driveWord + t.driveBell * bell;
    y = y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);
    y -= y * y * y * (1.0f / 3.0f);
    return y * pl.gain;
  }

  // The looping slice with its pass window, plus the previous slice fading out for one fade.
  float readLoop(Player& pl) {
    float s = data_[static_cast<size_t>(pl.curIdx)][static_cast<size_t>(pl.pos)] * passWindow(pl.pos, pl.curLen);
    if (pl.prevOn) {
      if (pl.pos >= fadeSmp_) {
        pl.prevOn = false;
      } else {
        const float down = 1.0f - smoothstep(static_cast<float>(pl.pos) * invFade_);
        s += data_[static_cast<size_t>(pl.prevIdx)][static_cast<size_t>(pl.prevPos)] * passWindow(pl.prevPos, pl.prevLen) * down;
        if (++pl.prevPos >= pl.prevLen) pl.prevPos = 0;
      }
    }
    if (++pl.pos >= pl.curLen) pl.pos = 0;
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

  std::array<std::array<float, kSliceMax>, kPlayers * kKept> data_{};
  std::array<Player, kPlayers> pl_{};
  Biquad shelf_;
  Biquad capLp_;  // the low-pass a slice passes through as it is cut
  bool shelfLive_ = false;

  // Word and key.
  int wordPlayer_ = -1;
  bool running_ = false;
  bool ownOpen_ = false;
  bool keyHeld_ = false;
  int keyHold_ = 0;
  float wl_ = 0.0f;
  float pairScale_ = 1.0f;
  float prevSrc_ = 0.0f;
  uint32_t wordSeq_ = 0;
  uint32_t commitSeq_ = 0;

  // Event log for tests and the wiring.
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
  int closeSmp_ = 3840;
  int yieldSmp_ = 960;
  uint32_t limitSmp_ = 120000;
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
  float tailMs_ = 0.0f, tauScale_ = 0.0f, tauMin_ = 0.0f, tauMax_ = 0.0f, staticMs_ = 0.0f;
  float wet_ = 0.0f, wetAmount_ = -1.0f, wetHiKey_ = 0.0f, wetExpKey_ = 0.0f;
  float trim_ = 1.0f;
  float trimWl_ = -1.0f;
  float trimDb_ = 0.0f;
  float trimHi_ = 0.0f;
};

}  // namespace cv
