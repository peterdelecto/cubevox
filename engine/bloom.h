#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/smooth.h"

// Chatter, the engine behind the Bloom card. While a word lasts, short slices of the voice loop
// under it. Each slice plays twice, then the next cut splices in at the slice boundary. When the word
// ends, its last good slice keeps repeating and fades over LENGTH, gritty in the middle and clean at
// the end (spec 2026-11-01-bloom-design, sections 14 to 17).
//
// A word is a key. The Input gate hands its detector state to process(), or the engine listens to its
// own input. Three slices cover a word, its capture and the old tail fading under the next word.
// Slices are cut from the `src` signal, through the low-pass, and the dry path is `in` at unity. Each
// voice's wet path runs drive, soft clip and its gain, then one high shelf and one level trim run over
// their sum, and the result adds to the dry.
//
// A chunk runs voice by voice. The cut schedule (arm, zero crossing, finish, splice) and two voice
// changes (the word's close fade, the end of a crossfade) act at single sample instants, so the chunk
// splits at those instants. Between two of them nothing changes the schedule, and each voice runs
// its stretch in one loop with its state in locals. The last voice's loop also carries the capture's
// low-pass and the pair gain, shelf and trim, because both filters are chains of dependent multiplies
// and run side by side in one loop at the cost of one. The sound is the sample-by-sample sound, bit
// for bit.

namespace cv {

// The values a bench or a listen may change. Everything else the engine fixes in constants.
struct BloomTuning {
  // Level of the wet path alone, the slices and the tail. The dry passes at unity at every AMOUNT, and
  // AMOUNT at the bottom of its travel adds no wet. At the default setting this puts the three sample
  // clips at +0.20 to +0.33 dB RMS over the input, with the Input gate key and with the own key.
  float trimDb = -14.5f;

  float firstCaptureMs = 40.0f;  // first slice, after the word starts
  float skipDb = -12.0f;         // a slice this far under the looping slice is skipped

  // The clipper's drive through the word, and how far the middle of the tail swells it.
  float driveWord = 2.8f;
  float driveBell = 2.0f;

  // Each cut's length drifts by up to this fraction either way, so the interval between cuts, two
  // plays of the slice, drifts with it.
  float drift = 0.03f;
};

struct BloomParams {
  bool on = true;
  float lengthMs = 300.0f;   // panel LENGTH, 20..3000
  float amount = 0.5f;       // panel AMOUNT, 0..1
  bool keyFromGate = false;  // false makes the engine listen to its own input
  BloomTuning tuning;
};

class Bloom {
 public:
  static constexpr int kSlices = 3;                          // the loop, the capture, the fading tail
  static constexpr int kSliceMax = static_cast<int>(80.0f * kSmpPerMs);  // 80 ms, 3,840 samples
  static constexpr int kVoices = 4;                          // word, tail, and two fading tails
  static constexpr int kWord = 0;                            // voice numbers
  static constexpr int kTail = 1;
  static constexpr int kFade = 2;  // first of the fading voices
  static constexpr float kLengthMinMs = 20.0f;
  static constexpr float kLengthMaxMs = 3000.0f;
  static constexpr float kLowpassHz = 11000.0f;  // the low-pass a slice passes through as it is cut
  static constexpr float kLowpassQ = 0.70f;

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
    wetAmount_ = -1.0f;
    prepared_ = false;
  }

  // Cuts its slices from `in`, as a Chatter card with nothing ahead of it in the chain does.
  void process(const float* in, float* out, int n, const BloomParams& p, bool keyOpen = false) {
    process(in, in, out, n, p, keyOpen);
  }

  // `in` is the dry path and the own key's input. `src` is the signal the slices are cut from, the
  // same block time-aligned with `in`. `keyOpen` is the Input gate's detector state for this call and
  // counts only when `p.keyFromGate` is set. Any n; `out` may be `in`.
  void process(const float* in, const float* src, float* out, int n, const BloomParams& p, bool keyOpen = false) {
    const float lengthMs = clampf(p.lengthMs, kLengthMinMs, kLengthMaxMs);
    prepare(p.tuning, lengthMs);
    const Law law = lawFor(p, lengthMs);
    for (int done = 0; done < n; done += kBlock) {
      const int m = n - done < kBlock ? n - done : kBlock;
      runChunk(in + done, src + done, out + done, m, p, law, keyOpen);
    }
  }

  // Sum of the voices' wet gains before the shelf and the trim.
  float wetGain() const {
    float sum = 0.0f;
    for (const Voice& v : v_) sum += v.gain;
    return sum;
  }

  // True while a word is open (the key plus its hold).
  bool inWord() const { return v_[kWord].on; }

  // The gain on the summed loops: 1 with one loop sounding, kPairDb with more.
  float pairScale() const { return pairScale_; }

  // True while any voice has a slice looping.
  bool looping() const {
    for (const Voice& v : v_)
      if (v.on && v.slice >= 0) return true;
    return false;
  }

  // The accessors below report the engine's state to bloom_test and core_chain_run. The firmware
  // does not call them.

  // Starts the sample clock at `clock` instead of 0, so a test can run the engine across the wrap
  // of the 32-bit clock. Call it right after reset().
  void startClockAt(uint32_t clock) { clock_ = clock; }

  // The clock at the latest capture arm and write start, in samples since reset().
  uint32_t armedAt() const { return armedAt_; }
  uint32_t writeStartedAt() const { return writtenAt_; }

  // Slices committed, and slices skipped for a gap or for sitting too far under the looping one,
  // since reset().
  int taken() const { return taken_; }
  int skipped() const { return skipped_; }

  // Per voice: whether it runs, its gain, the commit number, cut clock and length of the slice it
  // plays (0, 0 and 0 when none), and the clock of its latest splice.
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
  static constexpr uint32_t kSeed = 0x2545F491u;

  // Word timing, in ms.
  static constexpr float kCrossingWaitMs = 10.0f;  // longest wait for a zero crossing before a cut starts anyway
  static constexpr float kRampMs = 5.0f;           // wet gain rise to its level
  static constexpr float kFadeMs = 5.8f;           // pass fade in and out, and the splice crossfade
  static constexpr float kKeyHoldMs = 50.0f;       // both key sources hold a word open this long
  static constexpr int kPlaysPerSlice = 2;
  static constexpr int kEdgeShare = 8;  // a cut's edge is one eighth of it

