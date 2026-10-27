/* The PC display options as a list of menu items, shared by the game's
 * Option Mode > DISPLAY screen and the F1 / Esc overlay. Changing an item
 * applies it immediately and saves sfa3.cfg. */
#pragma once
#include <stddef.h>

enum {
    OPT_WINDOW_MODE, OPT_WINDOW_SIZE, OPT_RESOLUTION, OPT_PICTURE, OPT_WIDESCREEN, OPT_COLOR, OPT_RENDER_RES, OPT_VSYNC,
    OPT_SCANLINES, OPT_SCANLINE_STRENGTH, OPT_TEXTURES, OPT_SKIP_INTRO, OPT_COUNT
};

const char *option_label(int i);
void option_value(int i, char *out, size_t n);  /* upper-case, game-font safe */
int option_enabled(int i);                      /* e.g. RESOLUTION only in exclusive mode */
void option_change(int i, int dir);             /* dir = -1 / +1 */
void options_reset(void);                       /* DEFAULT */
void options_apply_window(void);                /* push window settings to SDL */
