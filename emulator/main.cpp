// cubevox prototype face: loads a loop, plays it through the engine, mirrors
// the panel controls. Windowless probe: --layout.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#endif
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "../third_party/miniaudio.h"

#include "../emulator/open_panel.h"
#include "engine/common.h"
#include "engine/distortion.h"
#include "engine/pitch_fx.h"
#include "engine/slapback.h"
#include "engine/polish.h"
#include "engine/reverb.h"
#include "engine/spring.h"
#include "engine/unison.h"

namespace {

constexpr int kWindowW = 1440;
constexpr int kWindowH = 880;
constexpr float kHarmonyColumnW = 592.0f;  // Harmony column width
constexpr float kLabelW = 175.0f;          // room right of each slider for its label
constexpr float kMeterW = 240.0f;
constexpr int kProbeFrames = 4;
constexpr float kMeterFloorDb = -60.0f;
constexpr float kSilenceDb = -120.0f;
constexpr float kMeterDecayDbPerFrame = 0.6f;

// The emulator opens with the knob where the owner left it; on the box the
// pot decides (owner 2026-10-01: DEPTH 80 %).
constexpr float kStartDepth = 0.8f;
constexpr float kStartIntensity = 0.5f;
constexpr float kStartDrive = 0.3f;

struct ProtoParams {
  bool playing = false;
  cv::PitchFxParams pitchFx;
  cv::UnisonParams unison{false, kStartDepth, {}};
  cv::SlapbackParams slapback{true, kStartIntensity, {}};
  cv::DistortionParams distortion{true, kStartDrive, {}};
  cv::ReverbParams reverb;
  cv::PolishParams eq;

