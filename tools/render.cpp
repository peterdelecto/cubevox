// cubevox-render: loop in, the front half of VOX EQ, gates + autotune + formant + octave + crowd +
// unison + slapback + growl + distortion + gate + lofi + bloom + reverb, then the back half of VOX EQ
// and the output ceiling like the box, processed mono 48 kHz f32 WAV out.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../third_party/miniaudio.h"
#include "engine/autotune_fx.h"
#include "engine/bloom.h"
#include "engine/crowd.h"
#include "engine/distortion.h"
#include "engine/formant_follower.h"
#include "engine/formant_fx.h"
#include "engine/gate.h"
#include "engine/growl.h"
#include "engine/lofi.h"
#include "engine/octave_fx.h"
#include "engine/pitch_params.h"
#include "engine/polish.h"
#include "engine/reverb.h"
#include "engine/slapback.h"
#include "engine/soft_ceiling.h"
#include "engine/unison.h"

namespace {

struct TuningField {
  const char* name;
  float* value;
};

struct RenderParams {
  bool inGateOn = false;
  bool gateOn = false;
  bool autotuneOn = false;  // Autotune runs, so Autotune's tracker serves the formant, crowd and growl cards
  bool formantOn = false;   // also true for Apraxia
  bool octaveOn = false;
  bool crowdOn = false;
  bool growlOn = false;
  bool unisonOn = false;
  bool slapOn = false;
  bool distOn = false;
  bool lofiOn = false;
  bool bloomOn = false;
  bool reverbOn = false;
  bool eqOn = true;
  bool ceilingStats = false;  // print the output ceiling's peaks after the render
  const char* drivePath = nullptr;  // where Apraxia's drive goes, one value a sample
  const char* shiftPath = nullptr;  // where the glided SHIFT goes, -7..0 st onto -1..0, one value a sample
  cv::GateParams inGate;
  cv::GateParams gate;
  cv::PitchCardParams pitchCards;
  cv::FormantFollowerParams follower;
  cv::CrowdParams crowd;
  cv::UnisonParams unison;
  cv::SlapbackParams slapback;
  cv::DistortionParams distortion;
  cv::GrowlParams growl;
  cv::LofiParams lofi;
  cv::BloomParams bloom;
  cv::ReverbParams reverb;
  cv::PolishParams eq;
};

static_assert(cv::kKeyCount == 12, "the usage text names keys 0..11");

int usage() {
  std::fprintf(stderr,
               "usage: cubevox-render <in> <out.wav> [--depth <0..1>] [--on 0|1] "
               "[--tuning k=v ...]\n"
               "       [--autotune] [--atkey <0..11>] [--response <1..500 ms>]\n"
               "       [--octave <-12..12>] [--omix <0..1>, default 0.1, the box's start]\n"
               "       [--formant <-7..7>] [--fmix <0..1>]\n"
               "       [--follower <-12..12>, RANGE] [--sensitivity <0..1>] [--follower-drive <drive.wav>]\n"
               "       [--follower-shift <shift.wav>]\n"
               "       [--crowd] [--width <0..1>] [--voices <0..9>]\n"
               "       [--slap <0..1>] [--drive <0..1>] [--tone <0..1>]\n"
               "       [--growl <0..1>, GROWL] [--gdrive <0..1>, the Growl card's DRIVE, default 0.5]\n"
               "       [--reverb spring|chasm] [--time <0..1>] [--mix <0..1>]\n"
               "       [--ingate <-80..-30 dB>] [--gate <-70..-10 dB>]\n"
               "       [--lofi <0..1>, the AMOUNT pot] [--lofi-tone <0..1>, the TONE pot]\n"
               "       [--amount <0..1>] [--length <20..3000 ms>]\n"
               "       [--vox-eq off|on|cuts] [--ceiling-stats]\n"
               "       [--eqdip <150..600 hz>,<-6..0 db>] [--eqpres <2000..6000 hz>,<0..6 db>]\n"
               "  autotune runs with --autotune, formant with --formant, octave with --octave, in that order, each on the\n"
               "  output of the one before it\n"
               "  formant sets SHIFT in whole semitones with --formant and the MIX with --fmix (default 1); with --autotune it\n"
               "  sits directly behind it, reads its tracker and cuts its voice from the tuned note\n"
               "  --follower runs the Apraxia card, the formant voice alone with SHIFT swept on each syllable, down for a\n"
               "  negative RANGE and up for a positive one; its value is RANGE in semitones, any value from -12 to +12 (the\n"
               "  box's default is -5), and --sensitivity is the SENSITIVITY pot (default 0.4); the card pins the voice's\n"
               "  MIX at 1 and overrides --formant's SHIFT and --fmix.\n"
               "  --follower-drive writes the card's drive, 0 to 1, and --follower-shift the voice's glided SHIFT at 1/12\n"
               "  of full scale a semitone (-12 st at -1, +12 st at +1), each as a mono WAV beside the render; the tuning\n"
               "  key decayMs sets the sweep's fall\n"
               "  crowd runs only with --crowd; --width sets DIVERGE (default 1) and --voices the VOICES step, 0 to 8 copies or 9 for MOB (default 4)\n"
               "  unison runs only with --on 1 or --depth; slapback runs only with --slap\n"
               "  distortion runs only with --drive; reverb runs only with --reverb\n"
               "  growl runs with --growl or --gdrive (the other one defaults to 0.5) where slot 6 sits, after slapback; it\n"
               "  cuts its voice from a ring of its own input, which is also its dry path, and reads Autotune's tracker\n"
               "  with --autotune and a tracker of its own without it\n"
               "  lofi runs with --lofi or --lofi-tone (the other one defaults to 0.5), where the gate sits\n"
               "  at least one stage must run; order is eq front, ingate, autotune, formant, octave, crowd, unison, slapback,\n"
               "  growl, distortion,\n"
               "  gate, lofi, bloom, reverb, eq back\n"
               "  bloom, the Chatter card, runs with --amount (AMOUNT, default 0.5) or --length (LENGTH, default 300 ms); with --ingate its\n"
               "  word key is the input gate's open state, otherwise it listens to its own input; it cuts its slices from\n"
               "  its own input, which is also its dry path\n"
               "  the box's output ceiling, bit-exact below -2 dBFS, always runs after the last stage\n"
               "  ingate and gate run only with their flag; a flag enables that gate at that threshold\n"
               "  VOX EQ's front half (40 Hz subsonic high-pass, de-esser, and with cuts the 70 Hz high-pass and 6 kHz low-pass) runs ahead of every\n"
               "  stage and its back half (300 Hz dip, 3.5 kHz presence, trim) runs after the last stage\n"
               "  the eq runs by default at cuts (the box's menu default is off); --vox-eq off switches both halves out\n"
               "  for A/B listening, --vox-eq on runs the eq without the cuts\n"
               "  --ceiling-stats prints the output ceiling's peaks in and out and the samples past its knee\n"
               "  k: baseDelayMs0 baseDelayMs1 lfoHz0 lfoHz1 swingMinMs swingMaxMs wetMaxDb trimDb\n"
               "     topFrom topSwingGain topWetDb\n"
               "     (the Unison keys: the taps must stay on the line, lfoHz 0..20, the dB keys -24..24, topFrom 0..1,\n"
               "     topSwingGain 0..2)\n"
               "     octGrainWindowMs (20..80) octTrimDb (-24..24)\n"
               "     slapTimeMs (60..250) slapLowpassHz (500..12000) slapFeedback (0..0.5) slapWetMaxDb slapTrimDb (-24..24)\n"
               "     distInputHpHz distS1BassHz distS1BassDb distS1LpHz distGain1Min distGain1Max distTopBoostDb\n"
               "     distTopBoostFrom distStackBassHz\n"
               "     distStackBassDb distStackTrebleHz distStackTrebleDb distStackLossDb distS2HpHz\n"
               "     distS2LpHz distGain2Min distGain2Max distRailAsym distRailSoft distTrebleCutHz "
               "distTrebleCutDb\n"
               "     distToneMinDb distToneMaxDb distBassPeakHz distBassPeakDb distBassPeakQ distTrimDb "
               "distFadeDrive\n"
               "     (the dist dB keys -24..24 and distTopBoostDb 0..24, the gains 0.1..1000, the corners 20..21600,\n"
               "     distRailAsym 0..0.5, distRailSoft 0.01..1, distTopBoostFrom 0..1, distBassPeakQ 0.1 and up,\n"
               "     distFadeDrive 0.0001..1)\n"
               "     chmBassCutHz chmBassCutHzTop (20..21600) chmWobbleDepth (0..64) chmWobbleRateHz (0..10)\n"
               "     chmInputTrim (0..4) chmWetDb (-36..36)\n"
               "     springMs1 (10..37.6) springMs2 (10..45.6) springMs3 (10..56.6) springFeedbackLo springFeedbackHi (0.1..0.97) springDiffuse (0..0.9)\n"
               "     springChirpSections (0..32) springChirpA (0..0.95) springTrebleLossHz springBassCutHz\n"
               "     springReturnLpHz (the three corners 20..10800, the loops' Nyquist is 12000) springWobbleDepth (0..32) springWobbleRateHz (0..10)\n"
               "     springWetDb (-24..24) springRestLevel (0..0.001, a silent input rests the springs below this; 0 never)\n"
               "     eqDipQ eqPresenceQ (0.1..10) eqTrimDb (-12..12)\n"
               "     eqEssBandLoHz eqEssBandHiHz eqEssThresholdDb eqEssMaxCutDb eqEssReleaseMs eqEssNotchHz eqEssNotchQ\n"
               "     atTrimDb (-24..24) atGrainDelay fmTrimDb (-24..24, added to every entry of the SHIFT trim table)\n"
               "     crowdVoiceDb (-24..0) crowdTrimDb crowdTrimBottomDb (-12..12) crowdFormantSt0..3 crowdPitchSt0..3\n"
               "     (-12..12) crowdShelfDb0..3 (-12..12) crowdShelfHz crowdHpHz (20..21600) crowdDelayMs0..3 (%g..%d)\n"
               "     crowdLagScale (0.5..2) crowdScatterMs (0..%g) crowdDriftCents (0..50) crowdMaxLagMs (0..%d)\n"
               "     crowdWidthKnee (0.02..0.9) crowdWidthKneeFrac (0..1)\n"
               "     crowdWidthLagBottom (0.2..1) crowdWidthLagMs (0..60)\n"
               "     crowdLevelScatterDb (0..4) crowdContour0..3 (0.6..1.4)\n"
               "     growlPitchSt growlFormantSt (-12..12) growlHardTop (0..32) growlThirdTop (0..1)\n"
               "     growlLowShelfHz growlPeakHz (20..21600) growlLowShelfDb growlPeakDb (-24..24)\n"
               "     growlDriveDb (0..48) growlMakeupSat (1..1000) growlRailSoft (0.01..1) growlRailAsym (0..0.5)\n"
               "     growlTrimDb (-24..24)\n"
               "     decayMs (10..2000, default 300), the time Apraxia's sweep takes to fall back to 0\n"
               "  every value must be finite, and a key outside its range is refused, whichever stage it belongs to\n"
               "     bloomTrimDb (-24..24) bloomFirstCaptureMs (1..500) bloomSkipDb (-60..0)\n"
               "     bloomDriveWord (0.1..20) bloomDriveBell (0..20) bloomDrift (0..0.2)\n"
               "     ingAttackMs ingHoldMs ingReleaseMs ingRangeDb ingKneeDb ingDetectorHpHz ingHysteresisDb\n"
               "     ingDetectorAttackMs ingDetectorReleaseMs\n"
               "     gtAttackMs gtHoldMs gtReleaseMs gtRangeDb gtKneeDb gtDetectorHpHz gtHysteresisDb\n"
               "     gtDetectorAttackMs gtDetectorReleaseMs\n"
               "     (the gate keys: the attack and release times above 0, hold 0..10000 ms, the detector times 0 and up,\n"
               "     the range 0 dB and under, the knee, hysteresis and detector high-pass 0 and up)\n"
               "     lofiFcLoHz lofiFcHiHz lofiHpStartHz lofiHpEndRatio lofiLpStartHz lofiLpEndRatio lofiHonkDb lofiHonkQ\n"
               "     lofiDriveDb lofiHoldMaxHz lofiHoldMinHz lofiBitsMax lofiBitsMin lofiTrimDb\n"
               "     lofiMakeup<tone 0..4>_<amount 0..6> (dB, the makeup table by TONE 0, 25 ... 100 %% and the law's AMOUNT 0, 10 ... 60 %%)\n"
               "  --time and --mix are the panel's TIME and MIX positions for the selected reverb engine (default 0.5)\n",
               static_cast<double>(cv::crowd_detail::kMinDelayMs), cv::kCrowdMaxLagMs,
               static_cast<double>(cv::crowd_detail::kMaxScatterMs), cv::kCrowdMaxLagMs);
  return 2;
}

// A finite number and nothing after it. NaN passes every < and > range check, so it is refused here.
bool parseFloat(const char* s, float* out) {
  char* end = nullptr;
  *out = std::strtof(s, &end);
  return end != s && *end == '\0' && std::isfinite(*out);
}

// "hz,db", each a finite number.
bool parsePair(const char* s, float* hz, float* db) {
  const char* comma = std::strchr(s, ',');
  char head[32];
  if (comma == nullptr || static_cast<size_t>(comma - s) >= sizeof head) return false;
  std::memcpy(head, s, static_cast<size_t>(comma - s));
  head[comma - s] = '\0';
  return parseFloat(head, hz) && parseFloat(comma + 1, db);
}

bool inside(float v, float lo, float hi) { return v >= lo && v <= hi; }

// The Crowd card's per-copy keys, each inside the range the engine takes.
bool crowdCopiesInRange(const cv::CrowdTuning& t) {
  namespace cd = cv::crowd_detail;
  constexpr float kTopLag = static_cast<float>(cv::kCrowdMaxLagMs);
  for (size_t v = 0; v < cd::kVoices; ++v) {
    if (!inside(t.pitchSt[v], -cd::kMaxOffsetSt, cd::kMaxOffsetSt)) return false;
    if (!inside(t.formantSt[v], -cd::kMaxOffsetSt, cd::kMaxOffsetSt)) return false;
    if (!inside(t.shelfDb[v], -cd::kMaxShelfDb, cd::kMaxShelfDb)) return false;
    if (!inside(t.delayMs[v], cd::kMinDelayMs, kTopLag)) return false;
    if (!inside(t.contour[v], cd::kMinContour, cd::kMaxContour)) return false;
  }
  return true;
}

// Every Crowd key refuses a value outside the range the engine clamps to, so a typo in a key does not
// pass for a setting.
bool crowdTuningInRange(const cv::CrowdTuning& t) {
  namespace cd = cv::crowd_detail;
  return crowdCopiesInRange(t) && inside(t.voiceDb, cd::kMinVoiceDb, cd::kMaxVoiceDb) &&
         inside(t.trimDb, -cd::kMaxTrimDb, cd::kMaxTrimDb) && inside(t.trimBottomDb, -cd::kMaxTrimDb, cd::kMaxTrimDb) &&
         inside(t.shelfHz, cv::kFilterMinHz, cv::kFilterMaxHz) && inside(t.hpHz, cv::kFilterMinHz, cv::kFilterMaxHz) &&
         inside(t.lagScale, cd::kMinLagScale, cd::kMaxLagScale) &&
         inside(t.maxLagMs, 0.0f, static_cast<float>(cv::kCrowdMaxLagMs)) &&
         inside(t.scatterMs, 0.0f, cd::kMaxScatterMs) && inside(t.driftCents, 0.0f, cd::kMaxDriftCents) &&
         inside(t.levelScatterDb, 0.0f, cd::kMaxLevelScatterDb) &&
         inside(t.widthKnee, cd::kMinWidthKnee, cd::kMaxWidthKnee) &&
         inside(t.widthKneeFrac, 0.0f, cd::kMaxWidthKneeFrac) &&
         inside(t.widthLagBottom, cd::kMinLagBottom, cd::kMaxLagBottom) &&
         inside(t.widthLagMs, 0.0f, cd::kMaxWidthLagMs);
}

// Every Lo-fi key refuses a value outside the range the engine takes.
bool lofiTuningInRange(const cv::LofiTuning& t) {
  using Lofi = cv::Lofi;
  const auto hold = [](float hz) { return inside(hz / static_cast<float>(cv::kSampleRate), Lofi::kMinHoldRatio, 1.0f); };
  for (const auto& row : t.makeupDb)
    for (const float m : row)
      if (!inside(m, -24.0f, 36.0f)) return false;
  return inside(t.fcLoHz, cv::kFilterMinHz, cv::kFilterMaxHz) && inside(t.fcHiHz, cv::kFilterMinHz, cv::kFilterMaxHz) &&
         inside(t.hpStartHz, cv::kFilterMinHz, cv::kFilterMaxHz) &&
         inside(t.lpStartHz, cv::kFilterMinHz, cv::kFilterMaxHz) && inside(t.hpEndRatio, 0.05f, 4.0f) &&
         inside(t.lpEndRatio, 0.25f, 20.0f) && inside(t.honkDb, -24.0f, 24.0f) && inside(t.honkQ, cv::kPeakMinQ, 20.0f) &&
         inside(t.driveDb, -24.0f, 48.0f) && hold(t.holdMaxHz) &&
         hold(t.holdMinHz) && inside(t.bitsMax, Lofi::kMinBits, Lofi::kMaxBits) &&
         inside(t.bitsMin, Lofi::kMinBits, Lofi::kMaxBits) && inside(t.trimDb, -24.0f, 24.0f);
}

// Every gate key refuses a value the engine would clamp or turn into a degenerate time. The two
// detector times may be 0, which follows at once.
bool gateTuningInRange(const cv::GateTuning& t) {
  return t.attackMs > 0.0f && t.releaseMs > 0.0f && inside(t.holdMs, 0.0f, cv::kGateHoldMaxMs) &&
         t.rangeDb <= 0.0f && t.kneeDb >= 0.0f && inside(t.detectorHpHz, 0.0f, cv::kFilterMaxHz) &&
         t.hysteresisDb >= 0.0f && t.detectorAttackMs >= 0.0f && t.detectorReleaseMs >= 0.0f;
}

// Every Unison key refuses a value outside the range the engine takes, and a delay and swing that
// would carry a tap off the line.
bool unisonTuningInRange(const cv::UnisonTuning& t) {
  using Uni = cv::Unison;
  const auto level = [](float v) { return inside(v, -Uni::kLevelLimitDb, Uni::kLevelLimitDb); };
  return Uni::tapsFit(t) && inside(t.lfoHz[0], 0.0f, Uni::kLfoMaxHz) && inside(t.lfoHz[1], 0.0f, Uni::kLfoMaxHz) &&
         t.swingMinMs >= 0.0f && t.swingMaxMs >= 0.0f && level(t.wetMaxDb) && level(t.trimDb) && level(t.topWetDb) &&
         inside(t.topFrom, 0.0f, 1.0f) && inside(t.topSwingGain, 0.0f, Uni::kTopSwingGainMax);
}

// Every Echo key refuses a value outside the range the engine clamps to.
bool slapbackTuningInRange(const cv::SlapbackTuning& t) {
  using Echo = cv::Slapback;
  return inside(t.timeMs, Echo::kTimeMinMs, Echo::kTimeMaxMs) &&
         inside(t.lowpassHz, Echo::kCutoffMinHz, Echo::kCutoffMaxHz) && inside(t.feedback, 0.0f, Echo::kFeedbackMax) &&
         inside(t.wetMaxDb, -Echo::kLevelLimitDb, Echo::kLevelLimitDb) &&
         inside(t.trimDb, -Echo::kLevelLimitDb, Echo::kLevelLimitDb);
}

// Every Distortion key refuses a value outside the range the engine clamps to.
bool distortionTuningInRange(const cv::DistortionTuning& t) {
  using Dist = cv::Distortion;
  const auto db = [](float v) { return inside(v, -Dist::kDbLimit, Dist::kDbLimit); };
  const auto hz = [](float v) { return inside(v, cv::kFilterMinHz, cv::kFilterMaxHz); };
  const auto gain = [](float v) { return inside(v, Dist::kGainMin, Dist::kGainMax); };
  return hz(t.inputHpHz) && hz(t.s1BassHz) && db(t.s1BassDb) && hz(t.s1LpHz) && gain(t.gain1Min) &&
         gain(t.gain1Max) && inside(t.topBoostDb, 0.0f, Dist::kDbLimit) && inside(t.topBoostFrom, 0.0f, 1.0f) &&
         hz(t.stackBassHz) && db(t.stackBassDb) && hz(t.stackTrebleHz) && db(t.stackTrebleDb) &&
         db(t.stackLossDb) && hz(t.s2HpHz) && hz(t.s2LpHz) && gain(t.gain2Min) && gain(t.gain2Max) &&
         inside(t.railAsym, 0.0f, Dist::kRailAsymMax) && inside(t.railSoft, Dist::kRailSoftMin, 1.0f) &&
         hz(t.trebleCutHz) && db(t.trebleCutDb) && db(t.toneMinDb) && db(t.toneMaxDb) && hz(t.bassPeakHz) &&
         db(t.bassPeakDb) && t.bassPeakQ >= cv::kPeakMinQ && db(t.trimDb) && inside(t.fadeDrive, Dist::kFadeDriveMin, 1.0f);
}

// Every Growl key refuses a value outside the range the engine clamps to.
bool growlTuningInRange(const cv::GrowlTuning& t) {
  namespace gd = cv::growl_detail;
  const auto hz = [](float v) { return inside(v, cv::kFilterMinHz, cv::kFilterMaxHz); };
  const auto shelf = [](float v) { return inside(v, -gd::kMaxShelfDb, gd::kMaxShelfDb); };
  return inside(t.pitchSt, -gd::kMaxSt, gd::kMaxSt) && inside(t.formantSt, -gd::kMaxSt, gd::kMaxSt) &&
         inside(t.hardTop, 0.0f, gd::kMaxHardTop) && inside(t.thirdTop, 0.0f, 1.0f) && hz(t.lowShelfHz) &&
         hz(t.hpHz) && hz(t.peakHz) && inside(t.peakQ, cv::kPeakMinQ, gd::kMaxPeakQ) && inside(t.jitterTop, 0.0f, 1.0f) &&
         shelf(t.lowShelfDb) && shelf(t.peakDb) && inside(t.driveDb, 0.0f, gd::kMaxDriveDb) &&
         inside(t.makeupSat, gd::kMinMakeupSat, gd::kMaxMakeupSat) && inside(t.railSoft, gd::kMinRailSoft, 1.0f) &&
         inside(t.railAsym, 0.0f, gd::kMaxRailAsym) && inside(t.trimDb, -gd::kMaxTrimDb, gd::kMaxTrimDb);
}

// Every Spring key refuses a value outside the range the engine takes, so a typo in a key does not
// pass for a setting.
bool springTuningInRange(const cv::SpringTuning& t) {
  namespace sd = cv::spring_detail;
  constexpr float kMinMs = 10.0f;
  return inside(t.springMs1, kMinMs, sd::kMaxMs1) && inside(t.springMs2, kMinMs, sd::kMaxMs2) &&
         inside(t.springMs3, kMinMs, sd::kMaxMs3) &&
         inside(t.feedbackLo, sd::kFeedbackMin, sd::kFeedbackMax) &&
         inside(t.feedbackHi, sd::kFeedbackMin, sd::kFeedbackMax) && inside(t.diffuse, 0.0f, 0.9f) &&
         inside(t.chirpA, 0.0f, 0.95f) && inside(t.trebleLossHz, cv::kFilterMinHz, sd::kMaxFilterHz) &&
         inside(t.bassCutHz, cv::kFilterMinHz, sd::kMaxFilterHz) &&
         inside(t.returnLpHz, cv::kFilterMinHz, sd::kMaxFilterHz) && inside(t.wobbleDepth, 0.0f, sd::kMaxWobble) &&
         inside(t.wobbleRateHz, 0.0f, sd::kMaxWobbleHz) &&
         inside(t.wetDb, -sd::kMaxWetDb, sd::kMaxWetDb) &&
         inside(t.restLevel, 0.0f, 1.0e-3f);
}

// Every Chasm key refuses a value outside the range the engine takes, so a typo in a key does not
// pass for a setting.
bool chasmTuningInRange(const cv::ChasmTuning& t) {
  namespace cd = cv::chasm_detail;
  return inside(t.bassCutHz, cv::kFilterMinHz, cv::kFilterMaxHz) &&
         inside(t.bassCutHzTop, cv::kFilterMinHz, cv::kFilterMaxHz) && inside(t.wobbleDepth, 0.0f, cd::kMaxWobble) &&
         inside(t.wobbleRateHz, 0.0f, cd::kMaxWobbleHz) && inside(t.inputTrim, 0.0f, cd::kMaxInputTrim) &&
         inside(t.wetDb, -cd::kMaxWetDb, cd::kMaxWetDb);
}

bool applyTuning(RenderParams& rp, const char* kv) {
  cv::UnisonTuning& t = rp.unison.tuning;
  cv::SlapbackTuning& st = rp.slapback.tuning;
  cv::DistortionTuning& dt = rp.distortion.tuning;
  cv::ChasmTuning& ch = rp.reverb.chasm.tuning;
  cv::SpringTuning& spr = rp.reverb.spring.tuning;
  cv::OctaveTuning& ot = rp.pitchCards.octave.tuning;
  cv::AutotuneTuning& at = rp.pitchCards.autotune.tuning;
  cv::FormantTuning& ft = rp.pitchCards.formant.tuning;
  cv::CrowdTuning& ct = rp.crowd.tuning;
  cv::GrowlTuning& grt = rp.growl.tuning;
  cv::PolishTuning& et = rp.eq.tuning;
  cv::GateTuning& ig = rp.inGate.tuning;
  cv::GateTuning& gt = rp.gate.tuning;
  cv::LofiTuning& lt = rp.lofi.tuning;
  cv::BloomTuning& bt = rp.bloom.tuning;
  cv::FormantFollowerTuning& fft = rp.follower.tuning;
  float springSections = static_cast<float>(spr.chirpSections);
  const TuningField fields[] = {
      {"baseDelayMs0", &t.baseDelayMs[0]}, {"baseDelayMs1", &t.baseDelayMs[1]},
      {"lfoHz0", &t.lfoHz[0]},             {"lfoHz1", &t.lfoHz[1]},
      {"swingMinMs", &t.swingMinMs},       {"swingMaxMs", &t.swingMaxMs},
      {"wetMaxDb", &t.wetMaxDb},   {"trimDb", &t.trimDb},
      {"topFrom", &t.topFrom},             {"topSwingGain", &t.topSwingGain},
      {"topWetDb", &t.topWetDb},
      {"octGrainWindowMs", &ot.grainWindowMs},
      {"octTrimDb", &ot.trimDb},
      {"atTrimDb", &at.trimDb}, {"atGrainDelay", &at.grainDelay}, {"fmTrimDb", &ft.offsetDb},
      {"crowdVoiceDb", &ct.voiceDb},              {"crowdTrimDb", &ct.trimDb},
      {"crowdTrimBottomDb", &ct.trimBottomDb},
      {"crowdFormantSt0", &ct.formantSt[0]},      {"crowdFormantSt1", &ct.formantSt[1]},
      {"crowdFormantSt2", &ct.formantSt[2]},      {"crowdFormantSt3", &ct.formantSt[3]},
      {"crowdPitchSt0", &ct.pitchSt[0]},          {"crowdPitchSt1", &ct.pitchSt[1]},
      {"crowdPitchSt2", &ct.pitchSt[2]},          {"crowdPitchSt3", &ct.pitchSt[3]},
      {"crowdShelfDb0", &ct.shelfDb[0]},          {"crowdShelfDb1", &ct.shelfDb[1]},
      {"crowdShelfDb2", &ct.shelfDb[2]},          {"crowdShelfDb3", &ct.shelfDb[3]},
      {"crowdShelfHz", &ct.shelfHz},
      {"crowdDelayMs0", &ct.delayMs[0]},          {"crowdDelayMs1", &ct.delayMs[1]},
      {"crowdDelayMs2", &ct.delayMs[2]},          {"crowdDelayMs3", &ct.delayMs[3]},
      {"crowdLagScale", &ct.lagScale},
      {"crowdScatterMs", &ct.scatterMs},          {"crowdDriftCents", &ct.driftCents},
      {"crowdHpHz", &ct.hpHz},                    {"crowdMaxLagMs", &ct.maxLagMs},
      {"crowdWidthKnee", &ct.widthKnee},          {"crowdWidthKneeFrac", &ct.widthKneeFrac},
      {"crowdWidthLagBottom", &ct.widthLagBottom}, {"crowdWidthLagMs", &ct.widthLagMs},
      {"crowdLevelScatterDb", &ct.levelScatterDb},
      {"crowdContour0", &ct.contour[0]},          {"crowdContour1", &ct.contour[1]},
      {"crowdContour2", &ct.contour[2]},          {"crowdContour3", &ct.contour[3]},
      {"growlPitchSt", &grt.pitchSt},             {"growlFormantSt", &grt.formantSt},
      {"growlHardTop", &grt.hardTop},             {"growlThirdTop", &grt.thirdTop},
      {"growlLowShelfHz", &grt.lowShelfHz},       {"growlLowShelfDb", &grt.lowShelfDb},
      {"growlPeakHz", &grt.peakHz},               {"growlPeakDb", &grt.peakDb},
      {"growlDriveDb", &grt.driveDb},             {"growlMakeupSat", &grt.makeupSat},
      {"growlRailSoft", &grt.railSoft},           {"growlRailAsym", &grt.railAsym},
      {"growlTrimDb", &grt.trimDb},
      {"decayMs", &fft.decayMs},
      {"slapTimeMs", &st.timeMs},          {"slapLowpassHz", &st.lowpassHz},
      {"slapFeedback", &st.feedback},      {"slapWetMaxDb", &st.wetMaxDb},
      {"slapTrimDb", &st.trimDb},
      {"distInputHpHz", &dt.inputHpHz},       {"distS1BassHz", &dt.s1BassHz},
      {"distS1BassDb", &dt.s1BassDb},         {"distS1LpHz", &dt.s1LpHz},
      {"distGain1Min", &dt.gain1Min},         {"distGain1Max", &dt.gain1Max},
      {"distTopBoostDb", &dt.topBoostDb},
      {"distTopBoostFrom", &dt.topBoostFrom}, {"distStackBassHz", &dt.stackBassHz},
      {"distStackBassDb", &dt.stackBassDb},   {"distStackTrebleHz", &dt.stackTrebleHz},
      {"distStackTrebleDb", &dt.stackTrebleDb}, {"distStackLossDb", &dt.stackLossDb},
      {"distS2HpHz", &dt.s2HpHz},             {"distS2LpHz", &dt.s2LpHz},
      {"distGain2Min", &dt.gain2Min},         {"distGain2Max", &dt.gain2Max},
      {"distRailAsym", &dt.railAsym},
      {"distRailSoft", &dt.railSoft},         {"distTrebleCutHz", &dt.trebleCutHz},
      {"distTrebleCutDb", &dt.trebleCutDb},   {"distToneMinDb", &dt.toneMinDb}, {"distToneMaxDb", &dt.toneMaxDb},
      {"distBassPeakHz", &dt.bassPeakHz},     {"distBassPeakDb", &dt.bassPeakDb},
      {"distBassPeakQ", &dt.bassPeakQ},       {"distTrimDb", &dt.trimDb},
      {"distFadeDrive", &dt.fadeDrive},
      {"chmBassCutHz", &ch.bassCutHz},
      {"chmBassCutHzTop", &ch.bassCutHzTop},  {"chmWobbleDepth", &ch.wobbleDepth},
      {"chmWobbleRateHz", &ch.wobbleRateHz},  {"chmInputTrim", &ch.inputTrim},
      {"chmWetDb", &ch.wetDb},
      {"springMs1", &spr.springMs1},          {"springMs2", &spr.springMs2},
      {"springMs3", &spr.springMs3},
      {"springFeedbackLo", &spr.feedbackLo},  {"springFeedbackHi", &spr.feedbackHi},
      {"springDiffuse", &spr.diffuse},        {"springChirpSections", &springSections},
      {"springChirpA", &spr.chirpA},
      {"springTrebleLossHz", &spr.trebleLossHz}, {"springBassCutHz", &spr.bassCutHz},
      {"springReturnLpHz", &spr.returnLpHz},  {"springWobbleDepth", &spr.wobbleDepth},
      {"springWobbleRateHz", &spr.wobbleRateHz},
      {"springWetDb", &spr.wetDb},            {"springRestLevel", &spr.restLevel},
      {"eqDipQ", &et.dipQ}, {"eqPresenceQ", &et.presenceQ}, {"eqTrimDb", &et.trimDb},
      {"eqEssBandLoHz", &et.deEsser.bandLoHz}, {"eqEssBandHiHz", &et.deEsser.bandHiHz},
      {"eqEssThresholdDb", &et.deEsser.thresholdDb}, {"eqEssMaxCutDb", &et.deEsser.maxCutDb},
      {"eqEssReleaseMs", &et.deEsser.releaseMs}, {"eqEssNotchHz", &et.deEsser.notchHz},
      {"eqEssNotchQ", &et.deEsser.notchQ},
      {"ingAttackMs", &ig.attackMs},          {"ingHoldMs", &ig.holdMs},
      {"ingReleaseMs", &ig.releaseMs},        {"ingRangeDb", &ig.rangeDb},
      {"ingKneeDb", &ig.kneeDb},              {"ingDetectorHpHz", &ig.detectorHpHz},
      {"ingHysteresisDb", &ig.hysteresisDb},
      {"ingDetectorAttackMs", &ig.detectorAttackMs}, {"ingDetectorReleaseMs", &ig.detectorReleaseMs},
      {"gtAttackMs", &gt.attackMs},           {"gtHoldMs", &gt.holdMs},
      {"gtReleaseMs", &gt.releaseMs},         {"gtRangeDb", &gt.rangeDb},
      {"gtKneeDb", &gt.kneeDb},               {"gtDetectorHpHz", &gt.detectorHpHz},
      {"gtHysteresisDb", &gt.hysteresisDb},
      {"gtDetectorAttackMs", &gt.detectorAttackMs}, {"gtDetectorReleaseMs", &gt.detectorReleaseMs},
      {"lofiFcLoHz", &lt.fcLoHz},             {"lofiFcHiHz", &lt.fcHiHz},
      {"lofiHpStartHz", &lt.hpStartHz},       {"lofiHpEndRatio", &lt.hpEndRatio},
      {"lofiLpStartHz", &lt.lpStartHz},       {"lofiLpEndRatio", &lt.lpEndRatio},
      {"lofiHonkDb", &lt.honkDb},             {"lofiHonkQ", &lt.honkQ},
      {"lofiDriveDb", &lt.driveDb},           {"lofiHoldMaxHz", &lt.holdMaxHz},
      {"lofiHoldMinHz", &lt.holdMinHz},       {"lofiBitsMax", &lt.bitsMax},
      {"lofiBitsMin", &lt.bitsMin},           {"lofiTrimDb", &lt.trimDb},
      {"bloomTrimDb", &bt.trimDb},            {"bloomFirstCaptureMs", &bt.firstCaptureMs},
      {"bloomSkipDb", &bt.skipDb},
      {"bloomDriveWord", &bt.driveWord},      {"bloomDriveBell", &bt.driveBell},
      {"bloomDrift", &bt.drift},
  };
  const char* eq = std::strchr(kv, '=');
  if (!eq) return false;
  if (std::strncmp(kv, "lofiMakeup", 10) == 0) {
    // lofiMakeup<tone row 0..4>_<amount column 0..10>=<dB>
    int row = -1, col = -1, used = 0;
    if (std::sscanf(kv, "lofiMakeup%d_%d=%n", &row, &col, &used) != 2 || used == 0) return false;
    if (row < 0 || row >= static_cast<int>(cv::kLofiMakeupTones) || col < 0 ||
        col >= static_cast<int>(cv::kLofiMakeupAmounts))
      return false;
    return parseFloat(kv + used, &lt.makeupDb[static_cast<size_t>(row)][static_cast<size_t>(col)]) &&
           lofiTuningInRange(lt);
  }
  const size_t keyLen = static_cast<size_t>(eq - kv);
  for (const TuningField& f : fields) {
    if (std::strlen(f.name) != keyLen || std::strncmp(f.name, kv, keyLen) != 0) continue;
    if (!parseFloat(eq + 1, f.value)) return false;
    if (springSections < 0.0f || springSections > static_cast<float>(cv::spring_detail::kMaxSections) ||
        springSections != std::floor(springSections))
      return false;
    if (!springTuningInRange(spr) || !chasmTuningInRange(ch)) return false;
    if (!inside(ot.trimDb, -24.0f, 24.0f) ||
        !inside(ot.grainWindowMs, cv::OctaveVoice::kMinWindowMs, cv::OctaveVoice::kMaxWindowMs) ||
        !inside(at.trimDb, -24.0f, 24.0f) || !inside(ft.offsetDb, -24.0f, 24.0f))
      return false;
    if (!inside(bt.firstCaptureMs, 1.0f, 500.0f) || !inside(bt.skipDb, -60.0f, 0.0f) || !inside(bt.trimDb, -24.0f, 24.0f) ||
        !inside(bt.driveWord, 0.1f, 20.0f) || !inside(bt.driveBell, 0.0f, 20.0f) || !inside(bt.drift, 0.0f, 0.2f))
      return false;
    if (!crowdTuningInRange(ct) || !lofiTuningInRange(lt) || !growlTuningInRange(grt)) return false;
    if (!slapbackTuningInRange(st) || !distortionTuningInRange(dt)) return false;
    if (!unisonTuningInRange(t) || !gateTuningInRange(ig) || !gateTuningInRange(gt)) return false;
    if (!cv::polishParamsInRange(rp.eq)) return false;
    if (!inside(fft.decayMs, cv::kFollowerMinDecayMs, cv::kFollowerMaxDecayMs)) return false;
    spr.chirpSections = static_cast<int>(springSections);
    return true;
  }
  return false;
}

bool parseArgs(int argc, char** argv, const char** in, const char** out,
               RenderParams& rp) {
  cv::UnisonParams& p = rp.unison;
  cv::OctaveParams& op = rp.pitchCards.octave;
  cv::AutotuneParams& ap = rp.pitchCards.autotune;
  cv::FormantParams& fp = rp.pitchCards.formant;
  bool haveOctave = false;
  bool haveFormant = false;
  bool haveDepth = false;
  bool haveOn = false;
  bool haveSlap = false;
  bool haveDrive = false;
  bool haveLofi = false;
  bool haveReverb = false;
  bool haveInGate = false;
  bool haveGate = false;
  bool haveBloom = false;
  bool haveKnobs = false;  // --time or --mix: the reverb takes the panel's positions through applyKnobs
  float time = 0.5f, mix = 0.5f;
  int positional = 0;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--depth") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &p.depth) || p.depth < 0.0f || p.depth > 1.0f)
        return false;
      haveDepth = true;
    } else if (std::strcmp(a, "--on") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      if (std::strcmp(v, "0") != 0 && std::strcmp(v, "1") != 0) return false;
      p.on = v[0] == '1';
      haveOn = true;
    } else if (std::strcmp(a, "--slap") == 0 && i + 1 < argc) {
      cv::SlapbackParams& sp = rp.slapback;
      if (!parseFloat(argv[++i], &sp.amount) || sp.amount < 0.0f || sp.amount > 1.0f) return false;
      haveSlap = true;
    } else if (std::strcmp(a, "--drive") == 0 && i + 1 < argc) {
      cv::DistortionParams& dp = rp.distortion;
      if (!parseFloat(argv[++i], &dp.drive) || dp.drive < 0.0f || dp.drive > 1.0f) return false;
      haveDrive = true;
    } else if (std::strcmp(a, "--tone") == 0 && i + 1 < argc) {
      cv::DistortionParams& dp = rp.distortion;
      if (!parseFloat(argv[++i], &dp.tone) || dp.tone < 0.0f || dp.tone > 1.0f) return false;
    } else if (std::strcmp(a, "--growl") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.growl.growl) || rp.growl.growl < 0.0f || rp.growl.growl > 1.0f) return false;
      rp.growlOn = true;
    } else if (std::strcmp(a, "--gdrive") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.growl.drive) || rp.growl.drive < 0.0f || rp.growl.drive > 1.0f) return false;
      rp.growlOn = true;
    } else if (std::strcmp(a, "--lofi") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.lofi.amount) || rp.lofi.amount < 0.0f || rp.lofi.amount > 1.0f) return false;
      haveLofi = true;
    } else if (std::strcmp(a, "--lofi-tone") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.lofi.tone) || rp.lofi.tone < 0.0f || rp.lofi.tone > 1.0f) return false;
      haveLofi = true;
    } else if (std::strcmp(a, "--mix") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &mix) || mix < 0.0f || mix > 1.0f) return false;
      haveKnobs = true;
    } else if (std::strcmp(a, "--ingate") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.inGate.thresholdDb) || rp.inGate.thresholdDb < cv::kInputGateThresholdMinDb ||
          rp.inGate.thresholdDb > cv::kInputGateThresholdMaxDb)
        return false;
      haveInGate = true;
    } else if (std::strcmp(a, "--gate") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.gate.thresholdDb) || rp.gate.thresholdDb < cv::kGateThresholdMinDb ||
          rp.gate.thresholdDb > cv::kGateThresholdMaxDb)
        return false;
      haveGate = true;
    } else if (std::strcmp(a, "--amount") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.bloom.amount) || rp.bloom.amount < 0.0f || rp.bloom.amount > 1.0f) return false;
      haveBloom = true;
    } else if (std::strcmp(a, "--length") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.bloom.lengthMs) || rp.bloom.lengthMs < cv::Bloom::kLengthMinMs ||
          rp.bloom.lengthMs > cv::Bloom::kLengthMaxMs)
        return false;
      haveBloom = true;
    } else if (std::strcmp(a, "--ceiling-stats") == 0) {
      rp.ceilingStats = true;
    } else if (std::strcmp(a, "--vox-eq") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      if (std::strcmp(v, "cuts") == 0) {
        rp.eqOn = true;
        rp.eq.cuts = true;
      } else if (std::strcmp(v, "on") == 0) {
        rp.eqOn = true;
        rp.eq.cuts = false;
      } else if (std::strcmp(v, "off") == 0) {
        rp.eqOn = false;
      } else {
        return false;
      }
    } else if (std::strcmp(a, "--eqdip") == 0 && i + 1 < argc) {
      if (!parsePair(argv[++i], &rp.eq.dipHz, &rp.eq.dipDb)) return false;
    } else if (std::strcmp(a, "--eqpres") == 0 && i + 1 < argc) {
      if (!parsePair(argv[++i], &rp.eq.presenceHz, &rp.eq.presenceDb)) return false;
    } else if (std::strcmp(a, "--reverb") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      if (std::strcmp(v, "chasm") == 0)
        rp.reverb.engine = cv::kReverbChasm;
      else if (std::strcmp(v, "spring") == 0)
        rp.reverb.engine = cv::kReverbSpring;
      else
        return false;
      haveReverb = true;
    } else if (std::strcmp(a, "--time") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &time) || time < 0.0f || time > 1.0f) return false;
      haveKnobs = true;
    } else if (std::strcmp(a, "--autotune") == 0) {
      ap.on = true;
    } else if (std::strcmp(a, "--atkey") == 0 && i + 1 < argc) {
      float k = 0.0f;
      if (!parseFloat(argv[++i], &k) || k < 0.0f || k > static_cast<float>(cv::kKeyCount - 1) || k != std::floor(k))
        return false;
      ap.key = static_cast<int>(k);
    } else if (std::strcmp(a, "--response") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &ap.responseMs) ||
          ap.responseMs < cv::AutotuneVoice::kMinResponseMs ||
          ap.responseMs > cv::AutotuneVoice::kMaxResponseMs)
        return false;
    } else if (std::strcmp(a, "--octave") == 0 && i + 1 < argc) {
      float s = 0.0f;
      const float top = static_cast<float>(cv::OctaveVoice::kMaxSemis);
      if (!parseFloat(argv[++i], &s) || s < -top || s > top || s != std::floor(s)) return false;
      op.semitones = static_cast<int>(s);
      haveOctave = true;
    } else if (std::strcmp(a, "--omix") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &op.mix) || op.mix < 0.0f || op.mix > 1.0f) return false;
    } else if (std::strcmp(a, "--formant") == 0 && i + 1 < argc) {
      float s = 0.0f;
      const float top = static_cast<float>(cv::kFormantMaxShiftSt);
      if (!parseFloat(argv[++i], &s) || s < -top || s > top || s != std::floor(s)) return false;
      fp.shiftSt = static_cast<int>(s);
      haveFormant = true;
    } else if (std::strcmp(a, "--fmix") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &fp.mix) || fp.mix < 0.0f || fp.mix > 1.0f) return false;
    } else if (std::strcmp(a, "--follower") == 0 && i + 1 < argc) {
      float& range = rp.follower.rangeSt;
      if (!parseFloat(argv[++i], &range) || range < cv::kFollowerMinRangeSt || range > cv::kFollowerMaxRangeSt) return false;
      rp.follower.placed = true;
    } else if (std::strcmp(a, "--sensitivity") == 0 && i + 1 < argc) {
      float& sens = rp.follower.sensitivity;
      if (!parseFloat(argv[++i], &sens) || sens < 0.0f || sens > 1.0f) return false;
    } else if (std::strcmp(a, "--follower-drive") == 0 && i + 1 < argc) {
      rp.drivePath = argv[++i];
    } else if (std::strcmp(a, "--follower-shift") == 0 && i + 1 < argc) {
      rp.shiftPath = argv[++i];
    } else if (std::strcmp(a, "--crowd") == 0) {
      rp.crowdOn = true;
    } else if (std::strcmp(a, "--width") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.crowd.width) || rp.crowd.width < 0.0f || rp.crowd.width > 1.0f)
        return false;
    } else if (std::strcmp(a, "--voices") == 0 && i + 1 < argc) {
      float step = 0.0f;
      if (!parseFloat(argv[++i], &step) || step < 0.0f || step > static_cast<float>(cv::kVoicesMob) ||
          step != std::floor(step))
        return false;
      rp.crowd.voicesStep = static_cast<uint8_t>(step);
    } else if (std::strcmp(a, "--tuning") == 0) {
      int taken = 0;
      while (i + 1 < argc && argv[i + 1][0] != '-' && std::strchr(argv[i + 1], '=')) {
        if (!applyTuning(rp, argv[++i])) return false;
        ++taken;
      }
      if (taken == 0) return false;
    } else if (a[0] != '-' && positional < 2) {
      (positional++ == 0 ? *in : *out) = a;
    } else {
      return false;
    }
  }
  if (positional != 2 || !cv::polishParamsInRange(rp.eq)) return false;
  // --time and --mix reach the engines the way the panel does.
  if (haveKnobs) {
    rp.reverb.time = time;
    rp.reverb.mix = mix;
    cv::applyKnobs(rp.reverb);
  }
  // Each pitch card runs only when asked for, in the order autotune, formant, octave, so the formant
  // engine sits directly behind Autotune and reads its tracker. The Crowd and Growl cards read Autotune's
  // tracker when Autotune runs and run a tracker of their own otherwise. Apraxia plays the formant voice,
  // so it switches that on too.
  op.on = haveOctave;
  fp.on = haveFormant || rp.follower.placed;
  rp.crowd.on = rp.crowdOn;
  rp.growl.on = rp.growlOn;
  rp.autotuneOn = ap.on;
  rp.formantOn = fp.on;
  rp.octaveOn = haveOctave;
  if ((rp.drivePath != nullptr || rp.shiftPath != nullptr) && !rp.follower.placed) return false;
  // Unison runs only when asked for; --on 0 with --depth keeps it off.
  rp.unisonOn = haveOn ? p.on : haveDepth;
  p.on = rp.unisonOn;
  rp.slapOn = haveSlap;
  rp.distOn = haveDrive;
  rp.lofiOn = haveLofi;
  rp.lofi.on = haveLofi;
  rp.reverbOn = haveReverb;
  rp.reverb.on = haveReverb;
  rp.eq.on = rp.eqOn;
  rp.inGateOn = haveInGate;
  rp.inGate.on = haveInGate;
  rp.gateOn = haveGate;
  rp.gate.on = haveGate;
  rp.bloomOn = haveBloom;
  rp.bloom.on = haveBloom;
  rp.bloom.keyFromGate = haveInGate;
  return rp.inGateOn || rp.gateOn || rp.autotuneOn || rp.formantOn || rp.octaveOn || rp.crowdOn || rp.unisonOn || rp.slapOn || rp.growlOn || rp.distOn || rp.lofiOn ||
         rp.bloomOn ||
         rp.reverbOn ||
         rp.eqOn;
}

