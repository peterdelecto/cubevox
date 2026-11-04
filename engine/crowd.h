#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "engine/common.h"
#include "engine/dsp.h"
#include "engine/fastmath.h"
#include "engine/formant_voice.h"
#include "engine/pitch.h"
#include "engine/pitch_fx.h"
#include "engine/psola.h"
#include "engine/smooth.h"

// The Crowd card adds up to eight copies of the voice, each a different person saying the same line
// a little after Adam. Each copy sits a few semitones off the lead, has its own throat (a grain
// playback rate) and its own tone shelf, and lags the lead by its own delay. The crowd as a whole
// has a fixed low cut. The copies never read a key. They read the pitch period and Autotune's
// correction from PitchFx and cut their grains from its input ring.
//
// The tuning lists four copies, the tuned copies 0 to 3. Copies 4 to 7 are the raised copies: copy
// v + 4 takes tuned copy v's throat, tone shelf and contour, sits kRaisedSt higher and lags
// kRaisedLagMs further back, with its own scatter and drift. Every copy carries the four-copy gain
// over sqrt 2, so eight copies hold the power of four.
//
// VOICES is a stepped knob with ten positions. Position k from 0 to 8 plays k copies and the lead.
// Position 9 (MOB) plays all eight copies with the lead gone. Copies come in one at a time in the
// order tuned 0, raised 4, tuned 1, raised 5, tuned 2, raised 6, tuned 3, raised 7, so each tuned
// copy is followed by its own raised partner. Total power is unity at every position. A step in
// the knob glides, so a copy enters or leaves, and the lead moves, without a click.
//
// DIVERGE (the engine's width field) spreads the copies' formant offsets from none to their full
// values and their pitch offsets from none to two thirds of the listed ones (kWidthPitchTop), and it
// stretches their lags. The bottom tenth of the knob is for instruments and melodic lines, where the
// copies are plain delayed doubles of the lead, exactly on its pitch at DIVERGE 0. DIVERGE also
// scales the drift and the contour, so nothing else moves a copy there. A copy changes its lag only
// in a quiet gap, so it never bends in pitch mid-word. A gap is quiet against the recent phrase
// level, so gaps over a stage's noise floor count too. Scatter gives each copy a fresh timing offset
// at every gap, and drift walks each copy's pitch slowly around its offset. On a note the tracker
// cannot follow, or after a long unvoiced run, the copies drop their moves and play plain delayed
// doubles.
//
// Two more controls make a copy read as a person and not as Adam shifted. Level scatter gives a
// copy a new loudness at every gap. Contour sets how much of Adam's rises and falls a copy follows.
// Both stay inside what a human voice does, and the engine clamps each to that range. Each is
// neutral at 0 (contour 1), where it adds nothing. The defaults are the owner's working set (owner
// 2026-10-31). Grains are cut on the period grid.
//
// Mob 2 (an audition card, owner 2026-11-04) runs this engine with CrowdParams::mob2 set. It plays the
// four tuned copies alone at DIVERGE 1, and VOICES runs 0 to 4 copies with the lead, then MOB. Each
// copy adds its own vibrato, which restarts at each word onset its gap rule reads and fades in after
// it. The pitch drift is wider and faster, and at each gap a copy draws its lag over a wider jitter and
// its level over a smaller one. The DRIFT knob scales the vibrato, the drift and both jitters together
// from 0, where the copies are fixed clones, to the full amounts at 1.

namespace cv {

struct CrowdTuning {
  std::array<float, 4> pitchSt = {-1.75f, 2.75f, -2.5f, 1.0f};     // offset before the DIVERGE pitch top (2/3 sounds at DIVERGE 1)
  std::array<float, 4> formantSt = {-1.75f, -1.0f, -1.0f, -1.5f};  // grain playback rate 2^(st / 12) at DIVERGE 1
  std::array<float, 4> shelfDb = {-3.0f, -4.0f, 3.0f, 5.0f};       // high shelf gain above shelfHz
  std::array<float, 4> delayMs = {40.0f, 100.0f, 70.0f, 75.0f};    // lag behind the lead before lagScale
  float lagScale = 1.2f;       // a copy's lag is delayMs times this
  float shelfHz = 2500.0f;
  float voiceDb = -9.0f;       // a tuned copy and its raised copy together against the lead at unity
  float trimDb = 2.1f;         // level rule residue at DIVERGE 1
  float trimBottomDb = 0.7f;   // the same at DIVERGE 0; between, the trim follows the formant factor
  float hpHz = 120.0f;         // low cut on the crowd sum
  float maxLagMs = 150.0f;     // ceiling on a copy's lag before scatter, up to kCrowdMaxLagMs
  float gapFloorDb = -50.0f;   // a copy's read point under this level counts as a gap
  float scatterMs = 12.0f;     // peak random lag offset a copy takes at each gap
  float driftCents = 15.0f;    // peak of each copy's slow random pitch walk
  float driftHoldS = 2.0f;     // time between new drift targets

  // DIVERGE scales a copy's pitch and formant offsets by a factor that is linear between three
  // points. It is 0 at DIVERGE 0, widthKneeFrac at DIVERGE widthKnee and 1 at DIVERGE 1. At DIVERGE 0 every
  // copy sits exactly on the lead's pitch with the lead's throat. The same factor scales the drift
  // range and pulls each contour toward 1, so the bottom of the knob stays in tune for a melodic line.
  // The pitch offsets, the drift and the contour take the factor times kWidthPitchTop (2/3), so the
  // top of DIVERGE reaches two thirds of the listed pitch offsets. The formant offsets take it whole.
  float widthKnee = 0.1f;
  float widthKneeFrac = 0.012f;

  // DIVERGE also stretches every lag by one multiplier that is widthLagBottom at DIVERGE 0 (0.2..1) and
  // 1 + widthLagMs / (the longest base lag, delayMs times lagScale) at DIVERGE 1, linear between. At
  // DIVERGE 1 the longest copy lags widthLagMs (0..60) behind where lagScale alone puts it, and the
  // others in proportion.
  float widthLagBottom = 0.5f;
  float widthLagMs = 20.0f;

  // Each copy takes a new loudness at every gap, uniform in +- this many dB (0..4).
  float levelScatterDb = 2.0f;

  // The share of Adam's pitch deviation a copy follows (0.6..1.4). 1 follows him exactly, 0.7 flattens
  // the copy's rises and falls to 70 % of his, 1.3 exaggerates them.
  std::array<float, 4> contour = {0.8f, 1.2f, 0.7f, 1.1f};
};

// VOICES positions: 0 to 8 copies with the lead, then MOB. The readout shows the name.
constexpr int kVoicesSteps = 10;
constexpr uint8_t kVoicesMob = 9;
constexpr std::array<const char*, kVoicesSteps> kVoicesName = {"0", "1", "2", "3", "4",
                                                               "5", "6", "7", "8", "MOB"};

// Mob 2's VOICES positions: 0 to 4 copies with the lead, then MOB.
constexpr int kVoices2Steps = 6;
constexpr uint8_t kVoices2Mob = 5;
constexpr std::array<const char*, kVoices2Steps> kVoices2Name = {"0", "1", "2", "3", "4", "MOB"};

struct CrowdParams {
  bool on = false;
  float width = 1.0f;         // knob A (DIVERGE), 0..1
  uint8_t voicesStep = 4;     // knob B (VOICES), 0..9: that many copies and the lead, 9 is MOB
  CrowdTuning tuning;

  // A bit per copy (bit 0 is copy 0, bits 4 to 7 the raised copies), 0 for no solo. A soloed copy
  // plays and the others are silent, whatever the VOICES position. The lead gain follows from the
  // soloed power, and MOB scales the soloed copies to unity. This is a tuning hook for the
  // emulator, whose four boxes set bits 0 to 3. The firmware never sets it.
  uint8_t solo = 0;

