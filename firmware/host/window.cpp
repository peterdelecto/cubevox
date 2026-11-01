#define IMGUI_DEFINE_MATH_OPERATORS

#include "window.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "core/costs.h"
#include "core/registry.h"
#include "core/version.h"
#include "imgui.h"
#include "theme.h"

namespace host {

namespace {

using namespace theme;

constexpr ImGuiWindowFlags kFaceFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;

constexpr size_t kTextBytes = 32;  // a label or a CPU word, with its NUL
constexpr size_t kRgbBytes = 3;
constexpr size_t kRgbaBytes = 4;

// What the widgets of one frame share.
struct Frame {
  Box& box;
  Panel& panel;
  Window::Encoder& encoder;
  ImDrawList* dl;
  unsigned oled;
  bool micAvailable;
};

// ---- Text

ImVec2 textSize(float size, const char* s) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, s); }

void drawText(ImDrawList* dl, ImVec2 topLeft, float size, ImU32 col, const char* s) {
  dl->AddText(ImGui::GetFont(), size, topLeft, col, s);
}

void drawTextCentered(ImDrawList* dl, ImVec2 center, float size, ImU32 col, const char* s) {
  drawText(dl, center - textSize(size, s) * half(1), size, col, s);
}

void upper(const char* in, char* out, size_t size) {
  size_t n = 0;
  for (; in[n] != '\0' && n + 1 < size; ++n) out[n] = static_cast<char>(std::toupper(static_cast<unsigned char>(in[n])));
  out[n] = '\0';
}

// ---- Knob

float knobAngle(float v) { return kPi * (kKnobStartPi + kKnobSweepPi * v); }

ImVec2 onCircle(ImVec2 center, float angle, float radius) {
  return center + ImVec2(std::cos(angle), std::sin(angle)) * radius;
}

// The step a position selects on a stepped knob.
int stepOf(float v, int steps) { return std::clamp(static_cast<int>(v * static_cast<float>(steps)), 0, steps - 1); }

// A rotary pot. Drag vertically or scroll to turn it. A stepped knob (steps > 0) shows a tick per
// step and moves one step per scroll notch; the pot itself is continuous, as on the box.
void knob(Frame& f, const char* id, ImVec2 center, float& v, int steps) {
  ImGuiIO& io = ImGui::GetIO();
  const float r = half(kKnobD);
  ImGui::SetCursorScreenPos(center - ImVec2(half(kKnobBoxD), half(kKnobBoxD)));
  ImGui::InvisibleButton(id, ImVec2(kKnobBoxD, kKnobBoxD));
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();

  float next = v;
  if (active) next -= io.MouseDelta.y / kKnobDragPx;
  if (hovered && steps == 0) next += io.MouseWheel * kKnobWheelStep;
  if (hovered && steps > 0) {
    float& notches = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0);
    notches += io.MouseWheel * kWheelDetentsPerNotch;
    const int whole = static_cast<int>(notches);
    notches -= static_cast<float>(whole);
    if (whole != 0) next = (static_cast<float>(std::clamp(stepOf(v, steps) + whole, 0, steps - 1)) + half(1)) / static_cast<float>(steps);
  }
  v = std::clamp(next, kKnobMin, kKnobMax);

  ImDrawList* dl = f.dl;
  const float trackR = r + kKnobTrackOffset;
  dl->PathArcTo(center, trackR, knobAngle(kKnobMin), knobAngle(kKnobMax), kArcSegments);
  dl->PathStroke(kBorder, ImDrawFlags_None, kTrackW);
  float shown = v;
  if (steps == 0) {
    if (v > kKnobMin) {
      dl->PathArcTo(center, trackR, knobAngle(kKnobMin), knobAngle(v), kArcSegments);
      dl->PathStroke(kAccent, ImDrawFlags_None, kTrackW);
    }
  } else {
    const int on = stepOf(v, steps);
    for (int i = 0; i < steps; ++i) {
      const float a = knobAngle(static_cast<float>(i) / static_cast<float>(steps - 1));
      dl->AddLine(onCircle(center, a, r + kTickIn), onCircle(center, a, r + kTickOut), i == on ? kAccent : kDim, kTickW);
    }
    shown = static_cast<float>(on) / static_cast<float>(steps - 1);
  }
  dl->AddCircleFilled(center, r, kCtrl, kCircleSegments);
  dl->AddCircle(center, r, active ? kAccent : hovered ? kHover : kBorder, kCircleSegments, kRimW);
  const float a = knobAngle(shown);
  dl->AddLine(onCircle(center, a, r * kIndicatorIn), onCircle(center, a, r * kIndicatorOut), kText, kIndicatorW);
}