  // AMOUNT laws. The wet level is kWetHi * AMOUNT^kWetExp, 0 at AMOUNT 0. The slice runs from
  // kSliceLoMs at AMOUNT 0 to kSliceHiMs at 100 %, LENGTH caps it at kSliceLengthFrac of its setting,
  // and no slice runs under kSliceMinMs.
  static constexpr float kWetHi = 0.70f;
  static constexpr float kWetExp = 0.6f;
  static constexpr float kSliceLoMs = 80.0f;
  static constexpr float kSliceHiMs = 45.0f;
  static constexpr float kSliceMinMs = 35.0f;
  static constexpr float kSliceLengthFrac = 0.8f;

  // While two loops sound together each plays kPairDb down, so two loops carry the power of one. The
  // change rides the 20 ms smoother.
  static constexpr float kPairDb = -3.0f;

  // Age and fades. During a word no slice plays longer than kMaxAgeMs after it was cut, and
  // kCloseMs ends a loop that has nothing newer to play. A tail has no age limit and runs LENGTH's
  // law out. kNextWordMs is the fade a new word puts on the tail it finds, and kQuickFadeMs the
  // faster fade a tail already fading takes when another word follows.
  static constexpr float kMaxAgeMs = 2500.0f;
  static constexpr float kCloseMs = 80.0f;
  static constexpr float kNextWordMs = 150.0f;
  static constexpr float kQuickFadeMs = 20.0f;

  // LENGTH laws. Life falls with a time constant of max(LENGTH, min(kTauScale * LENGTH, kTauMaxMs)).
  // At kEndFadeLife the end fade takes over with a time constant of kEndFadeMs, or LENGTH when that is
  // shorter, and under kCloseGain the wet gain writes 0 and the loop stops.
  static constexpr float kTauScale = 1.5f;
  static constexpr float kTauMaxMs = 300.0f;
  static constexpr float kEndFadeLife = 0.20f;
  static constexpr float kEndFadeMs = 80.0f;
  static constexpr float kCloseGain = 0.001f;

  // Wet tone: a high shelf over the summed loops.
  static constexpr float kShelfHz = 3000.0f;
  static constexpr float kShelfDb = 2.2f;
  static constexpr float kShelfSlope = 0.7f;

  // Word key from the engine's own input.
  static constexpr float kOwnOpenDb = -45.0f;
  static constexpr float kOwnHysteresisDb = 3.0f;

  // The times above in samples.
  static constexpr int kCrossingWaitSmp = static_cast<int>(kCrossingWaitMs * kSmpPerMs);
  static constexpr int kFadeSmp = static_cast<int>(kFadeMs * kSmpPerMs + 0.5f);
  static constexpr int kHoldSmp = static_cast<int>(kKeyHoldMs * kSmpPerMs);
  static constexpr int kCloseSmp = static_cast<int>(kCloseMs * kSmpPerMs);
  static constexpr int kNextWordSmp = static_cast<int>(kNextWordMs * kSmpPerMs);
  static constexpr int kQuickFadeSmp = static_cast<int>(kQuickFadeMs * kSmpPerMs);
  static constexpr uint32_t kLimitSmp = static_cast<uint32_t>(kMaxAgeMs * kSmpPerMs);
  static constexpr int kMinSliceSmp = 2 * kFadeSmp + 2;
  static constexpr float kInvFade = 1.0f / static_cast<float>(kFadeSmp);
  static constexpr float kInvRampSmp = 1.0f / (kRampMs * kSmpPerMs);

  static_assert(kFadeSmp == 278 && kCrossingWaitSmp == 480 && kHoldSmp == 2400 && kCloseSmp == 3840, "sample counts");
  static_assert(kNextWordSmp == 7200 && kQuickFadeSmp == 960 && kLimitSmp == 120000 && kMinSliceSmp == 558, "sample counts");

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

  // Everything AMOUNT, LENGTH and the toggle set for one call: the wet level the call heads for (0 when
  // the card is off), the slice length AMOUNT asks for in ms, the longest slice LENGTH allows, and the
  // fraction the cut length may drift.
  struct Law {
    float target;
    float sliceMs;
    float capMs;
    float drift;
  };

  // One chunk's signals and scratch. `wet` holds the sum of the voices' wet per sample. `wlp` points at
  // the wet level for sample 0 and steps by `wlStep` a sample (1 while the level moves, 0 while it
  // holds). `prev0` is the `src` sample before the chunk and `clock0` the clock at its first sample.
  struct Run {
    const float* in;
    const float* src;
    float* out;
    float* wet;
    const float* wlp;
    int wlStep;
    float prev0;
    uint32_t clock0;
    const Law* law;
    const BloomTuning* tune;
  };

  // Where a voice stores its wet: `base + y * gain` goes to `dst`, and `base` steps by `step` a sample.
  // `later` marks a voice that adds to the sum of the voices before it.
  struct Sink {
    float* dst;
    const float* base;
    int step;
    bool later;
  };

  // The ways a voice's gain and read move. Steady is the word riding toward the wet level, Decay a
  // tail whose life falls, and Fade a gain that falls by a fixed step. Cross is Steady while the
  // previous slice fades out under a splice, which lasts kFadeSmp samples from the splice.
  enum class Kind : uint8_t { Steady, Decay, Fade, Cross };

  // A voice's loop state, kept in locals while its loop runs. `pd`, `plen` and `ppos` are the previous
  // slice of a Cross voice.
  struct Lane {
    const float* d;
    int len;
    int pos;
    float gain;
    float life;
    float step;
    float drive;
    float driveWord;
    float driveBell;
    float aTail;
    float aEnd;
    const float* pd;
    int plen;
    int ppos;
  };

  // The cut under way, kept in locals while the capture runs. The window covers the first kFadeSmp
  // samples and those from `winEnd` on, the peak rule the first `headEnd` and those from `tailStart`.
  struct CapLane {
    Biquad lp;
    float* dst;
    int pos;
    int len;
    int headEnd;
    int winEnd;
    int tailStart;
    float peak;
    float head;
    float tail;
  };

  static constexpr float kZero = 0.0f;
  static constexpr float kOne = 1.0f;

  static float lerp(float a, float b, float t) { return a + (b - a) * t; }
  static float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

  Slice playing(int k) const {
    const Voice& v = v_[static_cast<size_t>(k)];
    return v.on && v.slice >= 0 ? slice_[static_cast<size_t>(v.slice)] : Slice{};
  }

  Law lawFor(const BloomParams& p, float lengthMs) {
    const float amount = smooth::clamp01(p.amount);
    if (amount != wetAmount_) {
      wet_ = amount > 0.0f ? kWetHi * exp2Fast(kWetExp * log2Fast(amount > 1e-12f ? amount : 1e-12f)) : 0.0f;
      wetAmount_ = amount;
    }
    return {p.on ? wet_ : 0.0f, lerp(kSliceLoMs, kSliceHiMs, amount), kSliceLengthFrac * lengthMs, p.tuning.drift};
  }