  // Mob 2 mode. width is then ignored, voicesStep runs 0..kVoices2Mob, and drift is knob A (DRIFT).
  bool mob2 = false;
  float drift = 1.0f;  // 0..1
};

namespace crowd_detail {

constexpr size_t kVoices = 4;               // the tuned copies, one per entry of the tuning's arrays
constexpr size_t kCopies = 2 * kVoices;     // the tuned copies, then the raised copies
constexpr float kRaisedSt = 1.75f;          // a raised copy's pitch over its tuned copy's, before DIVERGE
constexpr float kRaisedLagMs = 75.0f;       // a raised copy's lag past its tuned copy's
constexpr float kCopyGain = 0.70710678f;    // 1 / sqrt(2): eight copies hold four copies' power
constexpr float kSmpPerMs = kSampleRate / 1000.0f;
static_assert(kVoicesMob == kCopies + 1, "VOICES runs 0 to kCopies, then MOB");
static_assert(kVoices2Mob == kVoices + 1, "Mob 2's VOICES runs 0 to kVoices, then MOB");

// Mob 2's fixed tuning, each amount at DRIFT 1. Four copies at gain 1 hold the power of Mob's eight.
// Copy v's vibrato runs at kMob2VibHz[v], peaks at kMob2VibCents[v] and fades in over
// kMob2VibFadeMs[v] after each word onset, on a square law.
constexpr float kMob2CopyGain = 1.0f;
constexpr std::array<float, kVoices> kMob2VibHz = {4.6f, 5.2f, 5.7f, 6.3f};
constexpr std::array<float, kVoices> kMob2VibCents = {16.0f, 26.0f, 12.0f, 30.0f};
constexpr std::array<float, kVoices> kMob2VibFadeMs = {250.0f, 150.0f, 400.0f, 300.0f};
constexpr float kMob2DriftCents = 20.0f;  // the drift peak heard, with no DIVERGE factor on it
constexpr float kMob2DriftHoldS = 1.5f;
constexpr float kMob2JitterMs = 25.0f;    // peak lag offset drawn at each gap
constexpr float kMob2WordGapMs = 80.0f;   // a gap quiet this long past its landing ends a word
constexpr float kMob2LevelDb = 1.5f;      // peak level offset drawn at each gap
constexpr float kMob2TrimDb = 3.0f;       // the level rule's trim at VOICES 4, DRIFT 1, on Adam's clips

// The copy that enters at VOICES position j, counting from 1. The order is tuned 0, raised 4,
// tuned 1, raised 5, and so on.
constexpr size_t entryCopy(size_t j) { return (j - 1) / 2 + ((j - 1) % 2) * kVoices; }

constexpr int kQuietBlocks = 2;        // blocks under the gap floor before a copy changes its lag
constexpr float kDriftSlewS = 1.0f;    // time constant of the drift toward its target
constexpr float kMinDriftHoldS = 0.05f;

// The ranges the human-voice controls keep to. The engine clamps to them.
constexpr float kMaxLevelScatterDb = 4.0f;
constexpr float kMinContour = 0.6f;
constexpr float kMaxContour = 1.4f;

// The ranges of the rest of the tuning. The engine clamps to them, and the render tool and the
// emulator's sliders read them. The offsets need no clamp of their own, since the ratio and rate
// clamps of 0.5..2 stop them at an octave either way.
constexpr float kMaxOffsetSt = 12.0f;
constexpr float kMaxShelfDb = 12.0f;
constexpr float kMinDelayMs = 5.0f;  // up to kCrowdMaxLagMs
constexpr float kMinLagScale = 0.5f;
constexpr float kMaxLagScale = 2.0f;
constexpr float kMinVoiceDb = -24.0f;
constexpr float kMaxVoiceDb = 0.0f;
constexpr float kMaxTrimDb = 12.0f;
constexpr float kMaxScatterMs = static_cast<float>(kCrowdScatterMs);  // the scatter the ring holds
constexpr float kMaxDriftCents = 50.0f;
static_assert(CrowdTuning{}.scatterMs <= kMaxScatterMs, "the shipped scatter exceeds what the ring holds");

// The gap floor sits this far under the recent phrase level, and never under gapFloorDb. The phrase
// level is the loudest mean square the copies read for the gap rule, through a one-pole that rises
// in kPhraseRiseS and falls in kPhraseFallS, so it holds a phrase's level through the gap after it.
// kGapUnderPhraseSq is the same distance as a ratio of mean squares.
constexpr float kGapUnderPhraseDb = 20.0f;
constexpr float kPhraseRiseS = 0.05f;
constexpr float kPhraseFallS = 1.0f;
inline const float kGapUnderPhraseSq = dbToGainFast(-2.0f * kGapUnderPhraseDb);

// A gap ends once the copy's read point sounds 6 dB over the floor, so noise wandering about the
// floor inside one pause counts as one gap and draws once.
constexpr float kGapRearm = 4.0f;

// The ceiling on a tuned copy's lag before scatter, from the tuning, and on its raised copy's, 75 ms
// further back. Both stop at kCrowdMaxLagMs, the lag the ring was sized for.
constexpr float kRingLagMs = static_cast<float>(kCrowdMaxLagMs);

inline float tunedCapMs(const CrowdTuning& t) { return clampf(t.maxLagMs, 0.0f, kRingLagMs); }

inline float raisedCapMs(float tunedCap) {
  const float cap = tunedCap + kRaisedLagMs;
  return cap < kRingLagMs ? cap : kRingLagMs;
}

// After an unvoiced run as long as a copy's lag cap plus the scatter the ring holds, the copy has read
// past the last voiced sample, so it goes plain without bending a word. The run counter stops at the
// longest run any copy waits for.
constexpr int kPlainAfterMax = (kCrowdMaxLagMs + kCrowdScatterMs) * (kSampleRate / 1000);

inline int plainAfter(float capMs) { return static_cast<int>((capMs + static_cast<float>(kCrowdScatterMs)) * kSmpPerMs); }

static_assert(static_cast<float>(kCrowdMaxLagMs + kCrowdScatterMs) * kSmpPerMs <= formant_detail::kDelayCeil,
              "the longest lag, its scatter and the longest grain do not fit the ring");

// The factor DIVERGE puts on a copy's pitch and formant offsets. It is linear from (0, 0) to
// (knee, kneeFrac) and on to (1, 1), so it is exactly 0 at DIVERGE 0 and exactly 1 at DIVERGE 1.
constexpr float kMinWidthKnee = 0.02f;
constexpr float kMaxWidthKnee = 0.9f;
constexpr float kMaxWidthKneeFrac = 1.0f;

// The pitch offsets, the drift and the contour deviation take the DIVERGE factor times this top, so
// DIVERGE 1 reaches two thirds of the pitch move the tuning lists (owner 2026-10-31). The formant
// offsets and the lag law keep the full factor.
constexpr float kWidthPitchTop = 2.0f / 3.0f;

inline float widthFactor(float width, float knee, float kneeFrac) {
  const float k = clampf(knee, kMinWidthKnee, kMaxWidthKnee);
  const float f = clampf(kneeFrac, 0.0f, kMaxWidthKneeFrac);
  if (width <= k) return f * (width / k);
  const float t = (width - k) / (1.0f - k);
  return f * (1.0f - t) + t;
}

// The multiplier DIVERGE puts on every lag target. The longest base lag is delayMs times lagScale; at
// DIVERGE 1 it grows by widthLagMs and the rest grow by the same ratio.
constexpr float kMinLagBottom = 0.2f;
constexpr float kMaxLagBottom = 1.0f;
constexpr float kMaxWidthLagMs = 60.0f;

// Copy v's lag before DIVERGE and the ceiling, delayMs times lagScale, each inside its range.
inline float baseLagMs(const CrowdTuning& t, size_t v) {
  return clampf(t.delayMs[v], kMinDelayMs, static_cast<float>(kCrowdMaxLagMs)) *
         clampf(t.lagScale, kMinLagScale, kMaxLagScale);
}

inline float lagMultiplier(const CrowdTuning& t, float width) {
  float longest = 0.0f;
  for (size_t v = 0; v < kVoices; ++v) {
    const float lag = baseLagMs(t, v);
    longest = lag > longest ? lag : longest;
  }
  const float bottom = clampf(t.widthLagBottom, kMinLagBottom, kMaxLagBottom);
  const float top = longest > 0.0f ? 1.0f + clampf(t.widthLagMs, 0.0f, kMaxWidthLagMs) / longest : 1.0f;
  return bottom * (1.0f - width) + top * width;
}

// A small random source that repeats from its seed. Each voice owns one per use.
class XorShift {
 public:
  void seed(uint32_t s) { s_ = s != 0u ? s : 1u; }

