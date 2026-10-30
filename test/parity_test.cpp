// Emulator-to-firmware parity: the engine params the firmware registry sets for a knob position
// against the ones the Mac emulator sets for the same position. Adam tuned in the emulator.
//
// The emulator's panel mappings live inline in emulator/main.cpp (ImGui code, not linkable), so each
// one is transcribed below with a citation of the function it comes from. The macro mappings
// (emulator/macros.h) are header-only and are called directly. Any drift on either side fails the
// test unless the case sits in kKnown, which keeps every open mismatch visible in the output.

#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "core/chain_params.h"
#include "core/registry.h"
#include "emulator/macros.h"
#include "engine/autotune.h"

namespace {

namespace mc = cv::macros;

int gFailures = 0;

// ---- Mismatch bookkeeping ----------------------------------------------------------

struct Hit {
  int count = 0;
  float pos = 0.0f;
  float fw = 0.0f;
  float em = 0.0f;
};
std::map<std::string, Hit> gHits;

struct Known {
  const char* key;
  const char* reason;
  const char* fwCite;
  const char* emCite;
};

// Open mismatches the owner has accepted, each with a reason and file:line on both sides.
// Every entry must still mismatch; a stale entry fails the test. Empty: the firmware matches.
constexpr std::array<Known, 0> kKnown{};

// 1e-4 relative: the emulator's macro defaults reproduce the engine tuning only to about 3e-5
// (macros.h kDefaultPos).
bool near(float a, float b) { return std::fabs(a - b) <= 1e-4f * std::fmax(std::fabs(a), std::fabs(b)) + 1e-6f; }

void expectNear(const std::string& key, float pos, float fw, float em) {
  if (near(fw, em)) return;
  Hit& h = gHits[key];
  if (h.count++ == 0) h = Hit{1, pos, fw, em};
}

#define EXPECT(key, pos, fw, em) expectNear((key), (pos), static_cast<float>(fw), static_cast<float>(em))

// ---- Emulator mapping, transcribed -------------------------------------------------

// emulator/main.cpp pedalRange; the faces in drawGateBlock (THRESHOLD linear, DECAY log).
float emuLinear(float lo, float hi, float p) { return lo + p * (hi - lo); }
float emuLog(float lo, float hi, float p) { return lo * std::pow(hi / lo, p); }

// emulator/main.cpp drawGateBlock and drawPedals: -70..-10 dB, 5..1000 ms log.
constexpr float kEmuThresholdLoDb = -70.0f, kEmuThresholdHiDb = -10.0f;
constexpr float kEmuDecayLoMs = 5.0f, kEmuDecayHiMs = 1000.0f;
// emulator/main.cpp kInputGatePots (the engine's kInputGate* ranges): -80..-30 dB, 20..2000 ms log.
constexpr float kEmuInThresholdLoDb = -80.0f, kEmuInThresholdHiDb = -30.0f;
constexpr float kEmuInDecayLoMs = 20.0f, kEmuInDecayHiMs = 2000.0f;
// emulator/main.cpp drawSlapbackBlock and drawPedals: 60..250 ms linear.
constexpr float kEmuSlapLoMs = 60.0f, kEmuSlapHiMs = 250.0f;
// emulator/main.cpp drawPedals: KEY 0..11, SEMITONES -12..12.
constexpr int kEmuKeys = 12, kEmuSemiLo = -12, kEmuSemiHi = 12;

// emulator/main.cpp drawPedals (RATE knob) -> macros::unison(tuning, rate).
void emuUnisonRate(float p, cv::UnisonTuning& t) { mc::unison(t, 100.0f * p); }

// ---- Field comparison helpers ------------------------------------------------------

#define CMP(prefix, pos, fwObj, emObj, field) \
  EXPECT(std::string(prefix) + "." #field, (pos), (fwObj).field, (emObj).field)

// `rest` adds the fields no panel knob drives (tuning and engine-side defaults).
void cmpUnison(const std::string& p, float pos, const cv::UnisonParams& f, const cv::UnisonParams& e, bool rest) {
  CMP(p, pos, f, e, depth);
  CMP(p, pos, f.tuning, e.tuning, lfoHz[0]);
  CMP(p, pos, f.tuning, e.tuning, lfoHz[1]);
  CMP(p, pos, f.tuning, e.tuning, swingMinMs);
  CMP(p, pos, f.tuning, e.tuning, swingMaxMs);
  if (!rest) return;
  CMP(p, pos, f.tuning, e.tuning, baseDelayMs[0]);
  CMP(p, pos, f.tuning, e.tuning, baseDelayMs[1]);
  CMP(p, pos, f.tuning, e.tuning, wetMaxDb);
  CMP(p, pos, f.tuning, e.tuning, trimDb);
}

void cmpGate(const std::string& p, float pos, const cv::GateParams& f, const cv::GateParams& e, bool rest) {
  CMP(p, pos, f, e, thresholdDb);
  CMP(p, pos, f.tuning, e.tuning, releaseMs);
  if (!rest) return;
  CMP(p, pos, f.tuning, e.tuning, attackMs);
  CMP(p, pos, f.tuning, e.tuning, holdMs);
  CMP(p, pos, f.tuning, e.tuning, rangeDb);
  CMP(p, pos, f.tuning, e.tuning, kneeDb);
  CMP(p, pos, f.tuning, e.tuning, detectorHpHz);
  CMP(p, pos, f.tuning, e.tuning, hysteresisDb);
  CMP(p, pos, f.tuning, e.tuning, detectorAttackMs);
  CMP(p, pos, f.tuning, e.tuning, detectorReleaseMs);
  EXPECT(std::string(p) + ".travelTimes", pos, f.tuning.travelTimes ? 1.0f : 0.0f, e.tuning.travelTimes ? 1.0f : 0.0f);
}

void cmpAutotune(const std::string& p, float pos, const cv::AutotuneParams& f, const cv::AutotuneParams& e,
                 bool rest) {
  CMP(p, pos, f, e, key);
  CMP(p, pos, f, e, responseMs);
  if (!rest) return;
  CMP(p, pos, f.tuning, e.tuning, trimDb);
  CMP(p, pos, f.tuning, e.tuning, maxCorrectSemis);
  CMP(p, pos, f.tuning, e.tuning, grainDelay);
}

void cmpOctave(const std::string& p, float pos, const cv::OctaveParams& f, const cv::OctaveParams& e, bool rest) {
  CMP(p, pos, f, e, semitones);
  CMP(p, pos, f, e, mix);
  if (!rest) return;
  CMP(p, pos, f.tuning, e.tuning, levelDb);
  CMP(p, pos, f.tuning, e.tuning, glideMs);
  CMP(p, pos, f.tuning, e.tuning, grainWindowMs);
  CMP(p, pos, f.tuning, e.tuning, grainCount);
  CMP(p, pos, f.tuning, e.tuning, trimDb);
}

void cmpSlapback(const std::string& p, float pos, const cv::SlapbackParams& f, const cv::SlapbackParams& e,
                 bool rest) {
  CMP(p, pos, f, e, mix);
  CMP(p, pos, f.tuning, e.tuning, timeMs);
  if (!rest) return;
  CMP(p, pos, f.tuning, e.tuning, lowpassHz);
  CMP(p, pos, f.tuning, e.tuning, feedback);
  CMP(p, pos, f.tuning, e.tuning, wetMaxDb);
  CMP(p, pos, f.tuning, e.tuning, trimDb);
}

void cmpDistortion(const std::string& p, float pos, const cv::DistortionParams& f, const cv::DistortionParams& e,
                   bool rest) {
  CMP(p, pos, f, e, drive);
  CMP(p, pos, f, e, tone);
  if (!rest) return;
  const cv::DistortionTuning& a = f.tuning;
  const cv::DistortionTuning& b = e.tuning;
  CMP(p, pos, a, b, inputHpHz);
  CMP(p, pos, a, b, s1BassHz);
  CMP(p, pos, a, b, s1BassDb);
  CMP(p, pos, a, b, s1LpHz);
  CMP(p, pos, a, b, gain1Min);
  CMP(p, pos, a, b, gain1Max);
  CMP(p, pos, a, b, stackBassHz);
  CMP(p, pos, a, b, stackBassDb);
  CMP(p, pos, a, b, stackTrebleHz);
  CMP(p, pos, a, b, stackTrebleDb);
  CMP(p, pos, a, b, stackLossDb);
  CMP(p, pos, a, b, s2HpHz);
  CMP(p, pos, a, b, s2LpHz);
  CMP(p, pos, a, b, gain2Min);
  CMP(p, pos, a, b, gain2Max);
  CMP(p, pos, a, b, railAsym);
  CMP(p, pos, a, b, railSoft);
  CMP(p, pos, a, b, trebleCutHz);
  CMP(p, pos, a, b, trebleCutDb);
  CMP(p, pos, a, b, toneMinDb);
  CMP(p, pos, a, b, toneMaxDb);
  CMP(p, pos, a, b, bassPeakHz);
  CMP(p, pos, a, b, bassPeakDb);
  CMP(p, pos, a, b, bassPeakQ);
  CMP(p, pos, a, b, trimDb);
  CMP(p, pos, a, b, fadeDrive);
}

void cmpChasmTuning(const std::string& p, float pos, const cv::ChasmTuning& ca, const cv::ChasmTuning& cb) {
  CMP(p + ".chasm", pos, ca, cb, timeLo);
  CMP(p + ".chasm", pos, ca, cb, timeHi);
  CMP(p + ".chasm", pos, ca, cb, trebleLossHz);
  CMP(p + ".chasm", pos, ca, cb, loopTrebleCut);
  CMP(p + ".chasm", pos, ca, cb, inputTrebleCut);
  CMP(p + ".chasm", pos, ca, cb, bassCutHz);
  CMP(p + ".chasm", pos, ca, cb, bassCutHzTop);
  CMP(p + ".chasm", pos, ca, cb, wobbleDepthMax);
  CMP(p + ".chasm", pos, ca, cb, wobbleRateLo);
  CMP(p + ".chasm", pos, ca, cb, wobbleRateHi);
  CMP(p + ".chasm", pos, ca, cb, inputTrim);
  CMP(p + ".chasm", pos, ca, cb, wobbleLevelDb);
  CMP(p + ".chasm", pos, ca, cb, dwellDrive);
  CMP(p + ".chasm", pos, ca, cb, dwellComp);
  CMP(p + ".chasm", pos, ca, cb, wetDb);
}

void cmpSpringTuning(const std::string& p, float pos, const cv::SpringTuning& sa, const cv::SpringTuning& sb) {
  CMP(p + ".spring", pos, sa, sb, springMs1);
  CMP(p + ".spring", pos, sa, sb, springMs2);
  CMP(p + ".spring", pos, sa, sb, springMs3);
  CMP(p + ".spring", pos, sa, sb, timeLo);
  CMP(p + ".spring", pos, sa, sb, timeHi);
  CMP(p + ".spring", pos, sa, sb, diffuse);
  CMP(p + ".spring", pos, sa, sb, chirpSections);
  CMP(p + ".spring", pos, sa, sb, chirpA);
  CMP(p + ".spring", pos, sa, sb, chirpSign);
  CMP(p + ".spring", pos, sa, sb, trebleLossHz);
  CMP(p + ".spring", pos, sa, sb, bassCutHz);
  CMP(p + ".spring", pos, sa, sb, returnLpHz);
  CMP(p + ".spring", pos, sa, sb, wobbleDepth);
  CMP(p + ".spring", pos, sa, sb, wobbleRateHz);
  CMP(p + ".spring", pos, sa, sb, inputTrim);
  CMP(p + ".spring", pos, sa, sb, dwellDrive);
  CMP(p + ".spring", pos, sa, sb, dwellComp);
  CMP(p + ".spring", pos, sa, sb, wetDb);
}

void cmpReverb(const std::string& p, float pos, const cv::ReverbParams& f, const cv::ReverbParams& e, bool rest) {
  CMP(p, pos, f, e, engine);
  CMP(p, pos, f, e, wet);
  CMP(p, pos, f, e, mix);
  CMP(p, pos, f, e, time);
  CMP(p, pos, f.chasm, e.chasm, decay);
  CMP(p, pos, f.spring, e.spring, decay);
  if (!rest) return;
  CMP(p, pos, f.chasm, e.chasm, wobble);
  CMP(p, pos, f.chasm, e.chasm, dwell);
  CMP(p, pos, f.spring, e.spring, dwell);
  cmpChasmTuning(p, pos, f.chasm.tuning, e.chasm.tuning);
  cmpSpringTuning(p, pos, f.spring.tuning, e.spring.tuning);
}

// ---- Emulator state as the app starts ------------------------------------------------

// emulator/main.cpp: the start constants and the ProtoParams constructor, applyMacros, and the
// drawFrame overrides. Engine structs keep their engine defaults where main.cpp does.
struct EmuState {
  cv::GateParams inputGate;
  cv::GateParams gate;
  cv::PitchFxParams pitchFx;
  cv::UnisonParams unison;
  cv::SlapbackParams slapback;
  cv::DistortionParams distortion;
  cv::ReverbParams reverb;
};

EmuState emuStart() {
  constexpr float kStartDepth = 0.5f;     // kStartDepth
  constexpr int kStartKey = 11;           // kStartKey
  constexpr float kStartSlapMix = 0.25f;  // kStartSlapMix
  constexpr float kStartDrive = 0.65f;    // kStartDrive
  constexpr float kStartTone = 1.0f;      // kStartTone
  EmuState s;
  s.unison = cv::UnisonParams{false, kStartDepth, {}};                     // ProtoParams members
  s.slapback = cv::SlapbackParams{true, kStartSlapMix, {}};
  s.distortion = cv::DistortionParams{true, kStartDrive, kStartTone, {}};
  // applyInputGateDefaults, called by the ProtoParams constructor
  s.inputGate.on = true;
  s.inputGate.thresholdDb = -55.0f;
  s.inputGate.tuning = cv::inputGateTuning();
  // The rest of the ProtoParams constructor
  s.gate.on = false;
  s.pitchFx.octave.mix = 0.10f;
  s.pitchFx.autotune.key = kStartKey;
  s.reverb.engine = cv::kReverbSpring;
  s.reverb.mix = 1.0f;
  s.reverb.time = 1.0f;
  // applyMacros at the default positions
  const auto& m = mc::kDefaultPos;
  mc::octaveSlide(s.pitchFx.octave.tuning, m[mc::OctaveSlide]);
  mc::unison(s.unison.tuning, m[mc::UnisonMotion]);
  mc::distBody(s.distortion.tuning, m[mc::DistBody]);
  mc::distBite(s.distortion.tuning, m[mc::DistBite]);
  mc::distGrit(s.distortion.tuning, m[mc::DistGrit]);
  mc::chasmWobble(s.reverb.chasm, m[mc::ChasmWobble]);
  mc::chasmBrightness(s.reverb.chasm.tuning, m[mc::ChasmBrightness]);
  mc::chasmBass(s.reverb.chasm.tuning, m[mc::ChasmBass]);
  mc::springDrip(s.reverb.spring.tuning, m[mc::SpringDrip]);
  mc::springFlutter(s.reverb.spring.tuning, m[mc::SpringFlutter]);
  mc::springBrightness(s.reverb.spring.tuning, m[mc::SpringBrightness]);
  cv::applyKnobs(s.reverb);  // drawFrame
  return s;
}

// ---- Firmware at the emulator's start --------------------------------------------------

// Knob position that gives the emulator's start value.
float posLinear(float lo, float hi, float v) { return (v - lo) / (hi - lo); }
float posLog(float lo, float hi, float v) { return std::log(v / lo) / std::log(hi / lo); }

// Every default-layout card with its knobs where the emulator opens, so a sweep moves one knob
// against the same background on both sides.
ChainParams fwAtEmuStart(const EmuState& em) {
  ChainParams fw;
  applyKnob(Card::InputGate, 0, posLinear(kEmuInThresholdLoDb, kEmuInThresholdHiDb, em.inputGate.thresholdDb), fw);
  applyKnob(Card::InputGate, 1, posLog(kEmuInDecayLoMs, kEmuInDecayHiMs, em.inputGate.tuning.releaseMs), fw);
  applyKnob(Card::Gate, 0, posLinear(kEmuThresholdLoDb, kEmuThresholdHiDb, em.gate.thresholdDb), fw);
  applyKnob(Card::Gate, 1, posLog(kEmuDecayLoMs, kEmuDecayHiMs, em.gate.tuning.releaseMs), fw);
  applyKnob(Card::Autotune, 0, static_cast<float>(em.pitchFx.autotune.key), fw);
  applyKnob(Card::Autotune, 1, cv::AutotuneVoice::mechanicalOf(em.pitchFx.autotune.responseMs), fw);
  applyKnob(Card::Octave, 0, static_cast<float>(em.pitchFx.octave.semitones - kEmuSemiLo), fw);
  applyKnob(Card::Octave, 1, em.pitchFx.octave.mix, fw);
  applyKnob(Card::Unison, 0, mc::kDefaultPos[mc::UnisonMotion] / 100.0f, fw);
  applyKnob(Card::Unison, 1, em.unison.depth, fw);
  applyKnob(Card::Slapback, 0, posLinear(kEmuSlapLoMs, kEmuSlapHiMs, em.slapback.tuning.timeMs), fw);
  applyKnob(Card::Slapback, 1, em.slapback.mix, fw);
  applyKnob(Card::Distortion, 0, em.distortion.tone, fw);
  applyKnob(Card::Distortion, 1, em.distortion.drive, fw);
  applyPlacement(Card::ReverbSpring, fw);
  applyKnob(Card::ReverbSpring, 0, em.reverb.time, fw);
  applyKnob(Card::ReverbSpring, 1, em.reverb.mix, fw);
  return fw;
}

// ---- Sweeps ---------------------------------------------------------------------------

constexpr int kSweepSteps = 100;

float sweepPos(int i) { return static_cast<float>(i) / kSweepSteps; }

void sweepGate(Card card, const char* name) {
  const bool plain = card == Card::Gate;
  for (int knob = 0; knob < 2; ++knob) {
    for (int i = 0; i <= kSweepSteps; ++i) {
      const float p = sweepPos(i);
      EmuState em = emuStart();
      ChainParams fw = fwAtEmuStart(em);
      cv::GateParams& e = plain ? em.gate : em.inputGate;
      applyKnob(card, knob, p, fw);
      const float thrLo = plain ? kEmuThresholdLoDb : kEmuInThresholdLoDb;
      const float thrHi = plain ? kEmuThresholdHiDb : kEmuInThresholdHiDb;
      const float decLo = plain ? kEmuDecayLoMs : kEmuInDecayLoMs;
      const float decHi = plain ? kEmuDecayHiMs : kEmuInDecayHiMs;
      if (knob == 0) e.thresholdDb = emuLinear(thrLo, thrHi, p);
      else e.tuning.releaseMs = emuLog(decLo, decHi, p);
      cmpGate(std::string("sweep.") + name + (knob == 0 ? ".threshold" : ".decay"), p,
              plain ? fw.gate : fw.inputGate, e, false);
    }
  }
}

void sweepAutotune() {
  for (int k = 0; k < kEmuKeys; ++k) {
    EmuState em = emuStart();
    ChainParams fw = fwAtEmuStart(em);
    applyKnob(Card::Autotune, 0, static_cast<float>(k), fw);
    em.pitchFx.autotune.key = k;  // drawPedals KEY encoder
    cmpAutotune("sweep.autotune.key", static_cast<float>(k), fw.pitchFx.autotune, em.pitchFx.autotune, false);
  }
  for (int i = 0; i <= kSweepSteps; ++i) {
    const float p = sweepPos(i);
    EmuState em = emuStart();
    ChainParams fw = fwAtEmuStart(em);
    applyKnob(Card::Autotune, 1, p, fw);
    em.pitchFx.autotune.responseMs = cv::AutotuneVoice::responseMsAt(p);  // drawPedals RESPONSE
    cmpAutotune("sweep.autotune.response", p, fw.pitchFx.autotune, em.pitchFx.autotune, false);
  }
}

void sweepOctave() {
  for (int pos = 0; pos < kEmuSemiHi - kEmuSemiLo + 1; ++pos) {
    EmuState em = emuStart();
    ChainParams fw = fwAtEmuStart(em);
    applyKnob(Card::Octave, 0, static_cast<float>(pos), fw);
    em.pitchFx.octave.semitones = kEmuSemiLo + pos;  // drawPedals SEMITONES
    cmpOctave("sweep.octave.semitones", static_cast<float>(pos), fw.pitchFx.octave, em.pitchFx.octave, false);
  }
  for (int i = 0; i <= kSweepSteps; ++i) {
    const float p = sweepPos(i);
    EmuState em = emuStart();
    ChainParams fw = fwAtEmuStart(em);
    applyKnob(Card::Octave, 1, p, fw);
    em.pitchFx.octave.mix = p;  // drawPedals MIX, pedalPercent
    cmpOctave("sweep.octave.mix", p, fw.pitchFx.octave, em.pitchFx.octave, false);
  }
}

void sweepUnison() {
  for (int i = 0; i <= kSweepSteps; ++i) {
    const float p = sweepPos(i);
    EmuState em = emuStart();
    ChainParams fw = fwAtEmuStart(em);
    applyKnob(Card::Unison, 1, p, fw);
    em.unison.depth = p;  // drawPedals DEPTH
    cmpUnison("sweep.unison.depth", p, fw.unison, em.unison, false);
    EmuState em2 = emuStart();
    ChainParams fw2 = fwAtEmuStart(em2);
    applyKnob(Card::Unison, 0, p, fw2);
    emuUnisonRate(p, em2.unison.tuning);
    cmpUnison("sweep.unison.rate", p, fw2.unison, em2.unison, false);
  }
}

void sweepSlapback() {
  for (int i = 0; i <= kSweepSteps; ++i) {
    const float p = sweepPos(i);
    EmuState em = emuStart();
    ChainParams fw = fwAtEmuStart(em);
    applyKnob(Card::Slapback, 1, p, fw);
    em.slapback.mix = p;  // drawPedals MIX
    cmpSlapback("sweep.slapback.mix", p, fw.slapback, em.slapback, false);
    EmuState em2 = emuStart();
    ChainParams fw2 = fwAtEmuStart(em2);
    applyKnob(Card::Slapback, 0, p, fw2);
    em2.slapback.tuning.timeMs = emuLinear(kEmuSlapLoMs, kEmuSlapHiMs, p);  // drawPedals TIME
    cmpSlapback("sweep.slapback.time", p, fw2.slapback, em2.slapback, false);
  }
}

void sweepDistortion() {
  for (int i = 0; i <= kSweepSteps; ++i) {
    const float p = sweepPos(i);
    EmuState em = emuStart();
    ChainParams fw = fwAtEmuStart(em);
    applyKnob(Card::Distortion, 1, p, fw);
    em.distortion.drive = p;  // drawPedals DRIVE
    cmpDistortion("sweep.distortion.drive", p, fw.distortion, em.distortion, false);
    EmuState em2 = emuStart();
    ChainParams fw2 = fwAtEmuStart(em2);
    applyKnob(Card::Distortion, 0, p, fw2);
    em2.distortion.tone = p;  // drawPedals TONE
    cmpDistortion("sweep.distortion.tone", p, fw2.distortion, em2.distortion, false);
  }
}

void sweepReverb(Card card, const char* name, int engine) {
  for (int knob = 0; knob < 2; ++knob) {
    for (int i = 0; i <= kSweepSteps; ++i) {
      const float p = sweepPos(i);
      EmuState em = emuStart();
      ChainParams fw = fwAtEmuStart(em);
      applyPlacement(card, fw);
      applyKnob(card, knob, p, fw);
      em.reverb.engine = engine;
      (knob == 0 ? em.reverb.time : em.reverb.mix) = p;  // drawPedals TIME and MIX
      cv::applyKnobs(em.reverb);                         // drawFrame
      cmpReverb(std::string("sweep.") + name + (knob == 0 ? ".time" : ".mix"), p, fw.reverb, em.reverb, false);
    }
  }
}

// ---- Defaults: knobs where the emulator opens, every other field as the firmware leaves it ---

void defaults() {
  const EmuState em = emuStart();
  const ChainParams fw = fwAtEmuStart(em);
  cmpGate("default.inputGate", 0.0f, fw.inputGate, em.inputGate, true);
  cmpGate("default.gate", 0.0f, fw.gate, em.gate, true);
  cmpAutotune("default.autotune", 0.0f, fw.pitchFx.autotune, em.pitchFx.autotune, true);
  cmpOctave("default.octave", 0.0f, fw.pitchFx.octave, em.pitchFx.octave, true);
  cmpUnison("default.unison", 0.0f, fw.unison, em.unison, true);
  cmpSlapback("default.slapback", 0.0f, fw.slapback, em.slapback, true);
  cmpDistortion("default.distortion", 0.0f, fw.distortion, em.distortion, true);
  cmpReverb("default.reverb", 0.0f, fw.reverb, em.reverb, true);
}

// ---- Report -----------------------------------------------------------------------------

void report() {
  int knownSeen = 0;
  for (const auto& [key, hit] : gHits) {
    const Known* known = nullptr;
    for (const Known& k : kKnown) {
      if (key == k.key) known = &k;
    }
    if (known != nullptr) {
      ++knownSeen;
      std::printf("KNOWN %s (%d cases, first at pos %.3f: firmware %.6g, emulator %.6g)\n  %s\n  fw %s | emu %s\n",
                  key.c_str(), hit.count, static_cast<double>(hit.pos), static_cast<double>(hit.fw),
                  static_cast<double>(hit.em), known->reason, known->fwCite, known->emCite);
    } else {
      std::printf("FAIL %s (%d cases, first at pos %.3f: firmware %.6g, emulator %.6g)\n", key.c_str(), hit.count,
                  static_cast<double>(hit.pos), static_cast<double>(hit.fw), static_cast<double>(hit.em));
      ++gFailures;
    }
  }
  for (const Known& k : kKnown) {
    if (gHits.count(k.key) == 0) {
      std::printf("FAIL stale known mismatch %s no longer differs\n", k.key);
      ++gFailures;
    }
  }
  std::printf("parity: %zu mismatching fields, %d known, %d failures\n", gHits.size(), knownSeen, gFailures);
}

}  // namespace

int main() {
  sweepGate(Card::InputGate, "inputGate");
  sweepGate(Card::Gate, "gate");
  sweepAutotune();
  sweepOctave();
  sweepUnison();
  sweepSlapback();
  sweepDistortion();
  sweepReverb(Card::ReverbSpring, "reverbSpring", cv::kReverbSpring);
  sweepReverb(Card::ReverbChasm, "reverbChasm", cv::kReverbChasm);
  defaults();
  report();
  return gFailures == 0 ? 0 : 1;
}
