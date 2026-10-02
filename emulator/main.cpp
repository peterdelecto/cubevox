// cubevox prototype face: loads a loop, plays it through the engine, mirrors
// the panel controls. Windowless probe: --layout.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include <sys/stat.h>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#endif
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "../third_party/miniaudio.h"

#include "../emulator/macros.h"
#include "../emulator/open_panel.h"
#include "../emulator/stage_feedback.h"
#include "engine/common.h"
#include "engine/distortion.h"
#include "engine/gate.h"
#include "engine/pitch_fx.h"
#include "engine/slapback.h"
#include "engine/polish.h"
#include "engine/reverb.h"
#include "engine/spring.h"
#include "engine/unison.h"

namespace {

constexpr int kWindowW = 1440;
constexpr int kWindowH = 840;  // owner's laptop shows ~847 px of window
constexpr float kHarmonyColumnW = 400.0f;  // Harmony column width
constexpr float kLabelW = 175.0f;          // room right of each slider for its label
constexpr float kHarmonyEngineX = 140.0f;  // Engine radios beside the HARMONY checkbox
constexpr float kHarmonyKeyW = 200.0f;     // KEY combo, leaves room for Chromatic
constexpr float kFormantW = 60.0f;         // FORMANT slider at the end of each Menu row
constexpr float kMenuRadioX = 52.0f;       // Menu row: radios start after the voice name
constexpr float kAutotuneKeyW = 80.0f;     // KEY combo beside Link key to Harmony
constexpr float kMeterW = 100.0f;
constexpr float kLoopNameW = 110.0f;                 // loop file name slot in the header, px
constexpr float kHeaderScale = 1.4f;                  // transport row type size vs the face
constexpr ImVec2 kHeaderPadding = ImVec2(6.0f, 6.0f);  // transport row frame padding, px
constexpr int kProbeFrames = 4;
constexpr float kMeterFloorDb = -60.0f;
constexpr float kSilenceDb = -120.0f;
constexpr float kMeterDecayDbPerFrame = 0.6f;

// The emulator opens with the knob where the owner left it; on the box the
// pot decides (owner 2026-10-01: DEPTH 80 %).
constexpr float kStartDepth = 0.8f;
constexpr float kStartIntensity = 0.25f;
constexpr float kStartDrive = 0.2f;
constexpr float kStartTone = 0.5f;   // BD-2 TONE at noon is flat
// Input soft gate: live-stage defaults (owner 2026-10-01). Range stays partial so a
// mis-trigger never reads as a dropout; release is short by owner choice.
constexpr float kInputGateThresholdDb = -40.0f;
constexpr float kInputGateRangeDb = -12.0f;
constexpr float kInputGateHoldMs = 100.0f;
constexpr float kInputGateReleaseMs = 25.0f;

void applyInputGateDefaults(cv::GateParams& g) {
  g.thresholdDb = kInputGateThresholdDb;
  g.tuning = cv::GateTuning{};
  g.tuning.rangeDb = kInputGateRangeDb;
  g.tuning.holdMs = kInputGateHoldMs;
  g.tuning.releaseMs = kInputGateReleaseMs;
}

struct ProtoParams {
  bool playing = false;
  cv::GateParams inputGate;
  cv::GateParams gate;
  cv::PitchFxParams pitchFx;
  // The box has one KEY encoder; linked, Autotune copies Harmony's key.
  bool linkAutotuneKey = true;
  cv::UnisonParams unison{false, kStartDepth, {}};
  cv::SlapbackParams slapback{true, kStartIntensity, {}};
  cv::DistortionParams distortion{true, kStartDrive, kStartTone, {}};
  cv::ReverbParams reverb;
  cv::PolishParams eq;
  // Cmd+Shift+D shows our test controls: Advanced and the input gate tuning.
  bool dev = false;
  // Advanced swaps the musician sliders for the raw tuning nodes.
  bool advanced = false;
  // Pedal mode shows each effect as a stompbox with only its panel knobs.
  bool pedalMode = false;
  cv::macros::State macros;
  // Prototype-only test signal. Not a panel control, not in Print tuning.
  bool stageFeedback = false;
  float feedbackAmount = 50.0f;    // percent of the simulator range
  float feedbackMovement = 53.0f;  // percent of the capped range (= 40 % of the old one)

  // Every effect opens off except the input gate, which has no switch on the
  // face. Engine defaults stay on for the firmware.
  ProtoParams() {
    inputGate.on = true;
    applyInputGateDefaults(inputGate);
    gate.on = false;
    pitchFx.harmony.on = false;
    // Owner default voices: High louder, Higher loud; third slot unused (matches HarmonyMenu).
    pitchFx.harmony.slots[0] = {cv::HarmonyVoice::High, 3, 0.0f};
    pitchFx.harmony.slots[1] = {cv::HarmonyVoice::Higher, 2, 0.0f};
    pitchFx.harmony.slots[2] = {cv::HarmonyVoice::Low, 0, 0.0f};
    pitchFx.octave.on = false;
    unison.on = false;
    slapback.on = false;
    distortion.on = false;
    reverb.on = false;
    eq.on = false;
  }
};

struct ProtoState {
  float peakDb = kSilenceDb;
  float playheadNorm = 0.0f;
  float pitchHz = 0.0f;
  bool voiced = false;
  float correctionSemis = 0.0f;
  float inGateDb = 0.0f;
  float gateDb = 0.0f;
};

struct LoopBuffer {
  std::vector<float> samples;
};

// ---- Shared state ----------------------------------------------------------

std::atomic<LoopBuffer*> gCurrentLoop{nullptr};
std::atomic<uint64_t> gCallbackCount{0};

struct RetiredBuffer {
  LoopBuffer* buffer;
  uint64_t retireAtCount;
};
std::vector<RetiredBuffer> gRetired;  // UI-thread-only

ProtoParams gParamsBuf[2];
std::atomic<int> gParamsPublishIndex{0};
int gParamsWriteIndex = 1;  // UI-thread-only

ProtoState gStateBuf[2];
std::atomic<int> gStatePublishIndex{0};
int gStateWriteIndex = 1;  // audio-thread-only

std::string gLoopPath;  // UI-thread-only

// Audio-thread-only.
cv::Gate gInputGate;
cv::Gate gGate;
cv::PitchFx gPitchFx;
cv::Unison gUnison;
cv::Slapback gSlapback;
cv::Distortion gDistortion;
cv::Reverb gReverb;
cv::Polish gPolish;
cv::stagefb::StageFeedback gStageFeedback;
LoopBuffer* gLastLoop = nullptr;
size_t gReadPos = 0;

// ---- Loop loading ------------------------------------------------------------

void loadLoop(const std::string& path) {
  ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, cv::kSampleRate);
  ma_decoder decoder;
  if (ma_decoder_init_file(path.c_str(), &cfg, &decoder) != MA_SUCCESS) {
    std::fprintf(stderr, "could not open %s\n", path.c_str());
    return;
  }
  ma_uint64 frames = 0;
  ma_decoder_get_length_in_pcm_frames(&decoder, &frames);
  if (frames == 0) {
    ma_decoder_uninit(&decoder);
    std::fprintf(stderr, "empty loop %s\n", path.c_str());
    return;
  }
  LoopBuffer* buf = new LoopBuffer();
  buf->samples.resize(frames);
  ma_uint64 framesRead = 0;
  ma_decoder_read_pcm_frames(&decoder, buf->samples.data(), frames, &framesRead);
  ma_decoder_uninit(&decoder);
  buf->samples.resize(framesRead);
  gLoopPath = path;

  LoopBuffer* old = gCurrentLoop.exchange(buf, std::memory_order_acq_rel);
  if (old != nullptr) {
    gRetired.push_back({old, gCallbackCount.load(std::memory_order_relaxed) + 2});
  }
}

void reclaimRetired() {
  const uint64_t now = gCallbackCount.load(std::memory_order_relaxed);
  for (size_t i = 0; i < gRetired.size();) {
    if (now >= gRetired[i].retireAtCount) {
      delete gRetired[i].buffer;
      gRetired[i] = gRetired.back();
      gRetired.pop_back();
    } else {
      ++i;
    }
  }
}

// ---- Audio callback ----------------------------------------------------------

// Copies n looped samples into dst, wrapping at the loop end.
void readLoop(const LoopBuffer& loop, float* dst, int n) {
  const size_t len = loop.samples.size();
  for (int i = 0; i < n; ++i) {
    dst[i] = loop.samples[gReadPos];
    if (++gReadPos >= len) gReadPos = 0;
  }
}

void publishState(float peak, const LoopBuffer* loop) {
  ProtoState& s = gStateBuf[gStateWriteIndex];
  s.peakDb = peak > 1e-6f ? 20.0f * std::log10(peak) : kSilenceDb;
  s.pitchHz = gPitchFx.pitch().hz;
  s.voiced = gPitchFx.pitch().voiced;
  s.correctionSemis = gPitchFx.correctionSemis();
  s.inGateDb = gInputGate.gainDb();
  s.gateDb = gGate.gainDb();
  s.playheadNorm = (loop != nullptr && !loop->samples.empty())
                       ? static_cast<float>(gReadPos) / static_cast<float>(loop->samples.size())
                       : 0.0f;
  gStatePublishIndex.store(gStateWriteIndex, std::memory_order_release);
  gStateWriteIndex ^= 1;
}

void dataCallback(ma_device*, void* output, const void*, ma_uint32 frameCount) {
  float* out = static_cast<float*>(output);
  const ProtoParams params =
      gParamsBuf[gParamsPublishIndex.load(std::memory_order_acquire)];
  LoopBuffer* loop = gCurrentLoop.load(std::memory_order_acquire);
  gCallbackCount.fetch_add(1, std::memory_order_relaxed);

  if (loop != gLastLoop) {
    gLastLoop = loop;
    gReadPos = 0;
    gInputGate.reset();
    gGate.reset();
    gPitchFx.reset();
    gUnison.reset();
    gSlapback.reset();
    gDistortion.reset();
    gReverb.reset();
    gPolish.reset();
    gStageFeedback.reset(1u);
  }
  // Movement 100 % = 75 % of the simulator range (owner 2026-10-02: full was too much).
  constexpr float kFeedbackCap = 0.75f;  // Movement only; Amount uses the full range (owner)
  gStageFeedback.configure(params.stageFeedback && params.playing,
                           0.01f * params.feedbackAmount,
                           kFeedbackCap * 0.01f * params.feedbackMovement);
  if (!params.playing || loop == nullptr || loop->samples.empty()) {
    std::memset(out, 0, sizeof(float) * 2 * frameCount);
    publishState(0.0f, loop);
    return;
  }

  std::array<float, cv::kBlock> in;
  std::array<float, cv::kBlock> tmp;
  std::array<float, cv::kBlock> mono;
  float peak = 0.0f;
  ma_uint32 done = 0;
  while (done < frameCount) {
    const int n = static_cast<int>(std::min<ma_uint32>(cv::kBlock, frameCount - done));
    readLoop(*loop, in.data(), n);
    const bool feedback = gStageFeedback.enabled();
    if (feedback) {
      for (int i = 0; i < n; ++i) in[i] += gStageFeedback.returnSample(in[i]);
    }
    gInputGate.process(in.data(), mono.data(), n, params.inputGate);
    gPitchFx.process(mono.data(), tmp.data(), n, params.pitchFx);
    gUnison.process(tmp.data(), mono.data(), n, params.unison);
    gSlapback.process(mono.data(), tmp.data(), n, params.slapback);
    gDistortion.process(tmp.data(), mono.data(), n, params.distortion);
    gGate.process(mono.data(), tmp.data(), n, params.gate);
    gReverb.process(tmp.data(), mono.data(), n, params.reverb);
    gPolish.process(mono.data(), tmp.data(), n, params.eq);
    if (feedback) {
      for (int i = 0; i < n; ++i) gStageFeedback.push(tmp[i]);
    }
    for (int i = 0; i < n; ++i) {
      out[2 * (done + i)] = tmp[i];
      out[2 * (done + i) + 1] = tmp[i];
      peak = std::max(peak, std::fabs(tmp[i]));
    }
    done += n;
  }
  publishState(peak, loop);
}

void publishParams(const ProtoParams& p) {
  gParamsBuf[gParamsWriteIndex] = p;
  gParamsPublishIndex.store(gParamsWriteIndex, std::memory_order_release);
  gParamsWriteIndex ^= 1;
}

// ---- UI ------------------------------------------------------------------------

const char* baseName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return path.c_str() + (slash == std::string::npos ? 0 : slash + 1);
}

char gTuningText[8192];  // last copied settings text