  // Returns a uniform value in [-1, 1].
  float next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return static_cast<float>(static_cast<int32_t>(s_)) * (1.0f / 2147483648.0f);
  }

 private:
  uint32_t s_ = 1u;
};

// A copy's slow pitch walk in cents. A new random target in [-peak, +peak] comes every hold, and the
// value slews toward it. A peak of 0 holds the value at exactly 0.
class PitchDrift {
 public:
  void reset() {
    cents_ = 0.0f;
    unit_ = 0.0f;
    left_ = 0.0f;
  }

  // slew is the one-pole coefficient for a block of n samples.
  float step(XorShift& rng, float peak, float holdS, float slew, int n) {
    left_ -= static_cast<float>(n) * (1.0f / kSampleRate);
    if (left_ <= 0.0f) {
      unit_ = rng.next();
      left_ += holdS > kMinDriftHoldS ? holdS : kMinDriftHoldS;
    }
    cents_ = smooth::step(cents_, peak > 0.0f ? unit_ * peak : 0.0f, slew);
    return cents_;
  }

  float cents() const { return cents_; }

 private:
  float cents_ = 0.0f;
  float unit_ = 0.0f;
  float left_ = 0.0f;
};

// Adam's pitch against his own running mean, in octaves. The grains of every copy are laid down at the
// tracker's current period, so a copy's pitch follows the deviation of the moment and not the one its
// lag ago, and nothing is kept but the mean.
class ContourTrack {
 public:
  void reset() {
    mean_ = 0.0f;
    dev_ = 0.0f;
    have_ = false;
  }

  // Per block. period is the tracker's (held through unvoiced frames, 0 before the first voiced
  // one), voiced says the frame is fresh, and slew is the one-pole coefficient of the mean for the
  // block. The mean moves only while the voice is voiced and holds through the rest. The first
  // voiced frame sets it, so the deviation starts at 0.
  void step(float period, bool voiced, float slew) {
    if (!(period > 0.0f)) return;
    const float l = log2Fast(period);
    if (voiced) {
      mean_ = have_ ? mean_ + slew * (l - mean_) : l;
      have_ = true;
    }
    dev_ = have_ ? mean_ - l : 0.0f;
  }

  // Octaves above the mean: positive when the voice is sharper than its average.
  float deviation() const { return dev_; }

 private:
  float mean_ = 0.0f;  // log2 of the period, averaged
  float dev_ = 0.0f;
  bool have_ = false;
};

// The block being prepared, as the gap rule sees it.
struct BlockView {
  const VoiceRing& ring;
  int first;          // ring index of the block's first sample
  int n;
  float quietSq;      // a mean square under this counts as quiet

  // True when the n samples that the ring holds `lag` behind the block are quiet.
  bool quietAt(float lag) const { return sumSqAt(lag) < quietSq * static_cast<float>(n); }

  // The sum of squares of the n samples that the ring holds `lag` behind the block. A window clear
  // of the ring's end runs on four accumulators through a straight pointer, so the adds do not wait
  // on one another. A window across the end, and a tail short of four, step one sample at a time.
  float sumSqAt(float lag) const {
    int at = (first - static_cast<int>(lag + 0.5f)) % kVoiceRingLen;
    if (at < 0) at += kVoiceRingLen;
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    int i = 0;
    if (at + n <= kVoiceRingLen) {
      const float* r = ring.data() + at;
      for (; i + 4 <= n; i += 4) {
        s0 += r[i] * r[i];
        s1 += r[i + 1] * r[i + 1];
        s2 += r[i + 2] * r[i + 2];
        s3 += r[i + 3] * r[i + 3];
      }
      at += i;
    }
    for (; i < n; ++i) {
      const float x = ring[static_cast<size_t>(at)];
      s0 += x * x;
      at = at + 1 == kVoiceRingLen ? 0 : at + 1;
    }
    return (s0 + s1) + (s2 + s3);
  }
};

// One copy's lag. A new target (a tuning move, or the scatter draw of a fresh gap)
// lands once the ring is quiet at both the lag the copy plays and the lag it moves to. The jump
// then replays or skips only silence. While material sounds the copy keeps the lag it has.
class CopyLag {
 public:
  void seed(uint32_t s) {
    rng_.seed(s);
    offset_ = rng_.next();
    level_ = 0.0f;
  }

  // Unsets the lag and keeps the random stream.
  void restart() {
    set_ = false;
    landed_ = false;
    quiet_ = 0;
  }

  float samples() const { return lag_; }

  // The loudness draw of the current gap, -1..1. It is 0 until a gap has drawn one, and it only
  // draws while level scatter is on.
  float levelUnit() const { return level_; }

  // The sum of squares the last settle() read at the copy's lag, or -1 when it read none.
  float heard() const { return heard_; }

  // True when the last settle() heard the copy's read point sound again after a landing, the word
  // onset at the copy's own lag.
  bool onset() const { return onset_; }

  // Per block. base is the target in samples before scatter. rollLevel says level scatter is on,
  // so each gap also draws a loudness, which keeps the gap rule running when the timing scatter is
  // 0. True when the lag landed on a new value, so the voice restarts its marks there.
  bool settle(const BlockView& b, float base, float scatterSmp, float period, bool rollLevel) {
    heard_ = -1.0f;
    onset_ = false;
    const float want = formant_detail::clampLag(base + offset_ * scatterSmp, period);
    if (!set_) return land(base, want, rollLevel);
    const bool moved = base != base_;
    if (!moved && scatterSmp <= 0.0f && !rollLevel) {
      quiet_ = 0;
      landed_ = false;
      return false;
    }
    const float sum = b.sumSqAt(formant_detail::clampLag(lag_, period));
    heard_ = sum;
    const float quiet = b.quietSq * static_cast<float>(b.n);
    const bool here = sum < quiet;
    if (sum >= kGapRearm * quiet) {
      onset_ = landed_;
      landed_ = false;
    }
    const bool pending = moved || !landed_;
    const bool calm = here && (!pending || b.quietAt(want));
    quiet_ = calm ? quiet_ + 1 : 0;
    return quiet_ >= kQuietBlocks && pending && land(base, want, rollLevel);
  }

 private:
  bool land(float base, float want, bool rollLevel) {
    lag_ = want;
    base_ = base;
    set_ = true;
    landed_ = true;
    quiet_ = 0;
    offset_ = rng_.next();
    if (rollLevel) level_ = rng_.next();
    return true;
  }