// The MENU knob: an encoder with no end stops. Drag vertically or scroll to turn, click to press.
// Left and Right arrows turn it; Enter and Space press it.
void encoderKnob(Frame& f, ImVec2 center) {
  ImGuiIO& io = ImGui::GetIO();
  Window::Encoder& e = f.encoder;
  const float r = half(kKnobD);
  ImGui::SetCursorScreenPos(center - ImVec2(half(kKnobBoxD), half(kKnobBoxD)));
  ImGui::InvisibleButton("menu", ImVec2(kKnobBoxD, kKnobBoxD));
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  if (ImGui::IsItemActivated()) {
    e.drag = 0;
    e.travel = 0;
  }
  if (active) {
    e.travel += std::fabs(io.MouseDelta.x) + std::fabs(io.MouseDelta.y);
    e.drag -= io.MouseDelta.y / kEncoderDragPx;
  }
  if (hovered) e.drag += io.MouseWheel * kWheelDetentsPerNotch;
  int turns = static_cast<int>(e.drag);
  e.drag -= static_cast<float>(turns);
  if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ++turns;
  if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) --turns;

  bool click = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
               ImGui::IsKeyPressed(ImGuiKey_Space, false);
  if (ImGui::IsItemDeactivated() && hovered && e.travel < kClickSlopPx) click = true;
  if (click) e.flashUntil = ImGui::GetTime() + kClickFlashSec;
  e.angle += static_cast<float>(turns) * kEncoderMarkRad;
  f.panel.input.detents += turns;
  f.panel.input.click = f.panel.input.click || click;

  const bool pressed = active || ImGui::GetTime() < e.flashUntil;
  ImDrawList* dl = f.dl;
  dl->AddCircleFilled(center, r, pressed ? kAccentSoft : kCtrl, kCircleSegments);
  dl->AddCircle(center, r, pressed ? kAccent : hovered ? kHover : kBorder, kCircleSegments, kRimW);
  for (int i = 0; i < kEncoderMarks; ++i) {
    const float a = e.angle + kPi * 2 * static_cast<float>(i) / static_cast<float>(kEncoderMarks);
    dl->AddLine(onCircle(center, a, r * kEncoderMarkIn), onCircle(center, a, r - kTickW), kBorder, kTickW);
  }
  dl->AddLine(onCircle(center, e.angle, r * kEncoderIndicatorIn), onCircle(center, e.angle, r * kIndicatorOut), kText,
              kIndicatorW);
}

// ---- Switches

