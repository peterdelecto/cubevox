// Menu font proof. Draws the menu list in Myrtle 7X12 through the firmware's own Canvas and fonts,
// in three candidates (A0, A1, B0), three frames each, and writes the frames as 1x PGMs with
// manifests. It also measures every row to decide which labels fit, and writes the audit.
// menu_font_sheet.py composes the PNG. Exploration only; the GUI spec decides the real menu.
//
// In bash, from the repo root. The third argument is the compile line, which the audit records.
//
//   CL="clang++ -std=c++17 -O1 -I firmware/cubevox/src -I tools/font-editor/fonts tools/oled-proof/menu_font_proof.cpp firmware/cubevox/src/core/canvas.cpp firmware/cubevox/src/core/fonts.cpp -o build/menu_font_proof"
//   mkdir -p build/menu-font-proof build/audit && $CL \
//   && build/menu_font_proof build/menu-font-proof build/audit/menu-font-proof.md "$CL" \
//   && python3 -I tools/oled-proof/menu_font_sheet.py build/menu-font-proof tools/oled-proof/menu-font-proof.png

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/canvas.h"
#include "core/fonts.h"

namespace {

// Ink columns 1..126 are the usable width (1 px pad on both edges).
constexpr int kLeftX = 1;
constexpr int kRightX = 126;
constexpr int kFitWidth = kRightX - kLeftX + 1;
constexpr int kMinGap = 3;
constexpr int kItemCount = 12;

struct Item {
  const char* full;
  const char* shortLabel;  // nullptr when the label has no shorter form
  std::vector<const char*> values;
};

// The menu list in firmware order, labels without the colon.
const std::vector<Item>& items() {
  static const std::vector<Item> list = {
      {"INPUT", nullptr, {"MIC", "LINE"}},
      {"OUTPUT", nullptr, {"LINE", "MIC"}},
      {"TOP SWITCH", "TOGGLE", {"MUTE", "BYPASS"}},
      {"REVERB", nullptr, {"SPRING", "CHASM", "NONE"}},
      {"POLISH COMP", "COMP", {"ON", "OFF"}},
      {"POLISH EQ", "EQ", {"ON", "OFF"}},
      {"DEBUG", nullptr, {"ON", "OFF"}},
      {"HPF", nullptr, {"OFF", "80", "100", "120", "150"}},
      {"LPF", nullptr, {"OFF", "4k", "5k", "6k", "8k", "10k", "12k"}},
      {"GATE LEARN >", nullptr, {}},
      {"UPDATE FIRMWARE >", "UPDATE >", {}},
      {"EXIT", nullptr, {}},
  };
  return list;
}

// What the panel shows in the three frames. Toggles read OFF, the widest value, throughout.
const char* const kShown[kItemCount] = {"MIC", "LINE", "BYPASS", "SPRING", "OFF", "OFF",
                                        "OFF", "120", "12k", "", "", ""};

struct Candidate {
  const char* id;
  const char* key;
  int pitch;
  int rows;
  int y0;
  bool rightAligned;
};

const Candidate kCandidates[] = {
    {"A0", "Myrtle 7X12, pitch 12 px, 5 rows visible, label left at x 1, value right-aligned to x 126", 12, 5, 2, true},
    {"A1", "Myrtle 7X12, pitch 13 px, 4 rows visible, label left at x 1, value right-aligned to x 126", 13, 4, 6, true},
    {"B0", "Myrtle 7X12, pitch 12 px, 5 rows visible, label and value joined by one space from x 1", 12, 5, 2, false},
};

const FontDesc& font() { return font7x12(); }

// The 7X12 font has no ">" cell. The proof stands in the 5x7 glyph for it, centred on the row.
struct Chevron {
  int left;
  int width;
  int top;
  int bottom;
  int index;
};

Chevron chevron() {
  const FontDesc& f = font5x7();
  Chevron c{99, 0, 99, -1, glyphIndex(f, '>')};
  const uint8_t* g = glyphAt(f, c.index);
  int right = -1;
  for (int col = 0; col < f.width; ++col) {
    if (!g[col]) continue;
    c.left = std::min(c.left, col);
    right = std::max(right, col);
    for (int r = 0; r < f.height; ++r) {
      if ((g[col] >> r) & 1) {
        c.top = std::min(c.top, r);
        c.bottom = std::max(c.bottom, r);
      }
    }
  }
  c.width = right - c.left + 1;
  return c;
}

bool hasChevron(const std::string& s) { return s.size() >= 2 && s.compare(s.size() - 2, 2, " >") == 0; }

// The text drawn in 7X12 for a label: the label itself, or its text up to and including the space
// before the chevron.
std::string baseOf(const std::string& s) { return hasChevron(s) ? s.substr(0, s.size() - 1) : s; }

int labelWidth(const std::string& s) {
  if (!hasChevron(s)) return Canvas::textWidth(font(), s.c_str());
  Canvas scratch;
  return scratch.drawText(0, 0, font(), baseOf(s).c_str()) + chevron().width;
}

int valueWidth(const char* v) { return Canvas::textWidth(font(), v); }

// Draws a label with its left ink at x and its cap top at y; returns the pen x after it.
int drawLabel(Canvas& c, int x, int y, const std::string& s) {
  const int pen = c.drawText(x, y, font(), baseOf(s).c_str());
  if (!hasChevron(s)) return pen;
  const Chevron ch = chevron();
  const FontDesc& f5 = font5x7();
  const uint8_t* g = glyphAt(f5, ch.index);
  const int yoff = (font().height - (ch.bottom - ch.top + 1)) / 2 - ch.top;
  for (int col = ch.left; col < ch.left + ch.width; ++col) {
    for (int r = 0; r < f5.height; ++r) {
      if ((g[col] >> r) & 1) c.setPixel(pen + col - ch.left, y + yoff + r, true);
    }
  }
  return pen + ch.width;
}

struct Fit {
  int labelW = 0;
  int widestValueW = 0;
  const char* widestValue = "";
  int rowA = 0;  // label, 3 px gap, widest value
  int rowB = 0;  // widest joined "LABEL VALUE" string
  bool fits = true;
};

Fit measure(const Item& it, const std::string& label) {
  Fit f;
  f.labelW = labelWidth(label);
  for (const char* v : it.values) {
    const int w = valueWidth(v);
    if (w > f.widestValueW) {
      f.widestValueW = w;
      f.widestValue = v;
    }
    f.rowB = std::max(f.rowB, Canvas::textWidth(font(), (label + " " + v).c_str()));
  }
  if (it.values.empty()) {
    f.rowA = f.rowB = f.labelW;
  } else {
    f.rowA = f.labelW + kMinGap + f.widestValueW;
  }
  f.fits = f.rowA <= kFitWidth && f.rowB <= kFitWidth;
  return f;
}

std::string frameValue(int i) { return kShown[i]; }

// The list scrolls like Menu::onTurn: the cursor stays in view, nothing more. Frame 2 gets there
// turning down from the top. Frame 3 turns down to LPF first and then up one to HPF, so both show.
int topFor(const Candidate& c, int frame) {
  const int downTo = frame == 1 ? 0 : (frame == 2 ? 10 : 8);
  int top = 0;
  for (int cur = 0; cur <= downTo; ++cur) {
    if (cur >= top + c.rows) top = cur - c.rows + 1;
  }
  return top;
}

int cursorFor(int frame) { return frame == 1 ? 0 : (frame == 2 ? 10 : 7); }

struct Rendered {
  Canvas canvas;
  std::string rowYs;
  std::string detail;
  int top = 0;
  int cursor = 0;
  std::string rows;
};

Rendered renderFrame(const Candidate& c, int frame, const std::vector<std::string>& labels) {
  Rendered out;
  out.top = topFor(c, frame);
  out.cursor = cursorFor(frame);
  int tightest = 999;
  int longestEnd = 0;
  for (int r = 0; r < c.rows; ++r) {
    const int i = out.top + r;
    if (i >= kItemCount) break;
    const int y = c.y0 + r * c.pitch;
    const std::string value = frameValue(i);
    if (r) out.rowYs += " ";
    out.rowYs += std::to_string(y);
    out.rows += (r ? "; " : "") + std::to_string(i + 1) + " " + labels[i] + (value.empty() ? "" : " " + value) + " at y " +
                std::to_string(y);
    if (c.rightAligned) {
      const int lastInk = kLeftX + labelWidth(labels[i]) - 1;
      drawLabel(out.canvas, kLeftX, y, labels[i]);
      if (!value.empty()) {
        const int vw = valueWidth(value.c_str());
        out.canvas.drawText(kRightX - vw + 1, y, font(), value.c_str());
        tightest = std::min(tightest, kRightX - vw + 1 - lastInk - 1);
      }
    } else {
      if (value.empty()) {
        drawLabel(out.canvas, kLeftX, y, labels[i]);
        longestEnd = std::max(longestEnd, kLeftX + labelWidth(labels[i]) - 1);
      } else {
        const std::string joined = labels[i] + " " + value;
        out.canvas.drawText(kLeftX, y, font(), joined.c_str());
        longestEnd = std::max(longestEnd, kLeftX + Canvas::textWidth(font(), joined.c_str()) - 1);
      }
    }
  }
  out.canvas.invertRect(0, c.y0 + (out.cursor - out.top) * c.pitch, kCanvasWidth, c.pitch);
  char buf[96];
  if (c.rightAligned) {
    std::snprintf(buf, sizeof(buf), "rows y %s, tightest label to value gap %d px", out.rowYs.c_str(), tightest);
  } else {
    std::snprintf(buf, sizeof(buf), "rows y %s, longest row ends at x %d", out.rowYs.c_str(), longestEnd);
  }
  out.detail = buf;
  return out;
}

bool lit(const Canvas& c, int x, int y) { return (c.page(y / 8)[x] >> (y % 8)) & 1; }

void writePgm(const std::string& path, const Canvas& c) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    std::exit(1);
  }
  std::fprintf(f, "P5\n%d %d\n255\n", kCanvasWidth, kCanvasHeight);
  for (int y = 0; y < kCanvasHeight; ++y) {
    for (int x = 0; x < kCanvasWidth; ++x) std::fputc(lit(c, x, y) ? 255 : 0, f);
  }
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: menu_font_proof <out dir> <audit path>\n");
    return 1;
  }
  const std::string dir = argv[1];
  FILE* audit = std::fopen(argv[2], "w");
  if (!audit) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }

  // 1. Measure every full label with its widest value, then shorten the ones that do not fit.
  std::vector<std::string> labels;
  std::vector<Fit> full, used;
  int shortened = 0;
  std::string shortenedList;
  for (const Item& it : items()) {
    const Fit f = measure(it, it.full);
    full.push_back(f);
    std::string label = it.full;
    if (!f.fits && it.shortLabel) {
      label = it.shortLabel;
      ++shortened;
      shortenedList += (shortenedList.empty() ? "" : ", ") + std::string(it.full) + " -> " + label;
    }
    labels.push_back(label);
    used.push_back(measure(it, label));
  }

  // 2. Render the frames and write the PGMs and manifests.
  FILE* frames = std::fopen((dir + "/menu-frames.txt").c_str(), "w");
  FILE* cands = std::fopen((dir + "/menu-candidates.txt").c_str(), "w");
  FILE* notes = std::fopen((dir + "/menu-notes.txt").c_str(), "w");
  if (!frames || !cands || !notes) {
    std::fprintf(stderr, "cannot write manifests in %s\n", dir.c_str());
    return 1;
  }
  std::string frameAudit;
  for (const Candidate& c : kCandidates) {
    std::fprintf(cands, "%s|%s\n", c.id, c.key);
    for (int n = 1; n <= 3; ++n) {
      const Rendered r = renderFrame(c, n, labels);
      writePgm(dir + "/menu-" + c.id + "-" + std::to_string(n) + ".pgm", r.canvas);
      const char* what = n == 1 ? "List top, cursor on INPUT"
                                : (n == 2 ? "Scrolled to the end, cursor on UPDATE" : "Cursor on HPF, HPF 120, LPF 12k");
      std::string caption = std::to_string(n) + ". " + what;
      if (n == 2) caption = "2. Scrolled to the end, cursor on " + labels[10];
      std::fprintf(frames, "%s|%d|%s|%s\n", c.id, n, caption.c_str(), r.detail.c_str());
      frameAudit += std::string("| ") + c.id + " | " + std::to_string(n) + " | " + std::to_string(r.top + 1) + " | " +
                    std::to_string(r.cursor + 1) + " | " + r.rows + " |\n";
    }
  }
  std::fprintf(notes, "Labels used in all three candidates: ");
  for (size_t i = 0; i < labels.size(); ++i) std::fprintf(notes, "%s%s", i ? ", " : "", labels[i].c_str());
  std::fprintf(notes, "\n");
  std::fprintf(notes, "Shortened to fit (%d rows): %s\n", shortened, shortenedList.empty() ? "none" : shortenedList.c_str());
  std::fprintf(notes,
               "Myrtle 7X12 has no > glyph, so the > on GATE LEARN and UPDATE is the 5x7 glyph centred on the row, as a "
               "stand-in. A lowercase k draws as K.\n");
  std::fprintf(notes,
               "Capitals fill all 12 rows of the glyph cell. At pitch 12 (A0, B0) rows touch and the cursor bar has no margin "
               "above or below the ink. At pitch 13 (A1) each row has 1 blank row under it.\n");
  std::fclose(frames);
  std::fclose(cands);
  std::fclose(notes);

  // 3. The audit.
  const Chevron ch = chevron();
  std::fprintf(audit, "# Menu font proof audit, 2026-10-30\n\n");
  std::fprintf(audit, "Sheet: `tools/oled-proof/menu-font-proof.png`. Source: `tools/oled-proof/menu_font_proof.cpp` and `menu_font_sheet.py`.\n\n");
  std::fprintf(audit, "## Finding: 7X12 has no > glyph\n\n");
  std::fprintf(audit,
               "1. The Myrtle 7X12 export inks `! \" # & ' ( ) , - . / 0-9 : A-Z b d m s t`. It has no `>` and no lowercase `k`.\n"
               "2. Canvas draws a missing glyph as a blank advance of 3 px, so a real menu in 7X12 would show GATE LEARN and UPDATE with no marker.\n"
               "3. The sheet stands in the 5x7 `>` (ink %d px wide, %d px tall) after one word space (5 px, a 2 px ink gap plus the 3 px space advance), centred on the 12 px cap height. The widths below include it.\n"
               "4. A lowercase `k` in `4k`..`12k` draws as `K` (the lowercase cell is blank, so Canvas maps it to the capital). Widths are measured as drawn.\n\n",
               ch.width, ch.bottom - ch.top + 1);

  std::fprintf(audit, "## Method\n\n");
  std::fprintf(audit,
               "1. Widths are `Canvas::textWidth(font7x12(), text)`: ink width, no trailing gap. Every glyph has a 2 px ink gap.\n"
               "2. Usable ink columns are x 1..126, so a row fits when it is 126 px or less.\n"
               "3. A0 and A1 fit test: label + 3 px + widest value <= 126 px. B0 fit test: the widest joined string `LABEL VALUE` <= 126 px (its space is 5 px). A label is shortened when either test fails, so all three candidates share one label set.\n"
               "4. Rows without a value fit when the label alone is <= 126 px.\n\n");

  std::fprintf(audit, "## Full labels (px, 7X12)\n\n");
  std::fprintf(audit, "| # | Full label | Label | Widest value | Value | A0/A1 row (label+3+value) | B0 row (joined) | Fits |\n");
  std::fprintf(audit, "|---|---|---|---|---|---|---|---|\n");
  for (int i = 0; i < kItemCount; ++i) {
    const Fit& f = full[i];
    std::fprintf(audit, "| %d | %s | %d | %s | %d | %d | %d | %s |\n", i + 1, items()[i].full, f.labelW,
                 items()[i].values.empty() ? "-" : f.widestValue, f.widestValueW, f.rowA, f.rowB, f.fits ? "yes" : "NO");
  }
  std::fprintf(audit, "\nLimit is 126 px.\n\n");

  std::fprintf(audit, "## Labels used\n\n");
  std::fprintf(audit, "Rows that needed a shorter label: **%d**.\n\n", shortened);
  if (shortened == 0) {
    std::fprintf(audit, "Every full label fits under both tests, so all three candidates use the full labels. The tightest rows are TOP SWITCH + BYPASS and UPDATE FIRMWARE >, both within 4 px of the limit at A0 and A1.\n\n");
  } else {
    std::fprintf(audit, "| # | Label on the sheet | Label | Widest value | Value | A0/A1 row | B0 row | Fits |\n");
    std::fprintf(audit, "|---|---|---|---|---|---|---|---|\n");
    for (int i = 0; i < kItemCount; ++i) {
      const Fit& f = used[i];
      std::fprintf(audit, "| %d | %s%s | %d | %s | %d | %d | %d | %s |\n", i + 1, labels[i].c_str(),
                   labels[i] == items()[i].full ? "" : " (shortened)", f.labelW,
                   items()[i].values.empty() ? "-" : f.widestValue, f.widestValueW, f.rowA, f.rowB, f.fits ? "yes" : "NO");
    }
    std::fprintf(audit, "\nShortened: %s.\n\n", shortenedList.c_str());
  }

  std::fprintf(audit, "## Row y per candidate\n\n");
  std::fprintf(audit, "Text top (the cap top of the 12 px glyph cell) sits at the row top, in all three. The cursor bar is the row's pitch tall, full width, x 0..127.\n\n");
  std::fprintf(audit, "| Candidate | Pitch | Rows visible | Row tops (y) | Bar of row 1 | Last ink row | Blank rows above and below |\n");
  std::fprintf(audit, "|---|---|---|---|---|---|---|\n");
  for (const Candidate& c : kCandidates) {
    std::string ys;
    for (int r = 0; r < c.rows; ++r) ys += (r ? ", " : "") + std::to_string(c.y0 + r * c.pitch);
    const int bottom = c.y0 + (c.rows - 1) * c.pitch + c.pitch - 1;
    std::fprintf(audit, "| %s | %d | %d | %s | y %d..%d | y %d | %d above, %d below |\n", c.id, c.pitch, c.rows, ys.c_str(),
                 c.y0, c.y0 + c.pitch - 1, c.y0 + (c.rows - 1) * c.pitch + 11, c.y0, kCanvasHeight - 1 - bottom);
  }
  std::fprintf(audit,
               "\nCapitals fill all 12 rows of the cell, so at pitch 12 (A0, B0) neighbouring rows touch with no blank line and the cursor bar has no ink margin. A1 adds 1 blank row under each row.\n\n");

  std::fprintf(audit, "## Frames\n\n");
  std::fprintf(audit, "Items are the list numbers 1..12 above. Frame 2 turns down from the top to item 11. Frame 3 turns down to item 9 (LPF) and back up one to item 8 (HPF), so HPF and LPF both show. Toggles read OFF throughout.\n\n");
  std::fprintf(audit, "| Candidate | Frame | Top item | Cursor item | Rows |\n|---|---|---|---|---|\n");
  std::fprintf(audit, "%s\n", frameAudit.c_str());

  std::fprintf(audit, "## Build\n\n");
  std::fprintf(audit, "`third_party/imgui` is empty in this worktree and was not copied; the proof compiles with the direct clang++ line and needs no cmake build.\n\n");
  std::fprintf(audit, "Compile line used:\n\n```\n%s\n```\n\n", argc > 3 ? argv[3] : "(not recorded)");
  std::fprintf(audit, "Run lines:\n\n```\nbuild/menu_font_proof build/menu-font-proof build/audit/menu-font-proof.md \"<compile line>\"\n");
  std::fprintf(audit, "python3 -I tools/oled-proof/menu_font_sheet.py build/menu-font-proof tools/oled-proof/menu-font-proof.png\n```\n");
  std::fclose(audit);
  return 0;
}