  XorShift rng_;
  float offset_ = 0.0f;  // the next scatter draw, -1..1
  float level_ = 0.0f;   // this gap's loudness draw, -1..1
  float lag_ = 0.0f;
  float base_ = 0.0f;
  float heard_ = -1.0f;
  int quiet_ = 0;
  bool set_ = false;
  bool landed_ = false;  // this gap has taken its landing
  bool onset_ = false;
};

// The VOICES law at one position. weight[c] is copy c's fixed gain when it plays and 0 when it does
// not, lead the lead's gain, mul the crowd multiplier that carries MOB.
struct VoicesLaw {
  std::array<float, kCopies> weight{};
  float lead = 1.0f;
  float mul = 1.0f;
};

// The copies playing at VOICES position `step`, a bit per copy. Position k plays the first k copies
// in entry order, and MOB plays all eight. Mob 2 plays tuned copies 0 to k - 1, and all four at MOB.
inline uint8_t playingCopies(uint8_t step, bool mob2 = false) {
  if (mob2) return static_cast<uint8_t>((1u << (step < kVoices2Mob ? step : kVoices)) - 1u);
  uint8_t mask = 0;
  const size_t count = step < kVoicesMob ? step : kCopies;
  for (size_t j = 1; j <= count; ++j) mask = static_cast<uint8_t>(mask | (1u << entryCopy(j)));
  return mask;
}

// solo is a bit per copy. When a bit is set, the soloed copies play at every position and the others
// stay silent, and the rest of the law runs on that crowd. The lead and the crowd multiplier then
// follow from the soloed power, so a single copy at MOB stands alone at unity.
//
// A playing copy holds the gain g from the tuning, and the lead takes what unity leaves,
// sqrt(1 - the copies' power), never below 0. MOB drops the lead and scales the copies together to
// unity power.
inline VoicesLaw voicesLaw(uint8_t step, const CrowdTuning& t, uint8_t solo = 0, bool mob2 = false) {
  VoicesLaw law;
  const float g = dbToGainFast(clampf(t.voiceDb, kMinVoiceDb, kMaxVoiceDb)) * (mob2 ? kMob2CopyGain : kCopyGain);
  const uint8_t mask = solo == 0 ? playingCopies(step, mob2) : solo;
  unsigned count = 0;
  for (size_t c = 0; c < kCopies; ++c) {
    const unsigned bit = (mask >> c) & 1u;
    count += bit;
    law.weight[c] = g * static_cast<float>(bit);
  }
  const float power = static_cast<float>(count) * g * g;
  if (step < (mob2 ? kVoices2Mob : kVoicesMob)) {
    law.lead = sqrtf(power < 1.0f ? 1.0f - power : 0.0f);
    return law;
  }
  law.lead = 0.0f;
  law.mul = power > 1e-6f ? sqrtf(1.0f / power) : 1.0f;
  return law;
}

// The four tone shelves share one filter. A shelf is linear, so each copy's shelf is written as
// 1 + wh HP + wb BP, where HP and BP are the high-pass and band-pass of one Butterworth pole pair at
// kShelfPoleScale times shelfHz. Each copy then adds into a plain sum and two weighted sums, and one
// two-input biquad filters the weighted sums for all four.
constexpr float kShelfPoleScale = 1.05f;
constexpr int kShelfFitPoints = 24;
constexpr float kShelfFitBelow = 0.2f;    // the fit spans 0.2 to 4.8 times shelfHz, the shelf's turn
constexpr float kShelfFitAbove = 4.8f;
constexpr float kShelfFitTopHz = 20000.0f;

// The shared poles and the two numerators: HP is h0 h1 h0, BP is q0 0 -q0.
struct ShelfBasis {
  float h0 = 0.0f, h1 = 0.0f, q0 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

// A copy's two weights. Both are exactly 0 for a 0 dB shelf.
struct ShelfWeights {
  float high = 0.0f;
  float band = 0.0f;
};

// Runs when the tuning moves, so libm is fine here.
inline ShelfBasis shelfBasis(float hz) {
  const float w0 = kTwoPi * clampf(kShelfPoleScale * hz, kFilterMinHz, kFilterMaxHz) / kSampleRate;
  const float cw = cosf(w0);
  const float alpha = sinf(w0) / (2.0f * kButterworthQ);
  const float a0 = 1.0f + alpha;
  ShelfBasis b;
  b.h0 = 0.5f * (1.0f + cw) / a0;
  b.h1 = -(1.0f + cw) / a0;
  b.q0 = alpha / a0;
  b.a1 = -2.0f * cw / a0;
  b.a2 = (1.0f - alpha) / a0;
  return b;
}

struct Cplx {
  float re = 0.0f;
  float im = 0.0f;
};

inline Cplx divide(const Cplx& num, const Cplx& den) {
  const float mag = den.re * den.re + den.im * den.im;
  return {(num.re * den.re + num.im * den.im) / mag, (num.im * den.re - num.re * den.im) / mag};
}

// (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2) at z^-1 = c - j s, where c and s are cos and sin
// of the frequency in radians per sample.
inline Cplx biquadAt(float b0, float b1, float b2, float a1, float a2, float c, float s) {
  const float c2 = c * c - s * s;
  const float s2 = 2.0f * s * c;
  return divide({b0 + b1 * c + b2 * c2, -(b1 * s + b2 * s2)}, {1.0f + a1 * c + a2 * c2, -(a1 * s + a2 * s2)});
}

// Least squares over kShelfFitPoints log-spaced frequencies across the shelf's turn: the real wh and
// wb that bring 1 + wh HP + wb BP closest to the RBJ high shelf of db at hz. Each point's error is
// taken relative to the shelf there, so it reads as dB and degrees. Over 100 Hz to 20 kHz with plain
// errors the fit missed the default shelves by 0.150 dB and 1.05 degrees; this one by 0.141 and 0.94.
inline ShelfWeights fitShelf(const ShelfBasis& b, float hz, float db) {
  if (db == 0.0f) return {};
  Biquad ref;
  ref.setHighShelf(hz, db);
  const float lo = kShelfFitBelow * hz;
  const float hi = kShelfFitAbove * hz < kShelfFitTopHz ? kShelfFitAbove * hz : kShelfFitTopHz;
  const float octaves = log2Fast(hi / lo);
  float uu = 0.0f, uw = 0.0f, ww = 0.0f, du = 0.0f, dw = 0.0f;
  for (int k = 0; k < kShelfFitPoints; ++k) {
    const float f = lo * exp2Fast(octaves * static_cast<float>(k) / (kShelfFitPoints - 1));
    float s = 0.0f;
    float c = 1.0f;
    sinCosFast(kTwoPi * f / kSampleRate, s, c);
    const Cplx r = biquadAt(ref.b0, ref.b1, ref.b2, ref.a1, ref.a2, c, s);
    const Cplx u = divide(biquadAt(b.h0, b.h1, b.h0, b.a1, b.a2, c, s), r);
    const Cplx w = divide(biquadAt(b.q0, 0.0f, -b.q0, b.a1, b.a2, c, s), r);
    const Cplx d = divide({r.re - 1.0f, r.im}, r);
    uu += u.re * u.re + u.im * u.im;
    ww += w.re * w.re + w.im * w.im;
    uw += u.re * w.re + u.im * w.im;
    du += d.re * u.re + d.im * u.im;
    dw += d.re * w.re + d.im * w.im;
  }
  const float det = uu * ww - uw * uw;
  return {(du * ww - dw * uw) / det, (dw * uu - du * uw) / det};
}

}  // namespace crowd_detail

class Crowd {
 public:
  // The constructor runs reset(), so the non-zero defaults below land in .bss with the rest.
  Crowd() { reset(); }