// Writes `frames` mono frames from `x`; false when the encoder takes fewer.
bool writeAll(ma_encoder* enc, const float* x, ma_uint64 frames) {
  ma_uint64 wrote = 0;
  return ma_encoder_write_pcm_frames(enc, x, frames, &wrote) == MA_SUCCESS && wrote == frames;
}

// Writes `v` held over `frames` mono frames, at most one block, when `enc` is open. False on a failed write.
bool writeHeld(ma_encoder* enc, float v, ma_uint64 frames, const char* path) {
  if (enc == nullptr) return true;
  float held[cv::kBlock];
  for (ma_uint64 i = 0; i < frames; ++i) held[i] = v;
  if (writeAll(enc, held, frames)) return true;
  std::fprintf(stderr, "cubevox-render: write failed on '%s'\n", path);
  return false;
}

// The Formant voice's glided SHIFT as a signal, 1/12 of full scale a semitone: -12 st at -1, +12 st at +1.
float shiftSignal(float st) { return st / -cv::kFollowerMinRangeSt; }

int render(const char* inPath, const char* outPath, const RenderParams& rp) {
  ma_decoder dec;
  const ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, 1, cv::kSampleRate);
  if (ma_decoder_init_file(inPath, &dcfg, &dec) != MA_SUCCESS) {
    std::fprintf(stderr, "cubevox-render: cannot decode '%s'\n", inPath);
    return 1;
  }

  ma_encoder enc;
  const ma_encoder_config ecfg =
      ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 1, cv::kSampleRate);
  if (ma_encoder_init_file(outPath, &ecfg, &enc) != MA_SUCCESS) {
    std::fprintf(stderr, "cubevox-render: cannot write '%s'\n", outPath);
    ma_decoder_uninit(&dec);
    return 1;
  }

  // Apraxia's drive and the glided SHIFT, each held over its block, when asked for.
  constexpr int kSides = 2;
  const char* const sidePath[kSides] = {rp.drivePath, rp.shiftPath};
  ma_encoder sideEnc[kSides];
  ma_encoder* side[kSides] = {nullptr, nullptr};
  for (int s = 0; s < kSides; ++s) {
    if (sidePath[s] == nullptr) continue;
    if (ma_encoder_init_file(sidePath[s], &ecfg, &sideEnc[s]) != MA_SUCCESS) {
      std::fprintf(stderr, "cubevox-render: cannot write '%s'\n", sidePath[s]);
      for (ma_encoder* e : side) {
        if (e != nullptr) ma_encoder_uninit(e);
      }
      ma_encoder_uninit(&enc);
      ma_decoder_uninit(&dec);
      return 1;
    }
    side[s] = &sideEnc[s];
  }

  static cv::Gate inGate;
  static cv::Gate gate;
  static cv::AutotuneFx autotune;
  static cv::FormantFx formant;
  static cv::OctaveFx octave;
  static cv::FormantFollower follower;
  static cv::Crowd crowd;
  static cv::Unison unison;
  static cv::Slapback slapback;
  static cv::Distortion distortion;
  static cv::Growl growl;
  static cv::Lofi lofi;
  static cv::Bloom bloom;
  static cv::Reverb reverb;
  static cv::Polish polish;
  inGate.reset();
  gate.reset();
  autotune.reset();
  formant.reset();
  octave.reset();
  follower.reset();
  crowd.reset();
  unison.reset();
  slapback.reset();
  distortion.reset();
  growl.reset();
  lofi.reset();
  bloom.reset();
  reverb.reset();
  polish.reset();

  float inBuf[cv::kBlock];
  float bufA[cv::kBlock];
  float bufB[cv::kBlock];
  float* const scratch[2] = {bufA, bufB};
  double ceilingPeakIn = 0.0;  // largest sample into the output ceiling and out of it
  double ceilingPeakOut = 0.0;
  double ceilingTouched = 0.0;  // samples past the ceiling's knee
  double ceilingSamples = 0.0;
  int rc = 0;
  for (;;) {
    ma_uint64 got = 0;
    const ma_result r = ma_decoder_read_pcm_frames(&dec, inBuf, cv::kBlock, &got);
    if (got > 0) {
      const int n = static_cast<int>(got);
      polish.processFront(inBuf, inBuf, n, rp.eq);
      // Each stage writes the scratch buffer the previous stage did not.
      const float* src = inBuf;
      int next = 0;
      if (rp.inGateOn) {
        inGate.process(src, scratch[next], n, rp.inGate);
        src = scratch[next];
        next ^= 1;
      }
      cv::UpstreamPitch up;  // Autotune's tracker and what the cards since have put on the pitch and the audio
      cv::CrowdPitch crowdPitch;
      if (rp.autotuneOn) {
        cv::TrackerNeeds need;
        need.run = rp.crowdOn || rp.growlOn || rp.formantOn;
        need.fine = rp.formantOn;
        need.outOfRange = rp.crowdOn || rp.formantOn;
        if (rp.crowdOn) crowd.write(src, n);
        autotune.process(src, scratch[next], n, rp.pitchCards.autotune, need);
        up = {&autotune.pitch(), autotune.correctionSemis(), autotune.outputLatency()};
        crowdPitch = {up.result, up.shiftSemis};
        src = scratch[next];
        next ^= 1;
      }
      if (rp.formantOn) {
        formant.process(src, scratch[next], n, follower.follow(src, n, rp.pitchCards.formant, rp.follower), up);
        const float sideValue[kSides] = {follower.drive(), shiftSignal(formant.shiftSemis())};
        bool wrote = true;
        for (int s = 0; s < kSides; ++s) wrote = wrote && writeHeld(side[s], sideValue[s], got, sidePath[s]);
        if (!wrote) {
          rc = 1;
          break;
        }
        if (up.result != nullptr) up.latency += formant.outputLatency();
        src = scratch[next];
        next ^= 1;
      }
      if (rp.octaveOn) {
        octave.process(src, scratch[next], n, rp.pitchCards.octave);
        if (up.result != nullptr) {
          if (octave.leads()) up.shiftSemis += octave.intervalSemis();
          up.latency += octave.outputLatency();
        }
        src = scratch[next];
        next ^= 1;
      }
      if (rp.crowdOn) {
        crowd.process(src, scratch[next], n, rp.crowd, crowdPitch);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.unisonOn) {
        unison.process(src, scratch[next], n, rp.unison);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.slapOn) {
        slapback.process(src, scratch[next], n, rp.slapback);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.growlOn) {
        growl.process(src, scratch[next], n, rp.growl, up);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.distOn) {
        distortion.process(src, scratch[next], n, rp.distortion);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.gateOn) {
        gate.process(src, scratch[next], n, rp.gate);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.lofiOn) {
        lofi.process(src, scratch[next], n, rp.lofi);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.bloomOn) {
        bloom.process(src, scratch[next], n, rp.bloom, inGate.open());
        src = scratch[next];
        next ^= 1;
      }
      if (rp.reverbOn) {
        reverb.process(src, scratch[next], n, rp.reverb);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.eqOn) {
        polish.processBack(src, scratch[next], n, rp.eq);
        src = scratch[next];
        next ^= 1;
      }
      for (int i = 0; i < n; ++i) {
        const float x = src[i];
        const float y = cv::softCeiling(x);
        ceilingPeakIn = std::fmax(ceilingPeakIn, std::fabs(x));
        ceilingPeakOut = std::fmax(ceilingPeakOut, std::fabs(y));
        if (std::fabs(x) > cv::kSoftCeilingKnee) ceilingTouched += 1.0;
        scratch[next][i] = y;
      }
      ceilingSamples += n;
      src = scratch[next];
      ma_uint64 wrote = 0;
      if (ma_encoder_write_pcm_frames(&enc, src, got, &wrote) != MA_SUCCESS ||
          wrote != got) {
        std::fprintf(stderr, "cubevox-render: write failed on '%s'\n", outPath);
        rc = 1;
        break;
      }
    }
    if (r == MA_AT_END || got == 0) break;
    if (r != MA_SUCCESS) {
      std::fprintf(stderr, "cubevox-render: decode error in '%s'\n", inPath);
      rc = 1;
      break;
    }
  }

  ma_encoder_uninit(&enc);
  for (ma_encoder* e : side) {
    if (e != nullptr) ma_encoder_uninit(e);
  }
  ma_decoder_uninit(&dec);
  if (rc == 0 && rp.ceilingStats) {
    std::printf("ceiling: peak in %.3f (%+.2f dBFS), out %.3f (%+.2f dBFS), %.0f of %.0f samples past the %.0f dBFS knee\n",
                ceilingPeakIn, 20.0 * std::log10(ceilingPeakIn), ceilingPeakOut, 20.0 * std::log10(ceilingPeakOut),
                ceilingTouched, ceilingSamples, 20.0 * std::log10(cv::kSoftCeilingKnee));
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  const char* in = nullptr;
  const char* out = nullptr;
  RenderParams rp;
  rp.inGate.tuning = cv::inputGateTuning();  // the box's slot-1 gate, as in the emulator
  if (!parseArgs(argc, argv, &in, &out, rp)) return usage();
  return render(in, out, rp);
}
