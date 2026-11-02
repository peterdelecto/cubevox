// cubevox-render: loop in, the front half of POLISH EQ, gates + autotune + octave + formant + crowd +
// unison + slapback + distortion + gate + lofi + bloom + reverb, then the polish tail (comp, the back half
// of eq) and the output ceiling like the box, processed mono 48 kHz f32 WAV out.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../third_party/miniaudio.h"
#include "engine/bloom.h"
#include "engine/crowd.h"
#include "engine/distortion.h"
#include "engine/gate.h"
#include "engine/lofi.h"
#include "engine/pitch_fx.h"
#include "engine/polish.h"
#include "engine/polish_comp.h"
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
  bool pitchOn = false;  // also true when the crowd or formant card is on, so the ring and tracker run
  bool crowdOn = false;
  bool unisonOn = false;
  bool slapOn = false;
  bool distOn = false;
  bool lofiOn = false;
  bool bloomOn = false;
  bool reverbOn = false;
  bool compOn = true;  // the polish tail runs as on the box unless switched off
  bool eqOn = true;
  bool compStats = false;  // print the comp's gain reduction after the render
  const char* compName = "firm";  // the tuning in rp.comp; the default tuning is firm
  cv::GateParams inGate;
  cv::GateParams gate;
  cv::PitchFxParams pitchFx;
  cv::CrowdParams crowd;
  cv::UnisonParams unison;
  cv::SlapbackParams slapback;
  cv::DistortionParams distortion;
  cv::LofiParams lofi;
  cv::BloomParams bloom;
  cv::ReverbParams reverb;
  cv::PolishCompParams comp;
  cv::PolishParams eq;
};

static_assert(cv::kKeyCount == 12, "the usage text names keys 0..11");