  // The length of the next cut in samples. AMOUNT's slice drifts by up to the law's drift either way,
  // from the fixed-seed generator, so renders repeat. LENGTH's cap and the floor apply after the drift.
  int nextSlice(const Law& law) {
    lcg_ = lcg_ * 1664525u + 1013904223u;
    const float r = static_cast<float>(static_cast<int32_t>(lcg_)) * (1.0f / 2147483648.0f);
    float ms = law.sliceMs * (1.0f + law.drift * r);
    ms = ms < law.capMs ? ms : law.capMs;
    ms = ms > kSliceMinMs ? ms : kSliceMinMs;
    const int smp = static_cast<int>(ms * kSmpPerMs);
    return smp < kMinSliceSmp ? kMinSliceSmp : (smp > kSliceMax ? kSliceMax : smp);
  }

  // Coefficients that follow the tuning and LENGTH. The constants' own are set once after a reset; the
  // ones that need libm run when their inputs change.
  void prepare(const BloomTuning& t, float lengthMs) {
    if (!prepared_) {
      openLin_ = dbToGainFast(kOwnOpenDb);
      closeLin_ = dbToGainFast(kOwnOpenDb - kOwnHysteresisDb);
      pairLin_ = dbToGainFast(kPairDb);
      capLp_.setLowPass(kLowpassHz, kLowpassQ);
      shelf_.setHighShelf(kShelfHz, kShelfDb, kShelfSlope);
      trimDb_ = -1000.0f;
      skipDb_ = -1000.0f;
      lengthMs_ = 0.0f;
    }
    firstSmp_ = static_cast<int>(t.firstCaptureMs * kSmpPerMs);
    if (t.skipDb != skipDb_) {
      skipDb_ = t.skipDb;
      skipLin_ = dbToGainFast(t.skipDb);
    }
    if (t.trimDb != trimDb_) {
      trimDb_ = t.trimDb;
      trim_ = dbToGainFast(t.trimDb);
    }
    if (lengthMs != lengthMs_) {
      const float scaled = kTauScale * lengthMs < kTauMaxMs ? kTauScale * lengthMs : kTauMaxMs;
      const float tauMs = lengthMs > scaled ? lengthMs : scaled;
      const float endMs = kEndFadeMs < lengthMs ? kEndFadeMs : lengthMs;
      aTail_ = exp2Fast(-kLog2E / (tauMs * kSmpPerMs));
      aEnd_ = exp2Fast(-kLog2E / (endMs * kSmpPerMs));
      lengthMs_ = lengthMs;
    }
    prepared_ = true;
  }

  void runChunk(const float* in, const float* src, float* out, int m, const BloomParams& p, const Law& law, bool keyOpen) {
    if (law.target == 0.0f && wl_ == 0.0f) {
      for (int i = 0; i < m; ++i) out[i] = in[i];
      if (running_) closeAll();
      prevSrc_ = src[m - 1];
      clock_ += static_cast<uint32_t>(m);
      return;
    }
    running_ = true;
    stepKey(in, m, p.keyFromGate, keyOpen);
    if (keyHeld_ && !v_[kWord].on) startWord();
    else if (!keyHeld_ && v_[kWord].on) endWord();

    // With nothing sounding and the levels settled, the call is a copy of the input.
    bool anyOn = false;
    for (const Voice& v : v_) anyOn = anyOn || v.on;
    if (!anyOn && wl_ == law.target && pairScale_ == 1.0f) {
      if (shelfLive_) {
        shelf_.clear();
        shelfLive_ = false;
      }
      for (int i = 0; i < m; ++i) out[i] = in[i];
      prevSrc_ = src[m - 1];
      clock_ += static_cast<uint32_t>(m);
      return;
    }
    float wetBuf[kBlock];
    float wlBuf[kBlock];
    float wlHeld = wl_;
    Run r{in, src, out, wetBuf, &wlHeld, 0, prevSrc_, clock_, &law, &p.tuning};
    if (wl_ != law.target) {
      stepWetLevel(law.target, m, wlBuf);
      r.wlp = wlBuf;
      r.wlStep = 1;
    }
    runSegments(r, m);
    clock_ = r.clock0 + static_cast<uint32_t>(m);
  }

  // The wet level for each of the chunk's m samples while it rides to the target on the smoother.
  void stepWetLevel(float target, int m, float* level) {
    for (int i = 0; i < m; ++i) {
      if (wl_ != target) wl_ = smooth::step(wl_, target, smooth::kSmoothCoef);
      level[i] = wl_;
    }
  }

  // Walks the chunk from one instant to the next. At an instant the capture and splice steps run on
  // that one sample, then the voice changes. The stretch from there to the next instant holds the
  // capture's writes and the voices, which nothing in the schedule can change.
  void runSegments(const Run& r, int m) {
    const bool word = v_[kWord].on;
    int i = 0;
    int next = nextEvent(r, 0, m, word);
    while (i < m) {
      const int from = i;
      if (next == i) {
        clock_ = r.clock0 + static_cast<uint32_t>(i);
        if (word) {
          stepCapture(r.src[i], *r.law);
          stepSplice(*r.law);
        }
        voiceEvents(clock_);
        next = nextEvent(r, ++i, m, word);
      }
      const float last = r.src[next - 1];
      if (from < i) runVoices(r, from, i, false);
      if (i < next) runVoices(r, i, next, cap_ == Cap::Writing);
      prevSrc_ = last;
      i = next;
    }
  }

  // The first sample at or after t0 where something changes besides the voices' plain playing, or m.
  int nextEvent(const Run& r, int t0, int m, bool word) const {
    const int room = m - t0;
    const uint32_t clk = r.clock0 + static_cast<uint32_t>(t0);
    int t = word ? nextScheduled(r, t0, m, clk, room) : m;
    const Voice& w = v_[kWord];
    if (w.on && w.slice >= 0 && w.fadeStep == 0.0f) t = earlier(t, t0 + dueIn(clk, w.staleAt, room));
    for (const Voice& v : v_)
      if (v.on && v.prevOn) t = earlier(t, t0 + (v.pos < kFadeSmp ? kFadeSmp - v.pos : 0));
    return t;
  }