// Plain-text snapshot a person can paste into a message: Macros, Choices, Knobs.
// Returns the bytes written, newline included.
size_t snapshotLines(const ProtoParams& p, char* buf, size_t size) {
  const auto onOff = [](bool b) { return b ? "on" : "off"; };
  static const char* const kLevel[4] = {"Off", "Quiet", "Loud", "Louder"};
  static const char* const kReverb[3] = {"SPRING", "CHASM", "PARKER SPRING"};
  const cv::HarmonyParams& h = p.pitchFx.harmony;
  const cv::AutotuneParams& at = p.pitchFx.autotune;
  const cv::OctaveParams& o = p.pitchFx.octave;
  const cv::ReverbParams& r = p.reverb;
  const int re = r.engine == cv::kReverbChasm ? 1 : (r.engine == cv::kReverbParker ? 2 : 0);
  const int atKey = p.linkAutotuneKey ? h.key : at.key;
  const auto lvl = [&](int i) { return kLevel[h.slots[static_cast<size_t>(i)].level & 3]; };
  const float decay = re == 1 ? r.chasm.decay : (re == 2 ? r.parker.tension : r.spring.tension);
  const float dwell = re == 1 ? r.chasm.dwell : (re == 2 ? r.parker.dwell : r.spring.dwell);
  char macros[640];
  cv::macros::formatAll(p.macros, macros, sizeof(macros));
  const int n = std::snprintf(
      buf, size,
      "%s\n"
      "Choices: Input gate on; Gate %s; Autotune %s, engine %c, key %s, chromatic %s; "
      "Octave %s, engine %c; Harmony %s, engine %c, chromatic %s, voices High %s / "
      "Higher %s, Follow my bends %s; Unison %s; Slapback %s; "
      "Distortion %s; Reverb %s, engine %s; Output EQ %s\n"
      "Knobs: Input gate threshold %.0f dB; Gate THRESHOLD %.0f dB, DECAY %.0f ms; Autotune KEY %s, RESPONSE "
      "%.0f ms, Pull range %.1f st; Octave SEMITONES %+d, FORMANT %+d st, MIX %.0f %%; Harmony KEY %s, MIX %.0f %%, "
      "voice formants High %+d / Higher %+d st; Unison DEPTH "
      "%.0f %%, RATE %.0f %%; Slapback INTENSITY %.0f %%, TIME %.0f ms; Distortion DRIVE %.0f %%, TONE %.0f %%; Reverb DECAY "
      "%.0f %%, DWELL %.0f %%, MIX %.0f %%\n",
      macros, onOff(p.gate.on), onOff(at.on), 'A' + at.engine,
      p.linkAutotuneKey ? "linked to Harmony" : "own", onOff(at.chromatic), onOff(o.on),
      'A' + o.engine, onOff(h.on), 'A' + h.engine, onOff(h.chromatic), lvl(0), lvl(1),
      onOff(!h.tuning.snapToScale), onOff(p.unison.on),
      onOff(p.slapback.on), onOff(p.distortion.on), onOff(r.on), kReverb[re], onOff(p.eq.on),
      static_cast<double>(p.inputGate.thresholdDb), static_cast<double>(p.gate.thresholdDb),
      static_cast<double>(p.gate.tuning.releaseMs),
      cv::kKeyName[atKey], static_cast<double>(at.responseMs),
      static_cast<double>(at.tuning.maxCorrectSemis), o.semitones,
      static_cast<int>(std::lround(o.formant)), static_cast<double>(o.mix * 100.0f),
      cv::kKeyName[h.key], static_cast<double>(h.mix * 100.0f),
      static_cast<int>(std::lround(h.slots[0].formant)),
      static_cast<int>(std::lround(h.slots[1].formant)),
      static_cast<double>(p.unison.depth * 100.0f),
      static_cast<double>(p.macros.pos[cv::macros::UnisonMotion]),
      static_cast<double>(p.slapback.intensity * 100.0f),
      static_cast<double>(p.slapback.tuning.timeMs),
      static_cast<double>(p.distortion.drive * 100.0f),
      static_cast<double>(p.distortion.tone * 100.0f), static_cast<double>(decay * 100.0f),
      static_cast<double>(dwell * 100.0f), static_cast<double>(r.mix * 100.0f));
  return n < 0 ? 0 : std::min(static_cast<size_t>(n), size - 1);
}

// Finder launches have no stdout, so the text also goes to the clipboard
// and into a read-only field beside the button.
void printTuning(const cv::HarmonyTuning& h, const cv::OctaveTuning& o,
                 const cv::UnisonTuning& t, const cv::SlapbackTuning& s,
                 const cv::DistortionTuning& d, const cv::SpringTuning& sp,
                 const cv::ChasmTuning& c, const cv::SpringCTuning& pk,
                 const cv::PolishParams& e, const cv::GateTuning& ig, const cv::GateTuning& g,
                 const cv::AutotuneTuning& at, const ProtoParams& params) {
  const int prefix = static_cast<int>(snapshotLines(params, gTuningText, sizeof(gTuningText)));
  char line[512];
  int len = std::snprintf(
      line, sizeof(line),
      "HarmonyTuning{{%.1ff, %.1ff, %.1ff}, %.1ff, %.2ff, %s, %s",
      h.levelDb[0], h.levelDb[1], h.levelDb[2], h.glideMs, h.voicedThreshold,
      h.muteUnvoiced ? "true" : "false", h.snapToScale ? "true" : "false");
  const int8_t* tables[] = {h.lower, h.low, h.high, h.higher};
  for (const int8_t* row : tables) {
    len += std::snprintf(line + len, sizeof(line) - len, ", {%d, %d, %d, %d, %d, %d, %d}",
                         row[0], row[1], row[2], row[3], row[4], row[5], row[6]);
  }
  const int8_t* cs = h.chromaticSemis;
  const cv::ShifterTuning& sh = h.shifter;
  std::snprintf(line + len, sizeof(line) - len,
                ", {%d, %d, %d, %d, %d}, {%.1ff, %.1ff, %.1ff}, {%.2ff, %.2ff, %.0ff, %.0ff, %d}}",
                cs[0], cs[1], cs[2], cs[3], cs[4], h.trimDb[0], h.trimDb[1], h.trimDb[2],
                sh.grainPeriods, sh.epochSearch, sh.epochLpHz, sh.grainWindowMs, sh.grainCount);
  std::snprintf(
      gTuningText + prefix, sizeof(gTuningText) - static_cast<size_t>(prefix),
      "%s\n"
      "OctaveTuning{%.1ff, %.1ff, %s, %.2ff, %.2ff, %.0ff, %.1ff, %.1ff, %.0ff, %d, %.1ff}\n"
      "UnisonTuning{{%.1ff, %.1ff}, {%.2ff, %.2ff}, %.1ff, %.1ff, %.1ff, {%.1ff, %.1ff}, %.0ff}\n"
      "SlapbackTuning{%.1ff, %.0ff, %.2ff, %.1ff}\n"
      "DistortionTuning{%.0ff, %.0ff, %.1ff, %.0ff, %.1ff, %.0ff, %.1ff, %.0ff, %.1ff, %.1ff, "
      "%.0ff, %.0ff, %.1ff, %.2ff, %.2ff, %.0ff, %.1ff, %.1ff, %.1ff, %.0ff, %.1ff, %.2ff, %.1ff, %.2ff, %s}\n"
      "SpringTuning{%.2ff, %.0ff, %.2ff, %.2ff, %.1ff, %.2ff, %.1ff, %.1ff, %.2ff, %.2ff, %d, %d, %.1ff, "
      "%.2ff, %.1ff, %.1ff, %.3ff}\n"
      "ChasmTuning{%.2ff, %.2ff, %.0ff, %.2ff, %.2ff, %.0ff, %.0ff, %.0ff, %.2ff, %.2ff, %.2ff, "
      "%.1ff, %.1ff, %.1ff, %.1ff}\n"
      "PolishTuning{%.2ff, %.2ff, %.0ff, %.1ff}\n"
      "// PolishParams: hpHz %.0f, dip %.0f Hz %.1f dB, presence %.0f Hz %.1f dB, air %.1f dB",
      line, o.levelDb, o.glideMs, o.muteUnvoiced ? "true" : "false", o.grainPeriods,
      o.epochSearch, o.epochLpHz, o.trimDbA, o.trimDbB, o.grainWindowMs, o.grainCount, o.trimDbC,
      t.baseDelayMs[0], t.baseDelayMs[1], t.lfoHz[0], t.lfoHz[1], t.swingMinMs,
      t.swingMaxMs, t.wetMaxDb, t.detuneCents[0], t.detuneCents[1], t.windowMs,
      s.timeMs, s.lowpassHz, s.feedback, s.wetMaxDb, d.inputHpHz, d.s1BassHz, d.s1BassDb, d.s1LpHz, d.gain1Max, d.stackBassHz, d.stackBassDb,
      d.stackTrebleHz, d.stackTrebleDb, d.stackLossDb, d.s2HpHz, d.s2LpHz, d.gain2Max,
      d.railAsym, d.railSoft, d.trebleCutHz, d.trebleCutDb, d.toneMinDb, d.toneMaxDb, d.bassPeakHz,
      d.bassPeakDb, d.bassPeakQ, d.trimDb, d.fadeDrive, d.oversample ? "true" : "false",
      sp.inputGain, sp.hpHz, sp.tensionLo, sp.tensionHi, sp.dwellDrive, sp.dwellComp, sp.hfMixDbLo,
      sp.hfMixDbHi, sp.rippleGain, sp.splashDiffuse, sp.hfSections, sp.springs, sp.modDepth,
      sp.modRateHz, sp.boingDb, sp.wetDb, sp.tankTrim, c.timeLo, c.timeHi, c.trebleLossHz,
      c.loopTrebleCut, c.inputTrebleCut, c.bassCutHz, c.bassCutHzTop, c.wobbleDepthMax,
      c.wobbleRateLo, c.wobbleRateHi, c.inputTrim, c.wobbleLevelDb, c.dwellDrive, c.dwellComp, c.wetDb, e.tuning.dipQ,
      e.tuning.presenceQ, e.tuning.airHz, e.tuning.trimDb, e.hpHz, e.dipHz, e.dipDb, e.presenceHz, e.presenceDb,
      e.airDb);
  size_t used = std::strlen(gTuningText);
  std::snprintf(
      gTuningText + used, sizeof(gTuningText) - used,
      "\nSpringCTuning{%.1ff, %.0ff, %d, %.2ff, %.2ff, %.2ff, %.2ff, %.2ff, %d, %.2ff, %.1ff, "
      "%.2ff, %.0ff, %.0ff, %.0ff, %.2ff, %.2ff, %.1ff, %.3ff, %d, {%.3ff, %.3ff, %.3ff}, "
      "{%.3ff, %.3ff, %.3ff}, %.0ff, %.0ff, %.1ff, %.2ff, %.0ff, %.1ff, %.2ff, %.3ff, %.1ff}",
      pk.tdMs, pk.fcLfHz, pk.mLow, pk.aLf, pk.gLo, pk.gHi, pk.gComp, pk.hfRatio, pk.mHigh, pk.aHf,
      pk.hfMixDb, pk.cross, pk.eqPeakHz, pk.eqBwHz, pk.lowHz, pk.echoGain, pk.rippleGain,
      pk.modDepth, pk.modPole, pk.springs, pk.tdFactor[0], pk.tdFactor[1], pk.tdFactor[2],
      pk.fcFactor[0], pk.fcFactor[1], pk.fcFactor[2], pk.hpHz, pk.lpHz, pk.dwellDrive,
      pk.dwellComp, pk.presenceHz, pk.presenceDb, pk.presenceQ, pk.tankTrim, pk.wetDb);
  used = std::strlen(gTuningText);
  std::snprintf(
      gTuningText + used, sizeof(gTuningText) - used,
      "\nInputGateTuning{%.1ff, %.0ff, %.0ff, %.1ff, %.1ff, %.0ff, %.1ff}"
      "\nGateTuning{%.1ff, %.0ff, %.0ff, %.1ff, %.1ff, %.0ff, %.1ff}"
      "\nAutotuneTuning{%.1ff, %.1ff, {%.2ff, %.2ff, %.0ff, %.0ff, %d}}",
      ig.attackMs, ig.holdMs, ig.releaseMs, ig.rangeDb, ig.kneeDb, ig.detectorHpHz, ig.hysteresisDb,
      g.attackMs, g.holdMs, g.releaseMs, g.rangeDb, g.kneeDb, g.detectorHpHz, g.hysteresisDb,
      at.trimDb, at.maxCorrectSemis, at.shifter.grainPeriods, at.shifter.epochSearch,
      at.shifter.epochLpHz, at.shifter.grainWindowMs, at.shifter.grainCount);
  std::printf(
      "// AutotuneTuning: trimDb, maxCorrectSemis, "
      "shifter{grainPeriods, epochSearch, epochLpHz, grainWindowMs, grainCount}\n"
      "// GateTuning: attackMs, holdMs, releaseMs, rangeDb, kneeDb, detectorHpHz, hysteresisDb\n"
      "// HarmonyTuning: {levelDb[3]}, glideMs, voicedThreshold, muteUnvoiced, snapToScale, "
      "lower, low, high, higher, chromaticSemis, trimDb[A, B, C], "
      "shifter{grainPeriods, epochSearch, epochLpHz, grainWindowMs, grainCount}\n"
      "// OctaveTuning: levelDb, glideMs, muteUnvoiced, grainPeriods, epochSearch, epochLpHz, trimDbA, trimDbB, grainWindowMs, grainCount, trimDbC\n"
      "// UnisonTuning: {baseDelayMs[0], baseDelayMs[1]}, {lfoHz[0], lfoHz[1]}, swingMinMs, "
      "swingMaxMs, wetMaxDb, {detuneCents[0], detuneCents[1]}, windowMs\n"
      "// SlapbackTuning: timeMs, lowpassHz, feedback, wetMaxDb\n"
      "// DistortionTuning: inputHpHz, s1BassHz, s1BassDb, s1LpHz, gain1Max, stackBassHz, "
      "stackBassDb, stackTrebleHz, stackTrebleDb, stackLossDb, s2HpHz, s2LpHz, gain2Max, "
      "railAsym, railSoft, trebleCutHz, trebleCutDb, toneMinDb, toneMaxDb, bassPeakHz, bassPeakDb, "
      "bassPeakQ, trimDb, fadeDrive, oversample\n"
      "// SpringTuning: inputGain, hpHz, tensionLo, tensionHi, dwellDrive, dwellComp, hfMixDbLo, "
      "hfMixDbHi, rippleGain, splashDiffuse, hfSections, springs, modDepth, modRateHz, "
      "boingDb, wetDb, tankTrim\n"
      "// ChasmTuning: timeLo, timeHi, trebleLossHz, loopTrebleCut, inputTrebleCut, bassCutHz, "
      "bassCutHzTop, wobbleDepthMax, wobbleRateLo, wobbleRateHi, inputTrim, wobbleLevelDb, "
      "dwellDrive, dwellComp, wetDb\n// PolishTuning: dipQ, presenceQ, airHz, trimDb\n"
      "// SpringCTuning: tdMs, fcLfHz, mLow, aLf, gLo, gHi, gComp, hfRatio, mHigh, aHf, hfMixDb, "
      "cross, eqPeakHz, eqBwHz, lowHz, echoGain, rippleGain, modDepth, modPole, springs, "
      "{tdFactor[3]}, {fcFactor[3]}, hpHz, lpHz, dwellDrive, dwellComp, presenceHz, presenceDb, "
      "presenceQ, tankTrim, wetDb\n%s\n",
      gTuningText);
  std::fflush(stdout);
  ImGui::SetClipboardText(gTuningText);
}