int usage() {
  std::fprintf(stderr,
               "usage: cubevox-render <in> <out.wav> [--depth <0..1>] [--on 0|1] "
               "[--tuning k=v ...]\n"
               "       [--autotune] [--atkey <0..11>] [--response <1..500 ms>]\n"
               "       [--octave <-12..12>] [--omix <0..1>]\n"
               "       [--formant <-7..7>] [--fmix <0..1>]\n"
               "       [--crowd] [--width <0..1>] [--voices <0..9>]\n"
               "       [--slap <0..1>] [--drive <0..1>] [--tone <0..1>]\n"
               "       [--reverb spring|springb|chasm] [--decay <0..1>] [--dwell <0..1>]\n"
               "       [--tension <0..1>, alias of --decay] [--wobble <0..1>, CHASM] [--rmix <0..1>]\n"
               "       [--ingate <-80..-30 dB>] [--gate <-70..-10 dB>]\n"
               "       [--lofi <0..1>, AMOUNT] [--lofi-tone <0..1>]\n"
               "       [--bloom <0..1>] [--length <20..3000 ms>, aliases --linger, --tail]\n"
               "       [--polish-comp gentle|medium|firm|off] [--polish-eq on|off] [--comp-stats]\n"
               "       [--eq] [--eqdip <hz>,<db>] [--eqpres <hz>,<db>]\n"
               "  pitch front end runs with --octave, --autotune, --formant or --crowd\n"
               "  formant runs inside the pitch front end, ahead of octave's voice; --formant sets SHIFT in whole semitones and\n"
               "  --fmix the MIX (default 1); with --autotune its voice follows the tuned note and lines up with it\n"
               "  crowd runs only with --crowd; --width sets DIVERGE (default 1) and --voices the VOICES step, 0 to 8 copies or 9 for MOB (default 4)\n"
               "  unison runs only with --on 1 or --depth; slapback runs only with --slap\n"
               "  distortion runs only with --drive; reverb runs only with --reverb\n"
               "  lofi runs with --lofi or --lofi-tone (the other one defaults to 0.5), where the gate sits\n"
               "  at least one stage must run; order is eq front, ingate, pitch, crowd, unison, slapback, distortion,\n"
               "  gate, lofi, bloom, reverb, comp, eq back\n"
               "  bloom, the Chatter card, runs with --bloom (AMOUNT, default 0.5) or --length (LENGTH, default 300 ms); with --ingate its word key\n"
               "  is the input gate's open state, otherwise bloom listens to its own input; it cuts its slices from the pitch\n"
               "  stage's output when one runs (before crowd), else from the input gate's output, and the dry path is its input\n"
               "  the box's output ceiling, bit-exact below -2 dBFS, always runs after the last stage\n"
               "  ingate and gate run only with their flag; a flag enables that gate at that threshold\n"
               "  the eq's front half (70 Hz high-pass, 6 kHz low-pass, de-esser) runs ahead of every stage and its\n"
               "  back half (300 Hz dip, 3.5 kHz presence, trim) runs after the comp, both on by default\n"
               "  comp and eq are the box's polish tail and run by default, the comp at its firm tuning\n"
               "  --polish-comp gentle|medium|firm picks the comp tuning, and --polish-comp off and --polish-eq off\n"
               "  switch them out for A/B listening, --polish-eq off for both halves; --eq is accepted and changes\n"
               "  nothing\n"
               "  --comp-stats prints the comp's average and peak gain reduction, read once per block, and the ceiling's peaks\n"
               "  k: baseDelayMs0 baseDelayMs1 lfoHz0 lfoHz1 swingMinMs swingMaxMs wetMaxDb\n"
               "     topFrom topSwingGain topWetDb\n"
               "     octGrainWindowMs octGrainCount (2|4) octTrimDb eqTrimDb\n"
               "     slapTimeMs (60..250) slapLowpassHz (500..12000) slapFeedback (0..0.5) slapWetMaxDb slapTrimDb (-24..24)\n"
               "     distInputHpHz distS1BassHz distS1BassDb distS1LpHz distGain1Max distTopBoostDb\n"
               "     distTopBoostFrom distStackBassHz\n"
               "     distStackBassDb distStackTrebleHz distStackTrebleDb distStackLossDb distS2HpHz\n"
               "     distS2LpHz distGain2Max distRailAsym distRailSoft distTrebleCutHz "
               "distTrebleCutDb\n"
               "     distToneMinDb distToneMaxDb distBassPeakHz distBassPeakDb distBassPeakQ distTrimDb "
               "distFadeDrive\n"
               "     chmTimeLo chmTimeHi chmTrebleLossHz chmLoopTrebleCut chmInputTrebleCut "
               "chmBassCutHz\n"
               "     chmBassCutHzTop chmWobbleDepthMax chmWobbleRateLo chmWobbleRateHi chmInputTrim\n"
               "     chmWobbleLevelDb chmDwellDrive chmDwellComp chmWetDb\n"
               "     sbSpringMs1 sbSpringMs2 sbSpringMs3 sbTimeLo sbTimeHi sbDiffuse sbChirpSections (0..32)\n"
               "     sbChirpA sbChirpSign (1 sweeps up, -1 sweeps down) sbTrebleLossHz sbBassCutHz sbReturnLpHz\n"
               "     sbWobbleDepth sbWobbleRateHz sbInputTrim sbDwellDrive sbDwellComp sbWetDb\n"
               "     eqDipQ eqPresenceQ\n"
               "     eqEssBandLoHz eqEssBandHiHz eqEssThresholdDb eqEssMaxCutDb eqEssReleaseMs eqEssNotchHz eqEssNotchQ\n"
               "     atTrimDb atGrainDelay fmTrimDb\n"
               "     crowdVoiceDb (-24..0) crowdTrimDb crowdTrimBottomDb (-12..12) crowdFormantSt0..3 crowdPitchSt0..3\n"
               "     (-12..12) crowdShelfDb0..3 (-12..12) crowdShelfHz crowdHpHz (20..21600) crowdDelayMs0..3 (5..150)\n"
               "     crowdLagScale (0.5..2) crowdScatterMs (0..30) crowdDriftCents (0..50) crowdMaxLagMs (0..150)\n"
               "     crowdWidthKnee (0.02..0.9) crowdWidthKneeFrac (0..1)\n"
               "     crowdWidthLagBottom (0.2..1) crowdWidthLagMs (0..60)\n"
               "     crowdLevelScatterDb (0..4) crowdContour0..3 (0.6..1.4)\n"
               "  every value must be finite, and a crowd key outside its range is refused\n"
               "     bloomTrimDb bloomFirstCaptureMs bloomSkipDb bloomTailOnly (0|1, loop only after the word)\n"
               "     bloomDriveWord (0.1..20) bloomDriveBell (0..20) bloomDrift (0..0.2)\n"
               "     ingAttackMs ingHoldMs ingReleaseMs ingRangeDb ingKneeDb ingDetectorHpHz ingHysteresisDb\n"
               "     ingDetectorAttackMs ingDetectorReleaseMs\n"
               "     gtAttackMs gtHoldMs gtReleaseMs gtRangeDb gtKneeDb gtDetectorHpHz gtHysteresisDb\n"
               "     lofiFcLoHz lofiFcHiHz lofiHpStartHz lofiHpEndRatio lofiLpStartHz lofiLpEndRatio lofiHonkDb lofiHonkQ\n"
               "     lofiDriveDb lofiHoldMaxHz lofiHoldMinHz lofiBitsMax lofiBitsMin lofiTrimDb\n"
               "     lofiHoldOverBand (floor on the held rate in low-pass corners, 0 = none)\n"
               "     lofiMakeup<tone 0..4>_<amount 0..10> (dB, the makeup table by TONE 0, 25 ... 100 %% and AMOUNT 0, 10 ... 100 %%)\n"
               "  a lofi key outside its range is refused\n"
               "  --decay and --dwell apply to the selected reverb engine\n");
  return 2;
}