// A slide switch. On is the accent with a fill and the thumb at the right; off is dim with the
// thumb at the left. Returns true when it was clicked.
bool slideSwitch(Frame& f, const char* id, ImVec2 topLeft, bool& on) {
  ImGui::SetCursorScreenPos(topLeft);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(kSwitchW, kSwitchH));
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  if (clicked) on = !on;

  ImDrawList* dl = f.dl;
  const ImVec2 bottomRight = topLeft + ImVec2(kSwitchW, kSwitchH);
  const float rr = half(kSwitchH);
  dl->AddRectFilled(topLeft, bottomRight, on ? kAccent : kCtrl, rr);
  dl->AddRect(topLeft, bottomRight, on ? kAccent : active ? kAccent : hovered ? kHover : kBorder, rr, ImDrawFlags_None,
              kRimW);
  const float thumbX = on ? bottomRight.x - rr : topLeft.x + rr;
  dl->AddCircleFilled(ImVec2(thumbX, topLeft.y + rr), rr - kThumbInset, on ? kAccentInk : kDim, kCircleSegments);
  const float wordX = on ? topLeft.x + half(kSwitchW - kSwitchH) : bottomRight.x - half(kSwitchW - kSwitchH);
  drawTextCentered(dl, ImVec2(wordX, topLeft.y + rr), kFontLabel, on ? kAccentInk : kDim, on ? "ON" : "OFF");
  return clicked;
}

// A two-position switch drawn as two words, the active one on an accent fill. A click anywhere on
// it asks for the other position; the caller decides what that means. A disabled one is dimmed and
// asks for nothing.
bool segmentedSwitch(Frame& f, const char* id, ImVec2 topLeft, float width, const char* left, const char* right,
                     bool rightOn, bool enabled) {
  ImGui::SetCursorScreenPos(topLeft);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, kSegH)) && enabled;
  const bool hovered = ImGui::IsItemHovered() && enabled;
  const bool active = ImGui::IsItemActive() && enabled;

  ImDrawList* dl = f.dl;
  const ImVec2 bottomRight = topLeft + ImVec2(width, kSegH);
  const float midX = topLeft.x + half(width);
  const ImVec2 inset(kSegInset, kSegInset);
  dl->AddRectFilled(topLeft, bottomRight, kCtrl, kRadiusMd);
  if (rightOn) {
    dl->AddRectFilled(ImVec2(midX, topLeft.y + kSegInset), bottomRight - inset, enabled ? kAccent : kAccentSoft,
                      kRadiusMd - kSegInset, ImDrawFlags_RoundCornersRight);
  } else {
    dl->AddRectFilled(topLeft + inset, ImVec2(midX, bottomRight.y - kSegInset), enabled ? kAccent : kAccentSoft,
                      kRadiusMd - kSegInset, ImDrawFlags_RoundCornersLeft);
  }
  dl->AddRect(topLeft, bottomRight, active ? kAccent : hovered ? kHover : kBorder, kRadiusMd, ImDrawFlags_None, kBorderW);
  const float midY = topLeft.y + half(kSegH);
  const ImU32 onInk = enabled ? kAccentInk : kDisabled;
  const ImU32 offInk = enabled ? kDim : kDisabled;
  drawTextCentered(dl, ImVec2(topLeft.x + half(half(width)), midY), kFontLabel, rightOn ? offInk : onInk, left);
  drawTextCentered(dl, ImVec2(midX + half(half(width)), midY), kFontLabel, rightOn ? onInk : offInk, right);
  return clicked;
}

// ---- Bands

void drawLed(Frame& f, ImVec2 center, bool lit) {
  f.dl->AddCircleFilled(center, kLedR, lit ? kOk : kCtrl, kCircleSegments);
  f.dl->AddCircle(center, kLedR, lit ? kOk : kBorder, kCircleSegments, kBorderW);
}