  void reset() {
    clearDsp();
    gain_.fill(0.0f);
    lead_ = 1.0f;
    wet_ = 0.0f;
    glideSet_ = false;
    for (size_t v = 0; v < kCopies; ++v) {
      lag_[v].seed(0x2545F491u * static_cast<uint32_t>(v + 1));
      driftRng_[v].seed(0x9E3779B9u * static_cast<uint32_t>(v + 1) ^ 0x85EBCA6Bu);
    }
    idle_ = true;
  }

  // in is this slot's input, the lead. pitch has already run this block, so the block's samples
  // sit at ring index head() - n + i. Off and settled, out equals in bit for bit.
  void process(const float* in, float* out, int n, const CrowdParams& p, const PitchFx& pitch) {
    if (settledOff(p)) {
      passThrough(in, out, n);
      return;
    }
    idle_ = false;
    setCoefs(n);
    const float period = holdPeriod(pitch.pitch().period);
    const Plan plan = makePlan(p, period > 0.0f);
    const int first = pitch.head() - n;
    prepareBlock(p, pitch, plan, period, first, n);
    for (int c = 0; c < n; c += kBlock) {
      const int m = n - c < kBlock ? n - c : kBlock;
      renderChunk(in + c, out + c, plan, Chunk{pitch.ring(), first + c, m});
    }
  }

  // Test hooks. leadGain is the lead's gain at the end of the last block. activeVoices counts the
  // copies that ticked in it and activeMask names them, a bit per copy. lagSamples, driftCents and
  // grainRate read copy v's current lag, pitch drift and grain playback rate. moveShare is the share
  // of copy v's pitch and formant moves in force, 1 on a note in range and 0 once it plays plain.
  float leadGain() const { return lead_; }
  uint8_t activeVoices() const { return active_; }
  uint8_t activeMask() const {
    uint8_t mask = 0;
    for (size_t v = 0; v < kCopies; ++v) mask = static_cast<uint8_t>(mask | (live_[v] ? 1u << v : 0u));
    return mask;
  }
  float lagSamples(size_t v) const { return lag_[v].samples(); }
  float driftCents(size_t v) const { return drift_[v].cents(); }
  float grainRate(size_t v) const { return voice_[v].rate(); }
  float moveShare(size_t v) const { return keep_[v / kVoices]; }

 private:
  using FormantPsolaVoice = formant_detail::FormantPsolaVoice;
  static constexpr size_t kVoices = crowd_detail::kVoices;  // the tuned copies
  static constexpr size_t kCopies = crowd_detail::kCopies;  // copy v + kVoices is tuned copy v raised
  static constexpr float kShelfFloor = 1e-20f;  // a shared shelf state under this is silence, 400 dB down

  // Block-end targets for everything the knobs and the tuning set.
  struct Plan {
    std::array<float, kCopies> voice{};  // each copy's gain, 0 for a copy not playing
    float lead = 1.0f;
    float wet = 0.0f;    // crowd multiplier times the trim
    float width = 0.0f;  // DIVERGE's factor on the formant offsets, from the smoothed knob
  };

  // Until the tracker has a period the copies cannot play, so the plan holds the lead at 1 and the
  // copies at 0. A switch-on then keeps the lead until the copies can take over. The DIVERGE smoother
  // steps on every block either way.
  Plan makePlan(const CrowdParams& p, bool havePeriod) {
    Plan plan;
    const CrowdTuning& t = p.tuning;
    plan.width = crowd_detail::widthFactor(smoothedWidth(knobWidth(p)), t.widthKnee, t.widthKneeFrac);
    if (!p.on || !havePeriod) {
      glideSet_ = false;
      return plan;
    }
    const crowd_detail::VoicesLaw& law = voicesLaw(p);
    float mul = 1.0f;
    glideLaw(law, plan.voice, plan.lead, mul);
    const float trim = p.mob2 ? crowd_detail::kMob2TrimDb : trimAt(t, plan.width);
    if (trim != trimDb_) {
      trimGain_ = dbToGainFast(trim);
      trimDb_ = trim;
    }
    plan.wet = mul * trimGain_;
    return plan;
  }

  // A VOICES step moves the law's targets at once. Each target runs through two one-poles of
  // smooth::kSmoothSec in a row, so it leaves its old value with zero slope and a copy entering or
  // leaving, the lead and the MOB multiplier glide without a kink. The glide takes a target outright
  // on the first block that plays, so a switch-on ramps in as it always has.
  struct Glide {
    float first = 0.0f;
    float second = 0.0f;

    void set(float v) { first = second = v; }
    float step(float target, float coef) {
      first = smooth::step(first, target, coef);
      second = smooth::step(second, first, coef);
      return second;
    }
  };

  void glideLaw(const crowd_detail::VoicesLaw& law, std::array<float, kCopies>& voice, float& lead, float& mul) {
    if (!glideSet_) {
      for (size_t c = 0; c < kCopies; ++c) weightGlide_[c].set(law.weight[c]);
      leadGlide_.set(law.lead);
      mulGlide_.set(law.mul);
      glideSet_ = true;
    }
    for (size_t c = 0; c < kCopies; ++c) voice[c] = weightGlide_[c].step(law.weight[c], smoothCoef_);
    lead = leadGlide_.step(law.lead, smoothCoef_);
    mul = mulGlide_.step(law.mul, smoothCoef_);
  }

  // A copy loses level as DIVERGE moves its pitch and formant, so the trim moves from trimBottomDb at
  // DIVERGE 0 to trimDb at DIVERGE 1 along the formant factor, and each end is exact.
  static float trimAt(const CrowdTuning& t, float width) {
    const float top = clampf(t.trimDb, -crowd_detail::kMaxTrimDb, crowd_detail::kMaxTrimDb);
    const float bottom = clampf(t.trimBottomDb, -crowd_detail::kMaxTrimDb, crowd_detail::kMaxTrimDb);
    return top * width + bottom * (1.0f - width);
  }

  // The tracker's period, or the last one it gave. The tracker drops its period when it stops, in
  // the same block the card goes off, so the copies keep playing through their ramp out.
  float holdPeriod(float period) {
    if (period > 0.0f) heldPeriod_ = period;
    return heldPeriod_;
  }

  // What the VOICES law reads. The law's roots run again only when one of them changes.
  struct LawKey {
    uint8_t step = 0;
    float voiceDb = 0.0f;
    uint8_t solo = 0;
    bool mob2 = false;
    bool set = false;

    bool operator==(const LawKey& o) const {
      return set == o.set && step == o.step && voiceDb == o.voiceDb && solo == o.solo && mob2 == o.mob2;
    }
  };

  const crowd_detail::VoicesLaw& voicesLaw(const CrowdParams& p) {
    const uint8_t top = p.mob2 ? kVoices2Mob : kVoicesMob;
    const LawKey key{p.voicesStep < top ? p.voicesStep : top, p.tuning.voiceDb, p.solo, p.mob2, true};
    if (!(key == lawKey_)) {
      law_ = crowd_detail::voicesLaw(key.step, p.tuning, p.solo, p.mob2);
      lawKey_ = key;
    }
    return law_;
  }

  // The DIVERGE the engine reads: the knob, or 1 for Mob 2.
  static float knobWidth(const CrowdParams& p) { return p.mob2 ? 1.0f : p.width; }

  // Mob 2's DRIFT through the 20 ms one-pole, taken outright on the first block after a reset. Mob
  // reads 0.
  float smoothedDrift(const CrowdParams& p) {
    if (!p.mob2) return 0.0f;
    const float target = smooth::clamp01(p.drift);
    driftSm_ = driftSet_ ? smooth::step(driftSm_, target, smoothCoef_) : target;
    driftSet_ = true;
    return driftSm_;
  }

