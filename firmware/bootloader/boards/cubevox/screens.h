// The update-mode screens of docs/specs/2026-10-24-field-update-design.md (section OLED in update
// mode), drawn into a frame buffer with no HAL, so tools/proof.c renders the same bytes the panel
// shows.

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

typedef enum {
  // Waiting, from the menu or MENU held: the installed image is valid and the timeout may run.
  CvScreenWaitMenu,
  // Waiting, no valid image.
  CvScreenWaitNoImage,
  // Waiting, three failed boots.
  CvScreenWaitBootFailed,
  CvScreenWriting,
  CvScreenWrongFile,
  CvScreenDone
} CvScreen;

// Clears the frame and draws `screen`. `installed` is the installed release name for the
// "Installed" line. `connected` puts "Connected." on the bottom row of a waiting screen; on the
// menu screen it replaces the cancel line, because a connected host stops the timeout.
void cvScreenDraw(uint8_t frame[CV_FRAME_BYTES], CvScreen screen, const char* installed, bool connected);

#ifdef __cplusplus
}
#endif
