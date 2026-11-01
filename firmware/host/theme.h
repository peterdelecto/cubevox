// Window tokens. Every colour, size, gap, font size, radius and duration the emulator window uses
// lives here, derived from one base unit and a few base colours. Dark mode only.

#pragma once

#include <cstdint>

#include "core/canvas.h"
#include "core/slots.h"
#include "imgui.h"

namespace theme {

// ---- Base unit and the scale built on it

constexpr float kUnit = 8.0f;

constexpr float units(float k) { return k * kUnit; }
constexpr float half(float v) { return v / 2.0f; }

constexpr float kPad = units(3);       // window edge to content
constexpr float kGap = units(2);       // between sibling controls and columns
constexpr float kGapTight = units(1);  // label to control, row to row inside a column
constexpr float kGapWide = units(3);   // band to band

// ---- Window

constexpr int kWindowW = 1440;
constexpr int kWindowH = 840;
constexpr const char* kWindowTitle = "cubevox";

// ---- Type. Body floor is 15 px, the support tier is the version line only.

constexpr float kFontName = 18.0f;     // a card's name over its column
constexpr float kFontLabel = 15.0f;    // knob labels, switch words, the CPU line
constexpr float kFontSupport = 13.0f;  // the version

// Tried in order; the first that loads is the window's font.
constexpr const char* kFontPaths[] = {
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/SFNS.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
};

// ---- Colour. Base colours first, then every surface and ink derived from them.

struct Rgb {
  float r, g, b;  // 0 to 255
};

constexpr Rgb mix(Rgb a, Rgb b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

constexpr uint32_t channel(float v) { return static_cast<uint32_t>(v + half(1.0f)); }

// 0xAABBGGRR, the order ImGui and the GL texture upload both read.
constexpr ImU32 pack(Rgb c) { return (0xFFu << 24) | (channel(c.b) << 16) | (channel(c.g) << 8) | channel(c.r); }

// 0xRRGGBB, for the contrast gate.
constexpr uint32_t hex(Rgb c) { return (channel(c.r) << 16) | (channel(c.g) << 8) | channel(c.b); }

constexpr Rgb kGround{9, 14, 18};      // window, near-neutral cool
constexpr Rgb kInk{217, 223, 229};     // text, never pure white
constexpr Rgb kBlack{0, 0, 0};
constexpr Rgb kAccentBase{112, 186, 255};  // the one accent
constexpr Rgb kGreenBase{91, 189, 116};    // on, stable
constexpr Rgb kRedBase{255, 155, 147};     // over budget, stable

// Share of the ink mixed into the ground for each step of the lightness ladder.
constexpr float kShadePanel = 0.043f;
constexpr float kShadeCtrl = 0.11f;
constexpr float kShadeBorder = 0.18f;
constexpr float kShadeHover = 0.34f;
constexpr float kShadeDim = 0.84f;
constexpr float kShadeDisabled = 0.45f;
constexpr float kShadeAccentSoft = 0.30f;
constexpr float kShadeOled = 0.60f;  // share of black mixed into the ground

constexpr Rgb kBgRgb = kGround;
constexpr Rgb kPanelRgb = mix(kGround, kInk, kShadePanel);
constexpr Rgb kCtrlRgb = mix(kGround, kInk, kShadeCtrl);
constexpr Rgb kBorderRgb = mix(kGround, kInk, kShadeBorder);
constexpr Rgb kHoverRgb = mix(kGround, kInk, kShadeHover);
constexpr Rgb kTextRgb = kInk;
constexpr Rgb kDimRgb = mix(kGround, kInk, kShadeDim);
constexpr Rgb kDisabledRgb = mix(kGround, kInk, kShadeDisabled);
constexpr Rgb kAccentRgb = kAccentBase;
constexpr Rgb kAccentInkRgb = kGround;  // text on an accent fill
constexpr Rgb kAccentSoftRgb = mix(kGround, kAccentBase, kShadeAccentSoft);
constexpr Rgb kOkRgb = kGreenBase;
constexpr Rgb kDangerRgb = kRedBase;
constexpr Rgb kOledOffRgb = mix(kGround, kBlack, kShadeOled);
constexpr Rgb kOledOnRgb = kInk;

constexpr ImU32 kBg = pack(kBgRgb);
constexpr ImU32 kPanel = pack(kPanelRgb);
constexpr ImU32 kCtrl = pack(kCtrlRgb);
constexpr ImU32 kBorder = pack(kBorderRgb);
constexpr ImU32 kHover = pack(kHoverRgb);
constexpr ImU32 kText = pack(kTextRgb);
constexpr ImU32 kDim = pack(kDimRgb);
constexpr ImU32 kDisabled = pack(kDisabledRgb);
constexpr ImU32 kAccent = pack(kAccentRgb);
constexpr ImU32 kAccentInk = pack(kAccentInkRgb);
constexpr ImU32 kAccentSoft = pack(kAccentSoftRgb);
constexpr ImU32 kOk = pack(kOkRgb);
constexpr ImU32 kDanger = pack(kDangerRgb);
constexpr ImU32 kOledOff = pack(kOledOffRgb);
constexpr ImU32 kOledOn = pack(kOledOnRgb);

// ---- Shape

constexpr float kRadiusSm = units(0.5f);
constexpr float kRadiusMd = units(1);
constexpr float kBorderW = 1.0f;
constexpr float kRimW = 2.0f;
constexpr int kArcSegments = 48;
constexpr int kCircleSegments = 48;

// ---- OLED

constexpr int kOledScale = 4;  // each panel pixel is 4 by 4 window pixels
constexpr float kOledW = static_cast<float>(kCanvasWidth * kOledScale);
constexpr float kOledH = static_cast<float>(kCanvasHeight * kOledScale);
constexpr float kBezel = units(1);

// ---- Knob

constexpr float kKnobD = units(10);          // body diameter
constexpr float kKnobOuter = units(1);       // room for the track and ticks beyond the body
constexpr float kKnobBoxD = kKnobD + 2 * kKnobOuter;
constexpr float kKnobTrackOffset = units(0.75f);  // track radius beyond the body radius
constexpr float kTrackW = 3.0f;
constexpr float kTickIn = units(0.375f);
constexpr float kTickOut = units(0.875f);
constexpr float kTickW = 2.0f;
constexpr float kIndicatorIn = 0.25f;   // indicator line from this share of the radius
constexpr float kIndicatorOut = 0.90f;  // to this share
constexpr float kIndicatorW = 3.0f;
constexpr float kKnobStartPi = 0.75f;  // sweep begins here, in units of pi
constexpr float kKnobSweepPi = 1.5f;   // and runs this far
constexpr float kPi = 3.14159265f;
constexpr float kKnobMin = 0.0f;
constexpr float kKnobMax = 1.0f;

// ---- Knob and encoder gestures

constexpr float kKnobDragPx = units(25);       // vertical drag for the full travel
constexpr float kKnobWheelStep = 0.02f;        // travel per scroll notch on a continuous knob
constexpr float kWheelDetentsPerNotch = 1.0f;  // a stepped knob moves one step per notch
constexpr float kEncoderDragPx = units(2);     // vertical drag per detent
constexpr float kClickSlopPx = units(0.5f);    // press and release within this is a click
constexpr double kClickFlashSec = 0.12;        // a key click shows pressed this long
constexpr float kEncoderMarkRad = kPi / 12.0f;  // the knob turns this far per detent
constexpr int kEncoderMarks = 12;               // grip marks round the MENU knob
constexpr float kEncoderMarkIn = 0.82f;         // marks start at this share of the radius
constexpr float kEncoderIndicatorIn = 0.45f;

// ---- Switches

constexpr float kSwitchW = units(9);
constexpr float kSwitchH = units(4);
constexpr float kThumbInset = units(0.5f);
constexpr float kSegH = units(4);
constexpr float kSegInset = 2.0f;

// ---- Slot column

constexpr float kColW = (kWindowW - 2 * kPad - (kSlotCount - 1) * kGap) / kSlotCount;
constexpr float kColPad = units(1);
constexpr float kColInnerW = kColW - 2 * kColPad;
constexpr float kNameH = units(3);
constexpr float kLabelH = units(3);
constexpr float kColH = kColPad + kNameH + kGap + 2 * (kKnobBoxD + kLabelH) + kGap + kSwitchH + kGapTight + kSegH + kColPad;

// ---- LED and CPU line

constexpr float kLedR = units(1);
constexpr float kLedRowH = units(3);
constexpr float kLedGap = units(1.5f);   // dot to text
constexpr float kCpuGapShort = units(0.5f);  // a figure to its unit
constexpr float kCpuGapLong = units(2);      // one reading to the next

// ---- Bands

constexpr float kTopY = kPad;
constexpr float kOledX = (kWindowW - kOledW) / 2;
constexpr float kRightX = kOledX + kOledW + kBezel + kGapWide;  // the MENU column
constexpr float kSlotY = kTopY + kOledH + kGapWide;
constexpr float kBottomY = kSlotY + kColH + kGapWide;
constexpr float kVersionH = units(2);
constexpr float kBottomEndY = kBottomY + kSegH + kGapTight + kVersionH;
constexpr float kSourceW = 2 * kSwitchW;

static_assert(kBottomEndY + kPad <= kWindowH, "the three bands must fit the window");

// ---- Proof

constexpr int kProofWarmFrames = 4;  // frames drawn before the capture, so glyphs and layout settle

}  // namespace theme
