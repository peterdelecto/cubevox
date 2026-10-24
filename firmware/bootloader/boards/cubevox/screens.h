// The update-mode screens of docs/specs/2026-10-24-field-update-design.md (section OLED in update
// mode), drawn into a frame buffer with no HAL, so tools/proof.c renders the same bytes the panel
// shows. Owner rules: every line is a full sentence in the 5x7 font, a screen holds four lines on
// rows 0, 2, 4 and 6 of the panel's eight 8 px rows, and the file extension never appears because
// the font blurs it.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 128 x 64 pixels in SH1106 page layout: 8 pages of 128 bytes, bit 0 the top pixel of its page.
#define CV_FRAME_WIDTH  128
#define CV_FRAME_HEIGHT 64
#define CV_FRAME_BYTES  1024u

// Line i of a screen sits on row 2 * i, and a line holds 21 characters.
#define CV_SCREEN_LINES      4u
#define CV_SCREEN_LINE_CHARS 21u
#define CV_SCREEN_LINE_BYTES 48u

typedef enum {
  // Waiting, from the menu or MENU held: the installed image is valid and the timeout may run.
  CvScreenWaitMenu,
  // Waiting, no valid image.
  CvScreenWaitNoImage,
  // Waiting, three failed boots.
  CvScreenWaitBootFailed,
  CvScreenWriting,
  CvScreenWrongFile,
  CvScreenDone,
  // The cable came out after a computer had connected, and update mode cannot time out.
  CvScreenCableOut,
  // The crystal or PLL1 did not start; USB runs from the internal oscillator.
  CvScreenClockFailed
} CvScreen;

// The text of a screen as it is drawn, one string per line, "" for a blank line.
typedef struct {
  char line[CV_SCREEN_LINES][CV_SCREEN_LINE_BYTES];
} CvScreenText;

// Composes the lines of `screen`. `installed` is the installed release name for the "You have"
// line, cut so the line keeps its period inside 21 characters. `connected` puts "Computer
// connected." on the last line of a waiting screen; on the menu screen it replaces the cancel line,
// because a connected host stops the timeout. The fixed wording is not cut here, so a check can see
// a line that is too long.
void cvScreenCompose(CvScreen screen, const char* installed, bool connected, CvScreenText* out);

// Clears the frame and draws `screen` as composed above, each line cut at 21 characters.
void cvScreenDraw(uint8_t frame[CV_FRAME_BYTES], CvScreen screen, const char* installed, bool connected);

#ifdef __cplusplus
}
#endif