// A finite number and nothing after it. NaN passes every < and > range check, so it is refused here.
bool parseFloat(const char* s, float* out) {
  char* end = nullptr;
  *out = std::strtof(s, &end);
  return end != s && *end == '\0' && std::isfinite(*out);
}

bool parseOnOff(const char* s, bool* out) {
  const bool on = std::strcmp(s, "on") == 0;
  if (!on && std::strcmp(s, "off") != 0) return false;
  *out = on;
  return true;
}

struct CompTuningName {
  const char* name;
  cv::PolishCompTuning (*make)();
};

constexpr CompTuningName kCompTunings[] = {{"gentle", cv::PolishCompTuning::gentle},
                                           {"medium", cv::PolishCompTuning::medium},
                                           {"firm", cv::PolishCompTuning::firm}};

// gentle, medium or firm picks a tuning, on keeps the default one, off switches the comp out.
bool parseCompChoice(const char* s, RenderParams* rp) {
  if (std::strcmp(s, "off") == 0) {
    rp->compOn = false;
    return true;
  }
  rp->compOn = true;
  if (std::strcmp(s, "on") == 0) return true;
  for (const CompTuningName& c : kCompTunings) {
    if (std::strcmp(s, c.name) != 0) continue;
    rp->comp.tuning = c.make();
    rp->compName = c.name;
    return true;
  }
  return false;
}