  // The one-pole coefficients for a call of n samples. They hang on n alone, so they are worked out
  // again only when n changes.
  void setCoefs(int n) {
    if (n == coefN_) return;
    smoothCoef_ = smooth::blockCoef(smooth::kSmoothSec, n);
    slewCoef_ = smooth::blockCoef(crowd_detail::kDriftSlewS, n);
    riseCoef_ = smooth::blockCoef(crowd_detail::kPhraseRiseS, n);
    fallCoef_ = smooth::blockCoef(crowd_detail::kPhraseFallS, n);
    perSample_ = 1.0f / static_cast<float>(n);
    for (size_t v = 0; v < kVoices; ++v) {
      const float turns = crowd_detail::kMob2VibHz[v] * static_cast<float>(n) * (1.0f / kSampleRate);
      vibStep_[v] = static_cast<uint32_t>((turns - static_cast<float>(static_cast<int>(turns))) * 4294967296.0f);
    }
    coefN_ = n;
  }

  bool settledOff(const CrowdParams& p) const {
    if (p.on || lead_ != 1.0f || wet_ != 0.0f) return false;
    for (float g : gain_) {
      if (g != 0.0f) return false;
    }
    return true;
  }

  void passThrough(const float* in, float* out, int n) {
    if (!idle_) {
      clearDsp();
      idle_ = true;
    }
    for (int i = 0; i < n; ++i) out[i] = in[i];
  }

  void clearDsp() {
    for (size_t v = 0; v < kCopies; ++v) retire(v);
    hp_.clear();
    shelfZ1_ = 0.0f;
    shelfZ2_ = 0.0f;
    contour_.reset();
    active_ = 0;
    widthSet_ = false;
    driftSet_ = false;
    heldPeriod_ = 0.0f;
    phraseSq_ = 0.0f;
    unvoiced_ = 0;
    keep_.fill(1.0f);
    keepSet_ = false;
  }

  void retire(size_t v) {
    voice_[v].reset();
    lag_[v].restart();
    drift_[v].reset();
    levelMul_[v] = 1.0f;
    levelDb_[v] = 0.0f;
    live_[v] = false;
    if (v < kVoices) {
      vibPhase_[v] = 0u;
      vibAge_[v] = 0.0f;
      vibGap_[v] = -1.0f;
      vibHeld_[v] = false;
    }
  }

  // The low cut on the crowd sum. The setter runs on the first block and when the tuning moved. A
  // reset keeps the coefficients, since they depend on the tuning alone.
  void setLowCut(const CrowdTuning& t) {
    if (hpSet_ && t.hpHz == hpHz_) return;
    hp_.setHighPass(t.hpHz, kButterworthQ);
    hpHz_ = t.hpHz;
    hpSet_ = true;
  }

  // The shared shelf's poles and each copy's weights. The setter runs on the first block and when
  // the tuning moved. A reset keeps them, since they depend on the tuning alone.
  void setShelves(const CrowdTuning& t) {
    const bool hzMoved = !shelfSet_ || t.shelfHz != shelfHz_;
    if (hzMoved) shelf_ = crowd_detail::shelfBasis(t.shelfHz);
    for (size_t v = 0; v < kVoices; ++v) {
      if (!hzMoved && t.shelfDb[v] == shelfDb_[v]) continue;
      const float db = clampf(t.shelfDb[v], -crowd_detail::kMaxShelfDb, crowd_detail::kMaxShelfDb);
      shelfWeights_[v] = crowd_detail::fitShelf(shelf_, t.shelfHz, db);
      shelfDb_[v] = t.shelfDb[v];
    }
    shelfHz_ = t.shelfHz;
    shelfSet_ = true;
  }

  // Copy v's lag target in samples. The scale and the DIVERGE multiplier stretch the tuned copy's
  // delay and the tuning's ceiling caps it. A raised copy lags kRaisedLagMs past that, capped at the
  // ring's ceiling.
  static float lagTarget(const CrowdTuning& t, size_t v, float widthMul) {
    const float lagMs = crowd_detail::baseLagMs(t, v % kVoices) * widthMul;
    const float top = crowd_detail::tunedCapMs(t);
    const float tuned = lagMs < top ? lagMs : top;
    if (v < kVoices) return tuned * crowd_detail::kSmpPerMs;
    const float raised = tuned + crowd_detail::kRaisedLagMs;
    const float ceiling = crowd_detail::kRingLagMs;
    return (raised < ceiling ? raised : ceiling) * crowd_detail::kSmpPerMs;
  }

  // What every voice of one block reads.
  struct Block {
    const CrowdTuning& t;
    crowd_detail::BlockView view;
    float period;
    float corr;        // Autotune's correction in octaves
    float width;       // factor on the formant offsets, 0 at DIVERGE 0
    float pitchWidth;  // width times kWidthPitchTop, the factor on the pitch offsets, drift and contour
    float lagMul;      // DIVERGE multiplier on every lag target
    float scatterSmp;  // timing scatter in samples
    float levelDb;     // level scatter, clamped to its range
    float driftPeak;   // drift range in cents, clamped and times pitchWidth
    float driftHoldS;  // time between new drift targets
    std::array<float, 2> keep;  // share of the moves in force, tuned and raised copies, 0 when plain
    float slew;        // one-pole coefficient of the slow walks for this block
    float spread;      // Mob 2's DRIFT on the vibrato, 0 for Mob
    bool mob2;
    int n;
  };

  // The DIVERGE the factors read. It follows the knob through the 20 ms one-pole, so a knob step moves
  // the copies' pitch over a few blocks and not in one jump, and it takes the knob value outright on
  // the first block after a reset. A settled value equals the knob bit for bit.
  float smoothedWidth(float width) {
    const float target = smooth::clamp01(width);
    widthSm_ = widthSet_ ? smooth::step(widthSm_, target, smoothCoef_) : target;
    widthSet_ = true;
    return widthSm_;
  }

  // The gap floor for this block, kGapUnderPhraseDb under the phrase level and never under
  // gapFloorDb, so lags land between phrases over a stage's noise floor as well as over silence.
  // gapFloorDb as a mean square is worked out again only when it moves.
  float gapFloor(const CrowdTuning& t) {
    if (t.gapFloorDb != floorDb_) {
      floorSq_ = dbToGainFast(2.0f * t.gapFloorDb);
      floorDb_ = t.gapFloorDb;
    }
    const float under = phraseSq_ * crowd_detail::kGapUnderPhraseSq;
    return under > floorSq_ ? under : floorSq_;
  }

  // The phrase level follows the loudest read point the copies measured for the gap rule this block,
  // so it costs no reading of its own. A block where none measured leaves it as it is.
  void followPhrase() {
    float loudest = -1.0f;
    for (size_t v = 0; v < kCopies; ++v) {
      if (live_[v] && lag_[v].heard() > loudest) loudest = lag_[v].heard();
    }
    if (loudest < 0.0f) return;
    const float sq = loudest * perSample_;
    phraseSq_ += (sq > phraseSq_ ? riseCoef_ : fallCoef_) * (sq - phraseSq_);
  }

  // The share of the tuned copies' pitch and formant moves in force, and of the raised copies'. Each
  // glides to 0 through the 20 ms one-pole once the unvoiced run reaches its copies' lag cap plus
  // scatter, or while the tracker reads a note outside its range, so a copy then plays a plain
  // delayed double, exact at any period. Each takes its target outright on the first block with a
  // period. Mob 2's tuned copies wait for their wider jitter as well.
  std::array<float, 2> keepShare(const CrowdTuning& t, const PitchResult& pr, int n, bool mob2) {
    unvoiced_ = pr.voiced ? 0 : (unvoiced_ < crowd_detail::kPlainAfterMax ? unvoiced_ + n : unvoiced_);
    const float tunedCap = crowd_detail::tunedCapMs(t);
    const float jitterMs = mob2 ? crowd_detail::kMob2JitterMs - crowd_detail::kMaxScatterMs : 0.0f;
    const std::array<float, 2> capMs = {tunedCap + jitterMs, crowd_detail::raisedCapMs(tunedCap)};
    for (size_t k = 0; k < 2; ++k) {
      const bool plain = unvoiced_ >= crowd_detail::plainAfter(capMs[k]) || pr.outOfRange;
      const float target = plain ? 0.0f : 1.0f;
      keep_[k] = keepSet_ ? smooth::step(keep_[k], target, smoothCoef_) : target;
    }
    keepSet_ = true;
    return keep_;
  }

