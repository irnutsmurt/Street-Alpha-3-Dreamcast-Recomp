/* sfa3.cfg: simple "key = value" lines. Environment variables SFA3_<KEY>
 * override the file (useful for testing) but are not written back. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controls.h"
#include "settings.h"

const Settings g_settings_default = {
    .window_mode = WINDOW_WINDOWED, .window_scale = 0,
    .fs_width = 0, .fs_height = 0, .fs_refresh = 0,  /* 0 = the desktop's mode */
    .picture = PICTURE_4_3, .color = COLOR_STANDARD, .scale = 3, .vsync = 0,
    .scanlines = 0, .scanline_strength = 50, .nearest = 0, .name = "PLAYER",
};
Settings g_settings;

static char s_path[1024];

static const struct {
    const char *key, *env, *comment;
    size_t off;
    int lo, hi;
} s_keys[] = {
#define K(field, env, lo, hi, comment) {#field, env, comment, offsetof(Settings, field), lo, hi}
    K(window_mode, "SFA3_WINDOW_MODE", 0, 2, "0 = window, 1 = borderless fullscreen, 2 = exclusive fullscreen"),
    K(window_scale, "SFA3_WINDOW_SCALE", 0, 4, "window size: 0 = fit the screen, N = N x 640x480"),
    K(fs_width, "SFA3_FS_WIDTH", 0, 16384, "exclusive fullscreen width (0 = desktop)"),
    K(fs_height, "SFA3_FS_HEIGHT", 0, 16384, "exclusive fullscreen height (0 = desktop)"),
    K(fs_refresh, "SFA3_FS_REFRESH", 0, 1000, "exclusive fullscreen refresh rate in Hz (0 = desktop)"),
    K(picture, "SFA3_PICTURE", 0, 2, "0 = 4:3, 1 = stretch to fill, 2 = integer scale (pixel perfect)"),
    K(color, "SFA3_COLOR", 0, 2, "0 = standard (VGA cable), 1 = TV levels (as on a television / in videos), 2 = vivid"),
    K(scale, "SFA3_SCALE", 1, 4, "internal render resolution multiplier (1 = 640x480, 3 = 1920x1440)"),
    K(vsync, "SFA3_VSYNC", 0, 1, "1 = wait for the display refresh (vsync)"),
    K(scanlines, "SFA3_SCANLINES", 0, 2, "CRT look: 0 off, 1 scanlines, 2 PVM monitor"),
    K(scanline_strength, "SFA3_SCANLINE_STRENGTH", 10, 100, "CRT look strength in percent (10-100)"),
    K(nearest, "SFA3_NEAREST", 0, 1, "1 = sharp point-sampled textures, 0 = the game's own filtering"),
    K(skip_intro, "SFA3_SKIP_INTRO", 0, 1, "1 = skip the Capcom logos and the intro at power-on"),
#undef K
};
#define NKEYS (sizeof s_keys / sizeof s_keys[0])

static int *field(Settings *st, size_t i) { return (int *)((char *)st + s_keys[i].off); }

static void set_key(Settings *st, size_t i, const char *v) {
    int x = atoi(v);
    if (x < s_keys[i].lo) x = s_keys[i].lo;
    if (x > s_keys[i].hi) x = s_keys[i].hi;
    *field(st, i) = x;
}

/* values as stored in the file (environment overrides are not persisted) */
static Settings s_file;

/* the player name: printable ASCII, upper case (the game's font), trimmed */
static void set_name(char *dst, const char *v) {
    char b[sizeof g_settings.name] = {0};
    int n = 0;
    for (; *v && n < NAME_MAX_LEN; v++) {
        char c = *v;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!(c == ' ' || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr(".-:+/?()'", c))) ||
            (c == ' ' && !n))
            continue;  /* only characters the game's font has */
        b[n++] = c;
    }
    while (n && b[n - 1] == ' ') b[--n] = 0;
    if (n) memcpy(dst, b, sizeof b);
}

void settings_load(const char *path) {
    snprintf(s_path, sizeof s_path, "%s", path);
    g_settings = g_settings_default;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256], key[64], val[128];
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#') continue;
            if (sscanf(line, " %63[a-z0-9_] = %127[^\r\n]", key, val) == 2 && controls_cfg_read(key, val)) continue;
            if (sscanf(line, " name = %127[^\r\n]", val) == 1) {
                set_name(g_settings.name, val);
                continue;
            }
            if (sscanf(line, " %63[a-z_] = %127s", key, val) != 2) continue;
            if (!strcmp(key, "fullscreen")) {  /* older config files */
                if (atoi(val)) g_settings.window_mode = WINDOW_BORDERLESS;
                continue;
            }
            for (size_t i = 0; i < NKEYS; i++)
                if (!strcmp(key, s_keys[i].key)) set_key(&g_settings, i, val);
        }
        fclose(f);
    }
    s_file = g_settings;
    for (size_t i = 0; i < NKEYS; i++) {
        const char *e = getenv(s_keys[i].env);
        if (e) set_key(&g_settings, i, e);
    }
    if (getenv("SFA3_NAME")) set_name(g_settings.name, getenv("SFA3_NAME"));
    settings_save();  /* list every key, so new settings appear after upgrades */
}

void settings_save(void) {
    if (!s_path[0]) return;
    for (size_t i = 0; i < NKEYS; i++)
        if (!getenv(s_keys[i].env)) *field(&s_file, i) = *field(&g_settings, i);
    if (!getenv("SFA3_NAME")) memcpy(s_file.name, g_settings.name, sizeof s_file.name);
    FILE *f = fopen(s_path, "w");
    if (!f) return;
    fputs("# Street Fighter Alpha 3 native port settings.\n"
          "# Also editable in-game: Option Mode > DISPLAY, or F1 / Esc at any time.\n", f);
    for (size_t i = 0; i < NKEYS; i++)
        fprintf(f, "# %s\n%s = %d\n", s_keys[i].comment, s_keys[i].key, *field(&s_file, i));
    fprintf(f, "# your name in netplay lobbies (NETWORK > NAME)\nname = %s\n", s_file.name);
    controls_cfg_write(f);
    fclose(f);
}

void settings_set_name(const char *name) {
    set_name(g_settings.name, name);
    settings_save();
}