// Probe scenario: which Tuning header and sub-node are forced open. Null block = live UI.
struct ProbeOpen {
  bool active = false;
  const char* block = nullptr;
  const char* node = nullptr;
  bool advanced = false;   // Advanced view instead of the macro view
  int reverbEngine = -1;   // forced reverb engine, -1 keeps the live one
  bool pedal = false;      // pedal mode instead of the full face
};
bool gAdvanced = false;  // UI-thread-only; mirrors ProtoParams::advanced
bool gDev = false;       // UI-thread-only; mirrors ProtoParams::dev
ProbeOpen gProbeOpen;

bool probeMatch(const char* want, const char* name) {
  return want != nullptr && std::strcmp(want, name) == 0;
}

// One Tuning header open per column at a time (accordion), so the face never grows
// past what the probe checked: the probe opens exactly one header per scenario.
constexpr int kColumnCount = 4;
int gCurrentColumn = 0;
std::array<const char*, kColumnCount> gOpenTuning{};

// Collapsed Tuning header under one effect block.
bool tuningHeader(const char* block) {
  const char*& openInColumn = gOpenTuning[static_cast<size_t>(gCurrentColumn)];
  if (gProbeOpen.active) {
    ImGui::SetNextItemOpen(probeMatch(gProbeOpen.block, block));
  } else if (openInColumn != nullptr && std::strcmp(openInColumn, block) != 0) {
    ImGui::SetNextItemOpen(false);
  }
  const bool open = ImGui::CollapsingHeader("Tuning");
  if (!gProbeOpen.active) {
    if (open) openInColumn = block;
    else if (openInColumn != nullptr && std::strcmp(openInColumn, block) == 0) openInColumn = nullptr;
  }
  return open;
}

// Sub-node inside a Tuning header; caller pops with TreePop.
bool tuningNode(const char* name) {
  if (gProbeOpen.active) ImGui::SetNextItemOpen(probeMatch(gProbeOpen.node, name));
  return ImGui::TreeNode(name);
}

// Parent node whose sub-sub-nodes are named "<name> ..."; open for the parent or any child.
bool tuningGroup(const char* name) {
  if (gProbeOpen.active) {
    const char* want = gProbeOpen.node;
    ImGui::SetNextItemOpen(want != nullptr && std::strncmp(want, name, std::strlen(name)) == 0);
  }
  return ImGui::TreeNode(name);
}

// Sub-sub-node whose probe name differs from its on-screen label.
bool tuningLeaf(const char* probeName, const char* label) {
  if (gProbeOpen.active) ImGui::SetNextItemOpen(probeMatch(gProbeOpen.node, probeName));
  return ImGui::TreeNode(probeName, "%s", label);
}

// Long tuning sections split into sub-nodes so no column ever needs a scroll bar.
void drawHarmonyRaw(cv::HarmonyTuning& h) {
  if (tuningNode("Levels")) {
    ImGui::SliderFloat("Level low", &h.levelDb[0], -24.0f, 6.0f, "%.1f dB");
    ImGui::SliderFloat("Level medium", &h.levelDb[1], -24.0f, 6.0f, "%.1f dB");
    ImGui::SliderFloat("Level high", &h.levelDb[2], -24.0f, 6.0f, "%.1f dB");
    ImGui::TreePop();
  }
  if (tuningNode("Trims")) {
    ImGui::SliderFloat("Trim A", &h.trimDb[0], -12.0f, 18.0f, "%.1f dB");
    ImGui::SliderFloat("Trim B", &h.trimDb[1], -12.0f, 18.0f, "%.1f dB");
    ImGui::SliderFloat("Trim C", &h.trimDb[2], -12.0f, 18.0f, "%.1f dB");
    ImGui::TreePop();
  }
  if (tuningNode("Tracking")) {
    ImGui::SliderFloat("Tracking speed (ms)", &h.glideMs, 0.0f, 100.0f, "%.0f ms");
    ImGui::SliderFloat("Voiced threshold", &h.voicedThreshold, 0.05f, 0.4f, "%.2f");
    ImGui::Checkbox("Mute unvoiced (A, B)", &h.muteUnvoiced);
    ImGui::Checkbox("Snap to scale", &h.snapToScale);
    ImGui::TreePop();
  }
  if (tuningNode("Chromatic intervals")) {
    // Rows follow HarmonyVoice; Fixed (index 2) is unused in chromatic mode.
    static const char* const kName[5] = {"Lower", "Low", nullptr, "High", "Higher"};
    for (int v = 0; v < 5; ++v) {
      if (kName[v] == nullptr) continue;
      int semis = h.chromaticSemis[v];
      if (ImGui::SliderInt(kName[v], &semis, -12, 12, "%+d st"))
        h.chromaticSemis[v] = static_cast<int8_t>(semis);
    }
    ImGui::TreePop();
  }
  if (tuningNode("Shifter B/C")) {
    cv::ShifterTuning& s = h.shifter;
    ImGui::SliderFloat("Grain length (B)", &s.grainPeriods, 1.5f, 3.0f, "%.2f periods");
    ImGui::SliderFloat("Epoch search (B)", &s.epochSearch, 0.0f, 0.3f, "%.2f period");
    ImGui::SliderFloat("Epoch low-pass (B)", &s.epochLpHz, 100.0f, 4000.0f, "%.0f Hz",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Grain window (C)", &s.grainWindowMs, 20.0f, 80.0f, "%.0f ms");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Grains (C)");
    ImGui::SameLine();
    ImGui::RadioButton("2", &s.grainCount, 2);
    ImGui::SameLine();
    ImGui::RadioButton("4", &s.grainCount, 4);
    ImGui::TreePop();
  }
}