  // Per block: filters and the contour track, then each live voice, then the phrase level. first is
  // the ring index of the block's first sample, and period the tracker's or the one held.
  void prepareBlock(const CrowdParams& p, const PitchFx& pitch, const Plan& plan, float period, int first, int n) {
    const CrowdTuning& t = p.tuning;
    setLowCut(t);
    setShelves(t);
    const PitchResult& pr = pitch.pitch();
    contour_.step(period, pr.voiced, slewCoef_);
    const float pitchWidth = plan.width * crowd_detail::kWidthPitchTop;
    const float spread = smoothedDrift(p);
    const bool mob2 = p.mob2;
    const float scatterMs = mob2 ? crowd_detail::kMob2JitterMs * spread : clampf(t.scatterMs, 0.0f, crowd_detail::kMaxScatterMs);
    const Block b{t,
                  {pitch.ring(), first, n, gapFloor(t)},
                  period,
                  pitch.correctionSemis() * (1.0f / 12.0f),
                  plan.width,
                  pitchWidth,
                  crowd_detail::lagMultiplier(t, smooth::clamp01(knobWidth(p))),
                  scatterMs * crowd_detail::kSmpPerMs,
                  mob2 ? crowd_detail::kMob2LevelDb * spread : clampf(t.levelScatterDb, 0.0f, crowd_detail::kMaxLevelScatterDb),
                  mob2 ? crowd_detail::kMob2DriftCents * spread
                       : clampf(t.driftCents, 0.0f, crowd_detail::kMaxDriftCents) * pitchWidth,
                  mob2 ? crowd_detail::kMob2DriftHoldS : t.driftHoldS,
                  period > 0.0f ? keepShare(t, pr, n, mob2) : std::array<float, 2>{1.0f, 1.0f},
                  slewCoef_,
                  spread,
                  mob2,
                  n};
    active_ = 0;
    for (size_t v = 0; v < kCopies; ++v) prepareVoice(v, plan, b);
    followPhrase();
  }

  // Copy v's lag, loudness, pitch ratio and playback rate. A copy whose gain sits at 0 across the
  // block is cleared and does not tick. With keep at 0 the ratio and the rate are exactly 1. A
  // raised copy reads its tuned copy's entries in the tuning and sits kRaisedSt higher.
  void prepareVoice(size_t v, const Plan& plan, const Block& b) {
    if (gain_[v] <= 0.0f && plan.voice[v] <= 0.0f) {
      if (live_[v]) retire(v);
      return;
    }
    const CrowdTuning& t = b.t;
    const size_t src = v % kVoices;
    const bool raised = v >= kVoices;
    const float keep = b.keep[raised ? 1 : 0];
    live_[v] = true;
    ++active_;
    const bool roll = b.levelDb > 0.0f;
    const bool landed = lag_[v].settle(b.view, lagTarget(t, v, b.lagMul), b.scatterSmp, b.period, roll);
    if (landed) voice_[v].markReset();

    // The loudness draw changes only at a landing, so its gain is worked out again only then or when
    // the level scatter moves.
    if (landed || b.levelDb != levelDb_[v]) {
      levelMul_[v] = roll ? dbToGainFast(lag_[v].levelUnit() * b.levelDb) : 1.0f;
      levelDb_[v] = b.levelDb;
    }
    float drift = drift_[v].step(driftRng_[v], b.driftPeak, b.driftHoldS, b.slew, b.n);
    if (b.mob2 && !raised) drift += vibratoCents(v, landed, b);
    const float offset = raised ? t.pitchSt[src] + crowd_detail::kRaisedSt : t.pitchSt[src];
    const float oct = offset * b.pitchWidth * (1.0f / 12.0f) + drift * (1.0f / 1200.0f) + b.corr;
    voice_[v].prepare(b.period, exp2Fast((oct + contourOctaves(src, t, b.pitchWidth)) * keep),
                      exp2Fast(t.formantSt[src] * b.width * (1.0f / 12.0f) * keep), lag_[v].samples(), b.n);
  }

  // Mob 2's vibrato on tuned copy v this block, in cents. A gap that stays quiet kMob2WordGapMs past
  // its landing ends a word. The vibrato drops to 0 there, where the copy is quiet, and holds. At the
  // onset that ends such a gap its phase starts from 0 and its depth fades in over the copy's fade
  // time on a square law. A shorter gap, a dip inside a word, leaves the vibrato running.
  float vibratoCents(size_t v, bool landed, const Block& b) {
    if (landed) vibGap_[v] = 0.0f;
    if (lag_[v].onset()) {
      vibGap_[v] = -1.0f;
      vibHeld_[v] = false;
    } else if (vibGap_[v] >= 0.0f && !vibHeld_[v]) {
      vibGap_[v] += static_cast<float>(b.n);
      if (vibGap_[v] >= crowd_detail::kMob2WordGapMs * crowd_detail::kSmpPerMs) {
        vibHeld_[v] = true;
        vibPhase_[v] = 0u;
        vibAge_[v] = 0.0f;
      }
    }
    if (vibHeld_[v]) return 0.0f;
    const float fade = crowd_detail::kMob2VibFadeMs[v] * crowd_detail::kSmpPerMs;
    const float x = vibAge_[v] < fade ? vibAge_[v] / fade : 1.0f;
    const float cents = crowd_detail::kMob2VibCents[v] * b.spread * x * x * sinTurns(vibPhase_[v]);
    vibPhase_[v] += vibStep_[v];
    vibAge_[v] = vibAge_[v] < fade ? vibAge_[v] + static_cast<float>(b.n) : fade;
    return cents;
  }

  // How far copy v's pitch moves from the lead's, in octaves: (contour - 1) of Adam's deviation.
  // The copy then sits contour times as far from his mean as he does. Contour 1 adds exactly 0. The
  // DIVERGE factor pulls the contour toward 1, so it is exactly 1 at the bottom of DIVERGE and
  // 1 + (contour - 1) * kWidthPitchTop at DIVERGE 1.
  float contourOctaves(size_t v, const CrowdTuning& t, float factor) const {
    const float c = clampf(t.contour[v], crowd_detail::kMinContour, crowd_detail::kMaxContour);
    return c == 1.0f ? 0.0f : (c - 1.0f) * factor * contour_.deviation();
  }

  // One chunk of the call: kBlock samples at most.
  struct Chunk {
    const VoiceRing& ring;
    int first;  // ring index of the chunk's first sample
    int n;
  };

  // One copy's part in a chunk's mix: its rendered voice, its gain ramping to the block-end target,
  // and its tuned copy's shelf weights.
  struct MixLane {
    const float* raw;
    float g;
    float step;
    float wh;
    float wb;
  };

  static constexpr size_t kLanes = 4;  // copies mixed in one pass

  MixLane renderLane(size_t v, const Chunk& k, float target) {
    std::array<float, kBlock>& raw = raw_[v];
    raw.fill(0.0f);
    voice_[v].render(k.ring, k.first, k.n, raw.data());
    const crowd_detail::ShelfWeights& w = shelfWeights_[v % kVoices];
    return {raw.data(), gain_[v], (target - gain_[v]) / static_cast<float>(k.n), w.high, w.band};
  }