  // Every effect opens off; engine defaults stay on for the firmware.
  ProtoParams() {
    pitchFx.harmony.on = false;
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
cv::PitchFx gPitchFx;
cv::Unison gUnison;
cv::Slapback gSlapback;
cv::Distortion gDistortion;
cv::Reverb gReverb;
cv::Polish gPolish;
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
    gPitchFx.reset();
    gUnison.reset();
    gSlapback.reset();
    gDistortion.reset();
    gReverb.reset();
    gPolish.reset();
  }
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
    gPitchFx.process(in.data(), tmp.data(), n, params.pitchFx);
    gUnison.process(tmp.data(), mono.data(), n, params.unison);
    gSlapback.process(mono.data(), tmp.data(), n, params.slapback);
    gDistortion.process(tmp.data(), mono.data(), n, params.distortion);
    gReverb.process(mono.data(), tmp.data(), n, params.reverb);
    gPolish.process(tmp.data(), mono.data(), n, params.eq);
    for (int i = 0; i < n; ++i) {
      out[2 * (done + i)] = mono[i];
      out[2 * (done + i) + 1] = mono[i];
      peak = std::max(peak, std::fabs(mono[i]));
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

char gTuningText[2560];  // last Print tuning output
char gTuningLine[2560];  // the same text on one line, shown on the face

// Finder launches have no stdout, so the text also goes to the clipboard
// and into a read-only field beside the button.
void printTuning(const cv::HarmonyTuning& h, const cv::OctaveTuning& o,
                 const cv::UnisonTuning& t, const cv::SlapbackTuning& s,
                 const cv::DistortionTuning& d, const cv::SpringTuning& sp,
                 const cv::ChasmTuning& c, const cv::PolishParams& e) {
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
  std::snprintf(line + len, sizeof(line) - len, "}");
  std::snprintf(
      gTuningText, sizeof(gTuningText),
      "%s\n"
      "OctaveTuning{%.1ff, %.1ff, %s, %.2ff, %.2ff, %.0ff}\n"
      "UnisonTuning{{%.1ff, %.1ff}, {%.2ff, %.2ff}, %.1ff, %.1ff, %.1ff, {%.1ff, %.1ff}, %.0ff}\n"
      "SlapbackTuning{%.1ff, %.0ff, %.2ff, %.1ff}\n"
      "DistortionTuning{%.0ff, %.0ff, %.1ff, %.0ff, %.1ff, %.0ff, %.1ff, %.0ff, %.1ff, %.1ff, "
      "%.0ff, %.0ff, %.1ff, %.2ff, %.2ff, %.0ff, %.1ff, %.1ff, %.1ff, %.0ff, %.1ff, %.2ff, %.1ff, %.2ff, %s}\n"
      "SpringTuning{%.2ff, %.0ff, %.2ff, %.2ff, %.1ff, %.2ff, %.1ff, %.1ff, %.2ff, %.2ff, %d, %d, %.1ff, "
      "%.2ff, %.1ff, %.1ff, %.3ff}\n"
      "ChasmTuning{%.2ff, %.2ff, %.0ff, %.2ff, %.2ff, %.0ff, %.0ff, %.0ff, %.2ff, %.2ff, %.2ff, "
      "%.1ff, %.1ff}\n"
      "PolishTuning{%.2ff, %.2ff, %.0ff}\n"
      "// PolishParams: hpHz %.0f, dip %.0f Hz %.1f dB, presence %.0f Hz %.1f dB, air %.1f dB",
      line, o.levelDb, o.glideMs, o.muteUnvoiced ? "true" : "false", o.grainPeriods,
      o.epochSearch, o.epochLpHz, t.baseDelayMs[0], t.baseDelayMs[1], t.lfoHz[0], t.lfoHz[1], t.swingMinMs,
      t.swingMaxMs, t.wetMaxDb, t.detuneCents[0], t.detuneCents[1], t.windowMs,
      s.timeMs, s.lowpassHz, s.feedback, s.wetMaxDb, d.inputHpHz, d.s1BassHz, d.s1BassDb, d.s1LpHz, d.gain1Max, d.stackBassHz, d.stackBassDb,
      d.stackTrebleHz, d.stackTrebleDb, d.stackLossDb, d.s2HpHz, d.s2LpHz, d.gain2Max,
      d.railAsym, d.railSoft, d.trebleCutHz, d.trebleCutDb, d.toneMinDb, d.toneMaxDb, d.bassPeakHz,
      d.bassPeakDb, d.bassPeakQ, d.trimDb, d.fadeDrive, d.oversample ? "true" : "false",
      sp.inputGain, sp.hpHz, sp.tensionLo, sp.tensionHi, sp.dwellDrive, sp.dwellComp, sp.hfMixDbLo,
      sp.hfMixDbHi, sp.rippleGain, sp.splashDiffuse, sp.hfSections, sp.springs, sp.modDepth,
      sp.modRateHz, sp.boingDb, sp.wetDb, sp.tankTrim, c.timeLo, c.timeHi, c.trebleLossHz,
      c.loopTrebleCut, c.inputTrebleCut, c.bassCutHz, c.bassCutHzTop, c.wobbleDepthMax,
      c.wobbleRateLo, c.wobbleRateHi, c.inputTrim, c.wobbleLevelDb, c.wetDb, e.tuning.dipQ,
      e.tuning.presenceQ, e.tuning.airHz, e.hpHz, e.dipHz, e.dipDb, e.presenceHz, e.presenceDb,
      e.airDb);
  std::printf(
      "// HarmonyTuning: {levelDb[3]}, glideMs, voicedThreshold, muteUnvoiced, snapToScale, "
      "lower, low, high, higher\n"
      "// OctaveTuning: levelDb, glideMs, muteUnvoiced, grainPeriods, epochSearch, epochLpHz\n"
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
      "wetDb\n// PolishTuning: dipQ, presenceQ, airHz\n%s\n",
      gTuningText);
  std::fflush(stdout);
  ImGui::SetClipboardText(gTuningText);
}

// "A3 +4 cents voiced" from a tracker frequency; "--" before the first pitch.
void formatDetected(float hz, bool voiced, char* dst, size_t size) {
  static const char* const kNote[12] = {"C", "C#", "D", "D#", "E", "F",
                                        "F#", "G", "G#", "A", "A#", "B"};
  if (hz <= 0.0f) {
    std::snprintf(dst, size, "Detected: --");
    return;
  }
  const float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
  const int note = static_cast<int>(std::lround(midi));
  const int cents = static_cast<int>(std::lround((midi - static_cast<float>(note)) * 100.0f));
  const int pitchClass = ((note % 12) + 12) % 12;
  std::snprintf(dst, size, "Detected: %s%d %+d cents %s", kNote[pitchClass],
                (note - pitchClass) / 12 - 1, cents, voiced ? "voiced" : "unvoiced");
}

// Probe scenario: which Tuning header and sub-node are forced open. Null block = live UI.
struct ProbeOpen {
  bool active = false;
  const char* block = nullptr;
  const char* node = nullptr;
};
ProbeOpen gProbeOpen;

bool probeMatch(const char* want, const char* name) {
  return want != nullptr && std::strcmp(want, name) == 0;
}

// Collapsed Tuning header under one effect block.
bool tuningHeader(const char* block) {
  if (gProbeOpen.active) ImGui::SetNextItemOpen(probeMatch(gProbeOpen.block, block));
  return ImGui::CollapsingHeader("Tuning");
}

// Sub-node inside a Tuning header; caller pops with TreePop.
bool tuningNode(const char* name) {
  if (gProbeOpen.active) ImGui::SetNextItemOpen(probeMatch(gProbeOpen.node, name));
  return ImGui::TreeNode(name);
}

// Long tuning sections split into sub-nodes so no column ever needs a scroll bar.
void drawHarmonyTuning(cv::HarmonyTuning& h) {
  if (!tuningHeader("harmony")) return;
  if (tuningNode("Voices & tracking")) {
    ImGui::SliderFloat("Level low", &h.levelDb[0], -24.0f, 6.0f, "%.1f dB");
    ImGui::SliderFloat("Level medium", &h.levelDb[1], -24.0f, 6.0f, "%.1f dB");
    ImGui::SliderFloat("Level high", &h.levelDb[2], -24.0f, 6.0f, "%.1f dB");
    ImGui::SliderFloat("Tracking speed (ms)", &h.glideMs, 0.0f, 100.0f, "%.0f ms");
    ImGui::SliderFloat("Voiced threshold", &h.voicedThreshold, 0.05f, 0.4f, "%.2f");
    ImGui::Checkbox("Mute unvoiced", &h.muteUnvoiced);
    ImGui::Checkbox("Snap to scale", &h.snapToScale);
    ImGui::TreePop();
  }
}

void drawOctaveTuning(cv::OctaveTuning& o) {
  if (!tuningHeader("octave")) return;
  ImGui::SliderFloat("Level", &o.levelDb, -24.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("Tracking speed (ms)", &o.glideMs, 0.0f, 100.0f, "%.0f ms");
  ImGui::Checkbox("Mute unvoiced", &o.muteUnvoiced);
  ImGui::SliderFloat("Grain length (B)", &o.grainPeriods, 1.5f, 3.0f, "%.2f periods");
  ImGui::SliderFloat("Epoch search (B)", &o.epochSearch, 0.0f, 0.3f, "%.2f period");
  ImGui::SliderFloat("Epoch low-pass (B)", &o.epochLpHz, 100.0f, 4000.0f, "%.0f Hz",
                     ImGuiSliderFlags_Logarithmic);
}

void drawUnisonTuning(cv::UnisonTuning& t) {
  if (!tuningHeader("unison")) return;
  ImGui::TextDisabled("DEPTH scales both methods; the blend lives in these constants.");
  ImGui::SeparatorText("Chorus (LFO-wobbled delay)");
  ImGui::SliderFloat("Base delay 1", &t.baseDelayMs[0], 5.0f, 40.0f, "%.1f ms");
  ImGui::SliderFloat("Base delay 2", &t.baseDelayMs[1], 5.0f, 40.0f, "%.1f ms");
  ImGui::SliderFloat("LFO rate 1", &t.lfoHz[0], 0.1f, 3.0f, "%.2f Hz");
  ImGui::SliderFloat("LFO rate 2", &t.lfoHz[1], 0.1f, 3.0f, "%.2f Hz");
  ImGui::SliderFloat("Swing at depth 0", &t.swingMinMs, 0.0f, 1.0f, "%.2f ms");
  ImGui::SliderFloat("Swing at depth 1", &t.swingMaxMs, 0.5f, 6.0f, "%.2f ms");
  ImGui::SeparatorText("Doubler (fixed detune, TC-Helicon style)");
  ImGui::SliderFloat("DETUNE 0", &t.detuneCents[0], -30.0f, 30.0f, "%.1f cents");
  ImGui::SliderFloat("DETUNE 1", &t.detuneCents[1], -30.0f, 30.0f, "%.1f cents");
  ImGui::SliderFloat("WINDOW", &t.windowMs, 5.0f, 30.0f, "%.0f ms");
  ImGui::SeparatorText("Shared");
  ImGui::SliderFloat("Wet level at depth 1", &t.wetMaxDb, -24.0f, 0.0f, "%.1f dB");
}

void drawSlapbackTuning(cv::SlapbackTuning& t) {
  if (!tuningHeader("slapback")) return;
  ImGui::SliderFloat("Time", &t.timeMs, 30.0f, 120.0f, "%.0f ms");
  ImGui::SliderFloat("Lowpass", &t.lowpassHz, 500.0f, 12000.0f, "%.0f Hz",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("Feedback", &t.feedback, 0.0f, 0.5f, "%.2f");
  ImGui::SliderFloat("Wet level at full", &t.wetMaxDb, -24.0f, 0.0f, "%.1f dB");
}

void drawDistortionTuning(cv::DistortionTuning& t) {
  if (!tuningHeader("distortion")) return;
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
    ImGui::SliderFloat("Bass peak gain", &t.bassPeakDb, 0.0f, 12.0f, "%.1f dB");
    ImGui::SliderFloat("Bass peak Q", &t.bassPeakQ, 0.3f, 3.0f, "%.2f");
    ImGui::SliderFloat("Output trim", &t.trimDb, -12.0f, 12.0f, "%.1f dB");
    ImGui::SliderFloat("Fade-in span", &t.fadeDrive, 0.01f, 0.3f, "%.2f");
    ImGui::TreePop();
  }
}

void drawPolishTuning(cv::PolishTuning& t) {
  if (!tuningHeader("eq")) return;
  ImGui::SliderFloat("Low-mid Q", &t.dipQ, 0.3f, 3.0f, "%.2f");
  ImGui::SliderFloat("Presence Q", &t.presenceQ, 0.3f, 3.0f, "%.2f");
  ImGui::SliderFloat("Air corner", &t.airHz, 4000.0f, 16000.0f, "%.0f Hz",
                     ImGuiSliderFlags_Logarithmic);
}

void drawReverbTuning(cv::SpringTuning& t, cv::ChasmTuning& c) {
  if (!tuningHeader("reverb")) return;
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
    ImGui::SliderFloat("Wet level", &t.wetDb, -24.0f, 6.0f, "%.1f dB");
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
    ImGui::SliderFloat("Input trim", &c.inputTrim, 0.05f, 1.5f, "%.2f", log);
    ImGui::SliderFloat("Wobble level lift", &c.wobbleLevelDb, 0.0f, 6.0f, "%.1f dB");
    ImGui::SliderFloat("Wet level", &c.wetDb, -24.0f, 6.0f, "%.1f dB");
    ImGui::TreePop();
  }
}

// Reset and Print cover every effect's tuning.
void drawTuningButtons(ProtoParams& params) {
  cv::HarmonyTuning& ht = params.pitchFx.harmony.tuning;
  cv::OctaveTuning& ot = params.pitchFx.octave.tuning;
  if (ImGui::Button("Reset to defaults")) {
    ht = cv::HarmonyTuning{};
    ot = cv::OctaveTuning{};
    params.unison.tuning = cv::UnisonTuning{};
    params.slapback.tuning = cv::SlapbackTuning{};
    params.distortion.tuning = cv::DistortionTuning{};
    params.reverb.spring.tuning = cv::SpringTuning{};
    params.reverb.chasm.tuning = cv::ChasmTuning{};
    params.eq.tuning = cv::PolishTuning{};
  }
  ImGui::SameLine();
  if (ImGui::Button("Print tuning")) {
    printTuning(ht, ot, params.unison.tuning, params.slapback.tuning,
                params.distortion.tuning, params.reverb.spring.tuning,
                params.reverb.chasm.tuning, params.eq);
    // One line for the field; the clipboard keeps the line breaks.
    std::snprintf(gTuningLine, sizeof(gTuningLine), "%s", gTuningText);
    std::replace(gTuningLine, gTuningLine + sizeof(gTuningLine), '\n', ' ');
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##tuning", "printed tuning, also copied to clipboard", gTuningLine,
                           sizeof(gTuningLine), ImGuiInputTextFlags_ReadOnly);
}

void drawTransportRow(ProtoParams& params, float& meterDb, bool probe) {
  if (ImGui::Button("Load loop") && !probe) {
    const std::string path = openFilePanel();
    if (!path.empty()) loadLoop(path);
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(gLoopPath.empty() ? "(no loop)" : baseName(gLoopPath));
  ImGui::SameLine();
  if (ImGui::Button(params.playing ? "Stop" : "Play")) params.playing = !params.playing;
  ImGui::SameLine();

  const float frac = std::clamp((meterDb - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
  char label[32];
  std::snprintf(label, sizeof(label), "%.1f dBFS", meterDb);
  ImGui::ProgressBar(frac, ImVec2(kMeterW, 0.0f), label);
  ImGui::SameLine();
  drawTuningButtons(params);
}

// ---- Harmony menu: five voice rows, at most two non-off ----------------------

constexpr int kVoiceRows = 5;
constexpr int kMaxActiveVoices = 2;

struct HarmonyMenu {
  std::array<int, kVoiceRows> level{};      // 0 off, 1 low, 2 med, 3 high
  std::array<int, kMaxActiveVoices> order{};  // active rows, earliest first
  int activeCount = 0;
};
HarmonyMenu gMenu;  // UI-thread-only

void deactivate(HarmonyMenu& m, int row) {
  m.level[row] = 0;
  for (int i = 0; i < m.activeCount; ++i) {
    if (m.order[i] != row) continue;
    for (int k = i; k + 1 < m.activeCount; ++k) m.order[k] = m.order[k + 1];
    --m.activeCount;
    return;
  }
}

// Sets a row's level; a third active row turns the earliest one off.
void setRowLevel(HarmonyMenu& m, int row, int level) {
  if (level == 0) {
    deactivate(m, row);
    return;
  }
  if (m.level[row] == 0) {
    if (m.activeCount == kMaxActiveVoices) deactivate(m, m.order[0]);
    m.order[m.activeCount++] = row;
  }
  m.level[row] = level;
}

void syncSlots(const HarmonyMenu& m, cv::HarmonyParams& h) {
  for (int i = 0; i < kMaxActiveVoices; ++i) {
    h.slots[i] = cv::HarmonySlot{};
    if (i < m.activeCount) {
      h.slots[i].voice = static_cast<cv::HarmonyVoice>(m.order[i]);
      h.slots[i].level = m.level[m.order[i]];
    }
  }
}

void drawHarmonyMenu(cv::HarmonyParams& h) {
  if (!ImGui::CollapsingHeader("Menu", ImGuiTreeNodeFlags_DefaultOpen)) return;
  static const char* const kRowName[kVoiceRows] = {"Lower", "Low", "Fixed", "High", "Higher"};
  static const char* const kLevelName[4] = {"Off", "Low", "Med", "High"};
  for (int row = 0; row < kVoiceRows; ++row) {
    ImGui::PushID(row);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kRowName[row]);
    for (int level = 0; level < 4; ++level) {
      ImGui::SameLine(level == 0 ? 80.0f : 0.0f);
      int shown = gMenu.level[row];
      if (ImGui::RadioButton(kLevelName[level], &shown, level)) setRowLevel(gMenu, row, level);
    }
    ImGui::PopID();
  }
  syncSlots(gMenu, h);
}

// Panel knob shown in percent.
void percentSlider(const char* label, float& value) {
  float percent = value * 100.0f;
  if (ImGui::SliderFloat(label, &percent, 0.0f, 100.0f, "%.0f %%")) value = percent / 100.0f;
}

void drawHarmonyBlock(cv::HarmonyParams& h, const ProtoState& state) {
  ImGui::PushID("harmony");
  ImGui::Checkbox("HARMONY", &h.on);
  ImGui::Combo("KEY", &h.key, cv::kKeyName, 12);
  percentSlider("MIX", h.mix);
  drawHarmonyMenu(h);
  drawHarmonyTuning(h.tuning);
  char detected[64];
  formatDetected(state.pitchHz, state.voiced, detected, sizeof(detected));
  ImGui::TextUnformatted(detected);
  ImGui::PopID();
}

void drawOctaveBlock(cv::OctaveParams& o) {
  ImGui::PushID("octave");
  ImGui::Checkbox("OCTAVE", &o.on);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Engine");
  ImGui::SameLine();
  ImGui::RadioButton("A", &o.engine, 0);
  ImGui::SameLine();
  ImGui::RadioButton("B", &o.engine, 1);
  ImGui::SliderInt("SEMITONES", &o.semitones, -12, 12, "%+d st");
  ImGui::BeginDisabled(o.engine == 0);
  int formant = static_cast<int>(std::lround(o.formant));
  if (ImGui::SliderInt("FORMANT", &formant, -12, 12, "%+d st")) o.formant = static_cast<float>(formant);
  ImGui::EndDisabled();
  percentSlider("MIX", o.mix);
  drawOctaveTuning(o.tuning);
  ImGui::PopID();
}

void drawUnisonBlock(cv::UnisonParams& u) {
  ImGui::PushID("unison");
  ImGui::Checkbox("UNISON", &u.on);
  percentSlider("DEPTH", u.depth);
  drawUnisonTuning(u.tuning);
  ImGui::PopID();
}

void drawSlapbackBlock(cv::SlapbackParams& s) {
  ImGui::PushID("slapback");
  ImGui::Checkbox("SLAPBACK", &s.on);
  percentSlider("INTENSITY", s.intensity);
  drawSlapbackTuning(s.tuning);
  ImGui::PopID();
}

void drawDistortionBlock(cv::DistortionParams& d) {
  ImGui::PushID("distortion");
  ImGui::Checkbox("DISTORTION", &d.on);
  percentSlider("DRIVE", d.drive);
  percentSlider("TONE", d.tone);
  drawDistortionTuning(d.tuning);
  ImGui::PopID();
}

void drawReverbBlock(cv::ReverbParams& r) {
  ImGui::PushID("reverb");
  ImGui::Checkbox("REVERB", &r.on);
  ImGui::RadioButton("SPRING", &r.engine, cv::kReverbSpring);
  ImGui::SameLine();
  ImGui::RadioButton("CHASM", &r.engine, cv::kReverbChasm);
  if (r.engine == cv::kReverbChasm) {
    percentSlider("DECAY", r.chasm.decay);
    percentSlider("WOBBLE", r.chasm.wobble);
  } else {
    percentSlider("TENSION", r.spring.tension);
    percentSlider("DWELL", r.spring.dwell);
  }
  percentSlider("MIX", r.mix);
  drawReverbTuning(r.spring.tuning, r.chasm.tuning);
  ImGui::PopID();
}

void drawEqBlock(cv::PolishParams& e) {
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
  ImGui::PushID("eq");
  ImGui::Checkbox("EQ", &e.on);
  ImGui::SliderFloat("HPF", &e.hpHz, 40.0f, 200.0f, "%.0f Hz", log);
  ImGui::SliderFloat("LOW-MID FREQ", &e.dipHz, 150.0f, 600.0f, "%.0f Hz", log);
  ImGui::SliderFloat("LOW-MID GAIN", &e.dipDb, -6.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("PRESENCE FREQ", &e.presenceHz, 2000.0f, 6000.0f, "%.0f Hz", log);
  ImGui::SliderFloat("PRESENCE GAIN", &e.presenceDb, 0.0f, 6.0f, "%.1f dB");
  ImGui::SliderFloat("AIR", &e.airDb, 0.0f, 4.0f, "%.1f dB");
  drawPolishTuning(e.tuning);
  ImGui::PopID();
}

constexpr float kModulePad = 14.0f;  // inner padding of a module box, px
constexpr float kModuleGap = 40.0f;  // vertical gap between module boxes, px
constexpr float kColumnGap = 32.0f;  // horizontal gap between columns, px
constexpr float kTopGap = 16.0f;     // gap between the transport row and the columns, px

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

constexpr int kColumns = 3;
std::array<float, kColumns> gColumnUsed{};  // content height per column this frame, px
float gColumnAvail = 0.0f;                  // column height, px

// One face column. Columns never scroll; the probe fails on any overrun.
template <class F>
void column(int index, float width, const F& draw) {
  ImGui::PushID(index);
  ImGui::BeginChild("column", ImVec2(width, 0.0f), ImGuiChildFlags_None,
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

  // Signal order runs down each column, then left to right.
  const float colW = (ImGui::GetContentRegionAvail().x - kHarmonyColumnW - 2.0f * kColumnGap) / 2.0f;
  column(0, kHarmonyColumnW, [&] {
    moduleBox("harmonyBox", true, [&] { drawHarmonyBlock(params.pitchFx.harmony, state); });
    moduleBox("octaveBox", false, [&] { drawOctaveBlock(params.pitchFx.octave); });
  });
  ImGui::SameLine(0.0f, kColumnGap);
  column(1, colW, [&] {
    moduleBox("unisonBox", true, [&] { drawUnisonBlock(params.unison); });
    moduleBox("slapbackBox", false, [&] { drawSlapbackBlock(params.slapback); });
    moduleBox("distortionBox", false, [&] { drawDistortionBlock(params.distortion); });
  });
  ImGui::SameLine(0.0f, kColumnGap);
  column(2, colW, [&] {
    moduleBox("reverbBox", true, [&] { drawReverbBlock(params.reverb); });
    moduleBox("eqBox", false, [&] { drawEqBlock(params.eq); });
  });
  ImGui::End();
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
  io.Fonts->Build();

  // All headers closed, then each Tuning header open, then each sub-node open alone.
  static const ProbeOpen kScenarios[] = {
      {true, nullptr, nullptr},          {true, "harmony", nullptr},
      {true, "harmony", "Voices & tracking"},
      {true, "octave", nullptr},         {true, "unison", nullptr},
      {true, "slapback", nullptr},       {true, "distortion", nullptr},
      {true, "distortion", "Stage 1"},   {true, "distortion", "Tone stack"},
      {true, "distortion", "Stage 2"},   {true, "distortion", "Post"},
      {true, "reverb", nullptr},         {true, "reverb", "Tank"},
      {true, "reverb", "Splash"},        {true, "reverb", "Levels"},
      {true, "reverb", "Chasm"},
      {true, "eq", nullptr},
  };

  ProtoParams params;
  ProtoState state;
  float meterDb = kSilenceDb;
  bool fits = true;
  for (const ProbeOpen& s : kScenarios) {
    gProbeOpen = s;
    for (int frame = 0; frame < kProbeFrames; ++frame) {
      ImGui::NewFrame();
      drawFrame(params, state, meterDb, true);
      ImGui::Render();
    }
    char name[48];
    std::snprintf(name, sizeof(name), "%s%s%s", s.block ? s.block : "all closed",
                  s.node ? " / " : "", s.node ? s.node : "");
    const float worst = *std::max_element(gColumnUsed.begin(), gColumnUsed.end());
    const bool ok = worst <= gColumnAvail;
    fits = fits && ok;
    std::printf("%-30s col1 %4.0f  col2 %4.0f  col3 %4.0f  of %.0f px%s\n", name, gColumnUsed[0],
                gColumnUsed[1], gColumnUsed[2], gColumnAvail, ok ? "" : "  OVERRUN");
  }
  gProbeOpen = ProbeOpen{};
  ImGui::DestroyContext();

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
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 150");

  ProtoParams params;
  gParamsBuf[0] = params;
  gParamsBuf[1] = params;
  float meterDb = kSilenceDb;

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

    ImGui::Render();
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

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