void drawOctaveRaw(cv::OctaveTuning& o) {
  ImGui::SliderFloat("Level", &o.levelDb, -24.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("Trim A", &o.trimDbA, -12.0f, 12.0f, "%.1f dB");
  ImGui::SliderFloat("Trim B", &o.trimDbB, -12.0f, 12.0f, "%.1f dB");
  ImGui::SliderFloat("Trim C", &o.trimDbC, -12.0f, 12.0f, "%.1f dB");
  ImGui::SliderFloat("Tracking speed (ms)", &o.glideMs, 0.0f, 100.0f, "%.0f ms");
  ImGui::Checkbox("Mute unvoiced", &o.muteUnvoiced);
  ImGui::SliderFloat("Grain length (B)", &o.grainPeriods, 1.5f, 3.0f, "%.2f periods");
  ImGui::SliderFloat("Epoch search (B)", &o.epochSearch, 0.0f, 0.3f, "%.2f period");
  ImGui::SliderFloat("Epoch low-pass (B)", &o.epochLpHz, 100.0f, 4000.0f, "%.0f Hz",
                     ImGuiSliderFlags_Logarithmic);
}

void drawAutotuneRaw(cv::AutotuneTuning& t) {
  ImGui::SliderFloat("Trim", &t.trimDb, -12.0f, 12.0f, "%.1f dB");
  ImGui::SliderFloat("Grain length (B)", &t.shifter.grainPeriods, 1.5f, 3.0f, "%.2f periods");
  ImGui::SliderFloat("Epoch search (B)", &t.shifter.epochSearch, 0.0f, 0.3f, "%.2f period");
  ImGui::SliderFloat("Epoch low-pass (B)", &t.shifter.epochLpHz, 100.0f, 4000.0f, "%.0f Hz",
                     ImGuiSliderFlags_Logarithmic);
}

void drawUnisonRaw(cv::UnisonTuning& t) {
  ImGui::TextDisabled("DEPTH scales both methods; the blend lives in these constants.");
  if (tuningNode("Chorus (LFO-wobbled delay)")) {
    ImGui::SliderFloat("Base delay 1", &t.baseDelayMs[0], 5.0f, 40.0f, "%.1f ms");
    ImGui::SliderFloat("Base delay 2", &t.baseDelayMs[1], 5.0f, 40.0f, "%.1f ms");
    ImGui::SliderFloat("LFO rate 1", &t.lfoHz[0], 0.1f, 3.0f, "%.2f Hz");
    ImGui::SliderFloat("LFO rate 2", &t.lfoHz[1], 0.1f, 3.0f, "%.2f Hz");
    ImGui::SliderFloat("Swing at depth 0", &t.swingMinMs, 0.0f, 1.0f, "%.2f ms");
    ImGui::SliderFloat("Swing at depth 1", &t.swingMaxMs, 0.5f, 6.0f, "%.2f ms");
    ImGui::TreePop();
  }
  if (tuningNode("Doubler (fixed detune, TC-Helicon style)")) {
    ImGui::SliderFloat("DETUNE 0", &t.detuneCents[0], -30.0f, 30.0f, "%.1f cents");
    ImGui::SliderFloat("DETUNE 1", &t.detuneCents[1], -30.0f, 30.0f, "%.1f cents");
    ImGui::SliderFloat("WINDOW", &t.windowMs, 5.0f, 30.0f, "%.0f ms");
    ImGui::TreePop();
  }
  if (tuningNode("Shared")) {
    ImGui::SliderFloat("Wet level at depth 1", &t.wetMaxDb, -24.0f, 0.0f, "%.1f dB");
    ImGui::TreePop();
  }
}

void drawSlapbackRaw(cv::SlapbackTuning& t) {
  ImGui::SliderFloat("Lowpass", &t.lowpassHz, 500.0f, 12000.0f, "%.0f Hz",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("Feedback", &t.feedback, 0.0f, 0.5f, "%.2f");
  ImGui::SliderFloat("Wet level at full", &t.wetMaxDb, -24.0f, 12.0f, "%.1f dB");
}

void drawDistortionRaw(cv::DistortionTuning& t) {
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
  if (tuningNode("Stage 1")) {
    ImGui::SliderFloat("Input high-pass", &t.inputHpHz, 10.0f, 200.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Bass cut corner", &t.s1BassHz, 100.0f, 2000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Bass cut depth", &t.s1BassDb, -24.0f, 0.0f, "%.1f dB");
    ImGui::SliderFloat("Top roll-off", &t.s1LpHz, 1000.0f, 12000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Gain at full", &t.gain1Max, 1.0f, 300.0f, "%.1f x", log);
    ImGui::TreePop();
  }
  if (tuningNode("Tone stack")) {
    ImGui::SliderFloat("Bass corner", &t.stackBassHz, 100.0f, 1000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Bass boost", &t.stackBassDb, 0.0f, 20.0f, "%.1f dB");
    ImGui::SliderFloat("Treble corner", &t.stackTrebleHz, 500.0f, 8000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Treble cut", &t.stackTrebleDb, -20.0f, 0.0f, "%.1f dB");
    ImGui::SliderFloat("Insertion loss", &t.stackLossDb, -40.0f, 0.0f, "%.1f dB");
    ImGui::TreePop();
  }
  if (tuningNode("Stage 2")) {
    ImGui::SliderFloat("High-pass", &t.s2HpHz, 20.0f, 500.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Low-pass", &t.s2LpHz, 1000.0f, 12000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Gain at full", &t.gain2Max, 1.0f, 300.0f, "%.1f x", log);
    ImGui::SliderFloat("Rail asymmetry", &t.railAsym, 0.0f, 0.5f, "%.2f");
    ImGui::SliderFloat("Rail soft edge", &t.railSoft, 0.01f, 0.5f, "%.2f");
    ImGui::Checkbox("2x oversampling", &t.oversample);
    ImGui::TreePop();
  }
  if (tuningNode("Post")) {
    ImGui::SliderFloat("Treble cut corner", &t.trebleCutHz, 300.0f, 5000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Treble cut", &t.trebleCutDb, -12.0f, 0.0f, "%.1f dB");
    ImGui::SliderFloat("Tone min dB", &t.toneMinDb, -24.0f, 0.0f, "%.1f dB");
    ImGui::SliderFloat("Tone max dB", &t.toneMaxDb, 0.0f, 12.0f, "%.1f dB");
    ImGui::SliderFloat("Bass peak centre", &t.bassPeakHz, 60.0f, 400.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Bass peak gain", &t.bassPeakDb, -6.0f, 12.0f, "%.1f dB");
    ImGui::SliderFloat("Bass peak Q", &t.bassPeakQ, 0.3f, 3.0f, "%.2f");
    ImGui::SliderFloat("Output trim", &t.trimDb, -12.0f, 12.0f, "%.1f dB");
    ImGui::SliderFloat("Fade-in span", &t.fadeDrive, 0.01f, 0.3f, "%.2f");
    ImGui::TreePop();
  }
}

void drawGateRaw(cv::GateTuning& t) {
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
  ImGui::SliderFloat("Attack", &t.attackMs, 0.1f, 50.0f, "%.1f ms", log);
  ImGui::SliderFloat("Hold", &t.holdMs, 0.0f, 500.0f, "%.0f ms");
  ImGui::SliderFloat("Release", &t.releaseMs, 5.0f, 1000.0f, "%.0f ms", log);
  ImGui::SliderFloat("Range", &t.rangeDb, -80.0f, 0.0f, "%.0f dB");
  ImGui::SliderFloat("Knee", &t.kneeDb, 0.0f, 20.0f, "%.1f dB");
  ImGui::SliderFloat("Detector high-pass", &t.detectorHpHz, 20.0f, 500.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Hysteresis", &t.hysteresisDb, 0.0f, 12.0f, "%.1f dB");
}

void drawPolishRaw(cv::PolishTuning& t) {
  ImGui::SliderFloat("Low-mid Q", &t.dipQ, 0.3f, 3.0f, "%.2f");
  ImGui::SliderFloat("Presence Q", &t.presenceQ, 0.3f, 3.0f, "%.2f");
  ImGui::SliderFloat("Air corner", &t.airHz, 4000.0f, 16000.0f, "%.0f Hz",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("Output trim", &t.trimDb, -6.0f, 6.0f, "%.1f dB");
}

void drawParkerTuning(cv::SpringCTuning& p) {
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
  if (!tuningGroup("Parker")) return;
  if (tuningLeaf("Parker tank", "Tank")) {
    ImGui::SliderFloat("Delay T_D", &p.tdMs, 20.0f, 120.0f, "%.1f ms");
    ImGui::SliderFloat("Stretch corner", &p.fcLfHz, 2000.0f, 6000.0f, "%.0f Hz", log);
    ImGui::SliderInt("Sections", &p.mLow, 1, 100);
    ImGui::SliderFloat("Section pole", &p.aLf, 0.3f, 0.9f, "%.2f");
    ImGui::SliderFloat("Gain at tension 0", &p.gLo, 0.2f, 0.97f, "%.2f");
    ImGui::SliderFloat("Gain at tension 1", &p.gHi, 0.2f, 0.97f, "%.2f");
    ImGui::SliderFloat("Gain compensation", &p.gComp, 0.8f, 1.3f, "%.2f");
    ImGui::TreePop();
  }
  if (tuningLeaf("Parker taps", "Taps & EQ")) {
    ImGui::SliderFloat("Echo gain", &p.echoGain, 0.0f, 0.3f, "%.2f");
    ImGui::SliderFloat("Ripple gain", &p.rippleGain, 0.0f, 0.3f, "%.2f");
    ImGui::SliderFloat("Wander depth", &p.modDepth, 0.0f, 16.0f, "%.1f samples");
    ImGui::SliderFloat("Wander pole", &p.modPole, 0.8f, 0.99f, "%.3f");
    ImGui::SliderFloat("Chirp EQ centre", &p.eqPeakHz, 50.0f, 400.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Chirp EQ width", &p.eqBwHz, 0.0f, 300.0f, "%.0f Hz");
    ImGui::Text("Low cutoff %.0f Hz, fixed table", static_cast<double>(p.lowHz));
    ImGui::TreePop();
  }
  if (tuningLeaf("Parker high band", "High band")) {
    ImGui::SliderInt("Splash sections", &p.mHigh, 0, 200);
    ImGui::SliderFloat("Splash pole", &p.aHf, -0.9f, 0.0f, "%.2f");
    ImGui::SliderFloat("Splash gain ratio", &p.hfRatio, 0.8f, 1.5f, "%.2f");
    ImGui::SliderFloat("Splash level", &p.hfMixDb, -120.0f, 0.0f, "%.1f dB");
    ImGui::SliderFloat("Splash cross-feed", &p.cross, 0.0f, 0.3f, "%.2f");
    ImGui::TreePop();
  }
  if (tuningLeaf("Parker springs", "Springs")) {
    ImGui::SliderInt("Springs", &p.springs, 1, 3);
    for (int i = 0; i < 3; ++i) {
      char label[32];
      std::snprintf(label, sizeof(label), "Delay factor %d", i + 1);
      ImGui::SliderFloat(label, &p.tdFactor[static_cast<size_t>(i)], 0.8f, 1.25f, "%.3f");
    }
    for (int i = 0; i < 3; ++i) {
      char label[32];
      std::snprintf(label, sizeof(label), "Corner factor %d", i + 1);
      ImGui::SliderFloat(label, &p.fcFactor[static_cast<size_t>(i)], 0.9f, 1.1f, "%.3f");
    }
    ImGui::TreePop();
  }
  if (tuningLeaf("Parker drive", "Drive")) {
    ImGui::SliderFloat("Input high-pass", &p.hpHz, 60.0f, 400.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Output low-pass", &p.lpHz, 3000.0f, 12000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Drive at dwell 1", &p.dwellDrive, 1.0f, 64.0f, "%.1f x");
    ImGui::SliderFloat("Drive compensation", &p.dwellComp, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Presence centre", &p.presenceHz, 1000.0f, 6000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Presence gain", &p.presenceDb, 0.0f, 6.0f, "%.1f dB");
    ImGui::SliderFloat("Presence Q", &p.presenceQ, 0.5f, 2.0f, "%.2f");
    ImGui::SliderFloat("Tank input trim", &p.tankTrim, 0.1f, 2.0f, "%.3f");
    ImGui::SliderFloat("Wet level", &p.wetDb, -12.0f, 18.0f, "%.1f dB");
    ImGui::TreePop();
  }
  ImGui::TreePop();
}

void drawReverbRaw(cv::SpringTuning& t, cv::ChasmTuning& c, cv::SpringCTuning& p) {
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
  if (tuningNode("Tank")) {
    ImGui::SliderFloat("Feedback at tension 0", &t.tensionLo, 0.3f, 0.95f, "%.2f");
    ImGui::SliderFloat("Feedback at tension 1", &t.tensionHi, 0.6f, 0.995f, "%.3f");
    ImGui::SliderFloat("Pre-echo ripple", &t.rippleGain, 0.0f, 0.5f, "%.2f");
    ImGui::SliderInt("Springs", &t.springs, 2, 3);
    ImGui::SliderFloat("Wander depth", &t.modDepth, 0.0f, 24.0f, "%.1f samples");
    ImGui::SliderFloat("Wander rate", &t.modRateHz, 0.1f, 10.0f, "%.2f Hz", log);
    ImGui::TreePop();
  }
  if (tuningNode("Splash")) {
    ImGui::SliderFloat("Level at dwell 0", &t.hfMixDbLo, -40.0f, 0.0f, "%.1f dB");
    ImGui::SliderFloat("Level at dwell 1", &t.hfMixDbHi, -40.0f, 0.0f, "%.1f dB");
    ImGui::SliderFloat("Diffusion", &t.splashDiffuse, 0.0f, 0.9f, "%.2f");
    ImGui::SliderInt("Allpass sections", &t.hfSections, 0, 200);
    ImGui::TreePop();
  }
  if (tuningNode("Levels")) {
    ImGui::SliderFloat("Input gain", &t.inputGain, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Input high-pass", &t.hpHz, 20.0f, 1000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Drive at dwell 1", &t.dwellDrive, 1.0f, 100.0f, "%.1f x", log);
    ImGui::SliderFloat("Drive compensation", &t.dwellComp, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Tank input trim", &t.tankTrim, 0.05f, 1.5f, "%.3f", log);
    ImGui::SliderFloat("Boing 95 Hz", &t.boingDb, -12.0f, 12.0f, "%.1f dB");
    ImGui::SliderFloat("Wet level", &t.wetDb, -24.0f, 18.0f, "%.1f dB");
    ImGui::TreePop();
  }
  if (tuningNode("Chasm")) {
    ImGui::SliderFloat("Feedback at decay 0", &c.timeLo, 0.1f, 0.95f, "%.2f");
    ImGui::SliderFloat("Feedback at decay 1", &c.timeHi, 0.5f, 0.98f, "%.3f");
    ImGui::SliderFloat("Loop treble loss", &c.trebleLossHz, 500.0f, 12000.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Loop treble cut", &c.loopTrebleCut, 0.5f, 1.0f, "%.2f");
    ImGui::SliderFloat("Input treble cut", &c.inputTrebleCut, 0.5f, 1.0f, "%.2f");
    ImGui::SliderFloat("Bass cut", &c.bassCutHz, 20.0f, 400.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Bass cut at decay 1", &c.bassCutHzTop, 20.0f, 400.0f, "%.0f Hz", log);
    ImGui::SliderFloat("Wobble depth", &c.wobbleDepthMax, 0.0f, 192.0f, "%.0f samples");
    ImGui::SliderFloat("Wobble rate at 0", &c.wobbleRateLo, 0.1f, 10.0f, "%.2f Hz", log);
    ImGui::SliderFloat("Wobble rate at 1", &c.wobbleRateHi, 0.1f, 10.0f, "%.2f Hz", log);
    ImGui::TreePop();
  }
  if (tuningNode("Chasm levels")) {
    ImGui::SliderFloat("Drive at dwell 1", &c.dwellDrive, 1.0f, 64.0f, "%.1f x", log);
    ImGui::SliderFloat("Drive compensation", &c.dwellComp, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Input trim", &c.inputTrim, 0.05f, 1.5f, "%.2f", log);
    ImGui::SliderFloat("Wobble level lift", &c.wobbleLevelDb, 0.0f, 6.0f, "%.1f dB");
    ImGui::SliderFloat("Wet level", &c.wetDb, -24.0f, 24.0f, "%.1f dB");
    ImGui::TreePop();
  }
  drawParkerTuning(p);
}

// Reset and Print cover every effect's tuning.
void drawTuningButtons(ProtoParams& params) {
  cv::HarmonyTuning& ht = params.pitchFx.harmony.tuning;
  cv::OctaveTuning& ot = params.pitchFx.octave.tuning;
  if (ImGui::Button("Reset to defaults")) {
    ht = cv::HarmonyTuning{};
    ot = cv::OctaveTuning{};
    params.pitchFx.autotune.tuning = cv::AutotuneTuning{};
    params.unison.tuning = cv::UnisonTuning{};
    params.slapback.tuning = cv::SlapbackTuning{};
    params.distortion.tuning = cv::DistortionTuning{};
    params.reverb.spring.tuning = cv::SpringTuning{};
    params.reverb.chasm.tuning = cv::ChasmTuning{};
    params.reverb.parker.tuning = cv::SpringCTuning{};
    params.eq.tuning = cv::PolishTuning{};
    params.inputGate.tuning = cv::GateTuning{};
    params.inputGate.tuning.rangeDb = kInputGateRangeDb;
    params.inputGate.tuning.holdMs = kInputGateHoldMs;
    params.inputGate.tuning.releaseMs = kInputGateReleaseMs;
    params.gate.tuning = cv::GateTuning{};
    params.reverb.chasm.wobble = cv::ChasmParams{}.wobble;  // the CHASM Wobble macro writes it
    params.macros = cv::macros::State{};
  }
  // Right-justified at the full label's width, so "Copied" never moves it.
  constexpr const char* kCopyLabel = "Copy settings to clipboard";
  const float copyW = ImGui::CalcTextSize(kCopyLabel).x + 2.0f * ImGui::GetStyle().FramePadding.x;
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - copyW));
  // The label confirms the copy for a moment; the ### id keeps the button the same widget.
  static double copiedAt = -10.0;
  constexpr double kCopiedShowSec = 1.5;
  const bool justCopied = ImGui::GetTime() - copiedAt < kCopiedShowSec;
  if (ImGui::Button(justCopied ? "Copied###copy" : "Copy settings to clipboard###copy",
                    ImVec2(copyW, 0.0f))) {
    printTuning(ht, ot, params.unison.tuning, params.slapback.tuning,
                params.distortion.tuning, params.reverb.spring.tuning,
                params.reverb.chasm.tuning, params.reverb.parker.tuning, params.eq, params.inputGate.tuning,
                params.gate.tuning, params.pitchFx.autotune.tuning, params);
    copiedAt = ImGui::GetTime();
  }
}

constexpr float kFeedbackSliderW = 60.0f;  // px

float gHeaderRight = 0.0f;  // right edge of the transport row this frame, px

void drawTransportItems(ProtoParams& params, float& meterDb, bool probe);

// The transport row reads as the headline: larger type, taller controls.
void drawTransportRow(ProtoParams& params, float& meterDb, bool probe) {
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kHeaderScale);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kHeaderPadding);
  drawTransportItems(params, meterDb, probe);
  gHeaderRight = ImGui::GetItemRectMax().x;
  ImGui::PopStyleVar();
  ImGui::PopFont();
}

// Header on/off switch. A checkbox square at headline size reads as an empty tile,
// so the state is in the label and the button lights when on.
// Last drawn rect per toggle, for the probe's click test.
std::map<std::string, std::pair<ImVec2, ImVec2>> gToggleRects;

void toggleButton(const char* name, bool& on, bool withState = true) {
  char label[64];
  if (withState) std::snprintf(label, sizeof(label), "%s: %s###%s", name, on ? "ON" : "OFF", name);
  else std::snprintf(label, sizeof(label), "%s", name);
  // Push and pop follow the state at draw time; the click flips on in between.
  const bool lit = on;
  if (lit) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  if (ImGui::Button(label)) on = !on;
  if (lit) ImGui::PopStyleColor();
  gToggleRects[name] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
}

void drawTransportItems(ProtoParams& params, float& meterDb, bool probe) {
  if (ImGui::Button("Load loop") && !probe) {
    const std::string path = openFilePanel();
    if (!path.empty()) loadLoop(path);
  }
  ImGui::SameLine();
  // Fixed-width name slot so a long file name never pushes the row off screen.
  std::string name = gLoopPath.empty() ? "(no loop)" : baseName(gLoopPath);
  const float maxW = kLoopNameW;
  if (ImGui::CalcTextSize(name.c_str()).x > maxW) {
    while (!name.empty() && ImGui::CalcTextSize((name + "...").c_str()).x > maxW) name.pop_back();
    name += "...";
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(name.c_str());
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::Dummy(ImVec2(std::max(0.0f, maxW - ImGui::GetItemRectSize().x), 0.0f));
  ImGui::SameLine();
  if (ImGui::Button(params.playing ? "Stop" : "Play")) params.playing = !params.playing;
  ImGui::SameLine();

  const float frac = std::clamp((meterDb - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
  char label[32];
  std::snprintf(label, sizeof(label), "%.1f dBFS", meterDb);
  ImGui::ProgressBar(frac, ImVec2(kMeterW, 0.0f), label);
  ImGui::SameLine();
  if (params.dev) {
    toggleButton("Advanced", params.advanced, false);  // dev only; lit when on
    ImGui::SameLine();
  }
  toggleButton("STAGE FEEDBACK", params.stageFeedback);
  ImGui::BeginDisabled(!params.stageFeedback);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(kFeedbackSliderW);
  ImGui::SliderFloat("Amount", &params.feedbackAmount, 0.0f, 100.0f, "%.0f%%",
                     ImGuiSliderFlags_NoRoundToFormat);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(kFeedbackSliderW);
  ImGui::SliderFloat("Movement", &params.feedbackMovement, 0.0f, 100.0f, "%.0f%%",
                     ImGuiSliderFlags_NoRoundToFormat);
  ImGui::EndDisabled();
  ImGui::SameLine();
  drawTuningButtons(params);
}

// ---- Harmony voices: High / Higher, each Off / Quiet / Loud / Louder ----------------
//
// Adam has no harmony of his own to match, so the face offers the two voices above
// him (owner 2026-10-02). The engine's third slot stays off.

constexpr int kVoiceRows = 2;
constexpr cv::HarmonyVoice kVoiceOf[kVoiceRows] = {cv::HarmonyVoice::High, cv::HarmonyVoice::Higher};
constexpr const char* kVoiceKey[kVoiceRows] = {"high", "higher"};  // saved-state names

struct HarmonyMenu {
  // 0 off, 1 quiet, 2 loud, 3 louder. Owner default: High louder, Higher loud.
  std::array<int, kVoiceRows> level{{3, 2}};
  std::array<int, kVoiceRows> formant{};  // engine B formant, semitones
};
HarmonyMenu gMenu;  // UI-thread-only

void syncSlots(const HarmonyMenu& m, cv::HarmonyParams& h) {
  for (int i = 0; i < kVoiceRows; ++i) {
    h.slots[i].voice = kVoiceOf[i];
    h.slots[i].level = m.level[i];
    h.slots[i].formant = static_cast<float>(m.formant[i]);
  }
  for (size_t i = kVoiceRows; i < h.slots.size(); ++i) h.slots[i].level = 0;
}

// ---- Saved state: the face as Adam left it ----------------------------------------
//
// Plain "key value" lines. Unknown keys are skipped and missing keys keep their
// defaults, so a new build reads an old file. Raw Advanced edits are not saved;
// the macros rewrite those fields on load.

struct StateField {
  std::string key;
  char kind;  // 'f' float, 'i' int, 'b' bool
  void* ptr;
};

std::vector<StateField> stateFields(ProtoParams& p, HarmonyMenu& m) {
  std::vector<StateField> f;
  const auto add = [&](const char* key, auto* ptr) {
    using T = std::remove_pointer_t<decltype(ptr)>;
    const char kind = std::is_same_v<T, bool> ? 'b' : (std::is_same_v<T, int> ? 'i' : 'f');
    f.push_back({key, kind, ptr});
  };
  const auto gate = [&](const char* name, cv::GateParams& g) {
    const std::string n = name;
    f.push_back({n + ".on", 'b', &g.on});
    f.push_back({n + ".threshold", 'f', &g.thresholdDb});
    f.push_back({n + ".range", 'f', &g.tuning.rangeDb});
    f.push_back({n + ".attack", 'f', &g.tuning.attackMs});
    f.push_back({n + ".hold", 'f', &g.tuning.holdMs});
    f.push_back({n + ".release", 'f', &g.tuning.releaseMs});
  };
  gate("ingate", p.inputGate);
  gate("gate", p.gate);
  cv::AutotuneParams& at = p.pitchFx.autotune;
  add("autotune.on", &at.on);
  add("autotune.engine", &at.engine);
  add("autotune.key", &at.key);
  add("autotune.chromatic", &at.chromatic);
  add("autotune.response", &at.responseMs);
  add("autotune.pull", &at.tuning.maxCorrectSemis);
  add("autotune.link", &p.linkAutotuneKey);
  cv::OctaveParams& o = p.pitchFx.octave;
  add("octave.on", &o.on);
  add("octave.engine", &o.engine);
  add("octave.semitones", &o.semitones);
  add("octave.formant", &o.formant);
  add("octave.mix", &o.mix);
  cv::HarmonyParams& h = p.pitchFx.harmony;
  add("harmony.on", &h.on);
  add("harmony.engine", &h.engine);
  add("harmony.key", &h.key);
  add("harmony.chromatic", &h.chromatic);
  add("harmony.mix", &h.mix);
  add("harmony.snap", &h.tuning.snapToScale);
  for (int i = 0; i < kVoiceRows; ++i) {
    const std::string n = std::string("harmony.") + kVoiceKey[i];
    f.push_back({n + ".level", 'i', &m.level[static_cast<size_t>(i)]});
    f.push_back({n + ".formant", 'i', &m.formant[static_cast<size_t>(i)]});
  }
  add("unison.on", &p.unison.on);
  add("unison.depth", &p.unison.depth);
  add("slapback.on", &p.slapback.on);
  add("slapback.intensity", &p.slapback.intensity);
  add("slapback.time", &p.slapback.tuning.timeMs);
  add("slapback.repeats", &p.slapback.tuning.feedback);
  add("slapback.lowpass", &p.slapback.tuning.lowpassHz);
  add("distortion.on", &p.distortion.on);
  add("distortion.drive", &p.distortion.drive);
  add("distortion.tone", &p.distortion.tone);
  cv::ReverbParams& r = p.reverb;
  add("reverb.on", &r.on);
  add("reverb.engine", &r.engine);
  add("reverb.mix", &r.mix);
  add("reverb.spring.decay", &r.spring.tension);
  add("reverb.spring.dwell", &r.spring.dwell);
  add("reverb.chasm.decay", &r.chasm.decay);
  add("reverb.chasm.dwell", &r.chasm.dwell);
  add("reverb.parker.decay", &r.parker.tension);
  add("reverb.parker.dwell", &r.parker.dwell);
  add("eq.on", &p.eq.on);
  add("eq.lowcut", &p.eq.hpHz);
  add("eq.dipHz", &p.eq.dipHz);
  add("eq.dipDb", &p.eq.dipDb);
  add("eq.presenceHz", &p.eq.presenceHz);
  add("eq.presenceDb", &p.eq.presenceDb);
  add("eq.air", &p.eq.airDb);
  for (int i = 0; i < cv::macros::kCount; ++i)
    f.push_back({std::string("macro.") + cv::macros::kStateKey[i], 'f',
                 &p.macros.pos[static_cast<size_t>(i)]});
  add("dev", &p.dev);
  add("pedal", &p.pedalMode);
  add("advanced", &p.advanced);
  add("feedback.on", &p.stageFeedback);
  add("feedback.amount", &p.feedbackAmount);
  add("feedback.movement", &p.feedbackMovement);
  return f;
}

std::string stateText(ProtoParams& p, HarmonyMenu& m, const std::string& loop) {
  std::string out = "loop " + loop + "\n";
  char line[160];
  for (const StateField& f : stateFields(p, m)) {
    if (f.kind == 'f')
      std::snprintf(line, sizeof(line), "%s %.9g\n", f.key.c_str(),
                    static_cast<double>(*static_cast<float*>(f.ptr)));
    else if (f.kind == 'i')
      std::snprintf(line, sizeof(line), "%s %d\n", f.key.c_str(), *static_cast<int*>(f.ptr));
    else
      std::snprintf(line, sizeof(line), "%s %d\n", f.key.c_str(), *static_cast<bool*>(f.ptr) ? 1 : 0);
    out += line;
  }
  return out;
}

// Parses stateText output into p, m and loop. Returns the number of fields set.
int parseStateText(const std::string& text, ProtoParams& p, HarmonyMenu& m, std::string& loop) {
  const std::vector<StateField> fields = stateFields(p, m);
  int set = 0;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t space = line.find(' ');
    if (space == std::string::npos) continue;
    const std::string key = line.substr(0, space);
    const std::string value = line.substr(space + 1);
    if (key == "loop") {
      loop = value;
      continue;
    }
    for (const StateField& f : fields) {
      if (f.key != key) continue;
      char* tail = nullptr;
      if (f.kind == 'f') {
        const float v = std::strtof(value.c_str(), &tail);
        if (tail != value.c_str() && std::isfinite(v)) {
          *static_cast<float*>(f.ptr) = v;
          ++set;
        }
      } else {
        const long v = std::strtol(value.c_str(), &tail, 10);
        if (tail == value.c_str()) break;
        if (f.kind == 'i') *static_cast<int*>(f.ptr) = static_cast<int>(v);
        else *static_cast<bool*>(f.ptr) = v != 0;
        ++set;
      }
      break;
    }
  }
  return set;
}

// Macros own the raw fields they map to; re-applying every one restores those fields.
void applyMacros(ProtoParams& p) {
  namespace mc = cv::macros;
  const auto& m = p.macros.pos;
  cv::ReverbParams& r = p.reverb;
  mc::octaveSlide(p.pitchFx.octave.tuning, m[mc::OctaveSlide]);
  mc::harmonyTracking(p.pitchFx.harmony.tuning, m[mc::HarmonyTracking]);
  mc::unison(p.unison.tuning, m[mc::UnisonBlend], m[mc::UnisonMotion]);
  mc::distBody(p.distortion.tuning, m[mc::DistBody]);
  mc::distBite(p.distortion.tuning, m[mc::DistBite]);
  mc::distGrit(p.distortion.tuning, m[mc::DistGrit]);
  mc::springSplash(r.spring.tuning, m[mc::SpringSplash]);
  mc::springFlutter(r.spring.tuning, m[mc::SpringFlutter]);
  mc::springLowEnd(r.spring.tuning, m[mc::SpringLowEnd]);
  mc::chasmWobble(r.chasm, m[mc::ChasmWobble]);
  mc::chasmBrightness(r.chasm.tuning, m[mc::ChasmBrightness]);
  mc::chasmBass(r.chasm.tuning, m[mc::ChasmBass]);
  mc::parkerSplash(r.parker.tuning, m[mc::ParkerSplash]);
  mc::parkerDrip(r.parker.tuning, m[mc::ParkerDrip]);
  mc::parkerFlutter(r.parker.tuning, m[mc::ParkerFlutter]);
  mc::parkerBrightness(r.parker.tuning, m[mc::ParkerBrightness]);
}

// ~/Library/Application Support/cubevox-proto/state.txt; empty if HOME is unset.
std::string statePath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr) return {};
  const std::string dir = std::string(home) + "/Library/Application Support/cubevox-proto";
  mkdir(dir.c_str(), 0755);  // EEXIST is the normal case
  return dir + "/state.txt";
}

void loadState(const std::string& path, ProtoParams& p, HarmonyMenu& m) {
  std::FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
  if (f == nullptr) return;  // first launch
  std::string text;
  char buf[4096];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  std::fclose(f);
  std::string loop;
  if (parseStateText(text, p, m, loop) == 0)
    std::fprintf(stderr, "[WARN] %s has no readable settings\n", path.c_str());
  applyMacros(p);
  p.playing = false;
  if (!loop.empty()) loadLoop(loop);
}

// Writes through a temp file so a crash mid-write never leaves half a file.
void saveState(const std::string& path, const std::string& text) {
  if (path.empty()) return;
  const std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if (f == nullptr || std::fwrite(text.data(), 1, text.size(), f) != text.size()) {
    std::fprintf(stderr, "[ERROR] could not write %s\n", tmp.c_str());
    if (f != nullptr) std::fclose(f);
    return;
  }
  std::fclose(f);
  if (std::rename(tmp.c_str(), path.c_str()) != 0)
    std::fprintf(stderr, "[ERROR] could not replace %s\n", path.c_str());
}

// Each row: name, level radios, then FORMANT (engine B, voice on).
void drawHarmonyVoices(const cv::HarmonyParams& h) {
  static const char* const kRowName[kVoiceRows] = {"High", "Higher"};
  static const char* const kLevelName[4] = {"Off", "Quiet", "Loud", "Louder"};
  ImGui::TextUnformatted("Voices");
  for (int row = 0; row < kVoiceRows; ++row) {
    ImGui::PushID(row);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kRowName[row]);
    for (int level = 0; level < 4; ++level) {
      ImGui::SameLine(level == 0 ? kMenuRadioX : 0.0f);
      ImGui::RadioButton(kLevelName[level], &gMenu.level[row], level);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(h.engine != 1 || gMenu.level[row] == 0);
    ImGui::SetNextItemWidth(kFormantW);
    ImGui::SliderInt("Formant", &gMenu.formant[row], -12, 12, "%+d st");
    ImGui::EndDisabled();
    ImGui::PopID();
  }
}

// ---- Tuning headers: macro view by default, raw nodes under Advanced --------------

// One plain line under a tuning control: what it does, the technical term last.
void hint(const char* text) { ImGui::TextDisabled("%s", text); }

template <class F>
void macroSlider(const char* label, const char* help, float& pos, const F& apply) {
  if (ImGui::SliderFloat(label, &pos, 0.0f, 100.0f, "%.0f %%")) apply(pos);
  hint(help);
}

void drawHarmonyTuning(cv::HarmonyParams& h, cv::macros::State& m) {
  if (!tuningHeader("harmony")) return;
  drawHarmonyVoices(h);
  if (gAdvanced) {
    drawHarmonyRaw(h.tuning);
    return;
  }
  macroSlider("Tracking speed", "how fast the voices catch a new note",
              m.pos[cv::macros::HarmonyTracking],
              [&](float p) { cv::macros::harmonyTracking(h.tuning, p); });
  bool follow = !h.tuning.snapToScale;
  if (ImGui::Checkbox("Follow my bends", &follow)) h.tuning.snapToScale = !follow;
  hint("voices bend with you; off snaps them to the key");
}

void drawOctaveTuning(cv::OctaveParams& o, cv::macros::State& m) {
  if (!tuningHeader("octave")) return;
  if (gAdvanced) {
    drawOctaveRaw(o.tuning);
    return;
  }
  macroSlider("Slide", "how slowly it glides to a new note", m.pos[cv::macros::OctaveSlide],
              [&](float p) { cv::macros::octaveSlide(o.tuning, p); });
}

// Pull range limits how far a note may be moved; small values fix near-misses and
// leave deliberate bends alone. The raw nodes follow under Advanced.
void drawAutotuneTuning(cv::AutotuneParams& a) {
  if (!tuningHeader("autotune")) return;
  ImGui::SliderFloat("Pull range", &a.tuning.maxCorrectSemis, 0.5f, 6.0f, "%.1f st");
  hint("how far off a note it will still fix");
  ImGui::Checkbox("Correct to every note", &a.chromatic);
  hint("snaps to the nearest note; picking a KEY turns this off");
  if (gAdvanced) drawAutotuneRaw(a.tuning);
}

void drawUnisonTuning(cv::UnisonTuning& t, cv::macros::State& m) {
  if (!tuningHeader("unison")) return;
  if (gAdvanced) {
    drawUnisonRaw(t);
    return;
  }
  macroSlider("Chorus ↔ Double", "left more swirl, right a tighter double",
              m.pos[cv::macros::UnisonBlend],
              [&](float p) { cv::macros::unison(t, p, m.pos[cv::macros::UnisonMotion]); });
}

void drawSlapbackTuning(cv::SlapbackTuning& t) {
  if (!tuningHeader("slapback")) return;
  if (gAdvanced) {
    drawSlapbackRaw(t);
    return;
  }
  ImGui::SliderFloat("Repeats", &t.feedback, 0.0f, 0.5f, "%.2f");
  hint("echoes after the first; INTENSITY raises them too");
  ImGui::SliderFloat("Low pass", &t.lowpassHz, 500.0f, 12000.0f, "%.0f Hz",
                     ImGuiSliderFlags_Logarithmic);
  hint("lower makes the echoes darker");
}

void drawDistortionTuning(cv::DistortionTuning& t, cv::macros::State& m) {
  if (!tuningHeader("distortion")) return;
  if (gAdvanced) {
    drawDistortionRaw(t);
    return;
  }
  namespace mc = cv::macros;
  macroSlider("Body", "more low end going into the drive", m.pos[mc::DistBody],
              [&](float p) { mc::distBody(t, p); });
  macroSlider("Bite", "more top end and edge", m.pos[mc::DistBite],
              [&](float p) { mc::distBite(t, p); });
  macroSlider("Grit", "rougher, raspier breakup", m.pos[mc::DistGrit],
              [&](float p) { mc::distGrit(t, p); });
}

// Plain names, technical term in the hint. The input gate carries its threshold here;
// the GATE module keeps THRESHOLD on its face. Mute amount runs right = more muting.
void drawGateTuning(cv::GateParams& g, const char* block, bool withThreshold) {
  if (!tuningHeader(block)) return;
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
  cv::GateTuning& t = g.tuning;
  if (withThreshold) {
    ImGui::SliderFloat("Opens at", &g.thresholdDb, -70.0f, -10.0f, "%.0f dB");
    hint("sing louder than this to open (threshold)");
  }
  if (gAdvanced) {
    drawGateRaw(t);
    return;
  }
  float mute = -t.rangeDb;
  if (ImGui::SliderFloat("Mute amount", &mute, 0.0f, 80.0f, "%.0f dB")) t.rangeDb = -mute;
  hint("how far it turns down when shut (range)");
  ImGui::SliderFloat("Fade in", &t.attackMs, 0.1f, 50.0f, "%.1f ms", log);
  hint("how fast it opens when you sing (attack)");
  ImGui::SliderFloat("Stay open", &t.holdMs, 0.0f, 500.0f, "%.0f ms");
  hint("wait after you stop before shutting (hold)");
  // The GATE module has release on its face as DECAY.
  if (withThreshold) {
    ImGui::SliderFloat("Fade out", &t.releaseMs, 5.0f, 1000.0f, "%.0f ms", log);
    hint("how slowly it fades shut (release)");
  }
}

// Four plain controls; the band frequencies live under Advanced. Mud cut runs
// right = more cut.
void drawEqTuning(cv::PolishParams& e) {
  if (!tuningHeader("eq")) return;
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
  ImGui::SliderFloat("Low cut", &e.hpHz, 40.0f, 200.0f, "%.0f Hz", log);
  hint("removes boom and handling thumps");
  float cut = -e.dipDb;
  if (ImGui::SliderFloat("Mud cut", &cut, 0.0f, 6.0f, "%.1f dB")) e.dipDb = -cut;
  hint("scoops out boxy low-mids");
  ImGui::SliderFloat("Presence", &e.presenceDb, 0.0f, 6.0f, "%+.1f dB");
  hint("pushes the words forward");
  ImGui::SliderFloat("Air", &e.airDb, 0.0f, 4.0f, "%+.1f dB");
  hint("breathy sparkle on top");
  if (!gAdvanced) return;
  ImGui::SliderFloat("Mud cut centre", &e.dipHz, 150.0f, 600.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Presence centre", &e.presenceHz, 2000.0f, 6000.0f, "%.0f Hz", log);
  drawPolishRaw(e.tuning);
}

void drawReverbTuning(cv::ReverbParams& r, cv::macros::State& m) {
  if (!tuningHeader("reverb")) return;
  if (gAdvanced) {
    drawReverbRaw(r.spring.tuning, r.chasm.tuning, r.parker.tuning);
    return;
  }
  namespace mc = cv::macros;
  if (r.engine == cv::kReverbChasm) {
    macroSlider("Wobble", "pitch wobble in the tail", m.pos[mc::ChasmWobble],
                [&](float p) { mc::chasmWobble(r.chasm, p); });
    macroSlider("Brightness", "brighter tail", m.pos[mc::ChasmBrightness],
                [&](float p) { mc::chasmBrightness(r.chasm.tuning, p); });
    macroSlider("Bass", "more low end in the tail", m.pos[mc::ChasmBass],
                [&](float p) { mc::chasmBass(r.chasm.tuning, p); });
  } else if (r.engine == cv::kReverbParker) {
    cv::SpringCTuning& t = r.parker.tuning;
    macroSlider("Splash", "bright crash on loud notes", m.pos[mc::ParkerSplash],
                [&](float p) { mc::parkerSplash(t, p); });
    macroSlider("Drip", "the drippy boing on each note", m.pos[mc::ParkerDrip],
                [&](float p) { mc::parkerDrip(t, p); });
    macroSlider("Flutter", "springs drift apart, more shimmer", m.pos[mc::ParkerFlutter],
                [&](float p) { mc::parkerFlutter(t, p); });
    macroSlider("Brightness", "brighter tail", m.pos[mc::ParkerBrightness],
                [&](float p) { mc::parkerBrightness(t, p); });
  } else {
    cv::SpringTuning& t = r.spring.tuning;
    macroSlider("Splash", "bright crash on loud notes", m.pos[mc::SpringSplash],
                [&](float p) { mc::springSplash(t, p); });
    macroSlider("Flutter", "wobble and shimmer in the tail", m.pos[mc::SpringFlutter],
                [&](float p) { mc::springFlutter(t, p); });
    macroSlider("Low end", "more boom from the springs", m.pos[mc::SpringLowEnd],
                [&](float p) { mc::springLowEnd(t, p); });
  }
}

// Panel knob shown in percent.
void percentSlider(const char* label, float& value) {
  float percent = value * 100.0f;
  if (ImGui::SliderFloat(label, &percent, 0.0f, 100.0f, "%.0f %%")) value = percent / 100.0f;
}

void drawHarmonyBlock(cv::HarmonyParams& h, cv::macros::State& macros) {
  ImGui::PushID("harmony");
  ImGui::Checkbox("HARMONY", &h.on);
  ImGui::SameLine(kHarmonyEngineX);
  ImGui::TextUnformatted("Engine");
  ImGui::SameLine();
  ImGui::RadioButton("A", &h.engine, 0);
  ImGui::SameLine();
  ImGui::RadioButton("B", &h.engine, 1);
  ImGui::SameLine();
  ImGui::RadioButton("C", &h.engine, 2);
  ImGui::BeginDisabled(h.chromatic);
  ImGui::SetNextItemWidth(kHarmonyKeyW);
  ImGui::Combo("KEY", &h.key, cv::kKeyName, 12);
  ImGui::EndDisabled();
  if (gAdvanced) {
    ImGui::SameLine();
    ImGui::Checkbox("Chromatic", &h.chromatic);
  }
  percentSlider("MIX", h.mix);
  drawHarmonyTuning(h, macros);
  syncSlots(gMenu, h);
  ImGui::PopID();
}

// KEY is the box's one shared encoder: when linked, picking a key here sets
// Harmony's too (sharedKey), so the combo is never locked.
void drawAutotuneBlock(cv::AutotuneParams& a, bool& linkKey, int& sharedKey,
                       const ProtoState& state) {
  ImGui::PushID("autotune");
  ImGui::Checkbox("AUTOTUNE", &a.on);
  ImGui::SameLine(kHarmonyEngineX);
  ImGui::RadioButton("A", &a.engine, 0);
  ImGui::SameLine();
  ImGui::RadioButton("B", &a.engine, 1);
  // KEY stays live; picking a key leaves "Correct to every note" mode.
  ImGui::SetNextItemWidth(kAutotuneKeyW);
  if (ImGui::BeginCombo("KEY", a.chromatic ? "Every note" : cv::kKeyName[a.key])) {
    for (int k = 0; k < 12; ++k) {
      if (ImGui::Selectable(cv::kKeyName[k], !a.chromatic && k == a.key)) {
        a.key = k;
        a.chromatic = false;
        if (linkKey) sharedKey = k;
      }
    }
    ImGui::EndCombo();
  }
  if (gAdvanced) {
    ImGui::SameLine();
    ImGui::Checkbox("Link key to Harmony", &linkKey);
    ImGui::SameLine();
    ImGui::Text("Correction: %+.2f st", a.on ? state.correctionSemis : 0.0f);
  }
  ImGui::SliderFloat("RESPONSE", &a.responseMs, cv::AutotuneVoice::kMinResponseMs,
                     cv::AutotuneVoice::kMaxResponseMs, "%.0f ms", ImGuiSliderFlags_Logarithmic);
  drawAutotuneTuning(a);
  ImGui::PopID();
}

void drawOctaveBlock(cv::OctaveParams& o, cv::macros::State& macros) {
  ImGui::PushID("octave");
  ImGui::Checkbox("OCTAVE", &o.on);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Engine");
  ImGui::SameLine();
  ImGui::RadioButton("A", &o.engine, 0);
  ImGui::SameLine();
  ImGui::RadioButton("B", &o.engine, 1);
  ImGui::SameLine();
  ImGui::RadioButton("C", &o.engine, 2);
  ImGui::SliderInt("SEMITONES", &o.semitones, -12, 12, "%+d st");
  ImGui::BeginDisabled(o.engine != 1);
  int formant = static_cast<int>(std::lround(o.formant));
  if (ImGui::SliderInt("FORMANT", &formant, -12, 12, "%+d st")) o.formant = static_cast<float>(formant);
  ImGui::EndDisabled();
  percentSlider("MIX", o.mix);
  drawOctaveTuning(o, macros);
  ImGui::PopID();
}

// One gate instance: id keys the widgets and the probe's Tuning header.
// Panel-style LED: green open (gain within 1 dB of unity), amber in the knee,
// dark red closed. Off = unlit.
void gateLed(bool on, float gainDb) {
  const float r = ImGui::GetFrameHeight() * 0.32f;
  const ImVec2 c(ImGui::GetCursorScreenPos().x + r + 2.0f,
                 ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight() * 0.5f);
  ImU32 fill = IM_COL32(60, 60, 60, 255);
  if (on) {
    if (gainDb > -1.0f) fill = IM_COL32(60, 220, 80, 255);
    else if (gainDb > -12.0f) fill = IM_COL32(235, 170, 40, 255);
    else fill = IM_COL32(140, 30, 30, 255);
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCircleFilled(c, r, fill, 16);
  dl->AddCircle(c, r, IM_COL32(20, 20, 20, 255), 16, 1.0f);
  ImGui::Dummy(ImVec2(2.0f * r + 6.0f, ImGui::GetFrameHeight()));
  ImGui::SameLine();
}

// The input gate is always on and keeps its threshold under Tuning.
void drawGateBlock(const char* id, const char* label, cv::GateParams& g, float gainDb,
                   bool alwaysOn) {
  ImGui::PushID(id);
  gateLed(g.on, gainDb);
  // The input gate has no panel switch on the box; the prototype toggle exists to
  // A/B it against the stage feedback simulator (owner 2026-10-02).
  ImGui::Checkbox(label, &g.on);
  if (!alwaysOn) {
    ImGui::SliderFloat("THRESHOLD", &g.thresholdDb, -70.0f, -10.0f, "%.0f dB");
    ImGui::SliderFloat("DECAY", &g.tuning.releaseMs, 5.0f, 1000.0f, "%.0f ms",
                       ImGuiSliderFlags_Logarithmic);
  }
  ImGui::Text("Gain: %.1f dB", static_cast<double>(gainDb));
  // The input gate is fixed on the box; its tuning is ours, not Adam's.
  if (!alwaysOn || gDev) drawGateTuning(g, id, alwaysOn);
  ImGui::PopID();
}

void drawUnisonBlock(cv::UnisonParams& u, cv::macros::State& macros) {
  ImGui::PushID("unison");
  ImGui::Checkbox("UNISON", &u.on);
  percentSlider("DEPTH", u.depth);
  // RATE: chorus speed. Faster also deepens the swirl, up to a capped pitch swing.
  float& rate = macros.pos[cv::macros::UnisonMotion];
  if (ImGui::SliderFloat("RATE", &rate, 0.0f, 100.0f, "%.0f %%"))
    cv::macros::unison(u.tuning, macros.pos[cv::macros::UnisonBlend], rate);
  drawUnisonTuning(u.tuning, macros);
  ImGui::PopID();
}

void drawSlapbackBlock(cv::SlapbackParams& s) {
  ImGui::PushID("slapback");
  ImGui::Checkbox("SLAPBACK", &s.on);
  percentSlider("INTENSITY", s.intensity);
  ImGui::SliderFloat("TIME", &s.tuning.timeMs, 30.0f, 150.0f, "%.0f ms");
  drawSlapbackTuning(s.tuning);
  ImGui::PopID();
}

void drawDistortionBlock(cv::DistortionParams& d, cv::macros::State& macros) {
  ImGui::PushID("distortion");
  ImGui::Checkbox("DISTORTION", &d.on);
  percentSlider("DRIVE", d.drive);
  percentSlider("TONE", d.tone);
  drawDistortionTuning(d.tuning, macros);
  ImGui::PopID();
}

void drawReverbBlock(cv::ReverbParams& r, cv::macros::State& macros) {
  ImGui::PushID("reverb");
  ImGui::Checkbox("REVERB", &r.on);
  ImGui::RadioButton("SPRING", &r.engine, cv::kReverbSpring);
  ImGui::SameLine();
  ImGui::RadioButton("CHASM", &r.engine, cv::kReverbChasm);
  ImGui::SameLine();
  ImGui::RadioButton("PARKER SPRING", &r.engine, cv::kReverbParker);
  // DECAY and DWELL bind to whichever engine is selected.
  if (r.engine == cv::kReverbChasm) {
    percentSlider("DECAY", r.chasm.decay);
    percentSlider("DWELL", r.chasm.dwell);
  } else if (r.engine == cv::kReverbParker) {
    percentSlider("DECAY", r.parker.tension);
    percentSlider("DWELL", r.parker.dwell);
  } else {
    percentSlider("DECAY", r.spring.tension);
    percentSlider("DWELL", r.spring.dwell);
  }
  percentSlider("MIX", r.mix);
  drawReverbTuning(r, macros);
  ImGui::PopID();
}

void drawEqBlock(cv::PolishParams& e) {
  ImGui::PushID("eq");
  ImGui::Checkbox("Output EQ", &e.on);
  drawEqTuning(e);
  ImGui::PopID();
}

constexpr float kModulePad = 10.0f;  // inner padding of a module box, px
constexpr float kModuleGap = 24.0f;  // vertical gap between module boxes, px
constexpr float kColumnGap = 32.0f;  // horizontal gap between columns, px
constexpr float kTopGap = 10.0f;     // gap between the transport row and the columns, px

// Border around one effect module, sized to its content (open Tuning included).
template <class F>
void moduleBox(const char* id, bool first, const F& draw) {
  if (!first) ImGui::Dummy(ImVec2(0.0f, kModuleGap - ImGui::GetStyle().ItemSpacing.y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kModulePad, kModulePad));
  ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
                    ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PushItemWidth(-kLabelW);
  draw();
  ImGui::PopItemWidth();
  ImGui::EndChild();
}

constexpr float kFooterH = 52.0f;  // strip under the columns for the mode button, px
constexpr ImVec2 kModeButtonPadding = ImVec2(18.0f, 10.0f);  // mode button frame padding, px

// ---- Pedal mode: each effect drawn as a stompbox -----------------------------------

constexpr float kKnobR = 20.0f;          // knob radius, px
constexpr float kKnobDragPx = 200.0f;    // vertical drag for the full knob travel, px
constexpr float kKnobWheelStep = 0.02f;  // knob travel per scroll notch
constexpr float kEncoderStepPx = 14.0f;  // vertical drag per encoder detent, px
constexpr float kKnobGapY = 16.0f;       // space between knobs, px
constexpr float kPedalGap = 12.0f;       // space between pedals, px
constexpr float kPedalH = 420.0f;        // pedal height, px
constexpr float kFootH = 64.0f;          // footswitch strip at the pedal's foot, px
constexpr float kPedalNameScale = 1.3f;  // pedal name type size vs the face
constexpr float kPiF = 3.14159265f;

float gPedalRight = 0.0f;   // right edge of the pedal row this frame, px
float gPedalBottom = 0.0f;  // bottom edge of the pedal row this frame, px

// 7 o'clock at 0, 5 o'clock at 1; screen y points down.
float knobAngle(float v01) { return kPiF * (0.75f + 1.5f * v01); }

// Knob body, travel arc and pointer; an encoder shows its detents as ticks.
void drawKnobFace(ImVec2 c, float v01, int detents) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 body = IM_COL32(45, 45, 50, 255);
  const ImU32 rim = IM_COL32(150, 150, 160, 255);
  const ImU32 lit = IM_COL32(230, 180, 60, 255);
  dl->PathArcTo(c, kKnobR + 5.0f, knobAngle(0.0f), knobAngle(1.0f), 32);
  dl->PathStroke(IM_COL32(70, 70, 75, 255), 0, 3.0f);
  if (detents == 0 && v01 > 0.0f) {
    dl->PathArcTo(c, kKnobR + 5.0f, knobAngle(0.0f), knobAngle(v01), 32);
    dl->PathStroke(lit, 0, 3.0f);
  }
  for (int i = 0; i < detents; ++i) {
    const float a = knobAngle(static_cast<float>(i) / static_cast<float>(detents - 1));
    dl->AddLine(ImVec2(c.x + (kKnobR + 3.0f) * cosf(a), c.y + (kKnobR + 3.0f) * sinf(a)),
                ImVec2(c.x + (kKnobR + 7.0f) * cosf(a), c.y + (kKnobR + 7.0f) * sinf(a)), rim, 1.5f);
  }
  dl->AddCircleFilled(c, kKnobR, body, 32);
  dl->AddCircle(c, kKnobR, rim, 32, 1.5f);
  const float a = knobAngle(v01);
  dl->AddLine(ImVec2(c.x + 0.25f * kKnobR * cosf(a), c.y + 0.25f * kKnobR * sinf(a)),
              ImVec2(c.x + 0.9f * kKnobR * cosf(a), c.y + 0.9f * kKnobR * sinf(a)), lit, 3.0f);
}

// Knob on the left, label and value on the right. Drag up/down or scroll.
// Returns true when v01 changed.
bool pedalKnobRaw(const char* label, float& v01, const char* valueText, int detents) {
  ImGui::PushID(label);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float span = 2.0f * (kKnobR + 7.0f);
  ImGui::InvisibleButton("knob", ImVec2(span, span));
  float v = v01;
  if (ImGui::IsItemActive()) v -= ImGui::GetIO().MouseDelta.y / kKnobDragPx;
  if (ImGui::IsItemHovered()) v += ImGui::GetIO().MouseWheel * kKnobWheelStep;
  v = std::clamp(v, 0.0f, 1.0f);
  const bool changed = v != v01;
  v01 = v;
  drawKnobFace(ImVec2(at.x + 0.5f * span, at.y + 0.5f * span), v01, detents);
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::Dummy(ImVec2(0.0f, 0.5f * span - ImGui::GetTextLineHeightWithSpacing()));
  ImGui::TextUnformatted(label);
  ImGui::TextDisabled("%s", valueText);
  ImGui::EndGroup();
  ImGui::Dummy(ImVec2(0.0f, kKnobGapY));
  ImGui::PopID();
  return changed;
}

bool pedalPercent(const char* label, float& value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%.0f %%", static_cast<double>(value * 100.0f));
  return pedalKnobRaw(label, value, text, 0);
}

// A knob over [lo, hi], log-tapered for times.
bool pedalRange(const char* label, float& value, float lo, float hi, bool logTaper,
                const char* fmt) {
  float v01 = logTaper ? logf(value / lo) / logf(hi / lo) : (value - lo) / (hi - lo);
  char text[24];
  std::snprintf(text, sizeof(text), fmt, static_cast<double>(value));
  if (!pedalKnobRaw(label, v01, text, 0)) return false;
  value = logTaper ? lo * powf(hi / lo, v01) : lo + v01 * (hi - lo);
  return true;
}

// Detented encoder over [lo, hi]: one step per kEncoderStepPx of drag or per scroll notch.
bool pedalEncoder(const char* label, int& value, int lo, int hi, const char* text) {
  ImGui::PushID(label);
  float& acc = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetID("acc"), 0.0f);
  ImGui::PopID();
  float v01 = static_cast<float>(value - lo) / static_cast<float>(hi - lo);
  const float before = v01;
  pedalKnobRaw(label, v01, text, hi - lo + 1);
  // The raw knob moved v01 by drag or wheel; convert that travel into whole detents.
  acc += (v01 - before) * kKnobDragPx / kEncoderStepPx;
  if (ImGui::GetIO().MouseWheel != 0.0f && v01 != before)
    acc = ImGui::GetIO().MouseWheel > 0.0f ? 1.0f : -1.0f;
  int steps = static_cast<int>(acc);
  acc -= static_cast<float>(steps);
  const int next = std::clamp(value + steps, lo, hi);
  const bool changed = next != value;
  value = next;
  return changed;
}

// One stompbox: knobs from the top, footswitch with LED and name at the foot.
template <class F>
void pedal(const char* name, bool& on, float width, const F& knobs) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kModulePad, kModulePad + 4.0f));
  ImGui::BeginChild(name, ImVec2(width, kPedalH), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  knobs();
  ImGui::SetCursorPosY(kPedalH - kFootH);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float footW = ImGui::GetContentRegionAvail().x;
  if (ImGui::InvisibleButton("foot", ImVec2(footW, kFootH - kModulePad))) on = !on;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 led(at.x + 0.5f * footW, at.y + 8.0f);
  dl->AddCircleFilled(led, 6.0f, on ? IM_COL32(230, 40, 40, 255) : IM_COL32(70, 25, 25, 255), 16);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kPedalNameScale);
  const ImVec2 size = ImGui::CalcTextSize(name);
  dl->AddText(ImVec2(at.x + 0.5f * (footW - size.x), at.y + 22.0f), IM_COL32(230, 230, 235, 255), name);
  ImGui::PopFont();
  ImGui::EndChild();
  gPedalRight = std::max(gPedalRight, ImGui::GetItemRectMax().x);
  gPedalBottom = std::max(gPedalBottom, ImGui::GetItemRectMax().y);
}

// Panel knobs only, signal order left to right. Encoders sit below the knobs.
void drawPedals(ProtoParams& params) {
  constexpr int kPedals = 8;
  const float w = (ImGui::GetContentRegionAvail().x - (kPedals - 1) * kPedalGap) / kPedals;
  gPedalRight = 0.0f;
  gPedalBottom = 0.0f;
  const auto next = [] { ImGui::SameLine(0.0f, kPedalGap); };
  cv::AutotuneParams& at = params.pitchFx.autotune;
  cv::HarmonyParams& h = params.pitchFx.harmony;
  cv::OctaveParams& o = params.pitchFx.octave;
  char semis[16];
  std::snprintf(semis, sizeof(semis), "%+d st", o.semitones);

  pedal("AUTOTUNE", at.on, w, [&] {
    pedalRange("RESPONSE", at.responseMs, cv::AutotuneVoice::kMinResponseMs,
               cv::AutotuneVoice::kMaxResponseMs, true, "%.0f ms");
    if (pedalEncoder("KEY", at.key, 0, 11, at.chromatic ? "Every note" : cv::kKeyName[at.key])) {
      at.chromatic = false;  // picking a key leaves every-note mode, as on the full face
      if (params.linkAutotuneKey) h.key = at.key;
    }
  });
  next();
  pedal("HARMONY", h.on, w, [&] {
    pedalPercent("MIX", h.mix);
    if (pedalEncoder("KEY", h.key, 0, 11, cv::kKeyName[h.key]) && params.linkAutotuneKey)
      at.key = h.key;
  });
  next();
  pedal("OCTAVE", o.on, w, [&] {
    pedalPercent("MIX", o.mix);
    pedalEncoder("SEMITONES", o.semitones, -12, 12, semis);
  });
  next();
  pedal("UNISON", params.unison.on, w, [&] {
    pedalPercent("DEPTH", params.unison.depth);
    float rate = 0.01f * params.macros.pos[cv::macros::UnisonMotion];
    if (pedalPercent("RATE", rate)) {
      params.macros.pos[cv::macros::UnisonMotion] = 100.0f * rate;
      cv::macros::unison(params.unison.tuning, params.macros.pos[cv::macros::UnisonBlend],
                         params.macros.pos[cv::macros::UnisonMotion]);
    }
  });
  next();
  pedal("SLAPBACK", params.slapback.on, w, [&] {
    pedalPercent("INTENSITY", params.slapback.intensity);
    pedalRange("TIME", params.slapback.tuning.timeMs, 30.0f, 150.0f, false, "%.0f ms");
  });
  next();
  pedal("DISTORTION", params.distortion.on, w, [&] {
    pedalPercent("DRIVE", params.distortion.drive);
    pedalPercent("TONE", params.distortion.tone);
  });
  next();
  pedal("GATE", params.gate.on, w, [&] {
    pedalRange("THRESHOLD", params.gate.thresholdDb, -70.0f, -10.0f, false, "%.0f dB");
    pedalRange("DECAY", params.gate.tuning.releaseMs, 5.0f, 1000.0f, true, "%.0f ms");
  });
  next();
  cv::ReverbParams& r = params.reverb;
  pedal("REVERB", r.on, w, [&] {  // last pedal; Output EQ lives in the menu
    float& decay = r.engine == cv::kReverbChasm ? r.chasm.decay
                   : r.engine == cv::kReverbParker ? r.parker.tension : r.spring.tension;
    float& dwell = r.engine == cv::kReverbChasm ? r.chasm.dwell
                   : r.engine == cv::kReverbParker ? r.parker.dwell : r.spring.dwell;
    pedalPercent("DECAY", decay);
    pedalPercent("DWELL", dwell);
    pedalPercent("MIX", r.mix);
  });
}

void drawPedals(ProtoParams& params);
void drawColumns(ProtoParams& params, const ProtoState& state);
void drawModeButton(ProtoParams& params);

std::array<float, kColumnCount> gColumnUsed{};  // content height per column this frame, px
float gColumnAvail = 0.0f;                  // column height, px

// One face column. Columns never scroll; the probe fails on any overrun.
template <class F>
void column(int index, float width, const F& draw) {
  gCurrentColumn = index;
  ImGui::PushID(index);
  // The footer strip below the columns holds the mode button.
  ImGui::BeginChild("column", ImVec2(width, -kFooterH), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushItemWidth(-kLabelW);
  draw();
  ImGui::PopItemWidth();
  gColumnUsed[index] = ImGui::GetCursorPosY();
  gColumnAvail = ImGui::GetWindowHeight();
  ImGui::EndChild();
  ImGui::PopID();
}

void drawFrame(ProtoParams& params, const ProtoState& state, float& meterDb, bool probe) {
  // Hold-and-decay so short peaks stay readable at 60 fps.
  meterDb = std::max(state.peakDb, meterDb - kMeterDecayDbPerFrame);
  if (!probe && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_D))
    params.dev = !params.dev;  // Cmd+Shift+D on the Mac
  // Advanced never stays on while hidden.
  if (!params.dev) params.advanced = false;
  gDev = params.dev;
  gAdvanced = params.advanced;
  // Harmony chromatic and the key link are Advanced-only; the plain face keeps them at default.
  if (!params.advanced) {
    params.linkAutotuneKey = true;
    params.pitchFx.harmony.chromatic = false;
  }
  if (params.linkAutotuneKey) params.pitchFx.autotune.key = params.pitchFx.harmony.key;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::Begin("cubevox", nullptr, flags);
  drawTransportRow(params, meterDb, probe);
  ImGui::Separator();
  ImGui::Dummy(ImVec2(0.0f, kTopGap));
  if (params.pedalMode) drawPedals(params);
  else drawColumns(params, state);
  drawModeButton(params);
  ImGui::End();
}

// Bottom-right switch between the full face and pedal mode, at headline size.
void drawModeButton(ProtoParams& params) {
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * kHeaderScale);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, kModeButtonPadding);
  const char* label = params.pedalMode ? "Full view###mode" : "Pedal mode###mode";
  const float w = ImGui::CalcTextSize("Pedal mode").x + 2.0f * kModeButtonPadding.x;
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y -
                       ImGui::GetFrameHeight());
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - w);
  if (ImGui::Button(label, ImVec2(w, 0.0f))) params.pedalMode = !params.pedalMode;
  // Build version, bottom left at the button's size; tools/bump_version.sh per change.
  ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, ImGui::GetItemRectMin().y -
                                                                    ImGui::GetWindowPos().y));
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s", CUBEVOX_VERSION);
  ImGui::PopStyleVar();
  ImGui::PopFont();
}

void drawColumns(ProtoParams& params, const ProtoState& state) {
  // Signal order runs down each column, then left to right.
  const float colW =
      (ImGui::GetContentRegionAvail().x - kHarmonyColumnW - 3.0f * kColumnGap) / 3.0f;
  column(0, colW, [&] {
    moduleBox("ingateBox", true,
              [&] { drawGateBlock("ingate", "INPUT GATE", params.inputGate, state.inGateDb, true); });
    moduleBox("autotuneBox", false, [&] {
      drawAutotuneBlock(params.pitchFx.autotune, params.linkAutotuneKey,
                        params.pitchFx.harmony.key, state);
    });
  });
  ImGui::SameLine(0.0f, kColumnGap);
  // Autotune and Harmony share the KEY encoder, so Harmony follows Autotune.
  column(1, kHarmonyColumnW, [&] {
    moduleBox("harmonyBox", true,
              [&] { drawHarmonyBlock(params.pitchFx.harmony, params.macros); });
    moduleBox("octaveBox", false,
              [&] { drawOctaveBlock(params.pitchFx.octave, params.macros); });
    moduleBox("unisonBox", false, [&] { drawUnisonBlock(params.unison, params.macros); });
  });
  ImGui::SameLine(0.0f, kColumnGap);
  column(2, colW, [&] {
    moduleBox("slapbackBox", true, [&] { drawSlapbackBlock(params.slapback); });
    moduleBox("distortionBox", false, [&] { drawDistortionBlock(params.distortion, params.macros); });
    moduleBox("gateBox", false,
              [&] { drawGateBlock("gate", "GATE", params.gate, state.gateDb, false); });
  });
  ImGui::SameLine(0.0f, kColumnGap);
  column(3, colW, [&] {
    moduleBox("reverbBox", true, [&] { drawReverbBlock(params.reverb, params.macros); });
    moduleBox("eqBox", false, [&] { drawEqBlock(params.eq); });
  });
}

// ImGui's built-in font has no arrows. The system Unicode font fills in "Chorus \u2194 Double"
// when present; without it the arrow shows as "?".
void loadFonts(ImGuiIO& io) {
  io.Fonts->AddFontDefault();
  constexpr const char* kUnicodeFont = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf";
  if (std::FILE* f = std::fopen(kUnicodeFont, "rb")) {
    std::fclose(f);
    ImFontConfig cfg;
    cfg.MergeMode = true;
    // Size 0 inherits the default font's size; an explicit size asserts in MergeMode.
    io.Fonts->AddFontFromFileTTF(kUnicodeFont, 0.0f, &cfg);
  }
}

// ---- Probe (--layout): no window, no device ------------------------------------

int runLayoutProbe() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  io.DisplaySize = ImVec2(static_cast<float>(kWindowW), static_cast<float>(kWindowH));
  io.DeltaTime = 1.0f / 60.0f;
  loadFonts(io);
  io.Fonts->Build();

  // Macro view: all headers closed, then each Tuning header open (reverb once per engine).
  // Advanced view: the same, plus each raw sub-node open alone.
  constexpr int kSpring = cv::kReverbSpring, kChasm = cv::kReverbChasm, kParker = cv::kReverbParker;
  static const ProbeOpen kScenarios[] = {
      {true, nullptr, nullptr},
      {true, "harmony", nullptr},    {true, "octave", nullptr},
      {true, "autotune", nullptr},
      {true, "unison", nullptr},     {true, "slapback", nullptr},
      {true, "distortion", nullptr}, {true, "reverb", nullptr, false, kSpring},
      {true, "reverb", nullptr, false, kChasm},  {true, "reverb", nullptr, false, kParker},
      {true, "eq", nullptr},         {true, "ingate", nullptr},
      {true, "gate", nullptr},
      {true, nullptr, nullptr, true},
      {true, "harmony", nullptr, true},
      {true, "harmony", "Levels", true}, {true, "harmony", "Trims", true}, {true, "harmony", "Tracking", true},
      {true, "harmony", "Chromatic intervals", true}, {true, "harmony", "Shifter B/C", true},
      {true, "octave", nullptr, true},          {true, "unison", nullptr, true},
      {true, "unison", "Chorus (LFO-wobbled delay)", true},
      {true, "unison", "Doubler (fixed detune, TC-Helicon style)", true},
      {true, "unison", "Shared", true},
      {true, "slapback", nullptr, true},        {true, "distortion", nullptr, true},
      {true, "distortion", "Stage 1", true},    {true, "distortion", "Tone stack", true},
      {true, "distortion", "Stage 2", true},    {true, "distortion", "Post", true},
      {true, "reverb", nullptr, true},          {true, "reverb", "Tank", true},
      {true, "reverb", "Splash", true},         {true, "reverb", "Levels", true},
      {true, "reverb", "Chasm", true},          {true, "reverb", "Chasm levels", true},
      {true, "reverb", "Parker", true},
      {true, "reverb", "Parker tank", true},    {true, "reverb", "Parker taps", true},
      {true, "reverb", "Parker high band", true},
      {true, "reverb", "Parker springs", true}, {true, "reverb", "Parker drive", true},
      {true, "eq", nullptr, true},              {true, "ingate", nullptr, true},
      {true, "autotune", nullptr, true},        {true, "gate", nullptr, true},
      {true, nullptr, nullptr, false, -1, true},
  };

  ProtoParams params;
  params.dev = true;  // the dev face is the superset; Adam's face is a subset of it
  gLoopPath = "/samples/Adam Vox C-sharp Autotune.wav";  // a long real name for the header
  ProtoState state;
  float meterDb = kSilenceDb;
  bool fits = true;
  for (const ProbeOpen& s : kScenarios) {
    gProbeOpen = s;
    params.advanced = s.advanced;
    params.pedalMode = s.pedal;
    if (s.reverbEngine >= 0) params.reverb.engine = s.reverbEngine;
    for (int frame = 0; frame < kProbeFrames; ++frame) {
      ImGui::NewFrame();
      drawFrame(params, state, meterDb, true);
      ImGui::Render();
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%s %s%s%s%s", s.advanced ? "adv  " : "macro",
                  s.block ? s.block : "all closed", s.node ? " / " : "", s.node ? s.node : "",
                  s.pedal ? " (PEDAL MODE)" : s.reverbEngine == kChasm ? " (CHASM)" : (s.reverbEngine == kParker ? " (PARKER)" : ""));
    const float worst = *std::max_element(gColumnUsed.begin(), gColumnUsed.end());
    const bool headerFits = gHeaderRight <= static_cast<float>(kWindowW);
    if (!headerFits) std::printf("header row runs to %.0f px of %d\n", gHeaderRight, kWindowW);
    const bool pedalFits = gPedalRight <= static_cast<float>(kWindowW) &&
                           gPedalBottom <= static_cast<float>(kWindowH) - kFooterH;
    const bool ok = (s.pedal ? pedalFits : worst <= gColumnAvail) && headerFits;
    if (s.pedal) std::printf("pedal row: right %.0f of %d px, bottom %.0f of %.0f px\n", gPedalRight,
                             kWindowW, gPedalBottom, static_cast<float>(kWindowH) - kFooterH);
    fits = fits && ok;
    std::printf("%-44s col1 %4.0f  col2 %4.0f  col3 %4.0f  col4 %4.0f  of %.0f px%s\n", name,
                gColumnUsed[0], gColumnUsed[1], gColumnUsed[2], gColumnUsed[3], gColumnAvail,
                ok ? "" : "  OVERRUN");
  }
  // Click each header toggle on, then off, through ImGui's input queue. A style-stack
  // imbalance asserts inside EndFrame, so surviving both clicks is the check.
  gProbeOpen = ProbeOpen{};
  ProtoParams clicked;
  clicked.dev = true;
  bool* const targets[2] = {&clicked.stageFeedback, &clicked.advanced};
  const char* const names[2] = {"STAGE FEEDBACK", "Advanced"};
  for (int t = 0; t < 2; ++t) {
    bool flips = true;
    for (int c = 0; c < 2; ++c) {
      const bool before = *targets[t];
      ImGui::NewFrame();
      drawFrame(clicked, state, meterDb, true);
      ImGui::Render();
      const auto& r = gToggleRects[names[t]];
      const ImVec2 mid((r.first.x + r.second.x) * 0.5f, (r.first.y + r.second.y) * 0.5f);
      for (int phase = 0; phase < 3; ++phase) {
        io.AddMousePosEvent(mid.x, mid.y);
        if (phase == 1) io.AddMouseButtonEvent(0, true);
        if (phase == 2) io.AddMouseButtonEvent(0, false);
        ImGui::NewFrame();
        drawFrame(clicked, state, meterDb, true);
        ImGui::Render();
      }
      flips = flips && *targets[t] != before;
    }
    std::printf("toggle %s on and off: %s\n", names[t], flips ? "ok" : "FAILED");
    if (!flips) fits = false;
  }
  ImGui::DestroyContext();

  // Print tuning's snapshot lines must render whole at the defaults.
  char snap[2048];
  const size_t snapLen = snapshotLines(ProtoParams{}, snap, sizeof(snap));
  std::printf("%s", snap);
  if (snapLen == 0 || snapLen >= sizeof(snap) - 1) fits = false;

  // Saved state: move every field off its default, write, read into a fresh face, compare.
  ProtoParams moved;
  HarmonyMenu movedMenu;
  for (const StateField& f : stateFields(moved, movedMenu)) {
    if (f.kind == 'f') *static_cast<float*>(f.ptr) += 0.37f;
    else if (f.kind == 'i') *static_cast<int*>(f.ptr) += 1;
    else *static_cast<bool*>(f.ptr) = !*static_cast<bool*>(f.ptr);
  }
  const std::string text = stateText(moved, movedMenu, "/tmp/a loop.wav");
  ProtoParams back;
  HarmonyMenu backMenu;
  std::string backLoop;
  const int set = parseStateText(text, back, backMenu, backLoop);
  // The file path: write, read back, and the face must match the parsed one.
  const std::string probeFile = "/tmp/cubevox-proto-probe-state.txt";
  saveState(probeFile, text);
  ProtoParams fromFile;
  HarmonyMenu fromFileMenu;
  loadState(probeFile, fromFile, fromFileMenu);
  std::remove(probeFile.c_str());
  applyMacros(back);  // loadState re-applies macros; match that before comparing
  const bool fileOk = stateText(fromFile, fromFileMenu, gLoopPath) == stateText(back, backMenu, gLoopPath) &&
                      fromFile.reverb.spring.tuning.hpHz == back.reverb.spring.tuning.hpHz;
  const std::vector<StateField> want = stateFields(moved, movedMenu);
  const std::vector<StateField> got = stateFields(back, backMenu);
  int mismatches = 0;
  for (size_t i = 0; i < want.size(); ++i) {
    const size_t bytes = want[i].kind == 'f' ? sizeof(float) : (want[i].kind == 'i' ? sizeof(int) : sizeof(bool));
    if (std::memcmp(want[i].ptr, got[i].ptr, bytes) != 0) {
      std::printf("state round trip: %s differs\n", want[i].key.c_str());
      ++mismatches;
    }
  }
  const bool stateOk = fileOk && mismatches == 0 && set == static_cast<int>(want.size()) &&
                       backLoop == "/tmp/a loop.wav";
  std::printf("state round trip: %d fields, %s\n", set, stateOk ? "ok" : "FAILED");
  if (!stateOk) fits = false;

  std::printf("layout %dx%d: %s\n", kWindowW, kWindowH, fits ? "ok" : "column overrun");
  return fits ? 0 : 1;
}

