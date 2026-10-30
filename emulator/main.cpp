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
#include "engine/polish_comp.h"
#include "engine/reverb.h"
#include "engine/soft_ceiling.h"
#include "engine/unison.h"

namespace {

constexpr int kWindowW = 1440;
constexpr int kWindowH = 840;  // owner's laptop shows ~847 px of window
constexpr float kWideColumnW = 400.0f;     // second column (Octave, Unison) width
constexpr float kLabelW = 175.0f;          // room right of each slider for its label
constexpr float kAutotuneKeyW = 80.0f;     // KEY combo width
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
constexpr float kStartDepth = 0.5f;
constexpr int kStartKey = 11;  // F / Dm (kKeyName runs round the circle of fifths)
constexpr float kStartSlapMix = 0.25f;
constexpr float kStartDrive = 0.65f;
constexpr float kStartTone = 1.0f;
// Input soft gate: live-stage defaults (owner 2026-10-01). Range stays partial so a
// mis-trigger never reads as a dropout; release is short by owner choice.
constexpr float kInputGateThresholdDb = -12.0f;
constexpr float kInputGateRangeDb = -12.0f;
constexpr float kInputGateHoldMs = 100.0f;
constexpr float kInputGateReleaseMs = 10.0f;

void applyInputGateDefaults(cv::GateParams& g) {
  g.thresholdDb = kInputGateThresholdDb;
  g.tuning = cv::GateTuning{};
  g.tuning.rangeDb = kInputGateRangeDb;
  g.tuning.holdMs = kInputGateHoldMs;
  g.tuning.releaseMs = kInputGateReleaseMs;
}

struct ProtoParams {
  bool playing = false;
  // Source: 0 plays the loaded loop, 1 runs the mic macOS has chosen (System
  // Settings > Sound > Input) through the chain.
  int source = 0;
  float inputGainDb = 0.0f;  // mic only, -12..+24
  cv::GateParams inputGate;
  cv::GateParams gate;
  cv::PitchFxParams pitchFx;
  cv::UnisonParams unison{false, kStartDepth, {}};
  cv::SlapbackParams slapback{true, kStartSlapMix, {}};
  cv::DistortionParams distortion{true, kStartDrive, kStartTone, {}};
  cv::ReverbParams reverb;
  cv::PolishCompParams comp;  // the chain tail, always on here like the box default: comp, then eq
  cv::PolishParams eq;
  // Cmd+Shift+D shows our test control: Advanced.
  bool dev = false;
  // Advanced swaps the musician sliders for the raw tuning nodes.
  bool advanced = false;
  // Pedal mode shows each effect as a stompbox with only its panel knobs.
  bool pedalMode = false;
  // Mute silences the output; the chain keeps running underneath (the box's MUTE toggle).
  bool mute = false;
  cv::macros::State macros;
  // Prototype-only test signal. Not a panel control, not in Print tuning.
  bool stageFeedback = false;
  float feedbackAmount = 70.0f;    // percent of the simulator range
  float feedbackMovement = 50.0f;  // percent of the capped range

