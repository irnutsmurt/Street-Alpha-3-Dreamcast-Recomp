/* PC display options: values, cycling, and applying them to the window and
 * renderer. */
#include <SDL3/SDL.h>
#include <stdio.h>

#include "options.h"
#include "runtime.h"
#include "settings.h"

extern SDL_Window *g_window;
void renderer_settings_changed(void);  /* vk_renderer.c: vsync / render resolution */
int widescreen_wanted(void);           /* widescreen.c: the picture's margin each side (units; 16:9: 64) */

static const char *const s_labels[OPT_COUNT] = {
    "WINDOW MODE", "WINDOW SIZE", "RESOLUTION", "PICTURE", "COLOR", "RENDER RES", "VSYNC",
    "CRT", "CRT LEVEL", "TEXTURES", "SKIP INTRO",
};

const char *option_label(int i) { return i >= 0 && i < OPT_COUNT ? s_labels[i] : ""; }

/* ---------------- exclusive fullscreen modes ---------------- */

static SDL_DisplayID display(void) {
    SDL_DisplayID d = g_window ? SDL_GetDisplayForWindow(g_window) : 0;
    return d ? d : SDL_GetPrimaryDisplay();
}

/* Distinct width/height/refresh combinations, largest first (SDL's order). */
static int fs_modes(SDL_DisplayMode *out, int max) {
    int n = 0, count = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(display(), &count);
    for (int i = 0; modes && i < count && n < max; i++) {
        int hz = (int)(modes[i]->refresh_rate + 0.5f);
        int dup = 0;
        for (int k = 0; k < n; k++)
            dup |= out[k].w == modes[i]->w && out[k].h == modes[i]->h && (int)(out[k].refresh_rate + 0.5f) == hz;
        if (!dup) out[n++] = *modes[i];
    }
    SDL_free(modes);
    return n;
}

static int current_fs_index(const SDL_DisplayMode *m, int n) {
    for (int i = 0; i < n; i++)
        if (m[i].w == g_settings.fs_width && m[i].h == g_settings.fs_height &&
            (int)(m[i].refresh_rate + 0.5f) == g_settings.fs_refresh)
            return i;
    return -1;
}

/* ---------------- resolutions ----------------
 * Heights offered for a 4:3 picture and for a 16:9 one (PICTURE); the width follows the shape: 4:3 640x480 ...
 * 3840x2880, 16:9 854x480 ... 5120x2880 */
static const int s_h43[] = {480, 720, 960, 1080, 1440, 1920, 2160, 2400, 2880};
static const int s_h169[] = {480, 720, 1080, 1440, 2160, 2880};
#define COUNT(a) ((int)(sizeof a / sizeof a[0]))

static int width_for(int h, int wide) { return wide ? (h * 16 / 9 + 1) & ~1 : h * 4 / 3; }

/* the picture's shape, width / height (as the renderer makes it: 4:3 in an online game) */
static float picture_aspect(void) { return (640.0f + 2.0f * (float)widescreen_wanted() * 640.0f / 384.0f) / 480.0f; }

/* the list's entry at or below h (the first if none) */
static int list_index(const int *list, int n, int h) {
    int k = 0;
    while (k + 1 < n && list[k + 1] <= h) k++;
    return k;
}

/* ---------------- window ---------------- */

static int window_wide(void) { return g_settings.picture == PICTURE_16_9; }

/* Largest window client size with the window's shape that fits the usable
 * desktop, leaving room for the title bar. */
static void fit_size(int *w, int *h) {
    SDL_Rect r;
    int wide = window_wide();
    if (!SDL_GetDisplayUsableBounds(display(), &r)) { *h = 480; *w = width_for(480, wide); return; }
    /* exactly the shape: the height a multiple of 36 (16:9) or 12 (4:3), so the width is whole and even */
    int step = wide ? 36 : 12, hh = r.h - 48;
    if (width_for(hh, wide) > r.w - 16) hh = wide ? (r.w - 16) * 9 / 16 : (r.w - 16) * 3 / 4;
    hh = hh / step * step;
    if (hh < step) hh = step;
    *h = hh;
    *w = wide ? hh * 16 / 9 : hh * 4 / 3;
}

