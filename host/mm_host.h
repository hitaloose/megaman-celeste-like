/* Mega Man (NES) for handhelds: the libretro host and what the game layer uses from it. */
#ifndef MM_HOST_H
#define MM_HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Canvas the whole screen is drawn on: the NES picture in the middle,
 * HUD columns at both sides. Scaled 2x to the 640x480 panel. */
#define CANVAS_W 320
#define CANVAS_H 240
#define NES_X    32          /* left edge of the NES picture on the canvas */

/* Pad bits, libretro joypad ids, plus MENU. */
enum {
    PAD_B = 1 << 0, PAD_Y = 1 << 1, PAD_SELECT = 1 << 2, PAD_START = 1 << 3,
    PAD_UP = 1 << 4, PAD_DOWN = 1 << 5, PAD_LEFT = 1 << 6, PAD_RIGHT = 1 << 7,
    PAD_A = 1 << 8, PAD_X = 1 << 9, PAD_L = 1 << 10, PAD_R = 1 << 11,
    PAD_MENU = 1 << 16,
};
#define PAD_NES_MASK (PAD_B | PAD_SELECT | PAD_START | PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_A)

/* The NES's 2 KiB of work RAM. */
uint8_t* host_ram(void);
/* Whole-machine snapshots (libretro serialize). */
size_t   host_state_size(void);
bool     host_state_save(void* buf, size_t size);
bool     host_state_load(const void* buf, size_t size);
/* Directory for the port's own files (progress, settings). */
const char* host_save_dir(void);
/* Frames run since start. */
uint32_t host_frame(void);
void     host_request_quit(void);
/* Presses the console's RESET: back to the title screen. */
void     host_reset(void);
/* Drops the core's audio while on (hidden menu work). */
void     host_mute(bool on);
/* Sets a core option (the value must stay valid); the core picks it up on its next frame. */
void     host_set_option(const char* key, const char* value);
/* Runs one frame and puts the machine back, so the picture shows new options. */
void     host_refresh_picture(void);
/* The latest 256x240 NES picture (ARGB). */
const uint32_t* host_nes_frame(void);

#endif
