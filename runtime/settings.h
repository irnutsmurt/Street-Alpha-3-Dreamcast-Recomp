/* User settings, persisted in sfa3.cfg and editable from the game's
 * Option Mode > DISPLAY screen (and the F1 / Esc overlay). */
#pragma once

enum { WINDOW_WINDOWED, WINDOW_BORDERLESS, WINDOW_EXCLUSIVE };
enum { PICTURE_4_3, PICTURE_STRETCH, PICTURE_INTEGER };
enum { COLOR_STANDARD, COLOR_TV, COLOR_VIVID };

typedef struct {
    int window_mode;        /* WINDOW_* */
    int window_scale;       /* windowed size: 0 = fit the screen, N = N x 640x480 */
    int fs_width, fs_height, fs_refresh;  /* exclusive fullscreen mode (refresh in Hz) */
    int picture;            /* PICTURE_* */
    int color;              /* COLOR_*: colour response of the final picture */
    int scale;              /* internal render resolution multiplier (1 = 640x480) */
    int vsync;              /* 1 = wait for the display's refresh */
    int scanlines;          /* CRT look: 0 off, 1 scanlines, 2 PVM (Trinitron monitor) */
    int scanline_strength;  /* how strong the CRT look is, percent, 10..100 */
    int nearest;            /* 1 = sharp point-sampled textures */
    int skip_intro;         /* 1 = straight from power-on to the title (the save still loads) */
    /* cheats (cheats.h): fights against the CPU only */
    int cheat_health, cheat_super, cheat_cpu_ko, cheat_cpu_super;
    int cheat_final_bison;  /* 1 = unlocked (kept in the game's own save too) */
    char name[16];          /* netplay player name (NETWORK > NAME) */
} Settings;

#define NAME_MAX_LEN 10

extern Settings g_settings;
extern const Settings g_settings_default;

void settings_load(const char *path);
void settings_save(void);
void settings_set_name(const char *name);