// ---- Window ----------------------------------------------------------------------

int runWindow() {
  if (!glfwInit()) {
    std::fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  GLFWwindow* window = glfwCreateWindow(kWindowW, kWindowH, "cubevox proto", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  loadFonts(ImGui::GetIO());
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 150");

  ProtoParams params;
  const std::string stateFile = statePath();
  loadState(stateFile, params, gMenu);
  gParamsBuf[0] = params;
  gParamsBuf[1] = params;
  float meterDb = kSilenceDb;
  std::string savedText = stateText(params, gMenu, gLoopPath);
  double lastSaveCheck = glfwGetTime();
  constexpr double kSaveEverySec = 1.0;  // a crash loses at most this much

  ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
  deviceConfig.playback.format = ma_format_f32;
  deviceConfig.playback.channels = 2;
  deviceConfig.sampleRate = cv::kSampleRate;
  deviceConfig.dataCallback = dataCallback;
  ma_device device;
  if (ma_device_init(nullptr, &deviceConfig, &device) != MA_SUCCESS ||
      ma_device_start(&device) != MA_SUCCESS) {
    std::fprintf(stderr, "audio device failed\n");
    return 1;
  }

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    reclaimRetired();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ProtoState state =
        gStateBuf[gStatePublishIndex.load(std::memory_order_acquire)];
    drawFrame(params, state, meterDb, false);
    publishParams(params);
    if (glfwGetTime() - lastSaveCheck >= kSaveEverySec) {
      lastSaveCheck = glfwGetTime();
      const std::string text = stateText(params, gMenu, gLoopPath);
      if (text != savedText) {
        saveState(stateFile, text);
        savedText = text;
      }
    }

    ImGui::Render();
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  saveState(stateFile, stateText(params, gMenu, gLoopPath));
  ma_device_uninit(&device);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--layout") == 0) return runLayoutProbe();
    std::fprintf(stderr, "usage: cubevox-proto [--layout]\n");
    return 2;
  }
  return runWindow();
}