  // Adds K copies into crowd and into the two sums the shared shelf filters, in one pass that takes
  // the copies in lane order at each sample, so every sum adds its terms in copy order. The three
  // sums stay in registers across the K copies instead of a load and a store per copy.
  template <size_t K>
  void mixLanes(const MixLane* from, float* crowd, int n) {
    std::array<MixLane, K> lanes;
    for (size_t j = 0; j < K; ++j) lanes[j] = from[j];
    for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
      float c = crowd[i];
      float h = high_[i];
      float b = band_[i];
      for (MixLane& l : lanes) {
        l.g += l.step;
        const float y = l.g * l.raw[i];
        c += y;
        h += l.wh * y;
        b += l.wb * y;
      }
      crowd[i] = c;
      high_[i] = h;
      band_[i] = b;
    }
  }

  // Renders each live copy and mixes them in copy order, kLanes at a time. True when any copy is live.
  bool mixCopies(const Plan& plan, const Chunk& k, float* crowd) {
    std::array<MixLane, kLanes> group{};
    size_t count = 0;
    bool live = false;
    for (size_t v = 0; v < kCopies; ++v) {
      if (!live_[v]) continue;
      live = true;
      group[count++] = renderLane(v, k, plan.voice[v] * levelMul_[v]);
      if (count < kLanes) continue;
      mixLanes<kLanes>(group.data(), crowd, k.n);
      count = 0;
    }
    if (count == 1) mixLanes<1>(group.data(), crowd, k.n);
    if (count == 2) mixLanes<2>(group.data(), crowd, k.n);
    if (count == 3) mixLanes<3>(group.data(), crowd, k.n);
    return live;
  }

  // The shared shelf: one two-input biquad on the weighted sums, added into the crowd sum. With no
  // copy live and the state at 0 it has nothing to add. Its tail on silence would settle into a
  // limit cycle of a few subnormals and never reach 0, so with no copy live a state under
  // kShelfFloor goes to 0.
  void shelfCrowd(float* crowd, int n, bool live) {
    if (!live && shelfZ1_ == 0.0f && shelfZ2_ == 0.0f) return;
    const crowd_detail::ShelfBasis b = shelf_;
    float z1 = shelfZ1_;
    float z2 = shelfZ2_;
    for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
      const float xh = high_[i];
      const float xb = band_[i];
      const float y = b.h0 * xh + b.q0 * xb + z1;
      z1 = b.h1 * xh - b.a1 * y + z2;
      z2 = b.h0 * xh - b.q0 * xb - b.a2 * y;
      crowd[i] += y;
    }
    const bool settled = !live && fabsf(z1) < kShelfFloor && fabsf(z2) < kShelfFloor;
    shelfZ1_ = settled ? 0.0f : z1;
    shelfZ2_ = settled ? 0.0f : z2;
  }

  // The low cut on the crowd sum, in place. The filter runs on a local copy so its state stays in
  // registers. Its numerator sums to exactly 0, a double zero at DC, so the sum needs no DC blocker.
  void filterCrowd(float* crowd, int n) {
    Biquad hp = hp_;
    for (int i = 0; i < n; ++i) crowd[i] = hp.run(crowd[i]);
    hp_ = hp;
  }

  // Lead and crowd ramp linearly from the last chunk's values to the plan. A voice's gain target is
  // its plan gain times the loudness its last gap drew, which is exactly 1 with level scatter off.
  void renderChunk(const float* in, float* out, const Plan& plan, const Chunk& k) {
    std::array<float, kBlock> crowd{};
    high_.fill(0.0f);
    band_.fill(0.0f);
    const bool live = mixCopies(plan, k, crowd.data());
    shelfCrowd(crowd.data(), k.n, live);
    filterCrowd(crowd.data(), k.n);
    const float inv = 1.0f / static_cast<float>(k.n);
    for (int i = 0; i < k.n; ++i) {
      const float f = static_cast<float>(i + 1) * inv;
      const float wet = crowd[static_cast<size_t>(i)];
      out[i] = (lead_ + (plan.lead - lead_) * f) * in[i] + (wet_ + (plan.wet - wet_) * f) * wet;
    }
    for (size_t v = 0; v < kCopies; ++v) gain_[v] = plan.voice[v] * levelMul_[v];
    lead_ = plan.lead;
    wet_ = plan.wet;
  }

  std::array<FormantPsolaVoice, kCopies> voice_{};
  Biquad hp_;
  crowd_detail::ShelfBasis shelf_;
  std::array<crowd_detail::ShelfWeights, kVoices> shelfWeights_{};  // per tuned copy; its raised copy shares them
  std::array<std::array<float, kBlock>, kCopies> raw_{};  // each copy's voice for the chunk being mixed
  std::array<float, kBlock> high_{};  // the copies weighted by wh, for the shared shelf's high-pass
  std::array<float, kBlock> band_{};  // the copies weighted by wb, for its band-pass
  float shelfZ1_ = 0.0f;
  float shelfZ2_ = 0.0f;
  std::array<float, kVoices> shelfDb_{};
  std::array<float, kCopies> gain_{};
  std::array<bool, kCopies> live_{};
  std::array<uint32_t, kVoices> vibPhase_{};  // Mob 2's vibrato, per tuned copy, in sinTurns units
  std::array<uint32_t, kVoices> vibStep_{};   // its phase step per block of coefN_ samples
  std::array<float, kVoices> vibAge_{};       // samples since the onset, up to the fade time
  std::array<float, kVoices> vibGap_ = {-1.0f, -1.0f, -1.0f, -1.0f};  // samples since the landing, -1 out of a gap
  std::array<bool, kVoices> vibHeld_{};       // at 0 from a word's end to the onset after it
  std::array<crowd_detail::CopyLag, kCopies> lag_{};
  std::array<crowd_detail::PitchDrift, kCopies> drift_{};
  std::array<crowd_detail::XorShift, kCopies> driftRng_{};
  std::array<float, kCopies> levelMul_ = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, kCopies> levelDb_{};  // the level scatter levelMul_ was worked out at
  crowd_detail::ContourTrack contour_;
  float lead_ = 1.0f;
  float wet_ = 0.0f;
  float hpHz_ = 0.0f;
  float shelfHz_ = 0.0f;
  float widthSm_ = 1.0f;
  float driftSm_ = 1.0f;  // Mob 2's DRIFT, smoothed
  crowd_detail::VoicesLaw law_;
  LawKey lawKey_;
  std::array<Glide, kCopies> weightGlide_{};
  Glide leadGlide_;
  Glide mulGlide_;
  bool glideSet_ = false;
  float smoothCoef_ = 0.0f;  // the 20 ms one-pole, per block
  float slewCoef_ = 0.0f;
  float riseCoef_ = 0.0f;   // phrase level, rising
  float fallCoef_ = 0.0f;   // phrase level, falling
  float perSample_ = 0.0f;  // 1 / n
  int coefN_ = -1;
  float trimDb_ = 0.0f;    // the trim trimGain_ was worked out at
  float trimGain_ = 1.0f;
  float floorDb_ = 0.0f;   // the gapFloorDb floorSq_ was worked out at
  float floorSq_ = 1.0f;
  float heldPeriod_ = 0.0f;
  float phraseSq_ = 0.0f;  // the recent phrase level, a mean square
  int unvoiced_ = 0;       // samples since the tracker last read a voiced frame, up to kPlainAfterMax
  std::array<float, 2> keep_ = {1.0f, 1.0f};  // the tuned copies' move share, then the raised copies'
  bool keepSet_ = false;
  uint8_t active_ = 0;
  bool widthSet_ = false;
  bool driftSet_ = false;
  bool hpSet_ = false;
  bool shelfSet_ = false;
  bool idle_ = true;
};

}  // namespace cv