// "hz,db"
bool parsePair(const char* s, float* hz, float* db) {
  char* end = nullptr;
  *hz = std::strtof(s, &end);
  if (end == s || *end != ',') return false;
  return parseFloat(end + 1, db);
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
  for (const auto& row : t.makeupDb)
    for (const float m : row)
      if (!inside(m, -24.0f, 36.0f)) return false;
  return inside(t.fcLoHz, cv::kFilterMinHz, cv::kFilterMaxHz) && inside(t.fcHiHz, cv::kFilterMinHz, cv::kFilterMaxHz) &&
         inside(t.hpStartHz, cv::kFilterMinHz, cv::kFilterMaxHz) &&
         inside(t.lpStartHz, cv::kFilterMinHz, cv::kFilterMaxHz) && inside(t.hpEndRatio, 0.05f, 4.0f) &&
         inside(t.lpEndRatio, 0.25f, 20.0f) && inside(t.honkDb, -24.0f, 24.0f) && inside(t.honkQ, 0.1f, 20.0f) &&
         inside(t.driveDb, -24.0f, 48.0f) && inside(t.holdMaxHz, 480.0f, 48000.0f) &&
         inside(t.holdMinHz, 480.0f, 48000.0f) && inside(t.holdOverBand, 0.0f, 20.0f) && inside(t.bitsMax, 1.0f, 20.0f) && inside(t.bitsMin, 1.0f, 20.0f) &&
         inside(t.trimDb, -24.0f, 24.0f);
}

// Every Echo key refuses a value outside the range the engine clamps to.
bool slapbackTuningInRange(const cv::SlapbackTuning& t) {
  using Echo = cv::Slapback;
  return inside(t.timeMs, Echo::kTimeMinMs, Echo::kTimeMaxMs) &&
         inside(t.lowpassHz, Echo::kCutoffMinHz, Echo::kCutoffMaxHz) && inside(t.feedback, 0.0f, Echo::kFeedbackMax) &&
         inside(t.wetMaxDb, -Echo::kLevelLimitDb, Echo::kLevelLimitDb) &&
         inside(t.trimDb, -Echo::kLevelLimitDb, Echo::kLevelLimitDb);
}

