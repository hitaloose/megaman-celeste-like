/* Mega Man (NES) for handhelds: 5x7 text and icons on the 320x240 canvas. */
#ifndef MM_UI_H
#define MM_UI_H
#include <stdint.h>

/* Text at (x, y), `scale` pixels per font pixel, 6*scale per character,
 * with a 1-pixel black drop shadow. Characters: A-Z 0-9 space < > : - / ? */
void ui_text(uint32_t* canvas, int x, int y, const char* s, uint32_t ink, int scale);
int  ui_text_w(const char* s, int scale);
/* 7x7 skull icon. */
void ui_skull(uint32_t* canvas, int x, int y, uint32_t ink, int scale);
void ui_fill(uint32_t* canvas, int x0, int y0, int x1, int y1, uint32_t c);
/* Translucent black box with a light 1-pixel frame. */
void ui_box(uint32_t* canvas, int x0, int y0, int x1, int y1);

#endif