/* the window heights of the window's shape that fit the desktop: entries 0..n-1 of its list (at least one) */
static int window_heights(const int **list) {
    int fw, fh, wide = window_wide(), n = wide ? COUNT(s_h169) : COUNT(s_h43), k = 1;
    *list = wide ? s_h169 : s_h43;
    fit_size(&fw, &fh);
    while (k < n && width_for((*list)[k], wide) <= fw && (*list)[k] <= fh) k++;
    return k;
}

/* window size for the window_height setting: 0 = fit the screen, else the listed size at or below it */
static void window_size(int *w, int *h) {
    if (!g_settings.window_height) { fit_size(w, h); return; }
    const int *list;
    int n = window_heights(&list);
    *h = list[list_index(list, n, g_settings.window_height)];
    *w = width_for(*h, window_wide());
}

void options_apply_window(void) {
    if (!g_window) return;
    switch (g_settings.window_mode) {
    case WINDOW_WINDOWED: {
        SDL_SetWindowFullscreen(g_window, false);
        int w, h;
        window_size(&w, &h);
        SDL_SetWindowSize(g_window, w, h);
        SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        break;
    }
    case WINDOW_BORDERLESS:
        SDL_SetWindowFullscreenMode(g_window, NULL);  /* desktop resolution */
        SDL_SetWindowFullscreen(g_window, true);
        break;
    case WINDOW_EXCLUSIVE: {
        SDL_DisplayMode m;
        const SDL_DisplayMode *want = NULL;
        if (g_settings.fs_width &&
            SDL_GetClosestFullscreenDisplayMode(display(), g_settings.fs_width, g_settings.fs_height,
                                                (float)g_settings.fs_refresh, true, &m))
            want = &m;
        if (!want) {  /* default: the desktop's own mode, made exclusive */
            const SDL_DisplayMode *d = SDL_GetDesktopDisplayMode(display());
            if (d) { m = *d; want = &m; }
        }
        SDL_SetWindowFullscreenMode(g_window, want);
        SDL_SetWindowFullscreen(g_window, true);
        break;
    }
    }
    SDL_SyncWindow(g_window);
    int w, h, pw, ph;
    SDL_GetWindowSize(g_window, &w, &h);
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    const SDL_DisplayMode *fm = SDL_GetWindowFullscreenMode(g_window);
    const SDL_DisplayMode *cm = SDL_GetCurrentDisplayMode(display());
    logf_("window: mode=%d size=%dx%d pixels=%dx%d fullscreen=%d exclusive_mode=%s display=%dx%d@%.2f",
          g_settings.window_mode, w, h, pw, ph, (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0,
          fm ? "set" : "none", cm ? cm->w : 0, cm ? cm->h : 0, cm ? cm->refresh_rate : 0.0f);
}

/* ---------------- values ---------------- */

int option_enabled(int i) {
    switch (i) {
    case OPT_WINDOW_SIZE: return g_settings.window_mode == WINDOW_WINDOWED;
    case OPT_RESOLUTION: return g_settings.window_mode == WINDOW_EXCLUSIVE;
    case OPT_SCANLINE_STRENGTH: return g_settings.scanlines;
    default: return 1;
    }
}

void option_value(int i, char *out, size_t n) {
    static const char *const modes[] = {"WINDOW", "BORDERLESS", "EXCLUSIVE"};

    static const char *const colors[] = {"STANDARD", "TV", "VIVID"};
    static const char *const crts[] = {"OFF", "SCANLINES", "PVM"};
    switch (i) {
    case OPT_WINDOW_MODE: snprintf(out, n, "%s", modes[g_settings.window_mode]); break;
    case OPT_WINDOW_SIZE: {
        int w, h;
        window_size(&w, &h);
        if (!g_settings.window_height) snprintf(out, n, "FIT %dX%d", w, h);
        else snprintf(out, n, "%dX%d", w, h);
        break;
    }
    case OPT_RESOLUTION:
        if (!g_settings.fs_width) {
            const SDL_DisplayMode *d = SDL_GetDesktopDisplayMode(display());
            if (d) snprintf(out, n, "%dX%d %dHZ", d->w, d->h, (int)(d->refresh_rate + 0.5f));
            else snprintf(out, n, "DESKTOP");
        } else {
            snprintf(out, n, "%dX%d %dHZ", g_settings.fs_width, g_settings.fs_height, g_settings.fs_refresh);
        }
        break;
    case OPT_PICTURE: snprintf(out, n, "%s", g_settings.picture == PICTURE_16_9 ? "16:9" : "4:3"); break;
    case OPT_COLOR: snprintf(out, n, "%s", colors[g_settings.color]); break;
    case OPT_RENDER_RES: {
        int h = g_settings.render_height;
        snprintf(out, n, "%dX%d", (int)(picture_aspect() * (float)h + 0.5f), h);
        break;
    }
    case OPT_VSYNC: snprintf(out, n, "%s", g_settings.vsync ? "ON" : "OFF"); break;
    case OPT_SCANLINES: snprintf(out, n, "%s", crts[g_settings.scanlines]); break;
    case OPT_SCANLINE_STRENGTH: snprintf(out, n, "%d", g_settings.scanline_strength); break;  /* no % in the game font */
    case OPT_TEXTURES: snprintf(out, n, "%s", g_settings.nearest ? "SHARP" : "SMOOTH"); break;
    case OPT_SKIP_INTRO: snprintf(out, n, "%s", g_settings.skip_intro ? "ON" : "OFF"); break;
    default: out[0] = 0;
    }
}

static int wrap(int v, int dir, int lo, int hi) {
    v += dir;
    if (v < lo) v = hi;
    if (v > hi) v = lo;
    return v;
}

void option_change(int i, int dir) {
    if (!option_enabled(i)) return;
    switch (i) {
    case OPT_WINDOW_MODE:
        g_settings.window_mode = wrap(g_settings.window_mode, dir, 0, 2);
        options_apply_window();
        break;
    case OPT_WINDOW_SIZE: {  /* FIT, then the listed sizes that fit the desktop */
        const int *list;
        int count = window_heights(&list);
        int k = g_settings.window_height ? list_index(list, count, g_settings.window_height) + 1 : 0;
        k = wrap(k, dir, 0, count);
        g_settings.window_height = k ? list[k - 1] : 0;
        options_apply_window();
        break;
    }
    case OPT_RESOLUTION: {
        SDL_DisplayMode m[64];
        int n = fs_modes(m, 64);
        if (!n) break;
        int k = current_fs_index(m, n);
        k = k < 0 ? 0 : wrap(k, dir, 0, n - 1);
        g_settings.fs_width = m[k].w;
        g_settings.fs_height = m[k].h;
        g_settings.fs_refresh = (int)(m[k].refresh_rate + 0.5f);
        options_apply_window();
        break;
    }
    case OPT_PICTURE:  /* 4:3 / 16:9; a window takes the new shape (the render resolution's width follows) */
        g_settings.picture = g_settings.picture == PICTURE_16_9 ? PICTURE_4_3 : PICTURE_16_9;
        if (g_settings.window_mode == WINDOW_WINDOWED) options_apply_window();
        break;
    case OPT_COLOR: g_settings.color = wrap(g_settings.color, dir, 0, 2); break;
    case OPT_RENDER_RES: {  /* the 16:9 list for a 16:9 picture */
        int wide = picture_aspect() > 4.0f / 3.0f + 0.01f;
        const int *list = wide ? s_h169 : s_h43;
        int count = wide ? COUNT(s_h169) : COUNT(s_h43), h = g_settings.render_height;
        int k = h < list[0] ? -1 : list_index(list, count, h);
        if ((k < 0 || list[k] != h) && dir < 0) k++;  /* between two (or below the first): down is the lower */
        g_settings.render_height = list[wrap(k, dir, 0, count - 1)];
        renderer_settings_changed();
        break;
    }
    case OPT_VSYNC:
        g_settings.vsync = !g_settings.vsync;
        renderer_settings_changed();
        break;
    case OPT_SCANLINES: g_settings.scanlines = wrap(g_settings.scanlines, dir, 0, 2); break;
    case OPT_SCANLINE_STRENGTH: {
        int v = g_settings.scanline_strength + dir * 10;
        g_settings.scanline_strength = v < 10 ? 10 : v > 100 ? 100 : v;
        break;
    }
    case OPT_TEXTURES: g_settings.nearest = !g_settings.nearest; break;
    case OPT_SKIP_INTRO: g_settings.skip_intro = !g_settings.skip_intro; break;
    }
    settings_save();
}

void options_reset(void) {
    Settings old = g_settings;
    g_settings = g_settings_default;
    if (old.window_mode != g_settings.window_mode || old.window_height != g_settings.window_height ||
        old.picture != g_settings.picture)
        options_apply_window();
    if (old.render_height != g_settings.render_height || old.vsync != g_settings.vsync) renderer_settings_changed();
    settings_save();
}