  // The first sample at or after t0 where the capture or the splice does more than write the cut, or m.
  // Waiting for a zero crossing scans the signal for the crossing.
  int nextScheduled(const Run& r, int t0, int m, uint32_t clk, int room) const {
    int t = m;
    if (spliceSet_) t = t0 + dueIn(clk, spliceAt_, room);
    if (cap_ == Cap::Idle) {
      if (armSet_) t = earlier(t, t0 + dueIn(clk, armAt_, room));
    } else if (cap_ == Cap::Writing) {
      t = earlier(t, t0 + capLen_ - 1 - capPos_);
    } else {
      int stop = earlier(t, t0 + dueIn(clk, waitUntil_, room));
      float prev = t0 > 0 ? r.src[t0 - 1] : r.prev0;
      for (int k = t0; k < stop; ++k) {
        const float s = r.src[k];
        if ((prev < 0.0f) != (s < 0.0f)) stop = k;
        prev = s;
      }
      t = stop;
    }
    return t;
  }

  static int earlier(int a, int b) { return b < a ? b : a; }

  // Samples from `clk` until the clock reaches `at`, 0 when it has, and `limit` when that is further.
  static int dueIn(uint32_t clk, uint32_t at, int limit) {
    const int32_t d = static_cast<int32_t>(at - clk);
    if (d > 0) return d < limit ? d : limit;
    return d == INT32_MIN ? limit : 0;
  }

  // The word's slice that has used its age starts the close fade, and a previous slice whose crossfade
  // has run its length drops out. Both fall on one sample, after the schedule's steps.
  void voiceEvents(uint32_t clk) {
    Voice& w = v_[kWord];
    if (w.on && w.slice >= 0 && w.fadeStep == 0.0f && dueAt(clk, w.staleAt)) beginFade(w, kCloseSmp);
    for (Voice& v : v_)
      if (v.on && v.prevOn && v.pos >= kFadeSmp) v.prevOn = false;
  }

  // The key for this chunk: the gate's state or the engine's own detector, then the hold. The own
  // detector compares the input's magnitude bits as integers, so a NaN sample reads as the largest
  // value and leaves the detector where it was.
  void stepKey(const float* in, int m, bool keyFromGate, bool keyOpen) {
    bool key = keyOpen;
    if (!keyFromGate) {
      uint32_t peakBits = 0;
      for (int i = 0; i < m; ++i) {
        uint32_t u;
        std::memcpy(&u, &in[i], sizeof u);
        u &= 0x7FFFFFFFu;
        peakBits = u > peakBits ? u : peakBits;
      }
      float peak;
      std::memcpy(&peak, &peakBits, sizeof peak);
      if (peak >= openLin_) ownOpen_ = true;
      else if (peak < closeLin_) ownOpen_ = false;
      key = ownOpen_;
    }
    if (key) {
      keyHold_ = kHoldSmp;
      keyHeld_ = true;
    } else {
      keyHold_ = keyHold_ > m ? keyHold_ - m : 0;
      keyHeld_ = keyHold_ > 0;
    }
  }

  // A tail still sounding stays where it is, so a blip of key that never gives a slice leaves it
  // alone. The word counts down to its first capture.
  void startWord() {
    v_[kWord] = Voice{};
    v_[kWord].on = true;
    cap_ = Cap::Idle;
    fresh_ = -1;
    spliceSet_ = false;
    armSet_ = true;
    armAt_ = clock_ + static_cast<uint32_t>(firstSmp_);
    rearmed_ = false;
  }

  // A capture still open at the word's end is dropped. A finished cut that waits for its splice plays
  // now, so the tail repeats the newest slice. A word that never played a slice has no tail, and the
  // tail it found keeps sounding.
  void endWord() {
    Voice& w = v_[kWord];
    cap_ = Cap::Idle;
    armSet_ = false;
    spliceSet_ = false;
    if (fresh_ >= 0 && (w.slice >= 0 || fadeOldTail())) play(w, fresh_);
    fresh_ = -1;
    if (w.slice >= 0) v_[kTail] = w;
    w = Voice{};
  }

