/* Mega Man (NES) for handhelds: the game layer (checkpoints, deaths, weapons, HUD, options). */
#ifndef MM_GAME_H
#define MM_GAME_H
#include <stdint.h>

void     game_init(void);
/* Once per display frame: the player's pad (`pressed` = went down since the last tick). */
void     game_tick(uint32_t held, uint32_t pressed);
/* How many NES frames to run this display frame: 1 normally, 0 while the
 * options screen is open, more while a weapon switch runs hidden. */
int      game_frames_this_tick(void);
/* Before each NES frame: the pad the NES sees. */
uint32_t game_input(void);
/* After each NES frame. */
void     game_after_frame(void);
/* Draws HUD and menus on the 320x240 overlay canvas (transparent elsewhere). */
void     game_draw(uint32_t* canvas);
/* The 256x240 picture to show (live, or frozen while a menu is open). */
const uint32_t* game_picture(void);
/* Where the picture goes on a screen of sw x sh (ASPECT option). */
void     game_present_rect(int sw, int sh, int* x, int* y, int* w, int* h);
/* FILTER option: true = SMOOTH (sharp-bilinear), false = CRISP (nearest). */
int      game_smooth(void);
void     game_shutdown(void);

#endif