  // Every effect opens off except the input gate, which has no switch on the
  // face. Engine defaults stay on for the firmware.
  ProtoParams() {
    inputGate.on = true;
    applyInputGateDefaults(inputGate);
    gate.on = false;
    pitchFx.octave.on = false;
    unison.on = false;
    distortion.on = false;
    reverb.on = false;
    // Adam's starting point (owner 2026-10-02).
    slapback.on = true;
    eq.on = true;
    pitchFx.autotune.key = kStartKey;
    pitchFx.octave.mix = 0.10f;
    reverb.engine = cv::kReverbSpring;
    reverb.mix = 1.0f;
    reverb.time = 1.0f;  // full MIX with full decay, Adam's starting sound
    cv::applyKnobs(reverb);
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
std::atomic<bool> gMicAvailable{false};  // duplex device opened
std::string gInputName;                  // UI-thread-only; current capture device
unsigned gInputRate = 0;                 // UI-thread-only; its sample rate

// Audio-thread-only.
cv::Gate gInputGate;
cv::Gate gGate;
cv::PitchFx gPitchFx;
cv::Unison gUnison;
cv::Slapback gSlapback;
cv::Distortion gDistortion;
cv::Reverb gReverb;
cv::PolishComp gPolishComp;
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

void dataCallback(ma_device*, void* output, const void* input, ma_uint32 frameCount) {
  float* out = static_cast<float*>(output);
  const float* mic = static_cast<const float*>(input);  // mono f32, null on playback-only
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
    gPolishComp.reset();
    gPolish.reset();
    gStageFeedback.reset(1u);
  }
  // Movement 100 % = 75 % of the simulator range (owner 2026-10-02: full was too much).
  constexpr float kFeedbackCap = 0.75f;  // Movement only; Amount uses the full range (owner)
  const bool live = params.source == 1 && mic != nullptr;
  const bool loopRunning = params.playing && loop != nullptr && !loop->samples.empty();
  gStageFeedback.configure(params.stageFeedback && (live || params.playing),
                           0.01f * params.feedbackAmount,
                           kFeedbackCap * 0.01f * params.feedbackMovement);
  if (!live && !loopRunning) {
    std::memset(out, 0, sizeof(float) * 2 * frameCount);
    publishState(0.0f, loop);
    return;
  }
  const float micGain = std::pow(10.0f, params.inputGainDb / 20.0f);

  std::array<float, cv::kBlock> in;
  std::array<float, cv::kBlock> tmp;
  std::array<float, cv::kBlock> mono;
  float peak = 0.0f;
  ma_uint32 done = 0;
  while (done < frameCount) {
    const int n = static_cast<int>(std::min<ma_uint32>(cv::kBlock, frameCount - done));
    if (live) {
      for (int i = 0; i < n; ++i) in[i] = mic[done + i] * micGain;
    } else {
      readLoop(*loop, in.data(), n);
    }
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
    gPolishComp.process(mono.data(), tmp.data(), n, params.comp);
    gPolish.process(tmp.data(), mono.data(), n, params.eq);
    // The box's output ceiling, last in the chain.
    for (int i = 0; i < n; ++i) mono[i] = cv::softCeiling(mono[i]);
    // Mute: silence out, so the wedge loop hears exactly what a muted box would send.
    if (params.mute) std::fill(mono.begin(), mono.begin() + n, 0.0f);
    if (feedback) {
      for (int i = 0; i < n; ++i) gStageFeedback.push(mono[i]);
    }
    for (int i = 0; i < n; ++i) {
      out[2 * (done + i)] = mono[i];
      out[2 * (done + i) + 1] = mono[i];
      peak = std::max(peak, std::fabs(mono[i]));
    }
    done += n;
  }
  publishState(peak, live ? nullptr : loop);
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
  static const char* const kReverb[2] = {"SPRING", "CHASM"};
  const cv::AutotuneParams& at = p.pitchFx.autotune;
  const cv::OctaveParams& o = p.pitchFx.octave;
  const cv::ReverbParams& r = p.reverb;
  const int re = r.engine == cv::kReverbChasm ? cv::kReverbChasm : cv::kReverbSpring;
  const float decays[2] = {r.spring.decay, r.chasm.decay};
  const float dwells[2] = {r.spring.dwell, r.chasm.dwell};
  const float decay = decays[re];
  const float dwell = dwells[re];
  char macros[640];
  cv::macros::formatAll(p.macros, macros, sizeof(macros));
  const int n = std::snprintf(
      buf, size,
      "%s\n"
      "Choices: Input gate on; Gate %s; Autotune %s; Octave %s; Unison %s; Slapback %s; "
      "Distortion %s; Reverb %s, engine %s; Output EQ %s\n"
      "Knobs: Input gate THRESHOLD %.0f dB, DECAY %.0f ms; Gate THRESHOLD %.0f dB, DECAY %.0f ms; Autotune KEY %s, RESPONSE "
      "%.0f %% mechanical (%.0f ms), Pull range %.1f st; Octave SEMITONES %+d, MIX %.0f %%; Unison DEPTH "
      "%.0f %%, RATE %.0f %%; Slapback MIX %.0f %%, TIME %.0f ms; Distortion DRIVE %.0f %%, TONE %.0f %%; Reverb MIX "
      "%.0f %%, TIME %.0f %% (wet %.0f %%, decay %.0f %%, dwell %.0f %%)\n",
      macros, onOff(p.gate.on), onOff(at.on), onOff(o.on), onOff(p.unison.on),
      onOff(p.slapback.on), onOff(p.distortion.on), onOff(r.on), kReverb[re], onOff(p.eq.on),
      static_cast<double>(p.inputGate.thresholdDb),
      static_cast<double>(p.inputGate.tuning.releaseMs), static_cast<double>(p.gate.thresholdDb),
      static_cast<double>(p.gate.tuning.releaseMs),
      cv::kKeyName[at.key],
      static_cast<double>(cv::AutotuneVoice::mechanicalOf(at.responseMs) * 100.0f),
      static_cast<double>(at.responseMs),
      static_cast<double>(at.tuning.maxCorrectSemis), o.semitones,
      static_cast<double>(o.mix * 100.0f),
      static_cast<double>(p.unison.depth * 100.0f),
      static_cast<double>(p.macros.pos[cv::macros::UnisonMotion]),
      static_cast<double>(p.slapback.mix * 100.0f),
      static_cast<double>(p.slapback.tuning.timeMs),
      static_cast<double>(p.distortion.drive * 100.0f),
      static_cast<double>(p.distortion.tone * 100.0f), static_cast<double>(r.mix * 100.0f),
      static_cast<double>(r.time * 100.0f), static_cast<double>(r.wet * 100.0f), static_cast<double>(decay * 100.0f),
      static_cast<double>(dwell * 100.0f));
  return n < 0 ? 0 : std::min(static_cast<size_t>(n), size - 1);
}

// Finder launches have no stdout, so the text also goes to the clipboard
// and into a read-only field beside the button.
// Fills gTuningText with the copied-settings text.
void formatTuning(const cv::OctaveTuning& o, const cv::UnisonTuning& t, const cv::SlapbackTuning& s,
                  const cv::DistortionTuning& d, const cv::ChasmTuning& c, const cv::PolishParams& e,
                  const cv::GateTuning& ig, const cv::GateTuning& g, const cv::AutotuneTuning& at,
                  const ProtoParams& params) {
  const int prefix = static_cast<int>(snapshotLines(params, gTuningText, sizeof(gTuningText)));
  std::snprintf(
      gTuningText + prefix, sizeof(gTuningText) - static_cast<size_t>(prefix),
      "OctaveTuning{%.1ff, %.1ff, %.0ff, %d, %.1ff}\n"
      "UnisonTuning{{%.1ff, %.1ff}, {%.2ff, %.2ff}, %.1ff, %.1ff, %.1ff, %.1ff}\n"
      "SlapbackTuning{%.1ff, %.0ff, %.2ff, %.1ff, %.1ff}\n"
      "DistortionTuning{%.0ff, %.0ff, %.1ff, %.0ff, %.1ff, %.0ff, %.1ff, %.0ff, %.1ff, %.1ff, "
      "%.0ff, %.0ff, %.1ff, %.2ff, %.2ff, %.0ff, %.1ff, %.1ff, %.1ff, %.0ff, %.1ff, %.2ff, %.1ff, %.2ff}\n"
      "ChasmTuning{%.2ff, %.2ff, %.0ff, %.2ff, %.2ff, %.0ff, %.0ff, %.0ff, %.2ff, %.2ff, %.2ff, "
      "%.1ff, %.1ff, %.1ff, %.1ff}\n"
      "PolishTuning{%.2ff, %.2ff, %.0ff, %.1ff}\n"
      "// PolishParams: hpHz %.0f, dip %.0f Hz %.1f dB, presence %.0f Hz %.1f dB, air %.1f dB",
      o.levelDb, o.glideMs, o.grainWindowMs, o.grainCount, o.trimDb,
      t.baseDelayMs[0], t.baseDelayMs[1], t.lfoHz[0], t.lfoHz[1], t.swingMinMs,
      t.swingMaxMs, t.wetMaxDb, t.trimDb,
      s.timeMs, s.lowpassHz, s.feedback, s.wetMaxDb, s.trimDb, d.inputHpHz, d.s1BassHz, d.s1BassDb, d.s1LpHz, d.gain1Max, d.stackBassHz, d.stackBassDb,
      d.stackTrebleHz, d.stackTrebleDb, d.stackLossDb, d.s2HpHz, d.s2LpHz, d.gain2Max,
      d.railAsym, d.railSoft, d.trebleCutHz, d.trebleCutDb, d.toneMinDb, d.toneMaxDb, d.bassPeakHz,
      d.bassPeakDb, d.bassPeakQ, d.trimDb, d.fadeDrive,
      c.timeLo, c.timeHi, c.trebleLossHz,
      c.loopTrebleCut, c.inputTrebleCut, c.bassCutHz, c.bassCutHzTop, c.wobbleDepthMax,
      c.wobbleRateLo, c.wobbleRateHi, c.inputTrim, c.wobbleLevelDb, c.dwellDrive, c.dwellComp, c.wetDb, e.tuning.dipQ,
      e.tuning.presenceQ, e.tuning.airHz, e.tuning.trimDb, e.hpHz, e.dipHz, e.dipDb, e.presenceHz, e.presenceDb,
      e.airDb);
  size_t used = std::strlen(gTuningText);
  const cv::SpringTuning& sb = params.reverb.spring.tuning;
  std::snprintf(gTuningText + used, sizeof(gTuningText) - used,
                "\nSpringTuning{%.1ff, %.1ff, %.1ff, %.2ff, %.2ff, %.2ff, %d, %.2ff, %.0ff, %.0ff, "
                "%.0ff, %.0ff, %.1ff, %.2ff, %.2ff, %.1ff, %.2ff, %.1ff}",
                sb.springMs1, sb.springMs2, sb.springMs3, sb.timeLo, sb.timeHi, sb.diffuse,
                sb.chirpSections, sb.chirpA, sb.chirpSign, sb.trebleLossHz, sb.bassCutHz, sb.returnLpHz,
                sb.wobbleDepth, sb.wobbleRateHz, sb.inputTrim, sb.dwellDrive, sb.dwellComp, sb.wetDb);
  used = std::strlen(gTuningText);
  std::snprintf(
      gTuningText + used, sizeof(gTuningText) - used,
      "\nInputGateTuning{%.1ff, %.0ff, %.0ff, %.1ff, %.1ff, %.0ff, %.1ff}"
      "\nGateTuning{%.1ff, %.0ff, %.0ff, %.1ff, %.1ff, %.0ff, %.1ff}"
      "\nAutotuneTuning{%.1ff, %.1ff, %.0ff}",
      ig.attackMs, ig.holdMs, ig.releaseMs, ig.rangeDb, ig.kneeDb, ig.detectorHpHz, ig.hysteresisDb,
      g.attackMs, g.holdMs, g.releaseMs, g.rangeDb, g.kneeDb, g.detectorHpHz, g.hysteresisDb,
      at.trimDb, at.maxCorrectSemis, at.grainDelay);
}

void printTuning(const cv::OctaveTuning& o, const cv::UnisonTuning& t, const cv::SlapbackTuning& s,
                 const cv::DistortionTuning& d, const cv::ChasmTuning& c, const cv::PolishParams& e,
                 const cv::GateTuning& ig, const cv::GateTuning& g, const cv::AutotuneTuning& at,
                 const ProtoParams& params) {
  formatTuning(o, t, s, d, c, e, ig, g, at, params);
  std::printf(
      "// AutotuneTuning: trimDb, maxCorrectSemis, grainDelay\n"
      "// GateTuning: attackMs, holdMs, releaseMs, rangeDb, kneeDb, detectorHpHz, hysteresisDb\n"
      "// OctaveTuning: levelDb, glideMs, grainWindowMs, grainCount, trimDb\n"
      "// UnisonTuning: {baseDelayMs[0], baseDelayMs[1]}, {lfoHz[0], lfoHz[1]}, swingMinMs, "
      "swingMaxMs, wetMaxDb, trimDb\n"
      "// SlapbackTuning: timeMs, lowpassHz, feedback, wetMaxDb, trimDb\n"
      "// DistortionTuning: inputHpHz, s1BassHz, s1BassDb, s1LpHz, gain1Max, stackBassHz, "
      "stackBassDb, stackTrebleHz, stackTrebleDb, stackLossDb, s2HpHz, s2LpHz, gain2Max, "
      "railAsym, railSoft, trebleCutHz, trebleCutDb, toneMinDb, toneMaxDb, bassPeakHz, bassPeakDb, "
      "bassPeakQ, trimDb, fadeDrive\n"
      "// ChasmTuning: timeLo, timeHi, trebleLossHz, loopTrebleCut, inputTrebleCut, bassCutHz, "
      "bassCutHzTop, wobbleDepthMax, wobbleRateLo, wobbleRateHi, inputTrim, wobbleLevelDb, "
      "dwellDrive, dwellComp, wetDb\n// PolishTuning: dipQ, presenceQ, airHz, trimDb\n"
      "// SpringTuning: springMs1, springMs2, springMs3, timeLo, timeHi, diffuse, chirpSections, "
      "chirpA, chirpSign, trebleLossHz, bassCutHz, returnLpHz, wobbleDepth, wobbleRateHz, inputTrim, "
      "dwellDrive, dwellComp, wetDb\n%s\n",
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
  bool mic = false;        // source MIC, so the header shows the gain slider
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
void drawOctaveRaw(cv::OctaveTuning& o) {
  ImGui::SliderFloat("Level", &o.levelDb, -24.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("Trim", &o.trimDb, -12.0f, 12.0f, "%.1f dB");
  ImGui::SliderFloat("Tracking speed (ms)", &o.glideMs, 0.0f, 100.0f, "%.0f ms");
  ImGui::SliderFloat("Grain window", &o.grainWindowMs, 20.0f, 80.0f, "%.0f ms");
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Grains");
  ImGui::SameLine();
  ImGui::RadioButton("2", &o.grainCount, 2);
  ImGui::SameLine();
  ImGui::RadioButton("4", &o.grainCount, 4);
}

void drawAutotuneRaw(cv::AutotuneTuning& t) {
  ImGui::SliderFloat("Trim", &t.trimDb, -12.0f, 12.0f, "%.1f dB");
}

void drawUnisonRaw(cv::UnisonTuning& t) {
  ImGui::TextDisabled("DEPTH scales the swing and the wet level together.");
  if (tuningNode("Chorus (LFO-wobbled delay)")) {
    ImGui::SliderFloat("Base delay 1", &t.baseDelayMs[0], 5.0f, 40.0f, "%.1f ms");
    ImGui::SliderFloat("Base delay 2", &t.baseDelayMs[1], 5.0f, 40.0f, "%.1f ms");
    ImGui::SliderFloat("LFO rate 1", &t.lfoHz[0], 0.1f, 3.0f, "%.2f Hz");
    ImGui::SliderFloat("LFO rate 2", &t.lfoHz[1], 0.1f, 3.0f, "%.2f Hz");
    ImGui::SliderFloat("Swing at depth 0", &t.swingMinMs, 0.0f, 1.0f, "%.2f ms");
    ImGui::SliderFloat("Swing at depth 1", &t.swingMaxMs, 0.5f, 6.0f, "%.2f ms");
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
  ImGui::SliderFloat("Trim at full", &t.trimDb, -12.0f, 6.0f, "%.1f dB");
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

void drawReverbRaw(cv::ChasmTuning& c, cv::SpringTuning& b) {
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
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
  if (tuningNode("Spring")) {
    ImGui::SliderFloat("Echo spacing 1", &b.springMs1, 10.0f, 80.0f, "%.1f ms");
    ImGui::SliderFloat("Echo spacing 2", &b.springMs2, 10.0f, 80.0f, "%.1f ms");
    ImGui::SliderFloat("Echo spacing 3", &b.springMs3, 10.0f, 80.0f, "%.1f ms");
    ImGui::SliderFloat("Feedback at decay 0", &b.timeLo, 0.1f, 0.95f, "%.2f");
    ImGui::SliderFloat("Feedback at decay 1", &b.timeHi, 0.5f, 0.97f, "%.3f");
    ImGui::SliderFloat("Diffusion", &b.diffuse, 0.0f, 0.9f, "%.2f");
    ImGui::SliderInt("Sweep sections", &b.chirpSections, 0, 32);
    ImGui::SliderFloat("Sweep coefficient", &b.chirpA, 0.0f, 0.95f, "%.2f");
    bool down = b.chirpSign < 0.0f;
    if (ImGui::Checkbox("Sweep down", &down)) b.chirpSign = down ? -1.0f : 1.0f;
    ImGui::SliderFloat("Loop treble loss", &b.trebleLossHz, 1000.0f, 16000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Loop bass cut", &b.bassCutHz, 20.0f, 600.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Return low-pass", &b.returnLpHz, 1000.0f, 16000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Wobble depth", &b.wobbleDepth, 0.0f, 32.0f, "%.1f samples");
    ImGui::SliderFloat("Wobble rate", &b.wobbleRateHz, 0.1f, 10.0f, "%.2f Hz", ImGuiSliderFlags_Logarithmic);
    ImGui::TreePop();
  }
  if (tuningNode("Spring levels")) {
    ImGui::SliderFloat("Drive at dwell 1", &b.dwellDrive, 1.0f, 64.0f, "%.1f x", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Drive compensation", &b.dwellComp, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Input trim", &b.inputTrim, 0.05f, 1.5f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Wet level", &b.wetDb, -24.0f, 24.0f, "%.1f dB");
    ImGui::TreePop();
  }
}

// Reset and Print cover every effect's tuning.
void drawTuningButtons(ProtoParams& params, float resetW, float copyW) {
  cv::OctaveTuning& ot = params.pitchFx.octave.tuning;
  if (ImGui::Button("Reset to defaults", ImVec2(resetW, 0.0f))) {
    ot = cv::OctaveTuning{};
    params.pitchFx.autotune.tuning = cv::AutotuneTuning{};
    params.unison.tuning = cv::UnisonTuning{};
    params.slapback.tuning = cv::SlapbackTuning{};
    params.distortion.tuning = cv::DistortionTuning{};
    params.reverb.chasm.tuning = cv::ChasmTuning{};
    params.reverb.spring.tuning = cv::SpringTuning{};
    params.eq.tuning = cv::PolishTuning{};
    params.inputGate.tuning = cv::GateTuning{};
    params.inputGate.tuning.rangeDb = kInputGateRangeDb;
    params.inputGate.tuning.holdMs = kInputGateHoldMs;
    params.inputGate.tuning.releaseMs = kInputGateReleaseMs;
    params.gate.tuning = cv::GateTuning{};
    params.reverb.chasm.wobble = cv::ChasmParams{}.wobble;  // the CHASM Wobble macro writes it
    params.macros = cv::macros::State{};
  }
  ImGui::SameLine();
  // The label confirms the copy for a moment; the ### id keeps the button the same widget.
  static double copiedAt = -10.0;
  constexpr double kCopiedShowSec = 1.5;
  const bool justCopied = ImGui::GetTime() - copiedAt < kCopiedShowSec;
  if (ImGui::Button(justCopied ? "Copied###copy" : "Copy settings###copy",
                    ImVec2(copyW, 0.0f))) {
    printTuning(ot, params.unison.tuning, params.slapback.tuning, params.distortion.tuning,
                params.reverb.chasm.tuning, params.eq, params.inputGate.tuning,
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

void toggleButton(const char* name, bool& on, bool withState = true, float width = 0.0f) {
  char label[64];
  if (withState) std::snprintf(label, sizeof(label), "%s: %s###%s", name, on ? "ON" : "OFF", name);
  else std::snprintf(label, sizeof(label), "%s", name);
  // Push and pop follow the state at draw time; the click flips on in between.
  const bool lit = on;
  if (lit) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  if (ImGui::Button(label, ImVec2(width, 0.0f))) on = !on;
  if (lit) ImGui::PopStyleColor();
  gToggleRects[name] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
}

// Header items in order, each at its natural width plus an equal share of the
// spare row width, so the row spans the window.
struct HeaderWidths {
  float mic, load, name, play, meter, advanced, feedback, amount, movement, reset, copy;
};

HeaderWidths headerWidths(bool dev) {
  const ImGuiStyle& st = ImGui::GetStyle();
  const auto button = [&](const char* t) { return ImGui::CalcTextSize(t).x + 2.0f * st.FramePadding.x; };
  const auto slider = [&](const char* t) {
    return kFeedbackSliderW + st.ItemInnerSpacing.x + ImGui::CalcTextSize(t).x;
  };
  HeaderWidths w{button("MIC"),       button("Load loop"), kLoopNameW,  button("Stop"),
                 kMeterW,             dev ? button("Advanced") : 0.0f,
                 button("STAGE FEEDBACK: OFF"), slider("Amount"), slider("Movement"),
                 button("Reset to defaults"),   button("Copy settings")};
  float* const items[] = {&w.mic,      &w.load,   &w.name,     &w.play,   &w.meter, &w.advanced,
                          &w.feedback, &w.amount, &w.movement, &w.reset,  &w.copy};
  const int count = dev ? 11 : 10;
  float used = st.ItemSpacing.x * static_cast<float>(count - 1);
  for (float* v : items) used += *v;
  const float extra = std::max(0.0f, ImGui::GetContentRegionAvail().x - used) / static_cast<float>(count);
  for (float* v : items)
    if (*v > 0.0f) *v += extra;
  return w;
}

// Fixed-width name slot so a long name never pushes the row off screen.
void fixedName(std::string name, float maxW) {
  if (ImGui::CalcTextSize(name.c_str()).x > maxW) {
    while (!name.empty() && ImGui::CalcTextSize((name + "...").c_str()).x > maxW) name.pop_back();
    name += "...";
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(name.c_str());
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::Dummy(ImVec2(std::max(0.0f, maxW - ImGui::GetItemRectSize().x), 0.0f));
}

void drawTransportItems(ProtoParams& params, float& meterDb, bool probe) {
  const HeaderWidths hw = headerWidths(params.dev);
  const ImGuiStyle& st = ImGui::GetStyle();
  // MIC lights when the source is the input macOS has chosen. The loop group
  // then gives its three slots to the input name and gain, so the row keeps its width.
  const bool micAvailable = gMicAvailable.load(std::memory_order_relaxed);
  ImGui::BeginDisabled(!micAvailable);
  bool mic = params.source == 1 && micAvailable;
  toggleButton("MIC", mic, false, hw.mic);
  ImGui::EndDisabled();
  params.source = mic ? 1 : 0;
  ImGui::SameLine();
  if (mic) {
    // A Bluetooth headset mic runs at 8 or 16 kHz and carries 100 ms or more of lag;
    // naming the rate makes that visible.
    std::string name = gInputName;
    if (gInputRate > 0 && gInputRate < 32000) name += " " + std::to_string(gInputRate / 1000) + " kHz";
    fixedName(name, hw.name);
    ImGui::SameLine();
    const float gainW = hw.load + hw.play + st.ItemSpacing.x;
    ImGui::SetNextItemWidth(gainW - st.ItemInnerSpacing.x - ImGui::CalcTextSize("Gain").x);
    ImGui::SliderFloat("Gain", &params.inputGainDb, -12.0f, 24.0f, "%+.0f dB");
  } else {
    if (ImGui::Button("Load loop", ImVec2(hw.load, 0.0f)) && !probe) {
      const std::string path = openFilePanel();
      if (!path.empty()) loadLoop(path);
    }
    ImGui::SameLine();
    fixedName(gLoopPath.empty() ? "(no loop)" : baseName(gLoopPath), hw.name);
    ImGui::SameLine();
    if (ImGui::Button(params.playing ? "Stop###play" : "Play###play", ImVec2(hw.play, 0.0f)))
      params.playing = !params.playing;
  }
  ImGui::SameLine();

  const float frac = std::clamp((meterDb - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
  char label[32];
  std::snprintf(label, sizeof(label), "%.1f dBFS", meterDb);
  ImGui::ProgressBar(frac, ImVec2(hw.meter, 0.0f), label);
  ImGui::SameLine();
  if (params.dev) {
    toggleButton("Advanced", params.advanced, false, hw.advanced);  // dev only; lit when on
    ImGui::SameLine();
  }
  toggleButton("STAGE FEEDBACK", params.stageFeedback, true, hw.feedback);
  ImGui::BeginDisabled(!params.stageFeedback);
  ImGui::SameLine();
  const float labelGap = ImGui::GetStyle().ItemInnerSpacing.x;
  ImGui::SetNextItemWidth(hw.amount - labelGap - ImGui::CalcTextSize("Amount").x);
  ImGui::SliderFloat("Amount", &params.feedbackAmount, 0.0f, 100.0f, "%.0f%%",
                     ImGuiSliderFlags_NoRoundToFormat);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(hw.movement - labelGap - ImGui::CalcTextSize("Movement").x);
  ImGui::SliderFloat("Movement", &params.feedbackMovement, 0.0f, 100.0f, "%.0f%%",
                     ImGuiSliderFlags_NoRoundToFormat);
  ImGui::EndDisabled();
  ImGui::SameLine();
  drawTuningButtons(params, hw.reset, hw.copy);
}

// ---- Saved state: the face as Adam left it ----------------------------------------
//
// Plain "key value" lines. Unknown keys are skipped and missing keys keep their
// defaults, so a new build reads an old file. A key keeps the name it was first
// saved under. Raw Advanced edits are not saved; the macros rewrite those fields
// on load.

struct StateField {
  std::string key;
  char kind;  // 'f' float, 'i' int, 'b' bool
  void* ptr;
};

std::vector<StateField> stateFields(ProtoParams& p) {
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
  add("autotune.key", &at.key);
  add("autotune.response", &at.responseMs);
  add("autotune.pull", &at.tuning.maxCorrectSemis);
  cv::OctaveParams& o = p.pitchFx.octave;
  add("octave.on", &o.on);
  add("octave.semitones", &o.semitones);
  add("octave.mix", &o.mix);
  add("unison.on", &p.unison.on);
  add("unison.depth", &p.unison.depth);
  add("slapback.on", &p.slapback.on);
  add("slapback.intensity", &p.slapback.mix);
  add("slapback.time", &p.slapback.tuning.timeMs);
  add("slapback.repeats", &p.slapback.tuning.feedback);
  add("slapback.lowpass", &p.slapback.tuning.lowpassHz);
  add("distortion.on", &p.distortion.on);
  add("distortion.drive", &p.distortion.drive);
  add("distortion.tone", &p.distortion.tone);
  cv::ReverbParams& r = p.reverb;
  add("reverb.on", &r.on);
  add("reverb.engine", &r.engine);
  add("reverb.intensity", &r.mix);
  add("reverb.time", &r.time);
  add("reverb.chasm.decay", &r.chasm.decay);
  add("reverb.chasm.dwell", &r.chasm.dwell);
  add("reverb.springb.decay", &r.spring.decay);
  add("reverb.springb.dwell", &r.spring.dwell);
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
  add("mute", &p.mute);
  add("advanced", &p.advanced);
  add("feedback.on", &p.stageFeedback);
  add("feedback.amount", &p.feedbackAmount);
  add("feedback.movement", &p.feedbackMovement);
  add("source", &p.source);
  add("input.gainDb", &p.inputGainDb);
  return f;
}

std::string stateText(ProtoParams& p, const std::string& loop) {
  std::string out = "loop " + loop + "\n";
  char line[160];
  for (const StateField& f : stateFields(p)) {
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

// Parses stateText output into p and loop. Returns the number of fields set.
int parseStateText(const std::string& text, ProtoParams& p, std::string& loop) {
  const std::vector<StateField> fields = stateFields(p);
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
  mc::unison(p.unison.tuning, m[mc::UnisonMotion]);
  mc::distBody(p.distortion.tuning, m[mc::DistBody]);
  mc::distBite(p.distortion.tuning, m[mc::DistBite]);
  mc::distGrit(p.distortion.tuning, m[mc::DistGrit]);
  mc::chasmWobble(r.chasm, m[mc::ChasmWobble]);
  mc::chasmBrightness(r.chasm.tuning, m[mc::ChasmBrightness]);
  mc::chasmBass(r.chasm.tuning, m[mc::ChasmBass]);
  mc::springDrip(r.spring.tuning, m[mc::SpringDrip]);
  mc::springFlutter(r.spring.tuning, m[mc::SpringFlutter]);
  mc::springBrightness(r.spring.tuning, m[mc::SpringBrightness]);
}

// ~/Library/Application Support/cubevox-proto/state.txt; empty if HOME is unset.
std::string statePath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr) return {};
  const std::string dir = std::string(home) + "/Library/Application Support/cubevox-proto";
  mkdir(dir.c_str(), 0755);  // EEXIST is the normal case
  return dir + "/state.txt";
}

void loadState(const std::string& path, ProtoParams& p) {
  std::FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
  if (f == nullptr) return;  // first launch
  std::string text;
  char buf[4096];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
  std::fclose(f);
  std::string loop;
  if (parseStateText(text, p, loop) == 0)
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

// ---- Tuning headers: macro view by default, raw nodes under Advanced --------------

// One plain line under a tuning control: what it does, the technical term last.
void hint(const char* text) { ImGui::TextDisabled("%s", text); }

template <class F>
void macroSlider(const char* label, const char* help, float& pos, const F& apply) {
  if (ImGui::SliderFloat(label, &pos, 0.0f, 100.0f, "%.0f %%")) apply(pos);
  hint(help);
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
  if (gAdvanced) drawAutotuneRaw(a.tuning);
}

void drawUnisonTuning(cv::UnisonTuning& t) {
  if (!tuningHeader("unison")) return;
  if (gAdvanced) {
    drawUnisonRaw(t);
    return;
  }
  hint("DEPTH and RATE on the face set the chorus; Advanced shows its raw values");
}

void drawSlapbackTuning(cv::SlapbackTuning& t) {
  if (!tuningHeader("slapback")) return;
  if (gAdvanced) {
    drawSlapbackRaw(t);
    return;
  }
  ImGui::SliderFloat("Repeats", &t.feedback, 0.0f, 0.5f, "%.2f");
  hint("echoes after the first; MIX raises them too");
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
void drawGateTuning(cv::GateParams& g, const char* block) {
  if (!tuningHeader(block)) return;
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
  cv::GateTuning& t = g.tuning;
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
    drawReverbRaw(r.chasm.tuning, r.spring.tuning);
    return;
  }
  namespace mc = cv::macros;
  // Dwell (input drive into the soft clip) has no panel knob; it binds to the selected engine here.
  float& dwell = r.engine == cv::kReverbChasm ? r.chasm.dwell : r.spring.dwell;
  float dwellPct = dwell * 100.0f;
  if (ImGui::SliderFloat("Dwell", &dwellPct, 0.0f, 100.0f, "%.0f %%")) dwell = dwellPct / 100.0f;
  if (r.engine == cv::kReverbChasm) {
    macroSlider("Wobble", "pitch wobble in the tail", m.pos[mc::ChasmWobble],
                [&](float p) { mc::chasmWobble(r.chasm, p); });
    macroSlider("Brightness", "brighter tail", m.pos[mc::ChasmBrightness],
                [&](float p) { mc::chasmBrightness(r.chasm.tuning, p); });
    macroSlider("Bass", "more low end in the tail", m.pos[mc::ChasmBass],
                [&](float p) { mc::chasmBass(r.chasm.tuning, p); });
  } else {
    cv::SpringTuning& t = r.spring.tuning;
    macroSlider("Drip", "the drippy boing on each note", m.pos[mc::SpringDrip],
                [&](float p) { mc::springDrip(t, p); });
    macroSlider("Flutter", "wobble and crisper repeats", m.pos[mc::SpringFlutter],
                [&](float p) { mc::springFlutter(t, p); });
    macroSlider("Brightness", "brighter tail", m.pos[mc::SpringBrightness],
                [&](float p) { mc::springBrightness(t, p); });
  }
}

// Panel knob shown in percent.
void percentSlider(const char* label, float& value) {
  float percent = value * 100.0f;
  if (ImGui::SliderFloat(label, &percent, 0.0f, 100.0f, "%.0f %%")) value = percent / 100.0f;
}

// Autotune RESPONSE readout: the ends name the V3's NATURAL and MECHANICAL.
const char* responseFormat(float mechanical) {
  if (mechanical <= 0.005f) return "NATURAL";
  if (mechanical >= 0.995f) return "MECHANICAL";
  return "%.0f %%";
}

void drawAutotuneBlock(cv::AutotuneParams& a, const ProtoState& state) {
  ImGui::PushID("autotune");
  ImGui::Checkbox("AUTOTUNE", &a.on);
  ImGui::SetNextItemWidth(kAutotuneKeyW);
  ImGui::Combo("KEY", &a.key, cv::kKeyName, cv::kKeyCount);
  if (gAdvanced) {
    ImGui::SameLine();
    ImGui::Text("Correction: %+.2f st", a.on ? state.correctionSemis : 0.0f);
  }
  const float mechanical = cv::AutotuneVoice::mechanicalOf(a.responseMs);
  float percent = mechanical * 100.0f;
  if (ImGui::SliderFloat("RESPONSE", &percent, 0.0f, 100.0f, responseFormat(mechanical)))
    a.responseMs = cv::AutotuneVoice::responseMsAt(percent / 100.0f);
  drawAutotuneTuning(a);
  ImGui::PopID();
}

void drawOctaveBlock(cv::OctaveParams& o, cv::macros::State& macros) {
  ImGui::PushID("octave");
  ImGui::Checkbox("OCTAVE", &o.on);
  ImGui::SliderInt("SEMITONES", &o.semitones, -12, 12, "%+d st");
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
// Both gates carry the same controls (owner 2026-10-02): THRESHOLD and DECAY on the
// face, Mute amount / Fade in / Stay open under Tuning.
void drawGateBlock(const char* id, const char* label, cv::GateParams& g, float gainDb) {
  ImGui::PushID(id);
  gateLed(g.on, gainDb);
  ImGui::Checkbox(label, &g.on);
  ImGui::SliderFloat("THRESHOLD", &g.thresholdDb, -70.0f, -10.0f, "%.0f dB");
  ImGui::SliderFloat("DECAY", &g.tuning.releaseMs, 5.0f, 1000.0f, "%.0f ms",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::Text("Gain: %.1f dB", static_cast<double>(gainDb));
  drawGateTuning(g, id);
  ImGui::PopID();
}

void drawUnisonBlock(cv::UnisonParams& u, cv::macros::State& macros) {
  ImGui::PushID("unison");
  ImGui::Checkbox("UNISON", &u.on);
  percentSlider("DEPTH", u.depth);
  // RATE: chorus speed. Faster also deepens the swirl, up to a capped pitch swing.
  float& rate = macros.pos[cv::macros::UnisonMotion];
  if (ImGui::SliderFloat("RATE", &rate, 0.0f, 100.0f, "%.0f %%")) cv::macros::unison(u.tuning, rate);
  drawUnisonTuning(u.tuning);
  ImGui::PopID();
}

void drawSlapbackBlock(cv::SlapbackParams& s) {
  ImGui::PushID("slapback");
  ImGui::Checkbox("SLAPBACK", &s.on);
  percentSlider("MIX", s.mix);
  ImGui::SliderFloat("TIME", &s.tuning.timeMs, 60.0f, 250.0f, "%.0f ms");
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
  ImGui::RadioButton("CHASM", &r.engine, cv::kReverbChasm);
  ImGui::SameLine();
  ImGui::RadioButton("SPRING", &r.engine, cv::kReverbSpring);
  percentSlider("MIX", r.mix);
  percentSlider("TIME", r.time);
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

constexpr float kFooterH = 52.0f;    // strip under the columns for the mode button, px
constexpr float kMuteRowH = 46.0f;  // MUTE above the mode button, under the last column, px
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
constexpr ImVec2 kSwitchSize = ImVec2(96.0f, 44.0f);  // slide switch track, px
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

// Slide switch centred in the pedal; the thumb sits right and lights when on.
void pedalSwitch(const char* id, bool& on) {
  ImGui::Dummy(ImVec2(0.0f, kKnobGapY));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                       0.5f * std::max(0.0f, ImGui::GetContentRegionAvail().x - kSwitchSize.x));
  if (ImGui::InvisibleButton(id, kSwitchSize)) on = !on;
  const ImVec2 lo = ImGui::GetItemRectMin();
  const ImVec2 hi = ImGui::GetItemRectMax();
  const float r = 0.5f * kSwitchSize.y;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(lo, hi, on ? IM_COL32(230, 180, 60, 255) : IM_COL32(45, 45, 50, 255), r);
  dl->AddRect(lo, hi, IM_COL32(150, 150, 160, 255), r, 0, 1.5f);
  const ImVec2 thumb(on ? hi.x - r : lo.x + r, lo.y + r);
  dl->AddCircleFilled(thumb, r - 4.0f, IM_COL32(230, 230, 235, 255), 32);
  const char* text = on ? "ON" : "OFF";
  const ImVec2 size = ImGui::CalcTextSize(text);
  const float textX = on ? lo.x + r - 0.5f * size.x + 4.0f : hi.x - r - 0.5f * size.x - 4.0f;
  dl->AddText(ImVec2(textX, lo.y + r - 0.5f * size.y),
              on ? IM_COL32(30, 30, 30, 255) : IM_COL32(200, 200, 205, 255), text);
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
  cv::OctaveParams& o = params.pitchFx.octave;
  char semis[16];
  std::snprintf(semis, sizeof(semis), "%+d st", o.semitones);

  pedal("INPUT GATE", params.inputGate.on, w, [&] {
    pedalRange("THRESHOLD", params.inputGate.thresholdDb, -70.0f, -10.0f, false, "%.0f dB");
    pedalRange("DECAY", params.inputGate.tuning.releaseMs, 5.0f, 1000.0f, true, "%.0f ms");
  });
  next();
  pedal("AUTOTUNE", at.on, w, [&] {
    float mechanical = cv::AutotuneVoice::mechanicalOf(at.responseMs);
    char text[16];
    std::snprintf(text, sizeof(text), responseFormat(mechanical),
                  static_cast<double>(mechanical * 100.0f));
    if (pedalKnobRaw("RESPONSE", mechanical, text, 0))
      at.responseMs = cv::AutotuneVoice::responseMsAt(mechanical);
    pedalEncoder("KEY", at.key, 0, cv::kKeyCount - 1, cv::kKeyName[at.key]);
    pedalSwitch("autotuneSwitch", at.on);
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
      cv::macros::unison(params.unison.tuning, params.macros.pos[cv::macros::UnisonMotion]);
    }
  });
  next();
  pedal("SLAPBACK", params.slapback.on, w, [&] {
    pedalPercent("MIX", params.slapback.mix);
    pedalRange("TIME", params.slapback.tuning.timeMs, 60.0f, 250.0f, false, "%.0f ms");
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
    pedalPercent("MIX", r.mix);
    pedalPercent("TIME", r.time);
  });
}

void drawPedals(ProtoParams& params);
void drawColumns(ProtoParams& params, const ProtoState& state);
void drawModeButton(ProtoParams& params);

std::array<float, kColumnCount> gColumnUsed{};  // content height per column this frame, px
std::array<float, kColumnCount> gColumnAvail{};  // column height per column, px

// One face column. Columns never scroll; the probe fails on any overrun.
template <class F>
void column(int index, float width, const F& draw) {
  gCurrentColumn = index;
  ImGui::PushID(index);
  // The footer strip below the columns holds the mode button; the last column also
  // leaves room for MUTE above it.
  const float reserve = index == kColumnCount - 1 ? kFooterH + kMuteRowH : kFooterH;
  ImGui::BeginChild("column", ImVec2(width, -reserve), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PushItemWidth(-kLabelW);
  draw();
  ImGui::PopItemWidth();
  gColumnUsed[index] = ImGui::GetCursorPosY();
  gColumnAvail[index] = ImGui::GetWindowHeight();
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
  // A saved state can name an engine the face no longer offers; it loads as Spring.
  if (params.reverb.engine != cv::kReverbChasm) params.reverb.engine = cv::kReverbSpring;
  // A saved state can hold a TIME below the 60 ms floor.
  params.slapback.tuning.timeMs = std::clamp(params.slapback.tuning.timeMs, 60.0f, 250.0f);
  params.pitchFx.autotune.key = std::clamp(params.pitchFx.autotune.key, 0, cv::kKeyCount - 1);
  cv::applyKnobs(params.reverb);

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
  const float w = std::max(ImGui::CalcTextSize("Pedal mode").x, ImGui::CalcTextSize("MUTED").x) +
                  2.0f * kModeButtonPadding.x;
  ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y -
                       ImGui::GetFrameHeight());
  ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - w);
  if (ImGui::Button(label, ImVec2(w, 0.0f))) params.pedalMode = !params.pedalMode;
  const float modeTop = ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y;
  // MUTE above it, same size, lit red while muted.
  ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - w,
                             modeTop - ImGui::GetStyle().ItemSpacing.y - ImGui::GetFrameHeight()));
  const bool lit = params.mute;
  if (lit) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 40, 40, 255));
  if (ImGui::Button(params.mute ? "MUTED###mute" : "Mute###mute", ImVec2(w, 0.0f)))
    params.mute = !params.mute;
  if (lit) ImGui::PopStyleColor();
  gToggleRects["MUTE"] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
  ImGui::SetCursorPosY(modeTop);
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
      (ImGui::GetContentRegionAvail().x - kWideColumnW - 3.0f * kColumnGap) / 3.0f;
  column(0, colW, [&] {
    moduleBox("ingateBox", true,
              [&] { drawGateBlock("ingate", "INPUT GATE", params.inputGate, state.inGateDb); });
    moduleBox("autotuneBox", false, [&] { drawAutotuneBlock(params.pitchFx.autotune, state); });
  });
  ImGui::SameLine(0.0f, kColumnGap);
  column(1, kWideColumnW, [&] {
    moduleBox("octaveBox", true,
              [&] { drawOctaveBlock(params.pitchFx.octave, params.macros); });
    moduleBox("unisonBox", false, [&] { drawUnisonBlock(params.unison, params.macros); });
  });
  ImGui::SameLine(0.0f, kColumnGap);
  column(2, colW, [&] {
    moduleBox("slapbackBox", true, [&] { drawSlapbackBlock(params.slapback); });
    moduleBox("distortionBox", false, [&] { drawDistortionBlock(params.distortion, params.macros); });
    moduleBox("gateBox", false,
              [&] { drawGateBlock("gate", "GATE", params.gate, state.gateDb); });
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
  constexpr int kChasm = cv::kReverbChasm, kSpring = cv::kReverbSpring;
  static const ProbeOpen kScenarios[] = {
      {true, nullptr, nullptr},
      {true, "octave", nullptr},
      {true, "autotune", nullptr},
      {true, "unison", nullptr},     {true, "slapback", nullptr},
      {true, "distortion", nullptr},
      {true, "reverb", nullptr, false, kChasm},  {true, "reverb", nullptr, false, kSpring},
      {true, "eq", nullptr},         {true, "ingate", nullptr},
      {true, "gate", nullptr},
      {true, nullptr, nullptr, true},
      {true, "octave", nullptr, true},          {true, "unison", nullptr, true},
      {true, "unison", "Chorus (LFO-wobbled delay)", true},
      {true, "unison", "Shared", true},
      {true, "slapback", nullptr, true},        {true, "distortion", nullptr, true},
      {true, "distortion", "Stage 1", true},    {true, "distortion", "Tone stack", true},
      {true, "distortion", "Stage 2", true},    {true, "distortion", "Post", true},
      {true, "reverb", nullptr, true},
      {true, "reverb", "Chasm", true},          {true, "reverb", "Chasm levels", true},
      {true, "reverb", "Spring", true},         {true, "reverb", "Spring levels", true},
      {true, "eq", nullptr, true},              {true, "ingate", nullptr, true},
      {true, "autotune", nullptr, true},        {true, "gate", nullptr, true},
      {true, nullptr, nullptr, false, -1, true},
      {true, nullptr, nullptr, false, -1, false, true},
      {true, nullptr, nullptr, false, -1, true, true},
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
    params.source = s.mic ? 1 : 0;
    gMicAvailable.store(s.mic);
    gInputName = "Scarlett 2i2 USB";  // a long real name for the header
    if (s.reverbEngine >= 0) params.reverb.engine = s.reverbEngine;
    for (int frame = 0; frame < kProbeFrames; ++frame) {
      ImGui::NewFrame();
      drawFrame(params, state, meterDb, true);
      ImGui::Render();
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%s %s%s%s%s%s", s.advanced ? "adv  " : "macro",
                  s.block ? s.block : "all closed", s.node ? " / " : "", s.node ? s.node : "",
                  s.pedal ? " (PEDAL MODE)" : s.reverbEngine == kChasm ? " (CHASM)" : (s.reverbEngine == kSpring ? " (SPRING)" : ""),
                  s.mic ? " (MIC)" : "");
    const float worst = *std::max_element(gColumnUsed.begin(), gColumnUsed.end());
    const bool headerFits = gHeaderRight <= static_cast<float>(kWindowW);
    if (!headerFits) std::printf("header row runs to %.0f px of %d\n", gHeaderRight, kWindowW);
    else std::printf("header row ends at %.0f px of %d\n", gHeaderRight, kWindowW);
    const bool pedalFits = gPedalRight <= static_cast<float>(kWindowW) &&
                           gPedalBottom <= static_cast<float>(kWindowH) - kFooterH;
    bool columnsFit = true;
    for (int c = 0; c < kColumnCount; ++c) columnsFit = columnsFit && gColumnUsed[c] <= gColumnAvail[c];
    const bool ok = (s.pedal ? pedalFits : columnsFit) && headerFits;
    if (s.pedal) std::printf("pedal row: right %.0f of %d px, bottom %.0f of %.0f px\n", gPedalRight,
                             kWindowW, gPedalBottom, static_cast<float>(kWindowH) - kFooterH);
    fits = fits && ok;
    std::printf("%-44s col1 %4.0f  col2 %4.0f  col3 %4.0f  col4 %4.0f  of %.0f px%s\n", name,
                gColumnUsed[0], gColumnUsed[1], gColumnUsed[2], gColumnUsed[3], gColumnAvail[3],
                ok ? "" : "  OVERRUN");
  }
  // Click each header toggle on, then off, through ImGui's input queue. A style-stack
  // imbalance asserts inside EndFrame, so surviving both clicks is the check.
  gProbeOpen = ProbeOpen{};
  ProtoParams clicked;
  clicked.dev = true;
  bool* const targets[3] = {&clicked.stageFeedback, &clicked.advanced, &clicked.mute};
  const char* const names[3] = {"STAGE FEEDBACK", "Advanced", "MUTE"};
  for (int t = 0; t < 3; ++t) {
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
  for (const StateField& f : stateFields(moved)) {
    if (f.kind == 'f') *static_cast<float*>(f.ptr) += 0.37f;
    else if (f.kind == 'i') *static_cast<int*>(f.ptr) += 1;
    else *static_cast<bool*>(f.ptr) = !*static_cast<bool*>(f.ptr);
  }
  const std::string text = stateText(moved, "/tmp/a loop.wav");
  ProtoParams back;
  std::string backLoop;
  const int set = parseStateText(text, back, backLoop);
  // The file path: write, read back, and the face must match the parsed one.
  const std::string probeFile = "/tmp/cubevox-proto-probe-state.txt";
  saveState(probeFile, text);
  ProtoParams fromFile;
  loadState(probeFile, fromFile);
  std::remove(probeFile.c_str());
  applyMacros(back);  // loadState re-applies macros; match that before comparing
  const bool fileOk = stateText(fromFile, gLoopPath) == stateText(back, gLoopPath);
  const std::vector<StateField> want = stateFields(moved);
  const std::vector<StateField> got = stateFields(back);
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

  // Default macro positions must reproduce the engine tuning defaults (the firmware's).
  const auto tuningText = [](const ProtoParams& q) {
    formatTuning(q.pitchFx.octave.tuning, q.unison.tuning, q.slapback.tuning, q.distortion.tuning,
                 q.reverb.chasm.tuning, q.eq, q.inputGate.tuning, q.gate.tuning,
                 q.pitchFx.autotune.tuning, q);
    return std::string(gTuningText);
  };
  ProtoParams engineDefaults;
  ProtoParams viaMacros;
  applyMacros(viaMacros);
  const bool defaultsOk =
      tuningText(engineDefaults) == tuningText(viaMacros) &&
      std::fabs(engineDefaults.reverb.chasm.wobble - viaMacros.reverb.chasm.wobble) < 0.001f;
  std::printf("default macros match engine defaults: %s\n", defaultsOk ? "ok" : "FAILED");
  if (!defaultsOk) {
    std::printf("engine:\n%s\nmacros:\n", tuningText(engineDefaults).c_str());
    std::printf("%s\n", tuningText(viaMacros).c_str());
    fits = false;
  }

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
  loadState(stateFile, params);
  gParamsBuf[0] = params;
  gParamsBuf[1] = params;
  float meterDb = kSilenceDb;
  std::string savedText = stateText(params, gLoopPath);
  double lastSaveCheck = glfwGetTime();
  constexpr double kSaveEverySec = 1.0;  // a crash loses at most this much

  // Duplex on the devices macOS has chosen; miniaudio follows a change of
  // default input or output while running. No mic (or no permission) falls
  // back to playback only, so the loop still plays.
  ma_device_config deviceConfig = ma_device_config_init(ma_device_type_duplex);
  deviceConfig.capture.format = ma_format_f32;
  deviceConfig.capture.channels = 1;
  deviceConfig.playback.format = ma_format_f32;
  deviceConfig.playback.channels = 2;
  deviceConfig.sampleRate = cv::kSampleRate;
  deviceConfig.periodSizeInFrames = 64;  // duplex costs about four periods end to end
  deviceConfig.dataCallback = dataCallback;
  ma_device device;
  bool duplex = ma_device_init(nullptr, &deviceConfig, &device) == MA_SUCCESS;
  if (!duplex) {
    std::fprintf(stderr, "[WARN] no capture device; loop playback only\n");
    deviceConfig.deviceType = ma_device_type_playback;
    if (ma_device_init(nullptr, &deviceConfig, &device) != MA_SUCCESS) {
      std::fprintf(stderr, "[ERROR] audio device failed\n");
      return 1;
    }
  }
  gMicAvailable.store(duplex);
  std::fprintf(stderr, "[INFO] audio: capture %s %u Hz period %u, playback %s %u Hz period %u\n",
               device.capture.name, device.capture.internalSampleRate,
               device.capture.internalPeriodSizeInFrames, device.playback.name,
               device.playback.internalSampleRate, device.playback.internalPeriodSizeInFrames);
  if (ma_device_start(&device) != MA_SUCCESS) {
    std::fprintf(stderr, "[ERROR] audio device failed to start\n");
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
      ma_device_info info;
      if (duplex && ma_device_get_info(&device, ma_device_type_capture, &info) == MA_SUCCESS) {
        gInputName = info.name;
        gInputRate = device.capture.internalSampleRate;
      }
      const std::string text = stateText(params, gLoopPath);
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

  saveState(stateFile, stateText(params, gLoopPath));
  ma_device_uninit(&device);
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

// Prints the copied-settings text of a fresh app, no window.
int printDefaults() {
  ImGui::CreateContext();
  ProtoParams params;
  applyMacros(params);
  printTuning(params.pitchFx.octave.tuning, params.unison.tuning, params.slapback.tuning,
              params.distortion.tuning, params.reverb.chasm.tuning, params.eq,
              params.inputGate.tuning, params.gate.tuning, params.pitchFx.autotune.tuning, params);
  ImGui::DestroyContext();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--layout") == 0) return runLayoutProbe();
    if (std::strcmp(argv[i], "--print-defaults") == 0) return printDefaults();
    std::fprintf(stderr, "usage: cubevox-proto [--layout | --print-defaults]\n");
    return 2;
  }
  return runWindow();
}