bool applyTuning(RenderParams& rp, const char* kv) {
  cv::UnisonTuning& t = rp.unison.tuning;
  cv::SlapbackTuning& st = rp.slapback.tuning;
  cv::DistortionTuning& dt = rp.distortion.tuning;
  cv::ChasmTuning& ch = rp.reverb.chasm.tuning;
  cv::SpringTuning& sb = rp.reverb.spring.tuning;
  cv::OctaveTuning& ot = rp.pitchFx.octave.tuning;
  cv::AutotuneTuning& at = rp.pitchFx.autotune.tuning;
  cv::FormantTuning& ft = rp.pitchFx.formant.tuning;
  cv::CrowdTuning& ct = rp.crowd.tuning;
  cv::PolishTuning& et = rp.eq.tuning;
  cv::GateTuning& ig = rp.inGate.tuning;
  cv::GateTuning& gt = rp.gate.tuning;
  cv::LofiTuning& lt = rp.lofi.tuning;
  cv::BloomTuning& bt = rp.bloom.tuning;
  float sbSections = static_cast<float>(sb.chirpSections);
  float octGrains = static_cast<float>(ot.grainCount);
  float bloomTailOnly = bt.tailOnly ? 1.0f : 0.0f;
  const TuningField fields[] = {
      {"baseDelayMs0", &t.baseDelayMs[0]}, {"baseDelayMs1", &t.baseDelayMs[1]},
      {"lfoHz0", &t.lfoHz[0]},             {"lfoHz1", &t.lfoHz[1]},
      {"swingMinMs", &t.swingMinMs},       {"swingMaxMs", &t.swingMaxMs},
      {"wetMaxDb", &t.wetMaxDb},
      {"topFrom", &t.topFrom},             {"topSwingGain", &t.topSwingGain},
      {"topWetDb", &t.topWetDb},
      {"octGrainWindowMs", &ot.grainWindowMs}, {"octGrainCount", &octGrains},
      {"octTrimDb", &ot.trimDb},
      {"atTrimDb", &at.trimDb}, {"atGrainDelay", &at.grainDelay}, {"fmTrimDb", &ft.trimDb},
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
      {"slapTimeMs", &st.timeMs},          {"slapLowpassHz", &st.lowpassHz},
      {"slapFeedback", &st.feedback},      {"slapWetMaxDb", &st.wetMaxDb},
      {"slapTrimDb", &st.trimDb},
      {"distInputHpHz", &dt.inputHpHz},       {"distS1BassHz", &dt.s1BassHz},
      {"distS1BassDb", &dt.s1BassDb},         {"distS1LpHz", &dt.s1LpHz},
      {"distGain1Max", &dt.gain1Max},         {"distTopBoostDb", &dt.topBoostDb},
      {"distTopBoostFrom", &dt.topBoostFrom}, {"distStackBassHz", &dt.stackBassHz},
      {"distStackBassDb", &dt.stackBassDb},   {"distStackTrebleHz", &dt.stackTrebleHz},
      {"distStackTrebleDb", &dt.stackTrebleDb}, {"distStackLossDb", &dt.stackLossDb},
      {"distS2HpHz", &dt.s2HpHz},             {"distS2LpHz", &dt.s2LpHz},
      {"distGain2Max", &dt.gain2Max},         {"distRailAsym", &dt.railAsym},
      {"distRailSoft", &dt.railSoft},         {"distTrebleCutHz", &dt.trebleCutHz},
      {"distTrebleCutDb", &dt.trebleCutDb},   {"distToneMinDb", &dt.toneMinDb}, {"distToneMaxDb", &dt.toneMaxDb},
      {"distBassPeakHz", &dt.bassPeakHz},     {"distBassPeakDb", &dt.bassPeakDb},
      {"distBassPeakQ", &dt.bassPeakQ},       {"distTrimDb", &dt.trimDb},
      {"distFadeDrive", &dt.fadeDrive},
      {"chmTimeLo", &ch.timeLo},              {"chmTimeHi", &ch.timeHi},
      {"chmTrebleLossHz", &ch.trebleLossHz},  {"chmLoopTrebleCut", &ch.loopTrebleCut},
      {"chmInputTrebleCut", &ch.inputTrebleCut}, {"chmBassCutHz", &ch.bassCutHz},
      {"chmBassCutHzTop", &ch.bassCutHzTop},  {"chmWobbleDepthMax", &ch.wobbleDepthMax},
      {"chmWobbleRateLo", &ch.wobbleRateLo},  {"chmWobbleRateHi", &ch.wobbleRateHi},
      {"chmInputTrim", &ch.inputTrim},        {"chmWobbleLevelDb", &ch.wobbleLevelDb},
      {"chmDwellDrive", &ch.dwellDrive}, {"chmDwellComp", &ch.dwellComp},
      {"chmWetDb", &ch.wetDb},
      {"sbSpringMs1", &sb.springMs1},         {"sbSpringMs2", &sb.springMs2},
      {"sbSpringMs3", &sb.springMs3},
      {"sbTimeLo", &sb.timeLo},               {"sbTimeHi", &sb.timeHi},
      {"sbDiffuse", &sb.diffuse},             {"sbChirpSections", &sbSections},
      {"sbChirpA", &sb.chirpA},               {"sbChirpSign", &sb.chirpSign},
      {"sbTrebleLossHz", &sb.trebleLossHz},   {"sbBassCutHz", &sb.bassCutHz},
      {"sbReturnLpHz", &sb.returnLpHz},       {"sbWobbleDepth", &sb.wobbleDepth},
      {"sbWobbleRateHz", &sb.wobbleRateHz},   {"sbInputTrim", &sb.inputTrim},
      {"sbDwellDrive", &sb.dwellDrive},       {"sbDwellComp", &sb.dwellComp},
      {"sbWetDb", &sb.wetDb},
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
      {"lofiFcLoHz", &lt.fcLoHz},             {"lofiFcHiHz", &lt.fcHiHz},
      {"lofiHpStartHz", &lt.hpStartHz},       {"lofiHpEndRatio", &lt.hpEndRatio},
      {"lofiLpStartHz", &lt.lpStartHz},       {"lofiLpEndRatio", &lt.lpEndRatio},
      {"lofiHonkDb", &lt.honkDb},             {"lofiHonkQ", &lt.honkQ},
      {"lofiDriveDb", &lt.driveDb},           {"lofiHoldMaxHz", &lt.holdMaxHz},
      {"lofiHoldMinHz", &lt.holdMinHz},       {"lofiBitsMax", &lt.bitsMax},
      {"lofiBitsMin", &lt.bitsMin},           {"lofiTrimDb", &lt.trimDb},
      {"lofiHoldOverBand", &lt.holdOverBand},
      {"bloomTrimDb", &bt.trimDb},            {"bloomFirstCaptureMs", &bt.firstCaptureMs},
      {"bloomSkipDb", &bt.skipDb},            {"bloomTailOnly", &bloomTailOnly},
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
    if (sbSections < 0.0f || sbSections > static_cast<float>(cv::spring_detail::kMaxSections) ||
        sbSections != std::floor(sbSections))
      return false;
    if (sb.chirpSign != 1.0f && sb.chirpSign != -1.0f) return false;
    if (octGrains != 2.0f && octGrains != 4.0f) return false;
    ot.grainCount = static_cast<int>(octGrains);
    if (bloomTailOnly != 0.0f && bloomTailOnly != 1.0f) return false;
    if (!inside(bt.firstCaptureMs, 1.0f, 500.0f) || !inside(bt.skipDb, -60.0f, 0.0f) || !inside(bt.trimDb, -24.0f, 24.0f) ||
        !inside(bt.driveWord, 0.1f, 20.0f) || !inside(bt.driveBell, 0.0f, 20.0f) || !inside(bt.drift, 0.0f, 0.2f))
      return false;
    bt.tailOnly = bloomTailOnly == 1.0f;
    if (!crowdTuningInRange(ct) || !lofiTuningInRange(lt)) return false;
    if (!slapbackTuningInRange(st)) return false;
    sb.chirpSections = static_cast<int>(sbSections);
    return true;
  }
  return false;
}