// CPU  67 % mean   76 % worst   lines 75 / 95, from the cost table. A figure over its line is red.
void drawCpuLine(Frame& f, float x, float centerY) {
  const LayoutCost cost = f.box.cost();
  char avg[kTextBytes];
  char worst[kTextBytes];
  char lines[kTextBytes];
  std::snprintf(avg, sizeof avg, "%d %%", cost.avg);
  std::snprintf(worst, sizeof worst, "%d %%", cost.worst);
  std::snprintf(lines, sizeof lines, "lines %d / %d", static_cast<int>(costs::kBudgetAvgPercent),
                static_cast<int>(costs::kBudgetWorstPercent));
  struct Word {
    const char* text;
    ImU32 color;
    float gapAfter;
  };
  const Word words[] = {
      {"CPU", kText, kCpuGapLong},
      {avg, cost.avg > costs::kBudgetAvgPercent ? kDanger : kText, kCpuGapShort},
      {"mean", kDim, kCpuGapLong},
      {worst, cost.worst > costs::kBudgetWorstPercent ? kDanger : kText, kCpuGapShort},
      {"worst", kDim, kCpuGapLong},
      {lines, kDim, 0},
  };
  for (const Word& w : words) {
    const ImVec2 size = textSize(kFontLabel, w.text);
    drawText(f.dl, ImVec2(x, centerY - half(size.y)), kFontLabel, w.color, w.text);
    x += size.x + w.gapAfter;
  }
}

void drawTopBand(Frame& f) {
  ImDrawList* dl = f.dl;
  const float midY = kTopY + half(kOledH);

  // MUTE, at the box's top left.
  const float muteTop = midY - half(kLabelH + kSwitchH);
  drawTextCentered(dl, ImVec2(kPad + half(kSwitchW), muteTop + half(kLabelH)), kFontLabel, kText, "MUTE");
  slideSwitch(f, "mute", ImVec2(kPad, muteTop + kLabelH), f.panel.input.mute);

  // The OLED at 4x, nearest neighbour, on its bezel.
  const ImVec2 oledMin(kOledX, kTopY);
  const ImVec2 oledMax = oledMin + ImVec2(kOledW, kOledH);
  dl->AddRectFilled(oledMin - ImVec2(kBezel, kBezel), oledMax + ImVec2(kBezel, kBezel), kOledOff, kRadiusSm);
  dl->AddRect(oledMin - ImVec2(kBezel, kBezel), oledMax + ImVec2(kBezel, kBezel), kBorder, kRadiusSm, ImDrawFlags_None,
              kBorderW);
  dl->AddImage(ImTextureRef(static_cast<ImTextureID>(f.oled)), oledMin, oledMax);

  // The MENU knob over the LED and the CPU line.
  const float menuTop = midY - half(kKnobBoxD + kLabelH + kGap + kLedRowH);
  const float knobX = kRightX + half(kKnobBoxD);
  encoderKnob(f, ImVec2(knobX, menuTop + half(kKnobBoxD)));
  drawTextCentered(dl, ImVec2(knobX, menuTop + kKnobBoxD + half(kLabelH)), kFontLabel, kText, "MENU");
  const float rowY = menuTop + kKnobBoxD + kLabelH + kGap + half(kLedRowH);
  drawLed(f, ImVec2(kRightX + kLedR, rowY), f.box.led());
  drawCpuLine(f, kRightX + 2 * kLedR + kLedGap, rowY);
}

// The switch under a slot's bypass toggle, which follows the layout: the column that holds the
// Unison or MOB card, and the column that holds a reverb.
void drawSwapSwitch(Frame& f, int slot, ImVec2 topLeft) {
  const Layout& layout = f.box.layout();
  if (slot == unisonCrowdSlot(layout)) {
    const bool mob = layout.card[static_cast<size_t>(slot)] == Card::Crowd;
    if (segmentedSwitch(f, "swapUnison", topLeft, kColInnerW, "UNISON", "MOB", mob, true)) f.box.swapUnisonMob();
  } else if (slot == reverbSlot(layout)) {
    const bool chasm = layout.card[static_cast<size_t>(slot)] == Card::ReverbChasm;
    if (segmentedSwitch(f, "swapReverb", topLeft, kColInnerW, "SPRING", "CHASM", chasm, true)) f.box.swapReverb();
  }
}