  // The tail the word found moves to a fading voice and falls to 0 over the next-word fade, as the
  // word plays its first slice. A fading tail takes the quick fade. When every fading voice sounds, the
  // quietest one takes the quick fade and the call returns false, so the slice waits and the sounding
  // voices are never overwritten.
  bool fadeOldTail() {
    if (!v_[kTail].on) return true;
    int dst = -1;
    for (int k = kFade; k < kVoices && dst < 0; ++k)
      if (!v_[k].on) dst = k;
    if (dst < 0) {
      int quietest = kFade;
      for (int k = kFade + 1; k < kVoices; ++k)
        if (v_[k].gain < v_[quietest].gain) quietest = k;
      beginFade(v_[quietest], kQuickFadeSmp);
      return false;
    }
    for (int k = kFade; k < kVoices; ++k)
      if (v_[k].on) beginFade(v_[k], kQuickFadeSmp);
    v_[dst] = v_[kTail];
    beginFade(v_[dst], kNextWordSmp);
    v_[kTail] = Voice{};
    return true;
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

  // Arms a cut on the plan, waits for a zero crossing, and writes a free slice.
  CV_INLINE void stepCapture(float s, const Law& law) {
    if (armSet_ && cap_ == Cap::Idle && due(armAt_)) arm(law);
    if (cap_ == Cap::Waiting && (((prevSrc_ < 0.0f) != (s < 0.0f)) || due(waitUntil_))) beginCut();
    if (cap_ == Cap::Writing) {
      writeCapture(&s, 1);
      if (capPos_ >= capLen_) finishCapture();
    }
  }

  // Writes n samples of the cut under way, never past its last.
  void writeCapture(const float* s, int n) {
    CapLane c = capLane();
    for (int i = 0; i < n; ++i) capStep(c, s[i]);
    saveCap(c);
  }

  CapLane capLane() {
    return {capLp_,
            data_[static_cast<size_t>(capSlot_)].data(),
            capPos_,
            capLen_,
            capEdge_,
            capLen_ - kFadeSmp,
            capLen_ - capEdge_,
            capPeak_,
            capHead_,
            capTail_};
  }

  void saveCap(const CapLane& c) {
    capLp_ = c.lp;
    capPos_ = c.pos;
    capPeak_ = c.peak;
    capHead_ = c.head;
    capTail_ = c.tail;
  }

  // One sample of the cut. The slice takes its pass window as it is written, so playback reads it as
  // it stands. The peaks of the cut's first and last eighths tell finishCapture whether the voice
  // starts late or stops early.
  static CV_INLINE void capStep(CapLane& c, float s) {
    float v = c.lp.run(s);
    if (c.pos < kFadeSmp || c.pos >= c.winEnd) v *= passWindow(c.pos, c.len);
    const float a = fabsf(s);
    c.peak = a > c.peak ? a : c.peak;
    if (c.pos < c.headEnd) c.head = a > c.head ? a : c.head;
    else if (c.pos >= c.tailStart) c.tail = a > c.tail ? a : c.tail;
    c.dst[c.pos++] = v;
  }

  // Starts writing the armed cut into a free slice, if there is one.
  void beginCut() {
    const int slot = freeSlice();
    if (slot < 0) return;
    cap_ = Cap::Writing;
    capPos_ = 0;
    capPeak_ = 0.0f;
    capCut_ = clock_;
    capSlot_ = slot;
    capEdge_ = capLen_ / kEdgeShare;
    capHead_ = 0.0f;
    capTail_ = 0.0f;
    slice_[static_cast<size_t>(slot)].valid = false;
    capLp_.clear();
    writtenAt_ = clock_;
  }

  // The word's first cut is armed at the first-capture time and splices after the longest wait and
  // its length. Every later cut takes the length its plan fixed.
  void arm(const Law& law) {
    cap_ = Cap::Waiting;
    armSet_ = false;
    waitUntil_ = clock_ + static_cast<uint32_t>(kCrossingWaitSmp);
    armedAt_ = clock_;
    if (spliceSet_) {
      capLen_ = planLen_;
    } else {
      capLen_ = nextSlice(law);
      spliceAt_ = clock_ + static_cast<uint32_t>(kCrossingWaitSmp + capLen_);
      spliceSet_ = true;
    }
  }

  // The next splice falls `interval` after `base`, two plays of the slice that loops. The cut it will
  // play takes the next drifting length and is armed late enough to be the newest sound before the
  // splice, so it is ready by then at the latest.
  void planNext(uint32_t base, int interval, const Law& law) {
    spliceAt_ = base + static_cast<uint32_t>(interval);
    planLen_ = nextSlice(law);
    armAt_ = spliceAt_ - static_cast<uint32_t>(kCrossingWaitSmp + planLen_);
    if (static_cast<int32_t>(armAt_ - clock_) < 0) armAt_ = clock_;
    armSet_ = true;
  }

  // A free slice, which is not looping, not fading out under a splice and not a finished cut waiting
  // to play. An empty one goes first, else the oldest cut.
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
  static bool dueAt(uint32_t clk, uint32_t at) { return static_cast<int32_t>(clk - at) >= 0; }
  bool due(uint32_t at) const { return dueAt(clock_, at); }

  // Whether the slice that loops may play on until `ahead` samples from now plus one more interval
  // and still close within the age limit.
  bool mayPlayOn(const Voice& v, uint32_t ahead, int interval) const {
    if (v.slice < 0) return true;
    const uint32_t age = clock_ + ahead - slice_[static_cast<size_t>(v.slice)].cutAt;
    return age + static_cast<uint32_t>(interval + kCloseSmp) <= kLimitSmp;
  }

  // The new slice waits for its splice. A cut has a gap when no sample of its first or last eighth
  // comes within skipDb of its peak, because the voice starts late or stops early. A gap, or a peak
  // more than skipDb under the looping slice's, makes the cut quiet, and a quiet cut is skipped
  // unless the looping slice cannot play another interval, which it replaces even when quieter. The
  // word's first cut has no loop to keep, so a gap makes it cut again, once.
  void finishCapture() {
    cap_ = Cap::Idle;
    const Voice& w = v_[kWord];
    const float edgeFloor = capPeak_ * skipLin_;
    const bool gap = capHead_ < edgeFloor || capTail_ < edgeFloor;
    if (gap && w.slice < 0 && !rearmed_) {
      rearmed_ = true;
      ++skipped_;
      spliceSet_ = false;
      armSet_ = true;
      armAt_ = clock_;
      return;
    }
    const bool quiet = w.slice >= 0 && (gap || (w.refPeak > 0.0f && capPeak_ < w.refPeak * skipLin_));
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
  // the loop plays one more pass; with it skipped the loop keeps its slice and the grid moves on two
  // plays. The word's first slice waits, a sample at a time, until the tail it found has a fading voice.
  CV_INLINE void stepSplice(const Law& law) {
    if (!spliceSet_ || !due(spliceAt_)) return;
    Voice& w = v_[kWord];
    if (fresh_ >= 0) {
      if (w.slice < 0 && !fadeOldTail()) return;
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
    v.staleAt = s.cutAt + kLimitSmp - static_cast<uint32_t>(kCloseSmp);
    v.fadeStep = 0.0f;
  }

  // Runs the voices over samples [a, b) of the chunk, with the pair gain, the shelf and the trim, and
  // writes the capture's samples when `capture` is set. The range holds no instant, so the voices
  // keep their kind. The voices add their wet in index order, which fixes how the sum rounds.
  void runVoices(const Run& r, int a, int b, bool capture) {
    int sounding[kVoices];
    int n = 0;
    bool crossfade = false;
    for (int k = 0; k < kVoices; ++k) {
      const Voice& v = v_[static_cast<size_t>(k)];
      if (!v.on) continue;
      if (v.slice < 0) {
        if (k == kWord) rampWord(r, a, b);
        continue;
      }
      sounding[n++] = k;
      crossfade = crossfade || slowCrossfade(k);
    }
    if (n > 0 && !crossfade) {
      runFused(r, a, b, capture, sounding, n);
      return;
    }
    if (capture) writeCapture(r.src + a, b - a);
    int end[kVoices] = {a, a, a, a};
    for (int i = 0; i < n; ++i) storeSounding(r, a, b, sounding[i], i, end);
    mixOut(r, a, b, end);
  }

  // The voices but the last store their wet, and the last also runs the capture and the mix.
  void runFused(const Run& r, int a, int b, bool capture, const int (&sounding)[kVoices], int n) {
    int end[kVoices] = {a, a, a, a};
    for (int i = 0; i + 1 < n; ++i) storeSounding(r, a, b, sounding[i], i, end);
    const int last = sounding[n - 1];
    const Sink s{nullptr, n > 1 ? r.wet : &kZero, n > 1 ? 1 : 0, n > 1};
    end[last] = fusedVoice(last, r, s, a, b, capture, end);
  }

  // Runs the i-th sounding voice into the sum. The first one writes the sum and zeroes the rest of
  // the range. A later one adds to it.
  void storeSounding(const Run& r, int a, int b, int k, int i, int (&end)[kVoices]) {
    const Sink s = i == 0 ? Sink{r.wet, &kZero, 0, false} : Sink{r.wet, r.wet, 1, true};
    end[k] = storeVoice(k, r, s, a, b);
    if (i == 0) {
      for (int j = end[k]; j < b; ++j) r.wet[j] = 0.0f;
    }
  }

  // Which way voice k's gain and read move over the range. A previous slice still fading out under any
  // other voice than a steady word takes the sample-by-sample path.
  Kind kindOf(int k) const {
    const Voice& v = v_[static_cast<size_t>(k)];
    if (v.fadeStep > 0.0f) return Kind::Fade;
    if (k != kWord) return Kind::Decay;
    return v.prevOn && v.pos < kFadeSmp ? Kind::Cross : Kind::Steady;
  }

  // Whether voice k has a previous slice fading out that no loop kind covers.
  bool slowCrossfade(int k) const {
    const Voice& v = v_[static_cast<size_t>(k)];
    return v.prevOn && (v.fadeStep > 0.0f || k != kWord || v.pos >= kFadeSmp);
  }

  Lane laneOf(const Voice& v, const BloomTuning& t) const {
    return {data_[static_cast<size_t>(v.slice)].data(), v.len, v.pos, v.gain, v.life, v.fadeStep, driveOf(v.life, t), t.driveWord, t.driveBell, aTail_, aEnd_,
            v.prevOn ? data_[static_cast<size_t>(v.prev)].data() : nullptr, v.prevLen, v.prevPos};
  }

  // Keeps a voice's loop state after its loop ran to the end of the range, or closes the voice if the
  // loop stopped before it. A word that closes stays on, without a slice.
  void finishVoice(int k, const Lane& l, int stop, int b) {
    Voice& v = v_[static_cast<size_t>(k)];
    if (stop < b) {
      if (k == kWord) clearPlayback(v);
      else v = Voice{};
      return;
    }
    v.gain = l.gain;
    v.life = l.life;
    v.pos = l.pos;
    v.prevPos = l.ppos;
  }

  // Runs voice k over [a, b) into the sink. Returns the first sample where it no longer sounds. A word
  // that closes goes on riding its gain toward the wet level for the rest of the range.
  int storeVoice(int k, const Run& r, const Sink& s, int a, int b) {
    bool closed = false;
    int j = slowCrossfade(k) ? playCrossfade(k, r, s, a, b, closed) : a;
    if (!closed && j < b) {
      Lane l = laneOf(v_[static_cast<size_t>(k)], *r.tune);
      switch (kindOf(k)) {
        case Kind::Steady: j = storeLoop<Kind::Steady>(l, r, s, j, b); break;
        case Kind::Decay: j = storeLoop<Kind::Decay>(l, r, s, j, b); break;
        case Kind::Fade: j = storeLoop<Kind::Fade>(l, r, s, j, b); break;
        case Kind::Cross: j = storeLoop<Kind::Cross>(l, r, s, j, b); break;
      }
      finishVoice(k, l, j, b);
    }
    if (j < b && k == kWord) rampWord(r, j + 1, b);
    return j;
  }

  // The samples that mix the previous slice in run one at a time. Returns where the voice closed, or
  // b when the range ended first.
  int playCrossfade(int k, const Run& r, const Sink& s, int j, int b, bool& closed) {
    Voice& v = v_[static_cast<size_t>(k)];
    for (; j < b && v.prevOn; ++j) {
      if (!stepGain(k, r.wlp[j * r.wlStep], r.clock0 + static_cast<uint32_t>(j))) {
        closed = true;
        return j;
      }
      const float y = shape(readLoop(v), driveOf(v.life, *r.tune));
      s.dst[j] = s.base[j * s.step] + product(y, v.gain, s.later);
    }
    return j;
  }

  // The word with no slice: only its gain moves.
  void rampWord(const Run& r, int j0, int j1) {
    Voice& v = v_[kWord];
    float gain = v.gain;
    for (int j = j0; j < j1; ++j) {
      const float wl = r.wlp[j * r.wlStep];
      gain = rampTo(gain, wl, wl);
    }
    v.gain = gain;
  }

  // One sample of a voice: its gain, then its read through drive and clip. False when the voice closes.
  template <Kind K>
  static CV_INLINE bool advance(Lane& l, float wl, float& y, float& g) {
    if constexpr (K == Kind::Fade) {
      l.gain -= l.step;
      if (!(l.gain > 0.0f)) return false;
    } else if constexpr (K == Kind::Decay) {
      l.life *= l.life > kEndFadeLife ? l.aTail : l.aEnd;
      const float target = wl * l.life;
      if (target < kCloseGain) return false;
      l.gain = rampTo(l.gain, target, wl);
    } else {
      l.gain = rampTo(l.gain, wl, wl);
    }
    float s = l.d[l.pos];
    if constexpr (K == Kind::Cross) {
      const float down = 1.0f - smoothstep(static_cast<float>(l.pos) * kInvFade);
      s += l.pd[l.ppos] * down;
      if (++l.ppos >= l.plen) l.ppos = 0;
    }
    if (++l.pos >= l.len) l.pos = 0;
    if constexpr (K == Kind::Decay) {
      const float bell = 4.0f * l.life * (1.0f - l.life);
      y = shape(s, l.driveWord + l.driveBell * bell);
    } else {
      y = shape(s, l.drive);
    }
    g = l.gain;
    return true;
  }

  // A voice's loop over [j0, j1), storing `base + y * gain` for each sample until the voice closes.
  // Returns the sample where it closed, or j1.
  template <Kind K>
  int storeLoop(Lane& lane, const Run& r, const Sink& s, int j0, int j1) {
    Lane l = lane;
    float* dst = s.dst;
    const float* base = s.base + j0 * s.step;
    const float* wp = r.wlp + j0 * r.wlStep;
    int j = j0;
    for (; j < j1; ++j, base += s.step, wp += r.wlStep) {
      float y;
      float g;
      if (!advance<K>(l, *wp, y, g)) break;
      dst[j] = *base + product(y, g, s.later);
    }
    lane = l;
    return j;
  }

  // The last sounding voice over [a, b): its loop, the capture's low-pass, the pair gain, the shelf and
  // the trim, in pieces where the number of looping voices stays the same. The other voices' sum is
  // in `wet`. Returns the sample where the voice closed, or b.
  int fusedVoice(int k, const Run& r, const Sink& s, int a, int b, bool capture, int (&end)[kVoices]) {
    Lane l = laneOf(v_[static_cast<size_t>(k)], *r.tune);
    const Kind kind = kindOf(k);
    int cut[kVoices + 1];
    const int nc = cutsOf(end, k, a, b, cut);
    int j = a;
    for (int c = 0; c < nc; ++c) {
      if (cut[c] <= j) continue;
      int loops = 1;
      for (int i = 0; i < kVoices; ++i) loops += i != k && end[i] > j ? 1 : 0;
      const int stop = fusedRun(kind, capture, l, r, s, j, cut[c], loops > 1 ? pairLin_ : 1.0f);
      j = stop;
      if (stop < cut[c]) break;
    }
    finishVoice(k, l, j, b);
    end[k] = j;
    if (j < b) {
      if (capture) writeCapture(r.src + j, b - j);
      mixOut(r, j, b, end);
      if (k == kWord) rampWord(r, j + 1, b);
    }
    return j;
  }

  // The capture's pass window factors for a run of samples: a pointer and its step a sample. A run clear
  // of both window edges steps over a single 1.
  struct Win {
    const float* p;
    int step;
  };

  // Runs the voice's fused loop over [j0, j1), with the capture's window factors and edge peaks taken
  // up front, off the loop's chains and before the loop writes `out`, which may be `src`. A voice that
  // closes early leaves the rest of the range to writeCapture, which folds the same samples at the same
  // positions again. Returns the sample where the voice closed, or j1.
  int fusedRun(Kind kind, bool capture, Lane& l, const Run& r, const Sink& s, int j0, int j1, float pairTarget) {
    if (!capture) return fusedKind<false>(kind, l, r, s, j0, j1, pairTarget, Win{&kOne, 0});
    float winBuf[kBlock];
    Win w{&kOne, 0};
    if (inWindow(capPos_, j1 - j0)) w = fillWindow(winBuf, capPos_, capLen_, j1 - j0);
    capEdges(capPos_, r.src + j0, j1 - j0);
    return fusedKind<true>(kind, l, r, s, j0, j1, pairTarget, w);
  }

  template <bool kCap>
  int fusedKind(Kind kind, Lane& l, const Run& r, const Sink& s, int j0, int j1, float pairTarget, Win w) {
    switch (kind) {
      case Kind::Steady: return fusedLoop<Kind::Steady, kCap>(l, r, s, j0, j1, pairTarget, w);
      case Kind::Decay: return fusedLoop<Kind::Decay, kCap>(l, r, s, j0, j1, pairTarget, w);
      case Kind::Fade: return fusedLoop<Kind::Fade, kCap>(l, r, s, j0, j1, pairTarget, w);
      case Kind::Cross: return fusedLoop<Kind::Cross, kCap>(l, r, s, j0, j1, pairTarget, w);
    }
    return j1;
  }

  bool inWindow(int pos, int n) const { return pos < kFadeSmp || pos + n > capLen_ - kFadeSmp; }

  static Win fillWindow(float* buf, int pos, int len, int n) {
    for (int i = 0; i < n; ++i) buf[i] = passWindow(pos + i, len);
    return {buf, 1};
  }

  // The loop of the last voice with the capture and the mix. The capture's low-pass, the voice and the
  // shelf are independent chains, so one loop runs them side by side.
  template <Kind K, bool kCap>
  int fusedLoop(Lane& lane, const Run& r, const Sink& s, int j0, int j1, float pairTarget, Win win) {
    Lane l = lane;
    Biquad shelf = shelf_;
    float pair = pairScale_;
    const float trim = trim_;
    const float coef = smooth::kSmoothCoef;
    CapLane c{};
    if (kCap) c = capLane();
    const float* base = s.base + j0 * s.step;
    const float* wp = r.wlp + j0 * r.wlStep;
    const float* np = win.p;
    int j = j0;
    for (; j < j1; ++j, base += s.step, wp += r.wlStep, np += win.step) {
      float y;
      float g;
      if (!advance<K>(l, *wp, y, g)) break;
      if (kCap) {
        const float a = fabsf(r.src[j]);
        c.peak = a > c.peak ? a : c.peak;
        c.dst[c.pos++] = c.lp.run(r.src[j]) * *np;
      }
      float wet = *base + product(y, g, s.later);
      if (pair != pairTarget) pair = smooth::step(pair, pairTarget, coef);
      wet *= pair;
      wet = shelf.run(wet);
      r.out[j] = r.in[j] + wet * trim;
    }
    lane = l;
    shelf_ = shelf;
    pairScale_ = pair;
    if (j > j0) shelfLive_ = true;
    if (kCap) saveCap(c);
    return j;
  }

  // Folds n source samples that start at cut position `pos` into the peaks of the cut's first and last
  // eighths.
  void capEdges(int pos, const float* s, int n) {
    int h = capEdge_ - pos;
    h = h < 0 ? 0 : (h < n ? h : n);
    int t = capLen_ - capEdge_ - pos;
    t = t < h ? h : (t < n ? t : n);
    capHead_ = peakOver(capHead_, s, h);
    capTail_ = peakOver(capTail_, s + t, n - t);
  }

  // The larger of `m` and the magnitudes of n samples, on four chains so the compares overlap.
  static float peakOver(float m, const float* s, int n) {
    float m1 = m;
    float m2 = m;
    float m3 = m;
    int i = 0;
    for (; i + 4 <= n; i += 4) {
      const float a0 = fabsf(s[i]);
      const float a1 = fabsf(s[i + 1]);
      const float a2 = fabsf(s[i + 2]);
      const float a3 = fabsf(s[i + 3]);
      m = a0 > m ? a0 : m;
      m1 = a1 > m1 ? a1 : m1;
      m2 = a2 > m2 ? a2 : m2;
      m3 = a3 > m3 ? a3 : m3;
    }
    for (; i < n; ++i) {
      const float a = fabsf(s[i]);
      m = a > m ? a : m;
    }
    m = m1 > m ? m1 : m;
    m2 = m3 > m2 ? m3 : m2;
    return m2 > m ? m2 : m;
  }

  // A voice's product for the sum. A voice that adds to the sum rounds the product before the add on
  // clang builds, as the per-sample engine compiled there, and fuses it into the add on gcc builds,
  // as that engine compiled there. The empty asm hides the product from clang's fusing pass. The first
  // voice adds to 0, so fusing it changes no bit.
  static CV_INLINE float product(float y, float g, bool later) {
    float w = y * g;
#if defined(__clang__)
    if (later) {
#if defined(__aarch64__)
      __asm__("" : "+w"(w));
#elif defined(__x86_64__)
      __asm__("" : "+x"(w));
#else
      volatile float held = w;
      w = held;
#endif
    }
#else
    (void)later;
#endif
    return w;
  }

  // The sorted samples inside (a, b) where another voice stops sounding, then b.
  static int cutsOf(const int (&end)[kVoices], int skip, int a, int b, int (&cut)[kVoices + 1]) {
    int n = 0;
    for (int i = 0; i < kVoices; ++i) {
      const int e = end[i];
      if (i == skip || e <= a || e >= b) continue;
      int at = n++;
      while (at > 0 && cut[at - 1] > e) {
        cut[at] = cut[at - 1];
        --at;
      }
      cut[at] = e;
    }
    cut[n++] = b;
    return n;
  }

  // The wet gain rides up by wl / kRampMs toward its target, and drops to it at once when above.
  static CV_INLINE float rampTo(float gain, float target, float wl) {
    if (target > gain) {
      gain = fmaf(wl, kInvRampSmp, gain);
      return gain < target ? gain : target;
    }
    return target;
  }

  // Overdrive scale for a voice of this life: the word's, with the bell the tail's middle adds.
  static CV_INLINE float driveOf(float life, const BloomTuning& t) {
    const float bell = 4.0f * life * (1.0f - life);
    return t.driveWord + t.driveBell * bell;
  }

  // Drive, soft clip, then the cubic.
  static CV_INLINE float shape(float y, float drive) {
    y *= drive;
    y = y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);
    y -= y * y * y * (1.0f / 3.0f);
    return y;
  }

  // Applies the pair gain, the shelf and the trim over [a, b) in pieces of constant loop count. The
  // sum of the voices passes the pair gain, and the shelf runs while a slice loops. Its state clears
  // when none does.
  void mixOut(const Run& r, int a, int b, const int (&end)[kVoices]) {
    int cut[kVoices + 1];
    const int n = cutsOf(end, -1, a, b, cut);
    int p = a;
    for (int c = 0; c < n; ++c) {
      if (cut[c] == p) continue;
      int loops = 0;
      for (const int e : end) loops += e > p ? 1 : 0;
      mixPiece(r, p, cut[c], loops);
      p = cut[c];
    }
  }

  void mixPiece(const Run& r, int p, int q, int loops) {
    const float pairTarget = loops > 1 ? pairLin_ : 1.0f;
    const float coef = smooth::kSmoothCoef;
    const float trim = trim_;
    float pair = pairScale_;
    if (loops > 0) {
      Biquad shelf = shelf_;
      for (int j = p; j < q; ++j) {
        if (pair != pairTarget) pair = smooth::step(pair, pairTarget, coef);
        float wet = r.wet[j];
        wet *= pair;
        wet = shelf.run(wet);
        r.out[j] = r.in[j] + wet * trim;
      }
      shelf_ = shelf;
      shelfLive_ = true;
    } else {
      if (shelfLive_) {
        shelf_.clear();
        shelfLive_ = false;
      }
      for (int j = p; j < q; ++j) {
        if (pair != pairTarget) pair = smooth::step(pair, pairTarget, coef);
        float wet = 0.0f;
        wet *= pair;
        r.out[j] = r.in[j] + wet * trim;
      }
    }
    pairScale_ = pair;
  }

  // Life stays 1 through the word and decays over the tail. The wet gain rides toward its target, or
  // falls straight to 0 in a fade. A word's slice that has used its age starts the close fade; a tail
  // has no age limit. Returns false when the voice closed or lost its slice. This is the one-sample
  // form of what the voice loops run, used while a previous slice crossfades out.
  CV_INLINE bool stepGain(int k, float wl, uint32_t clk) {
    Voice& v = v_[static_cast<size_t>(k)];
    if (k == kWord && v.slice >= 0 && v.fadeStep == 0.0f && dueAt(clk, v.staleAt)) beginFade(v, kCloseSmp);
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
    float target = wl;
    if (k != kWord) {
      v.life *= v.life > kEndFadeLife ? aTail_ : aEnd_;
      target = wl * v.life;
      if (target < kCloseGain) {
        v = Voice{};
        return false;
      }
    }
    v.gain = rampTo(v.gain, target, wl);
    return true;
  }

  // The looping slice, plus the previous slice fading out for one fade.
  CV_INLINE float readLoop(Voice& v) {
    float s = data_[static_cast<size_t>(v.slice)][static_cast<size_t>(v.pos)];
    if (v.prevOn) {
      if (v.pos >= kFadeSmp) {
        v.prevOn = false;
      } else {
        const float down = 1.0f - smoothstep(static_cast<float>(v.pos) * kInvFade);
        s += data_[static_cast<size_t>(v.prev)][static_cast<size_t>(v.prevPos)] * down;
        if (++v.prevPos >= v.prevLen) v.prevPos = 0;
      }
    }
    if (++v.pos >= v.len) v.pos = 0;
    return s;
  }

  // Fades in over the first kFadeSmp samples and out over the last.
  static CV_INLINE float passWindow(int pos, int len) {
    const int toEnd = len - 1 - pos;
    const int edge = pos < toEnd ? pos : toEnd;
    return edge >= kFadeSmp ? 1.0f : smoothstep(static_cast<float>(edge) * kInvFade);
  }

  std::array<Slice, kSlices> slice_{};
  std::array<Voice, kVoices> v_{};
  Biquad shelf_;
  Biquad capLp_;
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
  int capEdge_ = 0;  // samples in the cut's first and last eighths
  float capPeak_ = 0.0f;
  float capHead_ = 0.0f;  // peak of the first eighth
  float capTail_ = 0.0f;  // peak of the last eighth
  uint32_t capCut_ = 0;
  bool rearmed_ = false;  // the word's first cut has already been cut again

  // The generator for the cut drift, and the event log the accessors read.
  uint32_t lcg_ = kSeed;
  uint32_t clock_ = 0;
  uint32_t armedAt_ = 0;
  uint32_t writtenAt_ = 0;
  int taken_ = 0;
  int skipped_ = 0;

  // Derived from the tuning and LENGTH in prepare().
  bool prepared_ = false;
  int firstSmp_ = 1920;
  float openLin_ = 0.0f;
  float closeLin_ = 0.0f;
  float skipLin_ = 0.0f;
  float skipDb_ = -1000.0f;
  float pairLin_ = 0.7079f;
  float aTail_ = 0.0f;
  float aEnd_ = 0.0f;
  float lengthMs_ = 0.0f;
  float wet_ = 0.0f;
  float wetAmount_ = -1.0f;
  float trim_ = 1.0f;  // the wet trim as a gain
  float trimDb_ = -1000.0f;

  // Last, so every field above sits within a short load offset of the object's base.
  std::array<std::array<float, kSliceMax>, kSlices> data_{};
};

}  // namespace cv
