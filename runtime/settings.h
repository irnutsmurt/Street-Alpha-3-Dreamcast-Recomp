/* User settings, persisted in sfa3.cfg and editable from the game's
 * Option Mode > DISPLAY screen (and the F1 / Esc overlay). */
#pragma once

enum { WINDOW_WINDOWED, WINDOW_BORDERLESS, WINDOW_EXCLUSIVE };
enum { PICTURE_4_3 = 0, PICTURE_16_9 = 3 };  /* (1 and 2 were STRETCH and INTEGER before 0.2.8: now 4:3) */
enum { COLOR_STANDARD, COLOR_TV, COLOR_VIVID };

typedef struct {
    int window_mode;        /* WINDOW_* */
    int window_height;      /* windowed size: 0 = fit the screen, else its height (the width follows the picture's
                             * shape: 4:3, or 16:9; options.c) */
    int fs_width, fs_height, fs_refresh;  /* exclusive fullscreen mode (refresh in Hz) */
    int picture;            /* PICTURE_*: 16:9 makes fights wide (widescreen.c) */
    int color;              /* COLOR_*: colour response of the final picture */
    int render_height;      /* internal render resolution's height (the width follows the picture's shape) */
    int vsync;              /* 1 = wait for the display's refresh */
    int scanlines;          /* CRT look: 0 off, 1 scanlines, 2 PVM (Trinitron monitor) */
    int scanline_strength;  /* how strong the CRT look is, percent, 10..100 */
    int nearest;            /* 1 = sharp point-sampled textures */
    int skip_intro;         /* 1 = straight from power-on to the title (the save still loads) */
    /* cheats (cheats.h): fights against the CPU only */
    int cheat_health, cheat_super, cheat_cpu_ko, cheat_cpu_super;
    int cheat_half_health;  /* 0 off, 1 you, 2 the CPU, 3 both */
    int cheat_guard, cheat_cpu_guard_crush;
    int cheat_final_bison;  /* 1 = unlocked (kept in the game's own save too) */
    int cheat_wt_exp, cheat_wt_x, cheat_wt_a, cheat_wt_v;  /* World Tour EXP at the most */
    char name[16];          /* netplay player name (NETWORK > NAME) */
} Settings;

#define NAME_MAX_LEN 10

extern Settings g_settings;
extern const Settings g_settings_default;

void settings_load(const char *path);
void settings_save(void);
void settings_set_name(const char *name);