void drawSlot(Frame& f, int slot) {
  ImDrawList* dl = f.dl;
  const float x = kPad + static_cast<float>(slot) * (kColW + kGap);
  const float y = kSlotY;
  const float centerX = x + half(kColW);
  const ImVec2 topLeft(x, y);
  const ImVec2 bottomRight(x + kColW, y + kColH);
  dl->AddRectFilled(topLeft, bottomRight, kPanel, kRadiusMd);
  dl->AddRect(topLeft, bottomRight, kBorder, kRadiusMd, ImDrawFlags_None, kBorderW);

  const Card card = f.box.layout().card[static_cast<size_t>(slot)];
  const CardDesc& desc = cardDesc(card);
  drawTextCentered(dl, ImVec2(centerX, y + kColPad + half(kNameH)), kFontName, card == Card::Empty ? kDim : kText,
                   desc.name);

  ImGui::PushID(slot);
  for (int k = 0; k < desc.knobCount; ++k) {
    const float boxTop = y + kColPad + kNameH + kGap + static_cast<float>(k) * (kKnobBoxD + kLabelH);
    float& v = f.panel.input.knob[static_cast<size_t>(slot * kKnobsPerCard + k)];
    knob(f, k == 0 ? "upper" : "lower", ImVec2(centerX, boxTop + half(kKnobBoxD)), v, knobSteps(card, k));
    char label[kTextBytes];
    upper(desc.knob[k].name, label, sizeof label);
    drawTextCentered(dl, ImVec2(centerX, boxTop + kKnobBoxD + half(kLabelH)), kFontLabel, kDim, label);
  }
  const float segTop = y + kColH - kColPad - kSegH;
  const float switchTop = segTop - kGapTight - kSwitchH;
  bool on = f.panel.input.toggle[static_cast<size_t>(slot)];
  slideSwitch(f, "bypass", ImVec2(centerX - half(kSwitchW), switchTop), on);
  f.panel.input.toggle[static_cast<size_t>(slot)] = on;
  drawSwapSwitch(f, slot, ImVec2(x + kColPad, segTop));
  ImGui::PopID();
}

void drawBottomBand(Frame& f) {
  const bool mic = f.panel.useMic && f.micAvailable;
  if (segmentedSwitch(f, "source", ImVec2(kPad, kBottomY), kSourceW, "CLIP", "MIC", mic, f.micAvailable)) {
    f.panel.useMic = !f.panel.useMic;
  }
  drawText(f.dl, ImVec2(kPad, kBottomY + kSegH + kGapTight), kFontSupport, kDim, kFirmwareVersion);
}

// Writes `rgba` (bottom row first, as GL reads it) as a top-down P6 PPM.
bool writePpm(const char* path, const std::vector<uint8_t>& rgba, int width, int height) {
  std::FILE* file = std::fopen(path, "wb");
  if (file == nullptr) return false;
  std::fprintf(file, "P6\n%d %d\n255\n", width, height);
  std::vector<uint8_t> row(static_cast<size_t>(width) * kRgbBytes);
  for (int y = height - 1; y >= 0; --y) {
    const uint8_t* src = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * kRgbaBytes;
    for (size_t x = 0; x < static_cast<size_t>(width); ++x) {
      for (size_t c = 0; c < kRgbBytes; ++c) row[x * kRgbBytes + c] = src[x * kRgbaBytes + c];
    }
    std::fwrite(row.data(), 1, row.size(), file);
  }
  return std::fclose(file) == 0;
}

void loadFont() {
  ImGuiIO& io = ImGui::GetIO();
  for (const char* path : kFontPaths) {
    if (access(path, R_OK) != 0) continue;
    if (io.Fonts->AddFontFromFileTTF(path, kFontLabel) != nullptr) return;
  }
  io.Fonts->AddFontDefault();
}

void applyStyle() {
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowPadding = ImVec2(0, 0);
  style.WindowBorderSize = 0;
  style.WindowRounding = 0;
  style.Colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(kBg);
  style.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(kText);
}

}  // namespace

