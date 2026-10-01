// cubevox-render: loop in, harmony + octave + unison + slapback + distortion + reverb processed
// mono 48 kHz f32 WAV out.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../third_party/miniaudio.h"
#include "engine/distortion.h"
#include "engine/pitch_fx.h"
#include "engine/reverb.h"
#include "engine/slapback.h"
#include "engine/unison.h"

namespace {

struct TuningField {
  const char* name;
  float* value;
};

struct RenderParams {
  bool pitchOn = false;
  bool unisonOn = false;
  bool slapOn = false;
  bool distOn = false;
  bool reverbOn = false;
  cv::PitchFxParams pitchFx;
  cv::UnisonParams unison;
  cv::SlapbackParams slapback;
  cv::DistortionParams distortion;
  cv::ReverbParams reverb;
};

int usage() {
  std::fprintf(stderr,
               "usage: cubevox-render <in> <out.wav> [--depth <0..1>] [--on 0|1] "
               "[--tuning k=v ...]\n"
               "       [--harmony] [--key <0..11>] [--mix <0..1>] "
               "[--voice lower|low|fixed|high|higher=<0..3> ...]\n"
               "       [--octave <-12..12>] [--omix <0..1>] [--oengine 0|1] [--formant <-12..12>]\n"
               "       [--slap <0..1>] [--drive <0..1>]\n"
               "       [--reverb spring|chasm] [--spring] [--tension <0..1>] [--dwell <0..1>]\n"
               "       [--decay <0..1>] [--wobble <0..1>]\n"
               "  pitch front end runs with --harmony or --octave\n"
               "  unison runs only with --on 1 or --depth; slapback runs only with --slap\n"
               "  distortion runs only with --drive; reverb runs only with --reverb or --spring\n"
               "  at least one stage must run; order is pitch, unison, slapback, distortion, reverb\n"
               "  k: baseDelayMs0 baseDelayMs1 lfoHz0 lfoHz1 swingMinMs swingMaxMs "
               "wetMaxDb detuneCents0 detuneCents1 windowMs\n"
               "     octGrainPeriods octEpochSearch octEpochLpHz\n"
               "     slapTimeMs slapLowpassHz slapFeedback slapWetMaxDb\n"
               "     distInputHpHz distS1BassHz distS1BassDb distS1LpHz distGain1Max distStackBassHz\n"
               "     distStackBassDb distStackTrebleHz distStackTrebleDb distStackLossDb distS2HpHz\n"
               "     distS2LpHz distGain2Max distRailAsym distRailSoft distTrebleCutHz "
               "distTrebleCutDb\n"
               "     distToneDb distBassPeakHz distBassPeakDb distBassPeakQ distTrimDb "
               "distFadeDrive distOversample (0|1)\n"
               "     sprInputGain sprHpHz sprTensionLo sprTensionHi sprDwellDrive sprDwellComp sprHfMixDbLo\n"
               "     sprHfMixDbHi sprRippleGain sprSplashDiffuse sprHfSections sprSprings (2|3)\n"
               "     sprModDepth sprModRateHz sprBoingDb sprWetDb sprTankTrim\n"
               "     chmTimeLo chmTimeHi chmTrebleLossHz chmLoopTrebleCut chmInputTrebleCut "
               "chmBassCutHz\n"
               "     chmBassCutHzTop chmWobbleDepthMax chmWobbleRateLo chmWobbleRateHi chmInputTrim\n"
               "     chmWobbleLevelDb chmWetDb\n");
  return 2;
}

bool parseFloat(const char* s, float* out) {
  char* end = nullptr;
  *out = std::strtof(s, &end);
  return end != s && *end == '\0';
}

bool applyTuning(RenderParams& rp, const char* kv) {
  cv::UnisonTuning& t = rp.unison.tuning;
  cv::SlapbackTuning& st = rp.slapback.tuning;
  cv::DistortionTuning& dt = rp.distortion.tuning;
  cv::SpringTuning& sp = rp.reverb.spring.tuning;
  cv::ChasmTuning& ch = rp.reverb.chasm.tuning;
  cv::OctaveTuning& ot = rp.pitchFx.octave.tuning;
  float oversample = dt.oversample ? 1.0f : 0.0f;
  float hfSections = static_cast<float>(sp.hfSections);
  float springs = static_cast<float>(sp.springs);
  const TuningField fields[] = {
      {"baseDelayMs0", &t.baseDelayMs[0]}, {"baseDelayMs1", &t.baseDelayMs[1]},
      {"lfoHz0", &t.lfoHz[0]},             {"lfoHz1", &t.lfoHz[1]},
      {"swingMinMs", &t.swingMinMs},       {"swingMaxMs", &t.swingMaxMs},
      {"wetMaxDb", &t.wetMaxDb},
      {"detuneCents0", &t.detuneCents[0]}, {"detuneCents1", &t.detuneCents[1]},
      {"windowMs", &t.windowMs},
      {"octGrainPeriods", &ot.grainPeriods}, {"octEpochSearch", &ot.epochSearch},
      {"octEpochLpHz", &ot.epochLpHz},
      {"slapTimeMs", &st.timeMs},          {"slapLowpassHz", &st.lowpassHz},
      {"slapFeedback", &st.feedback},      {"slapWetMaxDb", &st.wetMaxDb},
      {"distInputHpHz", &dt.inputHpHz},       {"distS1BassHz", &dt.s1BassHz},
      {"distS1BassDb", &dt.s1BassDb},         {"distS1LpHz", &dt.s1LpHz},
      {"distGain1Max", &dt.gain1Max},         {"distStackBassHz", &dt.stackBassHz},
      {"distStackBassDb", &dt.stackBassDb},   {"distStackTrebleHz", &dt.stackTrebleHz},
      {"distStackTrebleDb", &dt.stackTrebleDb}, {"distStackLossDb", &dt.stackLossDb},
      {"distS2HpHz", &dt.s2HpHz},             {"distS2LpHz", &dt.s2LpHz},
      {"distGain2Max", &dt.gain2Max},         {"distRailAsym", &dt.railAsym},
      {"distRailSoft", &dt.railSoft},         {"distTrebleCutHz", &dt.trebleCutHz},
      {"distTrebleCutDb", &dt.trebleCutDb},   {"distToneDb", &dt.toneDb},
      {"distBassPeakHz", &dt.bassPeakHz},     {"distBassPeakDb", &dt.bassPeakDb},
      {"distBassPeakQ", &dt.bassPeakQ},       {"distTrimDb", &dt.trimDb},
      {"distFadeDrive", &dt.fadeDrive},
      {"distOversample", &oversample},
      {"sprInputGain", &sp.inputGain}, {"sprHpHz", &sp.hpHz},                  {"sprTensionLo", &sp.tensionLo},
      {"sprTensionHi", &sp.tensionHi},        {"sprDwellDrive", &sp.dwellDrive},
      {"sprDwellComp", &sp.dwellComp},        {"sprHfMixDbLo", &sp.hfMixDbLo},
      {"sprHfMixDbHi", &sp.hfMixDbHi},        {"sprRippleGain", &sp.rippleGain},
      {"sprSplashDiffuse", &sp.splashDiffuse}, {"sprHfSections", &hfSections},
      {"sprSprings", &springs},               {"sprModDepth", &sp.modDepth},
      {"sprModRateHz", &sp.modRateHz},        {"sprBoingDb", &sp.boingDb},
      {"sprWetDb", &sp.wetDb},                {"sprTankTrim", &sp.tankTrim},
      {"chmTimeLo", &ch.timeLo},              {"chmTimeHi", &ch.timeHi},
      {"chmTrebleLossHz", &ch.trebleLossHz},  {"chmLoopTrebleCut", &ch.loopTrebleCut},
      {"chmInputTrebleCut", &ch.inputTrebleCut}, {"chmBassCutHz", &ch.bassCutHz},
      {"chmBassCutHzTop", &ch.bassCutHzTop},  {"chmWobbleDepthMax", &ch.wobbleDepthMax},
      {"chmWobbleRateLo", &ch.wobbleRateLo},  {"chmWobbleRateHi", &ch.wobbleRateHi},
      {"chmInputTrim", &ch.inputTrim},        {"chmWobbleLevelDb", &ch.wobbleLevelDb},
      {"chmWetDb", &ch.wetDb},
  };
  const char* eq = std::strchr(kv, '=');
  if (!eq) return false;
  const size_t keyLen = static_cast<size_t>(eq - kv);
  for (const TuningField& f : fields) {
    if (std::strlen(f.name) != keyLen || std::strncmp(f.name, kv, keyLen) != 0) continue;
    if (!parseFloat(eq + 1, f.value)) return false;
    dt.oversample = oversample != 0.0f;
    if (hfSections < 0.0f || hfSections > 200.0f || hfSections != std::floor(hfSections))
      return false;
    if (springs != 2.0f && springs != 3.0f) return false;
    sp.hfSections = static_cast<int>(hfSections);
    sp.springs = static_cast<int>(springs);
    return true;
  }
  return false;
}

bool parseVoice(const char* kv, cv::HarmonySlot& slot) {
  const struct {
    const char* name;
    cv::HarmonyVoice voice;
  } names[] = {
      {"lower", cv::HarmonyVoice::Lower}, {"low", cv::HarmonyVoice::Low},
      {"fixed", cv::HarmonyVoice::Fixed}, {"high", cv::HarmonyVoice::High},
      {"higher", cv::HarmonyVoice::Higher},
  };
  const char* eq = std::strchr(kv, '=');
  if (!eq || eq[1] < '0' || eq[1] > '3' || eq[2] != '\0') return false;
  const size_t keyLen = static_cast<size_t>(eq - kv);
  for (const auto& n : names) {
    if (std::strlen(n.name) == keyLen && std::strncmp(n.name, kv, keyLen) == 0) {
      slot.voice = n.voice;
      slot.level = eq[1] - '0';
      return true;
    }
  }
  return false;
}

bool parseArgs(int argc, char** argv, const char** in, const char** out,
               RenderParams& rp) {
  cv::UnisonParams& p = rp.unison;
  cv::HarmonyParams& hp = rp.pitchFx.harmony;
  cv::OctaveParams& op = rp.pitchFx.octave;
  bool haveHarmony = false;
  bool haveOctave = false;
  bool haveDepth = false;
  bool haveOn = false;
  bool haveSlap = false;
  bool haveDrive = false;
  bool haveReverb = false;
  int voices = 0;
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
      if (!parseFloat(argv[++i], &sp.intensity) || sp.intensity < 0.0f || sp.intensity > 1.0f)
        return false;
      haveSlap = true;
    } else if (std::strcmp(a, "--drive") == 0 && i + 1 < argc) {
      cv::DistortionParams& dp = rp.distortion;
      if (!parseFloat(argv[++i], &dp.drive) || dp.drive < 0.0f || dp.drive > 1.0f) return false;
      haveDrive = true;
    } else if (std::strcmp(a, "--spring") == 0) {
      rp.reverb.engine = cv::kReverbSpring;
      haveReverb = true;
    } else if (std::strcmp(a, "--reverb") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      if (std::strcmp(v, "spring") == 0)
        rp.reverb.engine = cv::kReverbSpring;
      else if (std::strcmp(v, "chasm") == 0)
        rp.reverb.engine = cv::kReverbChasm;
      else
        return false;
      haveReverb = true;
    } else if (std::strcmp(a, "--decay") == 0 && i + 1 < argc) {
      cv::ChasmParams& c = rp.reverb.chasm;
      if (!parseFloat(argv[++i], &c.decay) || c.decay < 0.0f || c.decay > 1.0f) return false;
    } else if (std::strcmp(a, "--wobble") == 0 && i + 1 < argc) {
      cv::ChasmParams& c = rp.reverb.chasm;
      if (!parseFloat(argv[++i], &c.wobble) || c.wobble < 0.0f || c.wobble > 1.0f) return false;
    } else if (std::strcmp(a, "--tension") == 0 && i + 1 < argc) {
      cv::SpringParams& s = rp.reverb.spring;
      if (!parseFloat(argv[++i], &s.tension) || s.tension < 0.0f || s.tension > 1.0f) return false;
    } else if (std::strcmp(a, "--dwell") == 0 && i + 1 < argc) {
      cv::SpringParams& s = rp.reverb.spring;
      if (!parseFloat(argv[++i], &s.dwell) || s.dwell < 0.0f || s.dwell > 1.0f) return false;
    } else if (std::strcmp(a, "--harmony") == 0) {
      haveHarmony = true;
    } else if (std::strcmp(a, "--key") == 0 && i + 1 < argc) {
      float k = 0.0f;
      if (!parseFloat(argv[++i], &k) || k < 0.0f || k > 11.0f || k != std::floor(k))
        return false;
      hp.key = static_cast<int>(k);
    } else if (std::strcmp(a, "--mix") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &hp.mix) || hp.mix < 0.0f || hp.mix > 1.0f) return false;
    } else if (std::strcmp(a, "--octave") == 0 && i + 1 < argc) {
      float s = 0.0f;
      if (!parseFloat(argv[++i], &s) || s < -12.0f || s > 12.0f || s != std::floor(s))
        return false;
      op.semitones = static_cast<int>(s);
      haveOctave = true;
    } else if (std::strcmp(a, "--oengine") == 0 && i + 1 < argc) {
      const char* v = argv[++i];
      if (std::strcmp(v, "0") != 0 && std::strcmp(v, "1") != 0) return false;
      op.engine = v[0] - '0';
    } else if (std::strcmp(a, "--formant") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &op.formant) || op.formant < -12.0f || op.formant > 12.0f)
        return false;
    } else if (std::strcmp(a, "--omix") == 0 && i + 1 < argc) {
      if (!parseFloat(argv[++i], &op.mix) || op.mix < 0.0f || op.mix > 1.0f) return false;
    } else if (std::strcmp(a, "--voice") == 0 && i + 1 < argc) {
      if (voices >= 2 || !parseVoice(argv[++i], hp.slots[voices])) return false;
      ++voices;
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
  if (voices > 0 && !haveHarmony) return false;
  rp.pitchOn = haveHarmony || haveOctave;
  // Unison runs only when asked for; --on 0 with --depth keeps it off.
  rp.unisonOn = haveOn ? p.on : haveDepth;
  p.on = rp.unisonOn;
  rp.slapOn = haveSlap;
  rp.distOn = haveDrive;
  rp.reverbOn = haveReverb;
  rp.reverb.on = haveReverb;
  return rp.pitchOn || rp.unisonOn || rp.slapOn || rp.distOn || rp.reverbOn;
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

  static cv::PitchFx pitchFx;
  static cv::Unison unison;
  static cv::Slapback slapback;
  static cv::Distortion distortion;
  static cv::Reverb reverb;
  pitchFx.reset();
  unison.reset();
  slapback.reset();
  distortion.reset();
  reverb.reset();

  float inBuf[cv::kBlock];
  float bufA[cv::kBlock];
  float bufB[cv::kBlock];
  float* const scratch[2] = {bufA, bufB};
  int rc = 0;
  for (;;) {
    ma_uint64 got = 0;
    const ma_result r = ma_decoder_read_pcm_frames(&dec, inBuf, cv::kBlock, &got);
    if (got > 0) {
      const int n = static_cast<int>(got);
      // Each stage writes the scratch buffer the previous stage did not.
      const float* src = inBuf;
      int next = 0;
      if (rp.pitchOn) {
        pitchFx.process(src, scratch[next], n, rp.pitchFx);
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
      if (rp.reverbOn) {
        reverb.process(src, scratch[next], n, rp.reverb);
        src = scratch[next];
      }
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
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  const char* in = nullptr;
  const char* out = nullptr;
  RenderParams rp;
  if (!parseArgs(argc, argv, &in, &out, rp)) return usage();
  return render(in, out, rp);
}
