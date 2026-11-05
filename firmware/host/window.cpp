#define IMGUI_DEFINE_MATH_OPERATORS

#include "window.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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

// A bat toggle, standing on its bushing. The lever stands up for on, in the accent with a filled
// tip, and hangs down for off, dim with a hollow tip. A click anywhere on its hit area flips it.
// Returns true when it was clicked.
bool batToggle(Frame& f, const char* id, ImVec2 center, bool& on) {
  const ImVec2 size(kBatW, kBatH);
  const ImVec2 topLeft = center - size * half(1);
  ImGui::SetCursorScreenPos(topLeft);
  const bool clicked = ImGui::InvisibleButton(id, size);
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  if (clicked) on = !on;

  ImDrawList* dl = f.dl;
  if (hovered || active) dl->AddRect(topLeft, topLeft + size, active ? kAccent : kHover, kRadiusSm, ImDrawFlags_None, kBorderW);
  const ImVec2 bush(half(kBushW), half(kBushH));
  dl->AddRectFilled(center - bush, center + bush, kCtrl, kRadiusSm);
  dl->AddRect(center - bush, center + bush, kBorder, kRadiusSm, ImDrawFlags_None, kBorderW);
  const ImU32 ink = on ? kAccent : kOff;
  const ImVec2 tip = center + ImVec2(0, on ? -kLeverLen : kLeverLen);
  dl->AddLine(center, tip, ink, kLeverW);
  if (on) {
    dl->AddCircleFilled(tip, kTipR, ink, kCircleSegments);
  } else {
    dl->AddCircleFilled(tip, kTipR, kBg, kCircleSegments);
    dl->AddCircle(tip, kTipR, ink, kCircleSegments, kRimW);
  }
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

// One cell of a grid switch. `left` and `across` are shares of the switch's width, and `row` counts
// rows from 0 at the top.
struct GridCell {
  const char* word;
  float left;
  int row;
  float across;
};

// A switch of rows of words, each `rowH` tall, the active one on an accent fill, in the foot of a
// column. Words that do not fit side by side in the column at the label size take a row each: OCTAVE
// and FORMANT together are 136 of the column's 144 px, so OCTAVE, FORMANT and YAWN stack a row each, and
// CHATTER is wider than a third of it. A click on a cell returns its index, and -1 means no click.
template <size_t N>
int segmentedGrid(Frame& f, const char* id, ImVec2 topLeft, float width, float rowH,
                  const std::array<GridCell, N>& cells, int active) {
  const ImVec2 inset(kSegInset, kSegInset);
  ImDrawList* dl = f.dl;
  int rows = 1;
  for (const GridCell& c : cells) rows = c.row + 1 > rows ? c.row + 1 : rows;
  const ImVec2 bottomRight = topLeft + ImVec2(width, static_cast<float>(rows) * rowH);
  dl->AddRectFilled(topLeft, bottomRight, kCtrl, kRadiusMd);
  for (const GridCell& c : cells) {
    if (c.left > 0.0f) {
      const float x = topLeft.x + c.left * width;
      const float y = topLeft.y + static_cast<float>(c.row) * rowH;
      dl->AddLine(ImVec2(x, y), ImVec2(x, y + rowH), kBorder, kBorderW);
    }
  }
  for (int r = 1; r < rows; ++r) {
    const float y = topLeft.y + static_cast<float>(r) * rowH;
    dl->AddLine(ImVec2(topLeft.x, y), ImVec2(bottomRight.x, y), kBorder, kBorderW);
  }

  int clicked = -1;
  ImGui::PushID(id);
  for (size_t k = 0; k < N; ++k) {
    const int i = static_cast<int>(k);
    const ImVec2 cellMin = topLeft + ImVec2(cells[k].left * width, static_cast<float>(cells[k].row) * rowH);
    const ImVec2 cellSize(cells[k].across * width, rowH);
    ImGui::SetCursorScreenPos(cellMin);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("cell", cellSize)) clicked = i;
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemActive();
    ImGui::PopID();
    if (i == active) dl->AddRectFilled(cellMin + inset, cellMin + cellSize - inset, kAccent, kRadiusMd - kSegInset);
    if (hovered || pressed) {
      dl->AddRect(cellMin + inset, cellMin + cellSize - inset, pressed ? kAccent : kHover, kRadiusMd - kSegInset,
                  ImDrawFlags_None, kBorderW);
    }
    drawTextCentered(dl, cellMin + cellSize * half(1), kFontLabel, i == active ? kAccentInk : kDim, cells[k].word);
  }
  ImGui::PopID();
  dl->AddRect(topLeft, bottomRight, kBorder, kRadiusMd, ImDrawFlags_None, kBorderW);
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

  // MUTE, at the box's top left: its label over a bat toggle.
  const float muteTop = midY - half(kLabelH + kBatH);
  const float muteX = kPad + half(kMuteW);
  drawTextCentered(dl, ImVec2(muteX, muteTop + half(kLabelH)), kFontLabel, kText, "MUTE");
  batToggle(f, "mute", ImVec2(muteX, muteTop + kLabelH + half(kBatH)), f.panel.input.mute);

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

// The cells of the four grid switches, in the order the menu cycles their cards. The slot 7 switch
// has GATE and LO-FI side by side over CHATTER, the slot 4 switch UNISON and MOB side by side, the slot
// 6 switch DISTORTION over THROAT, and the slot 3 switch stacks OCTAVE, FORMANT and YAWN.
constexpr std::array<GridCell, 3> kSlot7Cells = {{{"GATE", 0.0f, 0, 0.5f}, {"LO-FI", 0.5f, 0, 0.5f}, {"CHATTER", 0.0f, 1, 1.0f}}};
constexpr std::array<Card, 3> kSlot7Cards = {Card::Gate, Card::Lofi, Card::Bloom};
constexpr std::array<GridCell, 2> kSlot4Cells = {{{"UNISON", 0.0f, 0, 0.5f}, {"MOB", 0.5f, 0, 0.5f}}};
constexpr std::array<Card, 2> kSlot4Cards = {Card::Unison, Card::Crowd};
constexpr std::array<GridCell, kStackRows> kSlot6Cells = {
    {{"DISTORTION", 0.0f, 0, 1.0f}, {"THROAT", 0.0f, 1, 1.0f}, {"RING MOD", 0.0f, 2, 1.0f}}};
constexpr std::array<Card, kStackRows> kSlot6Cards = {Card::Distortion, Card::Growl, Card::RingMod};
constexpr std::array<GridCell, 3> kSlot8Cells = {{{"SPRING", 0.0f, 0, 0.5f}, {"CHASM", 0.5f, 0, 0.5f}, {"SYMPATH", 0.0f, 1, 1.0f}}};
constexpr std::array<Card, 3> kSlot8Cards = {Card::ReverbSpring, Card::ReverbChasm, Card::Strings};
constexpr std::array<GridCell, kStackRows> kSlot3Cells = {
    {{"OCTAVE", 0.0f, 0, 1.0f}, {"FORMANT", 0.0f, 1, 1.0f}, {"YAWN", 0.0f, 2, 1.0f}}};
constexpr std::array<Card, kStackRows> kSlot3Cards = {Card::Octave, Card::Formant, Card::FormantFollower};

// The cell of `cards` that `c` sits in, 0 when it sits in none.
template <size_t N>
int activeCell(const std::array<Card, N>& cards, Card c) {
  for (size_t i = 0; i < N; ++i) {
    if (cards[i] == c) return static_cast<int>(i);
  }
  return 0;
}

// The card switch at the foot of a slot, which follows the layout: the column that holds Octave,
// Formant or YAWN, the column that holds Unison or MOB, the column that holds
// Distortion or Growl, the column that holds a reverb, and the column that holds Gate, Lo-fi or
// Chatter. All but the reverb's are grids, and a click on a grid picks its card through the menu's own
// action. The slot 3 stack is taller than the foot, so it rises into the gap above it and ends on the
// foot's bottom edge.
void drawSwapSwitch(Frame& f, int slot, ImVec2 topLeft) {
  const Layout& layout = f.box.layout();
  const Card card = layout.card[static_cast<size_t>(slot)];
  if (slot == octaveFormantSlot(layout)) {
    const ImVec2 stackTop = topLeft + ImVec2(0.0f, kGridH - kStackH);
    const int picked =
        segmentedGrid(f, "slot3Card", stackTop, kColInnerW, kStackRowH, kSlot3Cells, activeCell(kSlot3Cards, card));
    if (picked >= 0) f.box.pickSlot3Card(kSlot3Cards[static_cast<size_t>(picked)]);
  } else if (slot == distortionGrowlSlot(layout)) {
    const ImVec2 stackTop = topLeft + ImVec2(0.0f, kGridH - kStackH);
    const int picked =
        segmentedGrid(f, "slot6Card", stackTop, kColInnerW, kStackRowH, kSlot6Cells, activeCell(kSlot6Cards, card));
    if (picked >= 0) f.box.pickSlot6Card(kSlot6Cards[static_cast<size_t>(picked)]);
  } else if (slot == unisonCrowdSlot(layout)) {
    const int picked =
        segmentedGrid(f, "slot4Card", topLeft, kColInnerW, kSegH, kSlot4Cells, activeCell(kSlot4Cards, card));
    if (picked >= 0) f.box.pickSlot4Card(kSlot4Cards[static_cast<size_t>(picked)]);
  } else if (slot == reverbSlot(layout)) {
    const int picked =
        segmentedGrid(f, "slot8Card", topLeft, kColInnerW, kSegH, kSlot8Cells, activeCell(kSlot8Cards, card));
    if (picked >= 0) f.box.pickSlot8Card(kSlot8Cards[static_cast<size_t>(picked)]);
  } else if (slot == slot7CardSlot(layout)) {
    const int picked =
        segmentedGrid(f, "slot7Card", topLeft, kColInnerW, kSegH, kSlot7Cells, activeCell(kSlot7Cards, card));
    if (picked >= 0) f.box.pickSlot7Card(kSlot7Cards[static_cast<size_t>(picked)]);
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
  drawTextCentered(dl, ImVec2(centerX, kNameY + half(kNameH)), kFontName, card == Card::Empty ? kDim : kText,
                   desc.name);

  ImGui::PushID(slot);
  for (int k = 0; k < desc.knobCount; ++k) {
    const float boxTop = y + kColPad + static_cast<float>(k) * (kKnobBoxD + kLabelH);
    float& v = f.panel.input.knob[static_cast<size_t>(slot * kKnobsPerCard + k)];
    knob(f, k == 0 ? "upper" : "lower", ImVec2(centerX, boxTop + half(kKnobBoxD)), v, knobSteps(card, k));
    char label[kTextBytes];
    upper(desc.knob[k].name, label, sizeof label);
    drawTextCentered(dl, ImVec2(centerX, boxTop + kKnobBoxD + half(kLabelH)), kFontLabel, kDim, label);
  }
  drawSwapSwitch(f, slot, ImVec2(x + kColPad, y + kColH - kColPad - kGridH));
  ImGui::PopID();
}

// The eight bypass toggles, each centred over its column.
void drawToggleRow(Frame& f) {
  for (int s = 0; s < kSlotCount; ++s) {
    const float centerX = kPad + static_cast<float>(s) * (kColW + kGap) + half(kColW);
    ImGui::PushID(s);
    batToggle(f, "bypass", ImVec2(centerX, kToggleY + half(kBatH)), f.panel.input.toggle[static_cast<size_t>(s)]);
    ImGui::PopID();
  }
}

void drawBottomBand(Frame& f) {
  const bool mic = f.panel.useMic && f.micAvailable;
  if (segmentedSwitch(f, "source", ImVec2(kPad, kBottomY), kSourceW, "CLIP", "MIC", mic, f.micAvailable)) {
    f.panel.useMic = !f.panel.useMic;
  }
  const ImVec2 version = textSize(kFontSupport, kFirmwareVersion);
  drawText(f.dl, ImVec2(kVersionX, kBottomY + half(kSegH) - half(version.y)), kFontSupport, kDim, kFirmwareVersion);
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

void Window::setClip(const char* name) {
  if (window_ == nullptr) return;
  std::string title = kWindowTitle;
  if (name != nullptr && name[0] != '\0') title += std::string(kWindowTitleSep) + name;
  glfwSetWindowTitle(window_, title.c_str());
}

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
  drawToggleRow(frame);
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
  const ImU32 lit = oledLit(box.contrast());
  for (size_t i = 0; i < pixels.size(); ++i) oledRgba_[i] = pixels[i] != 0 ? lit : kOledOff;
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