Window::Window(bool visible) : offscreen_(!visible) {
  if (!glfwInit()) {
    std::fprintf(stderr, "[ERROR] window: glfwInit failed\n");
    return;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, visible ? GLFW_TRUE : GLFW_FALSE);  // the proof is 1:1
  window_ = glfwCreateWindow(kWindowW, kWindowH, kWindowTitle, nullptr, nullptr);
  if (window_ == nullptr) {
    std::fprintf(stderr, "[ERROR] window: glfwCreateWindow failed\n");
    glfwTerminate();
    return;
  }
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  loadFont();
  applyStyle();
  ImGui_ImplGlfw_InitForOpenGL(window_, true);
  ImGui_ImplOpenGL3_Init("#version 150");
  imguiUp_ = true;

  glGenTextures(1, &oledTexture_);
  glBindTexture(GL_TEXTURE_2D, oledTexture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kCanvasWidth, kCanvasHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
}

Window::~Window() {
  if (window_ == nullptr) return;
  glDeleteTextures(1, &oledTexture_);
  if (proofFbo_ != 0) glDeleteFramebuffers(1, &proofFbo_);
  if (proofTexture_ != 0) glDeleteTextures(1, &proofTexture_);
  if (imguiUp_) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
  }
  glfwDestroyWindow(window_);
  glfwTerminate();
}

bool Window::shouldClose() const { return window_ == nullptr || glfwWindowShouldClose(window_) != 0; }

void Window::begin() {
  glfwPollEvents();
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

void Window::draw(Box& box, Panel& panel, bool micAvailable) {
  panel.input.detents = 0;
  panel.input.click = false;
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  ImGui::Begin(kWindowTitle, nullptr, kFaceFlags);
  Frame frame{box, panel, encoder_, ImGui::GetWindowDrawList(), oledTexture_, micAvailable};
  drawTopBand(frame);
  for (int s = 0; s < kSlotCount; ++s) drawSlot(frame, s);
  drawBottomBand(frame);
  ImGui::End();
}

void Window::ensureProofTarget() {
  if (proofFbo_ != 0) return;
  glGenTextures(1, &proofTexture_);
  glBindTexture(GL_TEXTURE_2D, proofTexture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kWindowW, kWindowH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &proofFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, proofFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, proofTexture_, 0);
}

void Window::end(const Box& box) {
  const Pixels& pixels = box.pixels();
  for (size_t i = 0; i < pixels.size(); ++i) oledRgba_[i] = pixels[i] != 0 ? kOledOn : kOledOff;
  glBindTexture(GL_TEXTURE_2D, oledTexture_);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kCanvasWidth, kCanvasHeight, GL_RGBA, GL_UNSIGNED_BYTE, oledRgba_.data());

  ImGui::Render();
  int width = kWindowW;
  int height = kWindowH;
  if (offscreen_) {
    ensureProofTarget();
    glBindFramebuffer(GL_FRAMEBUFFER, proofFbo_);
  } else {
    glfwGetFramebufferSize(window_, &width, &height);
  }
  glViewport(0, 0, width, height);
  const ImVec4 clear = ImGui::ColorConvertU32ToFloat4(kBg);
  glClearColor(clear.x, clear.y, clear.z, clear.w);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  if (!offscreen_) glfwSwapBuffers(window_);
}

bool Window::saveProof(Box& box, Panel& panel, bool micAvailable, const char* path) {
  if (!offscreen_ || window_ == nullptr) return false;
  for (int i = 0; i < kProofWarmFrames; ++i) {
    begin();
    draw(box, panel, micAvailable);
    end(box);
  }
  std::vector<uint8_t> rgba(static_cast<size_t>(kWindowW) * static_cast<size_t>(kWindowH) * kRgbaBytes);
  glBindFramebuffer(GL_FRAMEBUFFER, proofFbo_);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, kWindowW, kWindowH, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  return writePpm(path, rgba, kWindowW, kWindowH);
}

}  // namespace host
