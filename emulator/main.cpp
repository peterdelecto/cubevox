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
#include "engine/unison.h"

namespace {

constexpr int kWindowW = 900;
constexpr int kWindowH = 520;
constexpr int kProbeFrames = 4;
constexpr float kMeterFloorDb = -60.0f;
constexpr float kSilenceDb = -120.0f;
constexpr float kMeterDecayDbPerFrame = 0.6f;

struct ProtoParams {
  bool playing = false;
  cv::UnisonParams unison;
};

struct ProtoState {
  float peakDb = kSilenceDb;
  float playheadNorm = 0.0f;
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
cv::Unison gUnison;
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
    gUnison.reset();
  }
  if (!params.playing || loop == nullptr || loop->samples.empty()) {
    std::memset(out, 0, sizeof(float) * 2 * frameCount);
    publishState(0.0f, loop);
    return;
  }

  std::array<float, cv::kBlock> in;
  std::array<float, cv::kBlock> mono;
  float peak = 0.0f;
  ma_uint32 done = 0;
  while (done < frameCount) {
    const int n = static_cast<int>(std::min<ma_uint32>(cv::kBlock, frameCount - done));
    readLoop(*loop, in.data(), n);
    gUnison.process(in.data(), mono.data(), n, params.unison);
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

char gTuningText[256];  // last Print tuning output, shown on the face

// Finder launches have no stdout, so the line also goes to the clipboard
// and into a read-only field under the button.
void printTuning(const cv::UnisonTuning& t) {
  std::snprintf(
      gTuningText, sizeof(gTuningText),
      "UnisonTuning{{%.1ff, %.1ff}, {%.2ff, %.2ff}, %.1ff, %.1ff, %.1ff, {%.1ff, %.1ff}, %.0ff}",
      t.baseDelayMs[0], t.baseDelayMs[1], t.lfoHz[0], t.lfoHz[1], t.swingMinMs,
      t.swingMaxMs, t.wetMaxDb, t.detuneCents[0], t.detuneCents[1], t.windowMs);
  std::printf(
      "// {baseDelayMs[0], baseDelayMs[1]}, {lfoHz[0], lfoHz[1]}, swingMinMs, swingMaxMs, "
      "wetMaxDb, {detuneCents[0], detuneCents[1]}, windowMs\n%s\n", gTuningText);
  std::fflush(stdout);
  ImGui::SetClipboardText(gTuningText);
}

void drawTuning(cv::UnisonTuning& t) {
  if (!ImGui::CollapsingHeader("Tuning")) return;
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
  if (ImGui::Button("Reset to defaults")) t = cv::UnisonTuning{};
  ImGui::SameLine();
  if (ImGui::Button("Print tuning")) printTuning(t);
  if (gTuningText[0] != '\0') {
    ImGui::SameLine();
    ImGui::TextUnformatted("copied to clipboard");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##tuning", gTuningText, sizeof(gTuningText), ImGuiInputTextFlags_ReadOnly);
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

void drawPanelMirror(cv::UnisonParams& u) {
  ImGui::Checkbox("UNISON", &u.on);
  float percent = u.depth * 100.0f;
  if (ImGui::SliderFloat("DEPTH", &percent, 0.0f, 100.0f, "%.0f %%")) {
    u.depth = percent / 100.0f;
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
  drawPanelMirror(params.unison);
  ImGui::Separator();
  drawTuning(params.unison.tuning);
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