bool parseArgs(int argc, char** argv, const char** in, const char** out,
               RenderParams& rp) {
  cv::UnisonParams& p = rp.unison;
  cv::OctaveParams& op = rp.pitchFx.octave;
  cv::AutotuneParams& ap = rp.pitchFx.autotune;
  cv::FormantParams& fp = rp.pitchFx.formant;
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
  bool haveDecay = false;
  bool haveDwell = false;
  float decay = 0.0f;
  float dwell = 0.0f;
  int positional = 0;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (std::strcmp(a, "--depth") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &p.depth) || p.depth < 0.0f || p.depth > 1.0f)
        return false;
      haveDepth = true;
    } else if (std::strcmp(a, "--on") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      if (std::strcmp(v, "0") != 0 && std::strcmp(v, "1") != 0 && std::strcmp(v, "2") != 0)
        return false;
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
    } else if (std::strcmp(a, "--lofi") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.lofi.amount) || rp.lofi.amount < 0.0f || rp.lofi.amount > 1.0f) return false;
      haveLofi = true;
    } else if (std::strcmp(a, "--lofi-tone") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.lofi.tone) || rp.lofi.tone < 0.0f || rp.lofi.tone > 1.0f) return false;
      haveLofi = true;
    } else if (std::strcmp(a, "--rmix") == 0 && i + 1 < argc) {
      cv::ReverbParams& r = rp.reverb;
      if (!parseFloat(argv[++i], &r.wet) || r.wet < 0.0f || r.wet > 1.0f) return false;
    } else if (std::strcmp(a, "--ingate") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.inGate.thresholdDb) || rp.inGate.thresholdDb < cv::kInputGateThresholdMinDb ||
          rp.inGate.thresholdDb > cv::kInputGateThresholdMaxDb)
        return false;
      haveInGate = true;
    } else if (std::strcmp(a, "--gate") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.gate.thresholdDb) || rp.gate.thresholdDb < -70.0f ||
          rp.gate.thresholdDb > -10.0f)
        return false;
      haveGate = true;
    } else if (std::strcmp(a, "--bloom") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.bloom.bloom) || rp.bloom.bloom < 0.0f || rp.bloom.bloom > 1.0f) return false;
      haveBloom = true;
    } else if ((std::strcmp(a, "--length") == 0 || std::strcmp(a, "--linger") == 0 || std::strcmp(a, "--tail") == 0) &&
               i + 1 < argc) {
      if (!parseFloat(argv[++i], &rp.bloom.tailMs) || rp.bloom.tailMs < cv::Bloom::kTailMinMs ||
          rp.bloom.tailMs > cv::Bloom::kTailMaxMs)
        return false;
      haveBloom = true;
    } else if (std::strcmp(a, "--eq") == 0) {
      // The EQ runs by default; the flag stays so old command lines still parse.
    } else if (std::strcmp(a, "--comp-stats") == 0) {
      rp.compStats = true;
    } else if (std::strcmp(a, "--polish-comp") == 0 && i + 1 < argc) {
      if (!parseCompChoice(argv[++i], &rp)) return false;
    } else if (std::strcmp(a, "--polish-eq") == 0 && i + 1 < argc) {
      if (!parseOnOff(argv[++i], &rp.eqOn)) return false;
    } else if (std::strcmp(a, "--eqdip") == 0 && i + 1 < argc) {
      if (!parsePair(argv[++i], &rp.eq.dipHz, &rp.eq.dipDb)) return false;
    } else if (std::strcmp(a, "--eqpres") == 0 && i + 1 < argc) {
      if (!parsePair(argv[++i], &rp.eq.presenceHz, &rp.eq.presenceDb)) return false;
    } else if (std::strcmp(a, "--reverb") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      if (std::strcmp(v, "chasm") == 0)
        rp.reverb.engine = cv::kReverbChasm;
      else if (std::strcmp(v, "spring") == 0 || std::strcmp(v, "springb") == 0)
        rp.reverb.engine = cv::kReverbSpring;
      else
        return false;
      haveReverb = true;
    } else if ((std::strcmp(a, "--decay") == 0 || std::strcmp(a, "--tension") == 0) &&
               i + 1 < argc) {
      if (!parseFloat(argv[++i], &decay) || decay < 0.0f || decay > 1.0f) return false;
      haveDecay = true;
    } else if (std::strcmp(a, "--wobble") == 0 && i + 1 < argc) {
      cv::ChasmParams& c = rp.reverb.chasm;
      if (!parseFloat(argv[++i], &c.wobble) || c.wobble < 0.0f || c.wobble > 1.0f) return false;
    } else if (std::strcmp(a, "--dwell") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &dwell) || dwell < 0.0f || dwell > 1.0f) return false;
      haveDwell = true;
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
      if (!parseFloat(argv[++i], &s) || s < -12.0f || s > 12.0f || s != std::floor(s))
        return false;
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
  if (positional != 2) return false;
  // --decay and --dwell set whichever engine is selected.
  if (haveDecay) {
    float& d = rp.reverb.engine == cv::kReverbChasm ? rp.reverb.chasm.decay : rp.reverb.spring.decay;
    d = decay;
  }
  if (haveDwell) {
    float& d = rp.reverb.engine == cv::kReverbChasm ? rp.reverb.chasm.dwell : rp.reverb.spring.dwell;
    d = dwell;
  }
  // Each pitch stage runs only when asked for. The Crowd card reads the pitch stage's ring and tracker,
  // so it turns the stage on and tells it to keep them running, as the registry's toggle does.
  op.on = haveOctave;
  fp.on = haveFormant;
  rp.crowd.on = rp.crowdOn;
  rp.pitchFx.crowdOn = rp.crowdOn;
  rp.pitchOn = haveOctave || haveFormant || ap.on || rp.crowdOn;
  // Unison runs only when asked for; --on 0 with --depth keeps it off.
  rp.unisonOn = haveOn ? p.on : haveDepth;
  p.on = rp.unisonOn;
  rp.slapOn = haveSlap;
  rp.distOn = haveDrive;
  rp.lofiOn = haveLofi;
  rp.lofi.on = haveLofi;
  rp.reverbOn = haveReverb;
  rp.reverb.on = haveReverb;
  rp.comp.on = rp.compOn;
  rp.eq.on = rp.eqOn;
  rp.inGateOn = haveInGate;
  rp.inGate.on = haveInGate;
  rp.gateOn = haveGate;
  rp.gate.on = haveGate;
  rp.bloomOn = haveBloom;
  rp.bloom.on = haveBloom;
  rp.bloom.keyFromGate = haveInGate;
  return rp.inGateOn || rp.gateOn || rp.pitchOn || rp.unisonOn || rp.slapOn || rp.distOn || rp.lofiOn || rp.bloomOn ||
         rp.reverbOn ||
         rp.compOn || rp.eqOn;
}

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

  static cv::Gate inGate;
  static cv::Gate gate;
  static cv::PitchFx pitchFx;
  static cv::Crowd crowd;
  static cv::Unison unison;
  static cv::Slapback slapback;
  static cv::Distortion distortion;
  static cv::Lofi lofi;
  static cv::Bloom bloom;
  static cv::Reverb reverb;
  static cv::PolishComp polishComp;
  static cv::Polish polish;
  inGate.reset();
  gate.reset();
  pitchFx.reset();
  crowd.reset();
  unison.reset();
  slapback.reset();
  distortion.reset();
  lofi.reset();
  bloom.reset();
  reverb.reset();
  polishComp.reset();
  polish.reset();

  float inBuf[cv::kBlock];
  float bloomSrc[cv::kBlock];
  float bufA[cv::kBlock];
  float bufB[cv::kBlock];
  float* const scratch[2] = {bufA, bufB};
  double reductionSum = 0.0;  // comp gain reduction in dB, read at the end of each block, weighted by its length
  double reductionPeak = 0.0;
  double reductionSamples = 0.0;
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

      // Bloom cuts its slices from the pitch stage's output, or from the input gate's output when no
      // pitch card runs. The copy is taken ahead of the stages that follow.
      if (rp.inGateOn) {
        inGate.process(src, scratch[next], n, rp.inGate);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.bloomOn) std::memcpy(bloomSrc, src, sizeof(float) * static_cast<size_t>(n));
      if (rp.pitchOn) {
        pitchFx.process(src, scratch[next], n, rp.pitchFx);
        src = scratch[next];
        next ^= 1;
        if (rp.bloomOn) std::memcpy(bloomSrc, src, sizeof(float) * static_cast<size_t>(n));
      }
      if (rp.crowdOn) {
        crowd.process(src, scratch[next], n, rp.crowd, pitchFx);
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
        cv::BloomParams bp = rp.bloom;
        bp.keyOpen = inGate.open();
        bloom.process(src, bloomSrc, scratch[next], n, bp);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.reverbOn) {
        reverb.process(src, scratch[next], n, rp.reverb);
        src = scratch[next];
        next ^= 1;
      }
      if (rp.compOn) {
        polishComp.process(src, scratch[next], n, rp.comp);
        const double reduction = polishComp.gainReductionDb();
        reductionSum += reduction * n;
        reductionSamples += n;
        if (reduction > reductionPeak) reductionPeak = reduction;
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
  ma_decoder_uninit(&dec);
  if (rc == 0 && rp.compStats) {
    if (rp.compOn && reductionSamples > 0.0) {
      std::printf("comp %s: average gain reduction %.2f dB, peak %.2f dB\n", rp.compName, reductionSum / reductionSamples,
                  reductionPeak);
    } else {
      std::printf("comp: off\n");
    }
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
