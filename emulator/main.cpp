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
#include "engine/unison.h"

namespace {

constexpr int kWindowW = 900;
constexpr int kWindowH = 910;
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

char gTuningText[2048];  // last Print tuning output, shown on the face

// Finder launches have no stdout, so the text also goes to the clipboard
// and into a read-only field under the button.
void printTuning(const cv::HarmonyTuning& h, const cv::OctaveTuning& o,
                 const cv::UnisonTuning& t, const cv::SlapbackTuning& s,
                 const cv::DistortionTuning& d) {
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
      "OctaveTuning{%.1ff, %.1ff, %s}\n"
      "UnisonTuning{{%.1ff, %.1ff}, {%.2ff, %.2ff}, %.1ff, %.1ff, %.1ff, {%.1ff, %.1ff}, %.0ff}\n"
      "SlapbackTuning{%.1ff, %.0ff, %.2ff, %.1ff}\n"
      "DistortionTuning{%.0ff, %.0ff, %.1ff, %.0ff, %.1ff, %.0ff, %.1ff, %.0ff, %.1ff, %.1ff, "
      "%.0ff, %.0ff, %.1ff, %.2ff, %.2ff, %.0ff, %.1ff, %.1ff, %.0ff, %.1ff, %.2ff, %.1ff, %.2ff, %s}",
      line, o.levelDb, o.glideMs, o.muteUnvoiced ? "true" : "false", t.baseDelayMs[0], t.baseDelayMs[1], t.lfoHz[0], t.lfoHz[1], t.swingMinMs,
      t.swingMaxMs, t.wetMaxDb, t.detuneCents[0], t.detuneCents[1], t.windowMs,
      s.timeMs, s.lowpassHz, s.feedback, s.wetMaxDb, d.inputHpHz, d.s1BassHz, d.s1BassDb, d.s1LpHz, d.gain1Max, d.stackBassHz, d.stackBassDb,
      d.stackTrebleHz, d.stackTrebleDb, d.stackLossDb, d.s2HpHz, d.s2LpHz, d.gain2Max,
      d.railAsym, d.railSoft, d.trebleCutHz, d.trebleCutDb, d.toneDb, d.bassPeakHz,
      d.bassPeakDb, d.bassPeakQ, d.trimDb, d.fadeDrive, d.oversample ? "true" : "false");
  std::printf(
      "// HarmonyTuning: {levelDb[3]}, glideMs, voicedThreshold, muteUnvoiced, snapToScale, "
      "lower, low, high, higher\n"
      "// OctaveTuning: levelDb, glideMs, muteUnvoiced\n"
      "// UnisonTuning: {baseDelayMs[0], baseDelayMs[1]}, {lfoHz[0], lfoHz[1]}, swingMinMs, "
      "swingMaxMs, wetMaxDb, {detuneCents[0], detuneCents[1]}, windowMs\n"
      "// SlapbackTuning: timeMs, lowpassHz, feedback, wetMaxDb\n"
      "// DistortionTuning: inputHpHz, s1BassHz, s1BassDb, s1LpHz, gain1Max, stackBassHz, "
      "stackBassDb, stackTrebleHz, stackTrebleDb, stackLossDb, s2HpHz, s2LpHz, gain2Max, "
      "railAsym, railSoft, trebleCutHz, trebleCutDb, toneDb, bassPeakHz, bassPeakDb, "
      "bassPeakQ, trimDb, fadeDrive, oversample\n%s\n",
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

// One 7-entry interval table: semitones per scale degree, Do..Ti.
void drawIntervalRow(const char* name, int8_t (&row)[7]) {
  static const char* const kDegree[7] = {"Do", "Re", "Mi", "Fa", "Sol", "La", "Ti"};
  ImGui::PushID(name);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(name);
  for (int d = 0; d < 7; ++d) {
    ImGui::SameLine(d == 0 ? 80.0f : 0.0f);
    ImGui::SetNextItemWidth(44.0f);
    int v = row[d];
    if (ImGui::InputInt(kDegree[d], &v, 0, 0)) row[d] = static_cast<int8_t>(std::clamp(v, -12, 12));
  }
  ImGui::PopID();
}

void drawHarmonyTuning(cv::HarmonyTuning& h, const ProtoState& state) {
  if (!ImGui::TreeNode("Harmony")) return;
  char detected[64];
  formatDetected(state.pitchHz, state.voiced, detected, sizeof(detected));
  ImGui::TextUnformatted(detected);
  ImGui::SliderFloat("Level low", &h.levelDb[0], -24.0f, 6.0f, "%.1f dB");
  ImGui::SliderFloat("Level medium", &h.levelDb[1], -24.0f, 6.0f, "%.1f dB");
  ImGui::SliderFloat("Level high", &h.levelDb[2], -24.0f, 6.0f, "%.1f dB");
  ImGui::SliderFloat("Tracking speed (ms)", &h.glideMs, 0.0f, 100.0f, "%.0f ms");
  ImGui::SliderFloat("Voiced threshold", &h.voicedThreshold, 0.05f, 0.4f, "%.2f");
  ImGui::Checkbox("Mute unvoiced", &h.muteUnvoiced);
  ImGui::Checkbox("Snap to scale", &h.snapToScale);
  drawIntervalRow("Lower", h.lower);
  drawIntervalRow("Low", h.low);
  drawIntervalRow("High", h.high);
  drawIntervalRow("Higher", h.higher);
  ImGui::TreePop();
}

void drawOctaveTuning(cv::OctaveTuning& o) {
  if (!ImGui::TreeNode("Octave")) return;
  ImGui::SliderFloat("Level", &o.levelDb, -24.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("Tracking speed (ms)", &o.glideMs, 0.0f, 100.0f, "%.0f ms");
  ImGui::Checkbox("Mute unvoiced", &o.muteUnvoiced);
  ImGui::TreePop();
}

void drawUnisonTuning(cv::UnisonTuning& t) {
  if (!ImGui::TreeNode("Unison")) return;
  ImGui::SliderFloat("Base delay 1", &t.baseDelayMs[0], 5.0f, 40.0f, "%.1f ms");
  ImGui::SliderFloat("Base delay 2", &t.baseDelayMs[1], 5.0f, 40.0f, "%.1f ms");
  ImGui::SliderFloat("LFO rate 1", &t.lfoHz[0], 0.1f, 3.0f, "%.2f Hz");
  ImGui::SliderFloat("LFO rate 2", &t.lfoHz[1], 0.1f, 3.0f, "%.2f Hz");
  ImGui::SliderFloat("Swing at depth 0", &t.swingMinMs, 0.0f, 1.0f, "%.2f ms");
  ImGui::SliderFloat("Swing at depth 1", &t.swingMaxMs, 0.5f, 6.0f, "%.2f ms");
  ImGui::SliderFloat("Wet level at depth 1", &t.wetMaxDb, -24.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("DETUNE 0", &t.detuneCents[0], -30.0f, 30.0f, "%.1f cents");
  ImGui::SliderFloat("DETUNE 1", &t.detuneCents[1], -30.0f, 30.0f, "%.1f cents");
  ImGui::SliderFloat("WINDOW", &t.windowMs, 5.0f, 30.0f, "%.0f ms");
  ImGui::TreePop();
}

void drawSlapbackTuning(cv::SlapbackTuning& t) {
  if (!ImGui::TreeNode("Slapback")) return;
  ImGui::SliderFloat("Time", &t.timeMs, 30.0f, 120.0f, "%.0f ms");
  ImGui::SliderFloat("Lowpass", &t.lowpassHz, 500.0f, 12000.0f, "%.0f Hz",
                     ImGuiSliderFlags_Logarithmic);
  ImGui::SliderFloat("Feedback", &t.feedback, 0.0f, 0.5f, "%.2f");
  ImGui::SliderFloat("Wet level at full", &t.wetMaxDb, -24.0f, 0.0f, "%.1f dB");
  ImGui::TreePop();
}

void drawDistortionTuning(cv::DistortionTuning& t) {
  if (!ImGui::TreeNode("Distortion")) return;
  const ImGuiSliderFlags log = ImGuiSliderFlags_Logarithmic;
  ImGui::SliderFloat("Input high-pass", &t.inputHpHz, 10.0f, 200.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Stage 1 bass cut corner", &t.s1BassHz, 100.0f, 2000.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Stage 1 bass cut depth", &t.s1BassDb, -24.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("Stage 1 top roll-off", &t.s1LpHz, 1000.0f, 12000.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Stage 1 gain at full", &t.gain1Max, 1.0f, 300.0f, "%.1f x", log);
  ImGui::SliderFloat("Stack bass corner", &t.stackBassHz, 100.0f, 1000.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Stack bass boost", &t.stackBassDb, 0.0f, 20.0f, "%.1f dB");
  ImGui::SliderFloat("Stack treble corner", &t.stackTrebleHz, 500.0f, 8000.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Stack treble cut", &t.stackTrebleDb, -20.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("Stack insertion loss", &t.stackLossDb, -40.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("Stage 2 high-pass", &t.s2HpHz, 20.0f, 500.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Stage 2 low-pass", &t.s2LpHz, 1000.0f, 12000.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Stage 2 gain at full", &t.gain2Max, 1.0f, 300.0f, "%.1f x", log);
  ImGui::SliderFloat("Rail asymmetry", &t.railAsym, 0.0f, 0.5f, "%.2f");
  ImGui::SliderFloat("Rail soft edge", &t.railSoft, 0.01f, 0.5f, "%.2f");
  ImGui::SliderFloat("Treble cut corner", &t.trebleCutHz, 300.0f, 5000.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Treble cut", &t.trebleCutDb, -12.0f, 0.0f, "%.1f dB");
  ImGui::SliderFloat("Tone", &t.toneDb, -9.0f, 9.0f, "%.1f dB");
  ImGui::SliderFloat("Bass peak centre", &t.bassPeakHz, 60.0f, 400.0f, "%.0f Hz", log);
  ImGui::SliderFloat("Bass peak gain", &t.bassPeakDb, 0.0f, 12.0f, "%.1f dB");
  ImGui::SliderFloat("Bass peak Q", &t.bassPeakQ, 0.3f, 3.0f, "%.2f");
  ImGui::SliderFloat("Output trim", &t.trimDb, -12.0f, 12.0f, "%.1f dB");
  ImGui::SliderFloat("Fade-in span", &t.fadeDrive, 0.01f, 0.3f, "%.2f");
  ImGui::Checkbox("2x oversampling", &t.oversample);
  ImGui::TreePop();
}

void drawTuning(ProtoParams& params, const ProtoState& state) {
  if (!ImGui::CollapsingHeader("Tuning")) return;
  cv::HarmonyTuning& ht = params.pitchFx.harmony.tuning;
  cv::OctaveTuning& ot = params.pitchFx.octave.tuning;
  drawHarmonyTuning(ht, state);
  drawOctaveTuning(ot);
  drawUnisonTuning(params.unison.tuning);
  drawSlapbackTuning(params.slapback.tuning);
  drawDistortionTuning(params.distortion.tuning);
  if (ImGui::Button("Reset to defaults")) {
    ht = cv::HarmonyTuning{};
    ot = cv::OctaveTuning{};
    params.unison.tuning = cv::UnisonTuning{};
    params.slapback.tuning = cv::SlapbackTuning{};
    params.distortion.tuning = cv::DistortionTuning{};
  }
  ImGui::SameLine();
  if (ImGui::Button("Print tuning"))
    printTuning(ht, ot, params.unison.tuning, params.slapback.tuning,
                params.distortion.tuning);
  if (gTuningText[0] != '\0') {
    ImGui::SameLine();
    ImGui::TextUnformatted("copied to clipboard");
    ImGui::InputTextMultiline("##tuning", gTuningText, sizeof(gTuningText),
                              ImVec2(-1.0f, ImGui::GetTextLineHeight() * 5.0f),
                              ImGuiInputTextFlags_ReadOnly);
  }
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
  ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), label);
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

void drawPanelMirror(ProtoParams& params) {
  cv::HarmonyParams& h = params.pitchFx.harmony;
  cv::OctaveParams& o = params.pitchFx.octave;
  cv::UnisonParams& u = params.unison;
  ImGui::Checkbox("HARMONY", &h.on);
  ImGui::Combo("KEY", &h.key, cv::kKeyName, 12);
  float mixPercent = h.mix * 100.0f;
  if (ImGui::SliderFloat("MIX", &mixPercent, 0.0f, 100.0f, "%.0f %%")) {
    h.mix = mixPercent / 100.0f;
  }
  drawHarmonyMenu(h);
  ImGui::Separator();
  ImGui::Checkbox("OCTAVE", &o.on);
  ImGui::SliderInt("SEMITONES", &o.semitones, -12, 12, "%+d st");
  float octMixPercent = o.mix * 100.0f;
  if (ImGui::SliderFloat("MIX##octave", &octMixPercent, 0.0f, 100.0f, "%.0f %%")) {
    o.mix = octMixPercent / 100.0f;
  }
  ImGui::Separator();
  ImGui::Checkbox("UNISON", &u.on);
  float percent = u.depth * 100.0f;
  if (ImGui::SliderFloat("DEPTH", &percent, 0.0f, 100.0f, "%.0f %%")) {
    u.depth = percent / 100.0f;
  }
  ImGui::Separator();
  ImGui::Checkbox("SLAPBACK", &params.slapback.on);
  float slapPercent = params.slapback.intensity * 100.0f;
  if (ImGui::SliderFloat("INTENSITY", &slapPercent, 0.0f, 100.0f, "%.0f %%")) {
    params.slapback.intensity = slapPercent / 100.0f;
  }
  ImGui::Separator();
  ImGui::Checkbox("DISTORTION", &params.distortion.on);
  float drivePercent = params.distortion.drive * 100.0f;
  if (ImGui::SliderFloat("DRIVE", &drivePercent, 0.0f, 100.0f, "%.0f %%")) {
    params.distortion.drive = drivePercent / 100.0f;
  }
}

void drawFrame(ProtoParams& params, const ProtoState& state, float& meterDb, bool probe) {
  // Hold-and-decay so short peaks stay readable at 60 fps.
  meterDb = std::max(state.peakDb, meterDb - kMeterDecayDbPerFrame);

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::Begin("cubevox", nullptr, flags);
  drawTransportRow(params, meterDb, probe);
  ImGui::Separator();
  drawPanelMirror(params);
  ImGui::Separator();
  drawTuning(params, state);
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

  ProtoParams params;
  ProtoState state;
  float meterDb = kSilenceDb;
  for (int frame = 0; frame < kProbeFrames; ++frame) {
    ImGui::NewFrame();
    drawFrame(params, state, meterDb, true);
    ImGui::Render();
  }
  std::printf("layout %dx%d: ok\n", kWindowW, kWindowH);
  ImGui::DestroyContext();
  return 0;
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
