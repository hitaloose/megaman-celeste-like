/* CRT overlays (scanlines, screen frame) drawn over the whole 640x480 screen. */
#ifndef MM_OVERLAY_H
#define MM_OVERLAY_H
#include <stdint.h>

#define OVERLAY_W 640
#define OVERLAY_H 480

int             overlay_count(void);
const char*     overlay_name(int i);
/* Overlay i as OVERLAY_W x OVERLAY_H ARGB8888 pixels (decoded the first time,
 * then kept), or NULL if it can't be decoded. */
const uint32_t* overlay_pixels(int i);
/* Overlay i fitted to a picture w pixels wide (w <= OVERLAY_W), OVERLAY_H tall:
 * columns cut from the middle, where the pattern only repeats, so the frame's
 * rounded corners land on the picture's edges and the scanline/mask pattern
 * keeps its 1:1 pixels (scaling it would make moire). Valid until the next call. */
const uint32_t* overlay_fit(int i, int w);

#endif
