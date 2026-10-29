/* 16:9 fights (Option Mode > DISPLAY > PICTURE 16:9).
 *
 * The game draws for a 384 x 224 screen (its units; 640 x 448 pixels here).
 * A 16:9 picture is 64 units wider on each side. The game itself is
 * unchanged: its camera, the walls (the camera's limits) and so the furthest
 * the fighters can be apart are the 4:3 game's; only what is drawn changes:
 *
 *  - only the fight's own screen is wide (fighting()); the others (menus,
 *    the win quote, the map, the VS screen, the wipes between them) are
 *    4:3 in the middle of the picture, the renderer clipping what the game
 *    draws past their edges;
 *  - the HUD stays where it is, in the middle (the renderer moves everything
 *    right by the margin);
 *  - the wide view's camera is the game's, stopped a margin from each wall
 *    (view()): at a wall the picture's edge is the stage's edge, as the
 *    screen's is in the 4:3 game; the fighters and what moves with the
 *    camera are drawn that much further in (hle_scroll_object);
 *  - every layer moves at its own speed (rate(), from the stage's tables:
 *    the parallax is the game's);
 *  - the stage is laid out more than once a frame (hle_stage_layout): each
 *    layer once more for the margin on each side, placed by the global
 *    screen offset the layout adds (8C18EF4C), then the view itself;
 *  - a far layer has art for only so much of the wider view (the stages'
 *    far layers were painted as wide as the 4:3 game shows them), and some
 *    layers hold flat filler where the 4:3 game never looks: once a stage,
 *    on the wipe into its fight, from the layers drawn alone (capture()),
 *    wide_filler.c finds the filler and works out each layer's fill where its
 *    art runs out past what the 4:3 game shows of it or it has filler: its
 *    own art continued, blurred (the background extension); the fight is
 *    drawn wide once that's ready (fill_latch). Each frame the renderer draws
 *    that fill in the margins where it's in view (widescreen_extensions,
 *    vk_renderer.c, shaders/ext.frag), on that layer only, behind what's
 *    nearer, shaded as the layer is and following its palettes; the art
 *    itself is drawn as it is, and so are the ends of it the 4:3 game
 *    shows;
 *  - what the renderer needs of a frame (wide or not, the extensions) is
 *    kept by stage layout, for the TA list holding its polygons (publish():
 *    it's drawn a frame after the game has gone on to the next);
 *  - the sprite drawer's edges (it leaves out the tiles past the screen's)
 *    are as far out as the picture's (g_wide_cull), but the HUD's stay the
 *    4:3 screen's (widescreen_sprite_cull), and so are those past which a
 *    stage's objects aren't drawn (f_8C02472E: 64 units out, then
 *    g_wide_units more): recompile.py's REPLACE.
 *
 * 4:3 is the game's own picture: none of this runs (tested: pixel for pixel
 * the same as 0.2.6, before any of it). Online games are 4:3: what the game
 * draws is in its RAM (the polygons waiting for the frame's end, the
 * sprites' screen positions), and netplay.c compares the two players' RAM
 * to catch a desync. The game's state is the same either way (tested: the
 * same inputs, the fighters, game state and random number the same, frame
 * for frame).
 *
 * The background layers: 8C188818, 8C1888B4 (the main one, the camera) and
 * 8C188950 (objects' layers 1, 2 and 3), 0x9C bytes each; +0x10 the scroll X
 * (16.16, units), +0x62 the scroll Y, +0x7A its depth's index (layer_z()).
 * The main layer's +0x20 is the camera's X (16.16; +0x12 its whole part), +8
 * the stage's middle (448), +0x44 / +0x46 the camera's limits (0x100 and
 * 0x280 on every stage). See docs/dreamcast.md, Stage backgrounds. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cpu.h"
#include "netplay.h"
#include "runtime.h"
#include "settings.h"
#include "wide_filler.h"
#include "widescreen.h"

#define MAIN_LAYER 0x8C1888B4u
#define SCREEN_OFFSET_X 0x8C18EF4Cu  /* float pixels, added to everything the stage layout and sprites place */
#define VIEW 384                     /* the game's screen width, units */
#define MARGIN_16_9 64               /* (16 / 9 x 224 - 384) / 2, near enough: 853.3 pixels wide */
#define MARGIN_MAX 192
#define FLOOR_Q 0x8C188A08u          /* (camera - middle) / a stage's constant, 16.16 (f_8C0223AA) */
#define FLOOR_A 0x8C188A5Cu          /* that x a second constant */
#define FLOOR 0x8C1889ECu            /* the floor's block (+0x1C is FLOOR_Q, +0x70 FLOOR_A) */
#define STAGE_BLOCKS 0x8C188818u     /* layers 1, main, 3 and the floor's blocks, 0x9C bytes each */
#define STAGE_BLOCKS_SIZE (4 * 0x9Cu)
#define WIDE_LEFT 4096.0f            /* the left margin's layouts, this far right (vk_renderer.c moves them back) */

static const uint32_t s_layers[3] = {0x8C188818u, MAIN_LAYER, 0x8C188950u};  /* objects' layers 1, 2, 3 */
static const uint8_t s_bits[3] = {1, 2, 4};  /* their bits in main +0x7B (the layers to lay out) */
static int s_wide;               /* the picture's margin, units each side (0: 4:3): the renderer's */
static int s_margin;             /* the margin the game is drawn with: s_wide, 0 in an online game */
static int s_stage_frame;        /* the fight's stage is being laid out wide (cleared off the fight's screen) */

int widescreen_margin(void) { return s_wide; }
float widescreen_margin_px(void) { return (float)s_wide * 640.0f / (float)VIEW; }

/* the picture's margin (units each side): 16:9's with PICTURE 16:9, else 0 (the renderer asks each frame and
 * applies a new one with widescreen_set between frames) */
int widescreen_wanted(void) {
    if (netplay_active()) return 0;
    static int forced = -2;  /* SFA3_WIDE=<units>: a margin whatever the setting (testing headless) */
    if (forced == -2) forced = getenv("SFA3_WIDE") ? atoi(getenv("SFA3_WIDE")) : -1;
    int m = forced >= 0 ? forced : g_settings.picture == PICTURE_16_9 ? MARGIN_16_9 : 0;
    return m < 0 ? 0 : m > MARGIN_MAX ? MARGIN_MAX : m;
}

float g_wide_cull;
int g_wide_units;
/* each frame (video_vblank), and when the renderer changes the picture's margin */
static int s_probe = -1, s_probe_x;
static void fill_latch(void);
void widescreen_frame(void) {
    s_margin = netplay_active() ? 0 : s_wide;
    fill_latch();
    g_wide_cull = s_margin ? (float)(int)(widescreen_margin_px() + 0.5f) : 0.0f;
    g_wide_units = s_probe >= 0 ? -4096 : 2 * s_margin;  /* the margin and the most the camera moves (probing:
                                                           * the stage's objects out of sight) */
}

void widescreen_set(int units) {
    s_wide = units;
    widescreen_frame();
}

static int s_stage_m;
static int fighting(void);

static double fx(uint32_t a) { return (int32_t)R32(a) / 65536.0; }

int g_wide_probe;     /* a probe frame: the renderer draws only the probed layer (g_wide_probe_z) */
float g_wide_probe_z;

static float layer_z(int k) {  /* a layer's depth, 1/w: f_8C01511E's -w for its +0x7A */
    fpreg_t f;
    f.u = R32(0x8C0993E8u + (2 * R8(s_layers[k] + 0x7A) + 1) * 4);
    return f.f ? -1.0f / f.f : 0;
}

/* a fight's own screen (game state +2 = 2, +4 = 3: the round's intro, the round, the KO and the win pose); the
 * stage is laid out on the screens either side of it too (the win quote, the wipe into the next fight), which
 * stay 4:3. Drawn wide once its stage's fill is ready (s_ready: fill_latch) */
#define GAME_STATE 0x8C17FBC2u
static int s_ready;
static int fight_screen(void) { return R16(GAME_STATE) == 2 && R16(GAME_STATE + 2) == 3; }
static int fighting(void) { return s_margin && fight_screen() && s_ready; }

/* ---------------- the layers' speeds ----------------
 * How fast each layer moves across the screen against the camera (the fighters move 1), from the stage's data
 * (so the same from its first frame). The floor's block (8C1889EC, f_8C0223AA; +4 = 1 in use): q (8C188A08) =
 * (camera - middle) / F80, A (8C188A5C) = q x (F84 - F82, + 16 on background 11, + 8 on 28), F80 / F82 / F84 its
 * +0x80 / +0x82 / +0x84 (from the background's entries in 8C0A1610 and 8C0A15BC); D = A / q / F80 the floor's
 * share.
 *  - Layers 1 and 3 (f_8C02291C): X = middle + k (camera - middle - A) / 16 (+ offsets): k / 16 x (1 - D), k from
 *    the background's entry in 8C0A146C (4 x s16: layer 1's, -, layer 3's, -). Background 28's layer 1 (k 7) and
 *    background 4's layer 3 (f_8C022984): X = middle + camera - middle - q k: 1 - k / F80.
 *  - The main layer: its X is the camera's; its layout draws it at X less the floor's share: A (f_8C020804,
 *    f_8C01F57C), 8 q on stage 00 (+3 0xA0), q (F84 - F82 + 2) on stage 04 (0xA4), nothing on 0A / 1E (0xAA /
 *    0xBE, f_8C020AD8): 1 - D, 1 - 8 / F80, 1 - (F84 - F82 + 2) / F80, 1. */
#define LAYER_K 0x8C0A146Cu
#define BACKGROUND 0x8C17FC60u

static int floor_on(void) { return R8(FLOOR + 4) == 1 && R16(FLOOR + 0x80) && R16(FLOOR + 0x80) != 0xFFFF; }
static double floor_f80(void) { return R16(FLOOR + 0x80); }
static double floor_k(void) {  /* A / q */
    int bg = R16(BACKGROUND);
    return (int16_t)R16(FLOOR + 0x84) - (int16_t)R16(FLOOR + 0x82) + (bg == 11 ? 16 : bg == 28 ? 8 : 0);
}

/* the main layer's share of the floor, drawn: in units of q */
static double main_qk(void) {
    int kind = R8(MAIN_LAYER + 3);
    if (kind == 0xAA || kind == 0xBE) return 0;
    if (kind == 0xA0) return 8;
    if (kind == 0xA4) return (int16_t)R16(FLOOR + 0x84) - (int16_t)R16(FLOOR + 0x82) + 2;
    return floor_k();
}

static double rate(int k) {
    int bg = R16(BACKGROUND);
    double f80 = floor_on() ? floor_f80() : 0, d = f80 ? floor_k() / f80 : 0;
    if (s_layers[k] == MAIN_LAYER) return f80 ? 1 - main_qk() / f80 : 1;
    int16_t kk = (int16_t)R16(LAYER_K + (uint32_t)bg * 8 + (k ? 4 : 0));
    if ((k == 0 && bg == 28) || (k == 2 && bg == 4)) return f80 ? 1 - (k == 0 ? 7 : kk) / f80 : 1;
    return kk / 16.0 * (1 - d);
}

/* a layer that scrolls by itself (its +0x50 changing: clouds) goes round its map: any part of it can be shown */
static int wraps(int k) { return s_layers[k] != MAIN_LAYER && R32(s_layers[k] + 0x50) != 0; }

/* ---------------- the wide view ----------------
 * The game's camera c moves from lo to hi (its limits, the walls: 256 and 640), and the 4:3 game shows a layer of
 * speed r from where it is with the camera at lo to where it is at hi, plus a screen: 384 + 384 r units of it.
 *
 * The wide view (margin m each side) has its own camera, c' = c clamped to lo + m .. hi - m: in the middle of the
 * stage it is the game's (the 4:3 picture in the middle of the wide one), at a wall it stops m from it, so that
 * the picture's edge is the wall. The fighters and the objects on the main layer are drawn c' - c further left
 * (at most m: always in the picture). Each layer moves at its own speed r, as in the game: the picture shows
 * (1 - r) m more of it at each end than the 4:3 game does, which some layers have the art for and some don't
 * (the background extension: widescreen_extensions).
 *
 * With the extension off (SFA3_WIDE_EXT=0, for testing) the layers move at r' = 1 - (1 - r) (hi - lo) /
 * (hi - lo - 2 m) instead (a little more parallax than the game's): over c' from lo + m to hi - m each then shows
 * exactly the part the 4:3 game shows over c from lo to hi; a layer slower than a third of the camera (r' < 0)
 * has less of it than a wide view, and a stage with one is drawn as wide as its layers allow, m = 192 r of its
 * slowest such layer, the picture's sides past it black: bounded() */
typedef struct {
    uint64_t frame;
    int m;                    /* the margin the stage allows, units (<= s_margin) */
    double c, cw, d;          /* the game's camera, the wide view's, the difference (units) */
    double rate[3], wide[3];  /* each layer's speed, and the wide view's (r') */
    int32_t dx[3];            /* how much further right each layer's X is laid out (16.16) */
    double q, a;              /* the floor's q and A for the wide view's camera (16.16 values as doubles) */
} View;
static View s_view;

/* the background extension (wide_filler.c's wide_extend, vk_renderer.c, ext.frag): every layer moves at its own
 * speed and the picture is always the full 16:9; where a layer lacks art (past its ends, its filler), its own art is
 * continued, blurred. Testing: SFA3_WIDE_EXT=<mode>[,<band>[,<blur>]] (guest pixels: the band of art averaged, the
 * most rows): mode 0 off (the narrower picture of bounded() instead), 9 colours what's filled (green over the
 * layer, yellow behind it), anything else the blur (the default) */
static int s_ext_mode = -1;
static float s_ext_band = 16, s_ext_blur = 24;
static int ext_mode(void) {
    if (s_ext_mode < 0) {
        const char *e = getenv("SFA3_WIDE_EXT");
        s_ext_mode = e ? atoi(e) : 3;
        if (e && strchr(e, ',')) s_ext_band = (float)atof(strchr(e, ',') + 1);
        if (e && strchr(e, ',') && strchr(strchr(e, ',') + 1, ',')) s_ext_blur = (float)atof(strchr(strchr(e, ',') + 1, ',') + 1);
    }
    return s_ext_mode;
}

/* the margin a stage allows: all of it with the extension, else its layers' art (see above) */
static int bounded(void) {
    int m = s_margin;
    if (ext_mode()) return m;
    uint8_t layers = R8(MAIN_LAYER + 0x7B);
    for (int k = 0; k < 3; k++) {
        if (!(layers & s_bits[k]) || wraps(k)) continue;
        double r = rate(k), mk = 192 * r;  /* (hi - lo) / 2 x r: lo and hi are 256 and 640 on every stage */
        if (mk < m) m = (int)mk;
    }
    return m < 0 ? 0 : m;
}

static const View *view(void) {
    extern uint64_t g_vblank_count;
    View *v = &s_view;
    if (v->frame == g_vblank_count + 1) return v;
    memset(v, 0, sizeof *v);
    v->frame = g_vblank_count + 1;
    double lo = (int16_t)R16(MAIN_LAYER + 0x44), hi = (int16_t)R16(MAIN_LAYER + 0x46), mid = fx(MAIN_LAYER + 8);
    double c = fx(MAIN_LAYER + 0x10), cw = c, w = hi - lo;
    int m = v->m = bounded();
    if (w < 2 * m) cw = (lo + hi) / 2;
    else if (cw < lo + m) cw = lo + m;
    else if (cw > hi - m) cw = hi - m;
    v->c = c, v->cw = cw, v->d = cw - c;
    for (int k = 0; k < 3; k++) {
        double r = rate(k), rw = wraps(k) || w <= 2 * m || ext_mode() ? r : 1 - (1 - r) * w / (w - 2 * m);
        v->rate[k] = r, v->wide[k] = rw;
        if (s_layers[k] == MAIN_LAYER) {
            v->dx[k] = (int32_t)(v->d * 65536.0);  /* the camera's own move; the rest by the floor's share: */
            double f80 = floor_on() ? floor_f80() : 0, qk = main_qk();
            if (f80 && qk) {  /* drawn at c' - (1 - r') (c' - mid): q (c' - mid) / f80 scaled to that */
                v->q = (1 - rw) * (cw - mid) / qk;
                v->a = v->q * floor_k();
            } else {
                v->q = f80 ? (cw - mid) / f80 : 0;
                v->a = v->q * floor_k();
            }
        } else {
            v->dx[k] = (int32_t)((rw * (cw - mid) - r * (c - mid)) * 65536.0);
        }
    }
    return v;
}

static void layout_px(uint8_t which, const View *v, const int32_t *dx, float px, int flat, int extra);
static void layout(uint8_t which, const View *v, const int32_t *dx, int ds, int extra);

/* ---------------- filler (wide_filler.c) ----------------
 * Once a stage's fight is on screen, each layer is laid out once more over the part of its map the 4:3 and 16:9
 * games can show (CAP_SEGMENTS of VIEW units from cap_x0), to the renderer's layer captures (ATLAS_X on: off the
 * picture, into their own images: vk_renderer.c), a layer a frame; when the renderer has them back
 * (widescreen_atlas_ready) wide_filler finds the filler and wide_extend the fill, which goes to the renderer. Kept
 * for the background (8C17FC60) and its layers' maps: the next round's the same. */
#define ATLAS_X 8192.0f
#define ATLAS_SLOT 1024.0f
#define CAP_SEGMENTS 3
#define ATLAS_ROWS 240
#define FILLER_GROW 2  /* units */

static struct {
    int bg;                 /* the background captured (-1 none) */
    uint32_t sig;           /* its layers' maps and layouts */
    int next;               /* the next layer to capture (0-2), 3 all asked for, 4 analysed */
    int layers;             /* captured so far (bits) */
    int asked;              /* laid out for capture (bits) */
    double x0[3];           /* each capture's first map column */
    int y[3];               /* each layer's Y (+0x62) at its capture */
    float ox[3], oy[3];     /* the game's screen offset at its capture (pixels: the capture has it in) */
    double home[3];         /* each layer's map position at the 4:3 view's left edge with the camera in the middle */
    int has[3];             /* a layer has filler */
    int gen;                /* the analyses done (needs_build's) */
    int waited;             /* frames since the last capture was asked for */
    WideAtlas atlas[3];
} s_fill = {.bg = -1};

static int s_captured;  /* this stage layout laid out layer captures (capture_one; publish()) */

static uint32_t layers_sig(void) {
    static const uint32_t maps[3] = {0x8C19A424u, 0x8C19C424u, 0x8C19E424u};
    uint32_t h = 2166136261u;
    for (int k = 0; k < 3; k++) {
        h = (h ^ R8(s_layers[k] + 3)) * 16777619u;
        for (uint32_t a = 0; a < 0x2000; a += 4) h = (h ^ R32(maps[k] + a)) * 16777619u;
    }
    return h;
}

/* the layer's drawn map position at the 4:3 view's left edge for the game's camera now */
static double drawn_x(int k) {
    double x = fx(s_layers[k] + 0x10);
    if (s_layers[k] == MAIN_LAYER) x -= main_qk() * fx(FLOOR_Q);
    return x;
}

static struct {
    WideAtlas list[3];
    int idx[3], n, lo, hi, bg, y[3];
    double ms;
    SDL_AtomicInt done;
} s_job;

static int SDLCALL extend_job(void *j) {
    wide_extend_layer(s_job.list, s_job.n, (int)(intptr_t)j, s_job.lo, s_job.hi, VIEW, FILLER_GROW, s_ext_band, s_ext_blur);
    return 0;
}

static int SDLCALL filler_job(void *unused) {
    (void)unused;
    uint64_t t0 = SDL_GetTicksNS();
    wide_filler(s_job.list, s_job.n, s_job.lo, s_job.hi, VIEW);
    if (getenv("SFA3_WIDE_DEBUG"))  /* each capture -> build/wide/<background>_<layer>.{rgba,fill,txt} (testing) */
        for (int j = 0; j < s_job.n; j++) {
            int k = s_job.idx[j];
            WideAtlas *a = &s_job.list[j];
            char path[64];
            SDL_CreateDirectory("build/wide");
            snprintf(path, sizeof path, "build/wide/%02X_%d.rgba", s_job.bg, k);
            FILE *f = fopen(path, "wb");
            if (!f) continue;
            for (int i = 0; i < a->w * a->h; i++) {
                uint8_t px[4] = {(uint8_t)(a->rgb[i] >> 16), (uint8_t)(a->rgb[i] >> 8), (uint8_t)a->rgb[i],
                                 a->opaque[i] ? 255 : 0};
                fwrite(px, 1, 4, f);
            }
            fclose(f);
            snprintf(path, sizeof path, "build/wide/%02X_%d.fill", s_job.bg, k);
            if ((f = fopen(path, "wb"))) fwrite(a->filler, 1, (size_t)a->w * a->h, f), fclose(f);
            snprintf(path, sizeof path, "build/wide/%02X_%d.txt", s_job.bg, k);
            if ((f = fopen(path, "w")))
                fprintf(f, "%d %d %.3f %.6f %.6f %.3f %.9g %d %d %d\n", a->w, a->h, a->x0, a->home, a->rate, a->mid, a->z,
                        s_job.lo, s_job.hi, s_job.y[j]), fclose(f);
        }
    /* the fill, a layer a thread */
    SDL_Thread *t[WIDE_LAYERS] = {0};
    for (int j = 1; j < s_job.n; j++) t[j] = SDL_CreateThread(extend_job, "wide extend", (void *)(intptr_t)j);
    for (int j = 0; j < s_job.n; j++)
        if (j && t[j]) SDL_WaitThread(t[j], NULL);
        else extend_job((void *)(intptr_t)j);
    s_job.ms = (SDL_GetTicksNS() - t0) / 1e6;
    SDL_SetAtomicInt(&s_job.done, 1);
    return 0;
}

static void start_analysis(void);
static void capture_one(uint8_t en);
void widescreen_atlas_ready(int layers, const uint8_t *rgba, const uint8_t *pal, int w, int h) {
    if (s_fill.next > 3) return;  /* analysing or done */
    for (int k = 0; k < 3; k++) {
        if (!(layers & (1 << k))) continue;
        WideAtlas *a = &s_fill.atlas[k];
        a->w = w;
        a->h = ATLAS_ROWS;
        if (!a->rgb) a->rgb = malloc((size_t)w * ATLAS_ROWS * sizeof *a->rgb);
        if (!a->opaque) a->opaque = malloc((size_t)w * ATLAS_ROWS);
        if (!a->pal) a->pal = malloc((size_t)w * ATLAS_ROWS);
        memcpy(a->pal, pal + (size_t)k * WIDE_MASK_W * ATLAS_ROWS, (size_t)w * ATLAS_ROWS);
        const uint8_t *src = rgba + (size_t)k * w * h * 4;
        for (int y = 0; y < ATLAS_ROWS; y++)
            for (int x = 0; x < w; x++) {
                const uint8_t *p = src + ((size_t)(y * 2) * w + x) * 4;
                a->rgb[y * w + x] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
                a->opaque[y * w + x] = p[3] >= 128;
            }
        s_fill.layers |= 1 << k;
    }
    int want = s_fill.asked;
    if (s_fill.next != 3 || (s_fill.layers & want) != want) return;
    start_analysis();
}

/* all the captures in: the analysis (the 4:3 camera from wall to wall), on a thread of its own (some tens of ms) */
static void start_analysis(void) {
    int want = s_fill.asked;
    double mid = fx(MAIN_LAYER + 8);
    s_job.n = 0;
    for (int k = 0; k < 3; k++) {
        if (!(want & (1 << k))) continue;
        WideAtlas *a = &s_fill.atlas[k];
        a->x0 = s_fill.x0[k];
        a->home = s_fill.home[k];
        a->rate = rate(k);
        a->mid = mid;
        a->z = layer_z(k);
        a->behind = 0;  /* behind it, a layer that scrolls by itself (not captured: any part of it can show) */
        for (int j = 0; j < 3; j++)
            if (j != k && !(want & (1 << j)) && (R8(MAIN_LAYER + 0x7B) & s_bits[j]) && wraps(j) && layer_z(j) < a->z)
                a->behind = 1;
        s_job.list[s_job.n] = *a;
        s_job.y[s_job.n] = s_fill.y[k];
        s_job.idx[s_job.n++] = k;
    }
    s_job.lo = (int16_t)R16(MAIN_LAYER + 0x44);
    s_job.hi = (int16_t)R16(MAIN_LAYER + 0x46);
    s_job.bg = s_fill.bg;
    s_fill.next = 5;  /* analysing */
    SDL_SetAtomicInt(&s_job.done, 0);
    SDL_Thread *t = SDL_CreateThread(filler_job, "wide filler", NULL);
    if (t) SDL_DetachThread(t);
    else filler_job(NULL);
}

/* the analysis's results, once it's done (each frame: capture()) */
static void filler_results(void) {
    if (s_fill.next != 5 || !SDL_GetAtomicInt(&s_job.done)) return;
    if (s_job.bg != s_fill.bg) {  /* a stage since */
        s_fill.next = 0;
        return;
    }
    /* the extension's fill (wide_extend) to the renderer: each layer's modes (0 none, 64 the art next to a gap, 128 a
     * gap, 255 over the layer) with their palettes, and colours, WIDE_MASK_ROWS rows apiece */
    size_t np = (size_t)WIDE_MASK_W * WIDE_MASK_ROWS * 3;
    uint8_t *masks = calloc(np, 2);
    uint32_t *fill = calloc(np, sizeof *fill);
    for (int j = 0; j < s_job.n; j++) {
        int k = s_job.idx[j];
        WideAtlas *a = &s_job.list[j];
        s_fill.atlas[k].filler = a->filler;
        s_fill.atlas[k].mode = a->mode;
        s_fill.atlas[k].fill = a->fill;
        s_fill.atlas[k].fill_pal = a->fill_pal;
        s_fill.has[k] = 0;
        for (int i = 0; i < a->w * a->h; i++) s_fill.has[k] |= a->filler[i];
        for (int y = 0; y < ATLAS_ROWS && y < a->h; y++)
            for (int x = 0; x < WIDE_MASK_W && x < a->w; x++) {
                size_t o = ((size_t)k * WIDE_MASK_ROWS + y) * WIDE_MASK_W + x;
                uint8_t m = a->mode[y * a->w + x];
                masks[2 * o] = m == 2 ? 255 : m == 1 ? 128 : m == 3 ? 64 : 0;
                masks[2 * o + 1] = a->fill_pal[y * a->w + x];
                fill[o] = a->fill[y * a->w + x];
            }
    }
    if (getenv("SFA3_WIDE_DEBUG"))  /* testing: the fill's texels by the palette they follow (vk_renderer.c) */
        for (int j = 0; j < s_job.n; j++) {
            WideAtlas *a = &s_job.list[j];
            int cnt[256] = {0};
            char b[512];
            int bl = 0;
            for (int i = 0; i < a->w * a->h; i++)
                if (a->mode[i]) cnt[a->fill_pal[i]]++;
            for (int q = 0; q < 256 && bl < 480; q++)
                if (cnt[q]) bl += snprintf(b + bl, sizeof b - bl, " %d:%d", q, cnt[q]);
            logf_("wide pal: layer %d fill texels by palette:%s", s_job.idx[j], bl ? b : " none");
        }
    renderer_filler_masks(masks, fill);
    free(fill);
    free(masks);
    s_fill.next = 4;
    s_fill.gen++;
    logf_("widescreen: background %02X filler %d %d %d (analysis %.1f ms)", s_fill.bg, s_fill.has[0], s_fill.has[1],
          s_fill.has[2], s_job.ms);
}

/* whether a fight's frames can be drawn wide (each vblank: a frame is all one or the other): its stage's fill is
 * worked out (or nothing to work out). It's worked out on the wipe into the fight (stage_layout), well before its
 * first frame; were it still being worked out as the fight starts (a slow PC), the fight waits for it there, and
 * a fight shown before its fill (16:9 picked in the menu during it) is 4:3 until it's ready, a few frames */
static void fill_latch(void) {
    int screen = fight_screen();
    static int was;
    if (s_margin && screen && !was && s_fill.next == 5) {
        uint64_t t0 = SDL_GetTicksNS();
        while (!SDL_GetAtomicInt(&s_job.done) && SDL_GetTicksNS() - t0 < 500000000u) SDL_Delay(1);
        filler_results();
        logf_("widescreen: waited %.1f ms for the stage's fill", (SDL_GetTicksNS() - t0) / 1e6);
    }
    was = screen;
    s_ready = !ext_mode() || (s_fill.next == 4 && s_fill.bg == (int)R16(BACKGROUND));
}

/* this frame's layer captures (hle_stage_layout), if a new stage wants them: the next layer (a fight's frame, which
 * lays the stage out 7 times already), or all of them (all: the wipe into the fight, laid out once) */
static void capture(int all) {
    filler_results();
    if (s_fill.next == 5) return;  /* analysing: its captures in use */
    int bg = R16(BACKGROUND);
    if (bg != s_fill.bg) {
        uint32_t sig = layers_sig();  /* (one stage: one background; its maps too, for all we know) */
        if (bg != s_fill.bg || sig != s_fill.sig) {
            s_fill.bg = bg, s_fill.sig = sig, s_fill.next = 0, s_fill.layers = 0, s_fill.asked = 0;
            memset(s_fill.has, 0, sizeof s_fill.has);
        }
    }
    uint8_t en = R8(MAIN_LAYER + 0x7B);
    do capture_one(en);
    while (all && s_fill.next < 3);
}

static void capture_one(uint8_t en) {
    while (s_fill.next < 3 && (!(en & s_bits[s_fill.next]) || wraps(s_fill.next))) s_fill.next++;
    if (s_fill.next >= 3) {
        if (s_fill.next == 3 && !s_fill.asked) s_fill.next = 4;  /* nothing to capture */
        /* a capture that never comes back drew nothing (none of that layer in the part captured): empty */
        if (s_fill.next == 3 && ++s_fill.waited > 8) {
            for (int k = 0; k < 3; k++)
                if ((s_fill.asked & (1 << k)) && !(s_fill.layers & (1 << k))) {
                    WideAtlas *a = &s_fill.atlas[k];
                    a->w = WIDE_MASK_W, a->h = ATLAS_ROWS;
                    if (!a->rgb) a->rgb = malloc((size_t)a->w * a->h * sizeof *a->rgb);
                    if (!a->opaque) a->opaque = malloc((size_t)a->w * a->h);
                    if (!a->pal) a->pal = malloc((size_t)a->w * a->h);
                    memset(a->rgb, 0, (size_t)a->w * a->h * sizeof *a->rgb);
                    memset(a->opaque, 0, (size_t)a->w * a->h);
                    memset(a->pal, 0, (size_t)a->w * a->h);
                    s_fill.layers |= 1 << k;
                }
            start_analysis();
        }
        return;
    }
    s_fill.waited = 0;
    int k = s_fill.next++;
    s_fill.asked |= 1 << k;
    s_captured = 1;
    /* what the 4:3 and 16:9 games can show of it: its position with the camera at the left wall, less a margin */
    double lo = (int16_t)R16(MAIN_LAYER + 0x44), mid = fx(MAIN_LAYER + 8), cam = fx(MAIN_LAYER + 0x20);
    double x = drawn_x(k);
    s_fill.home[k] = x - rate(k) * (cam - mid);
    double x0 = floor(s_fill.home[k] + rate(k) * (lo - mid) - MARGIN_16_9 - 16);
    s_fill.x0[k] = x0;
    s_fill.y[k] = (int16_t)R16(s_layers[k] + 0x62);
    {
        fpreg_t o;
        o.u = R32(SCREEN_OFFSET_X), s_fill.ox[k] = o.f;
        o.u = R32(SCREEN_OFFSET_X + 4), s_fill.oy[k] = o.f;
    }
    Sh4Ctx saved = g;
    for (int seg = 0; seg < CAP_SEGMENTS; seg++) {
        int32_t dx[3] = {0, 0, 0};
        dx[k] = (int32_t)((x0 + seg * VIEW - x) * 65536.0);
        if (s_layers[k] == MAIN_LAYER) dx[k] = (int32_t)((x0 + seg * VIEW - fx(s_layers[k] + 0x10)) * 65536.0);
        layout_px(s_bits[k], NULL, dx, ATLAS_X + (float)(k * CAP_SEGMENTS + seg) * ATLAS_SLOT, 1, 1);
        g = saved;
    }
}

/* ---------------- what needs filling ----------------
 * The extension's fill (wide_extend, once a stage, with the filler) marks each layer's pixels it covers, in the
 * layer's map; as row bits by map column here. Each frame, the picture's columns, near to far: a layer needs its
 * pass if some of those pixels aren't behind a nearer layer's (its art or what its own extension fills), and only
 * over those columns. (A layer's pixels the game covers with objects, the stage's own or the fighters, aren't
 * known here: covered or not, a pass there costs little.) */
#define NEED_WORDS 4      /* rows as bits: up to 256 */
#define SCREEN_ROWS 240   /* the picture's rows (2 guest pixels; the game's screen is 224 of them, from 4 down) */
static struct {
    int gen;                                      /* for s_fill's results of this generation (0 none) */
    uint64_t need[3][WIDE_MASK_W * NEED_WORDS];   /* rows filled */
    uint64_t opq[3][WIDE_MASK_W * NEED_WORDS];    /* opaque rows */
} s_need;

static void needs_build(void) {
    memset(s_need.need, 0, sizeof s_need.need);
    memset(s_need.opq, 0, sizeof s_need.opq);
    for (int k = 0; k < 3; k++) {
        if (!(s_fill.asked & (1 << k))) continue;
        const WideAtlas *a = &s_fill.atlas[k];
        if (!a->mode) continue;
        int w = a->w < WIDE_MASK_W ? a->w : WIDE_MASK_W;
        for (int y = 0; y < a->h && y < NEED_WORDS * 64; y++)
            for (int u = 0; u < w; u++) {
                uint64_t bit = 1ull << (y % 64);
                if (a->mode[y * a->w + u] == 1 || a->mode[y * a->w + u] == 2) s_need.need[k][u * NEED_WORDS + y / 64] |= bit;
                if (a->opaque[y * a->w + u]) s_need.opq[k][u * NEED_WORDS + y / 64] |= bit;
            }
    }
    s_need.gen = s_fill.gen;
}

/* out = in's rows moved up by d (out's row j is in's j + d, of rows 0 to h - 1); rows past those: the nearest's
 * (clamp; ext.frag does the same) or all set */
static void rows_shift(const uint64_t *in, int d, int h, int past_set, uint64_t *out) {
    memset(out, 0, NEED_WORDS * sizeof *out);
    for (int j = 0; j < NEED_WORDS * 64; j++) {
        int r = j + d, bit;
        if (r < 0 || r >= h) bit = past_set ? 1 : (int)(in[(r < 0 ? 0 : h - 1) / 64] >> ((r < 0 ? 0 : h - 1) % 64) & 1);
        else bit = (int)(in[r / 64] >> (r % 64) & 1);
        if (bit) out[j / 64] |= 1ull << (j % 64);
    }
}

/* the extensions for the frame just laid out (hle_stage_layout), from the view it was laid out with: the
 * renderer draws a frame after the game has gone on to the next (and draws one again when the game doesn't lay
 * out a new one), so what the game's RAM says by then isn't what was drawn */
static struct { int n; WideExt ext[6]; } s_snap;  /* a layer's left and right margins apiece */

static void ext_snapshot(const View *v) {
    WideExt *ext = s_snap.ext;
    s_snap.n = 0;
    /* until the stage's fill is worked out (a few frames into its first round, behind the round's intro) none */
    if (s_fill.next != 4 || s_fill.bg != (int)R16(BACKGROUND)) return;
    if (s_need.gen != s_fill.gen) needs_build();
    uint8_t layers = R8(MAIN_LAYER + 0x7B);
    double m = s_margin, pw = VIEW + 2 * m, to_px = 640.0 / VIEW;
    struct { int on; double xd, dvf; int base, dv, c0[2], c1[2], j0[2], j1[2]; uint64_t rows[2][NEED_WORDS]; } L[3];
    /* the game's screen offset (pixels), which the layout adds to all it places (a hit's shake moves it) */
    fpreg_t ox, oy;
    ox.u = R32(SCREEN_OFFSET_X), oy.u = R32(SCREEN_OFFSET_X + 4);
    for (int k = 0; k < 3; k++) {
        L[k].on = (layers & s_bits[k]) && !wraps(k) && (s_fill.asked & (1 << k));
        if (!L[k].on) continue;
        double xd = fx(s_layers[k] + 0x10) + v->dx[k] / 65536.0;  /* the view's left edge on the layer's map */
        if (s_layers[k] == MAIN_LAYER) xd -= main_qk() * v->q;
        /* the layout draws from its whole unit, and the screen offset's change since the capture moves it */
        L[k].xd = xd = floor(xd) - (ox.f - s_fill.ox[k]) * VIEW / 640.0;
        L[k].base = (int)floor(xd - m - s_fill.x0[k]);
        L[k].dvf = (int16_t)R16(s_layers[k] + 0x62) - s_fill.y[k] - (oy.f - s_fill.oy[k]) / 2.0;  /* rows */
        L[k].dv = (int)lround(L[k].dvf);
        L[k].c0[0] = L[k].c0[1] = 1 << 30, L[k].c1[0] = L[k].c1[1] = -1;
        L[k].j0[0] = L[k].j0[1] = 1 << 30, L[k].j1[0] = L[k].j1[1] = -1;
        memset(L[k].rows, 0, sizeof L[k].rows);
    }
    /* the margins' columns, near to far: what each layer has filled that isn't behind a nearer one. (The middle is
     * the 4:3 game's own picture with its camera at the wide view's: everything's covered there, as it is in the
     * 4:3 game) */
    int order[3], n = 0, back = -1;
    for (int k = 0; k < 3; k++)
        if (L[k].on) order[n++] = k;
    /* the backdrop: the farthest layer laid out, if it's one of these (nothing is behind it); behind a layer that
     * scrolls by itself (20's, 25's clouds: the game draws it all) there's none */
    for (int k = 0; k < 3; k++)
        if ((layers & s_bits[k]) && (back < 0 || layer_z(k) < layer_z(back))) back = k;
    if (back >= 0 && !L[back].on) back = -1;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (layer_z(order[j]) > layer_z(order[i])) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    uint64_t screen[NEED_WORDS];
    for (int wd = 0; wd < NEED_WORDS; wd++)
        screen[wd] = wd * 64 + 64 <= SCREEN_ROWS ? ~0ull : wd * 64 >= SCREEN_ROWS ? 0 : (1ull << (SCREEN_ROWS - wd * 64)) - 1;
    for (int c = 0; c < (int)ceil(pw); c++) {
        if (c == (int)m) c = (int)m + VIEW;  /* past the middle */
        int side = c >= (int)m;
        uint64_t cov[NEED_WORDS] = {0};
        for (int i = 0; i < n; i++) {
            int k = order[i], u = L[k].base + c;
            if (u < 0 || u >= WIDE_MASK_W) continue;
            uint64_t nd[NEED_WORDS], op[NEED_WORDS], any = 0;
            int far = k == back, h = s_fill.atlas[k].h;
            rows_shift(&s_need.need[k][u * NEED_WORDS], L[k].dv, h, far, nd);
            rows_shift(&s_need.opq[k][u * NEED_WORDS], L[k].dv, h, 0, op);
            for (int wd = 0; wd < NEED_WORDS; wd++) {
                any |= nd[wd] & ~cov[wd] & screen[wd];
                cov[wd] |= op[wd] | nd[wd];
                /* its rows: all it fills in the margin, covered or not (a nearer layer is drawn up to a pixel off
                 * its capture: a seam at its edge shows this one's fill) */
                L[k].rows[side][wd] |= nd[wd] & screen[wd];
            }
            if (any) {  /* its columns */
                if (c < L[k].c0[side]) L[k].c0[side] = c;
                L[k].c1[side] = c;
            }
        }
    }
    for (int k = 0; k < 3; k++)
        for (int side = 0; side < 2; side++)
            for (int wd = 0; wd < NEED_WORDS && L[k].on; wd++) {
                uint64_t r = L[k].rows[side][wd];
                if (!r) continue;
                int lo = wd * 64 + __builtin_ctzll(r), hi = wd * 64 + 63 - __builtin_clzll(r);
                if (lo < L[k].j0[side]) L[k].j0[side] = lo;
                if (hi > L[k].j1[side]) L[k].j1[side] = hi;
            }
    /* the backdrop moved up or down from its capture (the camera risen for a jump): its layout leaves rows out at the
     * top, which its fill (behind it) covers: all of the margins */
    if (back >= 0 && L[back].dv) {
        int k = back;
        L[k].c0[0] = 0, L[k].c1[0] = (int)m - 1, L[k].c0[1] = (int)m + VIEW, L[k].c1[1] = (int)ceil(pw) - 1;
        L[k].j0[0] = L[k].j0[1] = 0, L[k].j1[0] = L[k].j1[1] = SCREEN_ROWS - 1;
    }
    static int debug = -1;
    if (debug < 0) debug = getenv("SFA3_WIDE_DEBUG") != NULL;
    extern uint64_t g_vblank_count;
    if (debug && g_vblank_count % 40 == 0)
        for (int k = 0; k < 3; k++)
            if (L[k].on)
                logf_("wide ext: layer %d cols %d..%d, %d..%d base %d dv %.2f (xd %.2f) offset %.2f,%.2f", k, L[k].c0[0],
                      L[k].c1[0], L[k].c0[1], L[k].c1[1], L[k].base, L[k].dvf, L[k].xd, ox.f, oy.f);
    int count = 0;
    for (int k = 0; k < 3; k++)
        for (int side = 0; side < 2; side++) {
            if (!L[k].on || L[k].c1[side] < 0) continue;
            int at = count;
            float zk = layer_z(k);
            while (at > 0 && ext[at - 1].z > zk) ext[at] = ext[at - 1], at--;
            WideExt *e = &ext[at];
            e->z = zk;
            e->q0 = (float)((L[k].c0[side] - 1) * to_px);
            e->q1 = (float)((L[k].c1[side] + 2) * to_px);
            /* its rows (2 guest pixels apiece) and one more each side (the layer's a fraction of a row off); to the
             * picture's edge from its first or last */
            e->r0 = L[k].j0[side] <= 0 ? 0.0f : (float)((L[k].j0[side] - 1) * 2);
            e->r1 = L[k].j1[side] >= SCREEN_ROWS - 1 ? 1e4f : (float)((L[k].j1[side] + 2) * 2);
            e->layer = k;
            e->backdrop = k == back;
            e->u0 = (float)(L[k].xd - m - s_fill.x0[k]);
            e->v0 = (float)L[k].dvf;
            count++;
        }
    s_snap.n = count;
}

/* each layout's state, by its id (widescreen.h): a fight's frame laid out wide, the picture's sides its stage leaves
 * black (pixels: a stage whose layers allow less than the picture's margin, bounded(); the same for the whole
 * fight), its extensions */
#define LISTS 8
typedef struct { uint32_t id; int wide, n, capture; float bar; WideExt ext[6]; } ListState;
static ListState s_lists[LISTS];
static uint32_t s_list_id, s_list_new;

static void publish(void) {  /* the end of a stage layout (hle_stage_layout) */
    uint32_t id = ++s_list_id ? s_list_id : ++s_list_id;
    ListState *e = &s_lists[id % LISTS];
    e->id = id;
    e->wide = s_stage_frame && fighting();
    e->bar = e->wide && s_stage_m < s_margin ? (float)(s_stage_m < 0 ? s_margin : s_margin - s_stage_m) * 640.0f / VIEW : 0;
    e->n = e->wide ? s_snap.n : 0;
    memcpy(e->ext, s_snap.ext, sizeof e->ext);
    e->capture = s_captured;
    s_captured = 0;
    s_list_new = id;
}

uint32_t widescreen_list_begin(void) {
    uint32_t id = s_list_new;
    s_list_new = 0;
    return id;
}

static const ListState *list_state(uint32_t id) {
    return id && s_lists[id % LISTS].id == id ? &s_lists[id % LISTS] : NULL;
}

int widescreen_stage_frame(uint32_t list, float *bar) {
    const ListState *e = list_state(list);
    *bar = e && e->wide ? e->bar : 0;
    return e && e->wide;
}

int widescreen_list_captures(uint32_t list) {
    const ListState *e = list_state(list);
    return e && e->capture;
}

int widescreen_extensions(uint32_t list, WideExt *ext, int max, int *mode) {
    const ListState *e = list_state(list);
    if (!ext_mode() || !e || !e->wide || g_wide_probe) return 0;
    int n = e->n < max ? e->n : max;
    memcpy(ext, e->ext, (size_t)n * sizeof *ext);
    *mode = ext_mode();
    return n;
}

/* f_8C01EBA8: lays out the background layers for the frame, those main +0x7B says (1 layer 1, 2 the main one,
 * 4 layer 3, 8 the stage's own drawing: the floor, f_8C02108C...), the polygons placed now and drawn later.
 * A layer's map is 1024 units wide (its X & 0x3FF); most layers' layouts stop at its end instead of going round
 * to its start (the main layer's does go round) */
void f_8C01EBA8_orig(void);
#define MAP 1024

/* lays out `which` layers (main +0x7B bits) as the view v has them (or as the game does, v NULL; no floor share,
 * flat), each layer's X dx[k] (16.16) further right, all drawn px guest pixels further right. A layout steps some
 * stages' animations as it goes (0A's and 1E's layers 1 and 3, +0x3C and +0x83; 15's layer 1): an extra one (the
 * margins, the captures: all but the view's own) leaves the stage's blocks as it found them, so they step once a
 * frame, as in the 4:3 game (in 0.2.8 they stepped three times a frame: 0A's rain and lightning ran fast) */
static void layout_px(uint8_t which, const View *v, const int32_t *dx, float px, int flat, int extra) {
    uint8_t blocks[STAGE_BLOCKS_SIZE];
    if (extra) memcpy(blocks, ram_ptr(STAGE_BLOCKS), sizeof blocks);
    uint32_t x[3], off = R32(SCREEN_OFFSET_X), q = R32(FLOOR_Q), a = R32(FLOOR_A);
    uint8_t enabled = R8(MAIN_LAYER + 0x7B);
    for (int k = 0; k < 3; k++) {
        x[k] = R32(s_layers[k] + 0x10);
        W32(s_layers[k] + 0x10, x[k] + (uint32_t)dx[k]);
    }
    if (flat) {
        W32(FLOOR_Q, 0);
        W32(FLOOR_A, 0);
    } else if (v && R8(FLOOR + 4) == 1) {
        W32(FLOOR_Q, (uint32_t)(int32_t)(v->q * 65536.0));
        W32(FLOOR_A, (uint32_t)(int32_t)(v->a * 65536.0));
    }
    fpreg_t f;
    f.u = off;
    f.f += px;
    W32(SCREEN_OFFSET_X, f.u);
    W8(MAIN_LAYER + 0x7B, (enabled & ~0x0Fu) | which);
    f_8C01EBA8_orig();
    W8(MAIN_LAYER + 0x7B, enabled);
    W32(SCREEN_OFFSET_X, off);
    W32(FLOOR_Q, q);
    W32(FLOOR_A, a);
    for (int k = 0; k < 3; k++) W32(s_layers[k] + 0x10, x[k]);
    if (extra) memcpy(ram_ptr(STAGE_BLOCKS), blocks, sizeof blocks);
}

/* Testing (control command "probe <what> <x>"): one layer alone, the picture showing its map from x on (a
 * picture of SFA3_WIDE=192: 768 units): what 0-2 (layers 1, main, 3), 3 the stage's own drawing (the floor, at
 * the main layer's X), -1 off */

void widescreen_probe(int what, int x) { s_probe = what, s_probe_x = x; }

static int probe(void) {
    g_wide_probe = 0;
    if (s_probe < 0 || s_probe > 3 || !R8(s_layers[0] + 7) || !R8(MAIN_LAYER + 7) || !R8(s_layers[2] + 7)) return 0;
    g_wide_probe = 1;
    int k = s_probe == 3 ? 1 : s_probe, dx = s_probe_x + s_margin - (int16_t)R16(s_layers[k] + 0x12);
    g_wide_probe_z = s_probe == 3 ? 0 : layer_z(k);
    s_stage_m = s_margin;  /* the whole picture */
    uint8_t which = s_probe == 3 ? 8 : s_bits[k];
    s_stage_frame = 1;
    Sh4Ctx saved = g;
    for (int j = -1; j <= 1; j++) {
        int32_t d[3] = {0, 0, 0};
        d[k] = (dx + j * VIEW) * 0x10000;
        layout(which, NULL, d, j * VIEW, 1);
        g = saved;
    }
    return 1;
}

/* the same, drawn ds units further right; what goes left of the screen is laid out WIDE_LEFT pixels to the right
 * (the renderer moves it back): the layout rounds each tile's place to a whole pixel symmetrically about 0
 * (f_8C0E0E9C), so left of 0 it would round the other way from the view's own tiles; and one pixel further left: at
 * a wall the stage's edge is a fraction of a pixel past the picture's (64 units: 106.67 pixels), which the rounding
 * would leave uncovered (the left margin's tiles reach into the view, which is drawn over them: no seam) */
static void layout(uint8_t which, const View *v, const int32_t *dx, int ds, int extra) {
    layout_px(which, v, dx, (float)ds * 640.0f / VIEW + (ds < 0 ? WIDE_LEFT - 1.0f : 0), 0, extra);
}

static void stage_layout(void) {
    if (!fighting()) {
        s_stage_frame = 0;
        /* the wipe into a fight (+4 = 2), or a fight not yet drawn wide (16:9 just picked): its stage's layers
         * captured, once they're set up (+7), for its fill (before the layout: what that leaves behind is the
         * game's) */
        if (widescreen_wanted() && ext_mode() && R16(GAME_STATE) == 2 &&
            (R16(GAME_STATE + 2) == 2 || R16(GAME_STATE + 2) == 3) && R8(s_layers[0] + 7) && R8(MAIN_LAYER + 7) &&
            R8(s_layers[2] + 7)) {
            uint64_t t0 = SDL_GetTicksNS();
            Sh4Ctx saved = g;
            capture(1);
            g = saved;
            g_perf.wide_ns += SDL_GetTicksNS() - t0;
        }
        f_8C01EBA8_orig();
        return;
    }
    if (probe()) return;
    s_stage_frame = 1;
    /* +7: a layer's set-up step, 0 until the layout's first call has started it: a stage's first frame is laid
     * out once, as the game does it (behind the wipe into the fight) */
    if (!R8(s_layers[0] + 7) || !R8(MAIN_LAYER + 7) || !R8(s_layers[2] + 7)) {
        s_stage_m = bounded();
        s_snap.n = 0;
        f_8C01EBA8_orig();
        return;
    }
    const View *v = view();
    s_stage_m = v->m;
    { static int on = -1; if (on < 0) on = getenv("SFA3_WIDE_LOG") != NULL;  /* SFA3_WIDE_LOG: the view (testing) */
      extern uint64_t g_vblank_count;
      if (on && g_vblank_count % 30 == 0)
          logf_("wide: bg %02X m %d c %.3f cw %.3f | rates %.3f %.3f %.3f -> %.3f %.3f %.3f | dx %.3f %.3f %.3f | q %.4f a %.4f (game %.4f %.4f)",
                R16(BACKGROUND), v->m, v->c, v->cw, v->rate[0], v->rate[1], v->rate[2], v->wide[0], v->wide[1], v->wide[2],
                v->dx[0] / 65536.0, v->dx[1] / 65536.0, v->dx[2] / 65536.0, v->q, v->a, fx(FLOOR_Q), fx(FLOOR_A)); }
    uint8_t enabled = R8(MAIN_LAYER + 0x7B);
    Sh4Ctx saved = g;
    /* each side's margin, layer by layer: what lies a screen (VIEW) left or right of the view; less than a
     * screen when that would take the layer's X past its map's ends, where its layout stops (so the layout
     * reaches the margin; the view's own part of it is laid out twice, the same) */
    for (int side = -1; side <= 1; side += 2)
        for (int k = 0; k < 3; k++) {
            if (!(enabled & s_bits[k])) continue;
            int at = ((int32_t)R32(s_layers[k] + 0x10) + v->dx[k]) >> 16, shift = VIEW;
            if (side < 0 && at < VIEW && at >= s_margin) shift = at;
            if (side > 0 && at + VIEW + s_margin > MAP && MAP - s_margin - at >= s_margin) shift = MAP - s_margin - at;
            int32_t dx[3] = {v->dx[0], v->dx[1], v->dx[2]};
            dx[k] += side * shift * 0x10000;
            layout((uint8_t)(s_bits[k] | (k == 1 ? enabled & 8 : 0)), v, dx, side * shift, 1);
            g = saved;
        }
    if (ext_mode()) {  /* a new stage's layers captured for the filler analysis (before the view: what the
                        * layout leaves behind is the view's, as the game expects) */
        uint64_t t0 = SDL_GetTicksNS();
        capture(0);
        g = saved;
        g_perf.wide_ns += SDL_GetTicksNS() - t0;
    }
    layout(enabled & 0x0F, v, v->dx, 0, 0);  /* the view itself last: what the layout leaves behind is the
                                              * view's, as the game expects */
    if (ext_mode()) {
        uint64_t t0 = SDL_GetTicksNS();
        ext_snapshot(v);
        g_perf.wide_ns += SDL_GetTicksNS() - t0;
    }
    s_view.frame = 0;  /* the frame's done: the next one's view is worked out afresh (a vblank can see two) */
}

void hle_stage_layout(void) {
    uint64_t t0 = SDL_GetTicksNS();
    stage_layout();
    publish();  /* the renderer's, when it draws this frame (widescreen_list_begin) */
    g_perf.layout_ns += SDL_GetTicksNS() - t0;
}

int widescreen_busy(void) { return s_fill.next == 5; }  /* the analysis running (SFA3_FRAME_LOG) */

/* ---------------- objects ----------------
 * The stage program's objects (effect pool C, 8C1853A8, 16 x 0xB0) that are fixed to the screen (layer 0) but
 * place themselves by the camera, as a far layer moves (stage 06's city behind the window, 0.875 as fast as the
 * camera): how fast each moves, from where it was as it appeared (the camera then in the middle, as a rule);
 * 0 (fixed: not moved) until the camera has moved 16 units from there. The HUD's objects are elsewhere. */
#define POOL_C 0x8C1853A8u
static struct { uint32_t kind; int x0, c0; double rate; } s_own[16];

static double own_rate(uint32_t obj) {
    unsigned i = (obj - POOL_C) / 0xB0u;
    uint32_t kind = R8(obj) ? R16(obj + 2) | 0x10000u : 0;  /* active, type and subtype */
    int x = (int16_t)R16(obj + 0x12), c = (int16_t)R16(MAIN_LAYER + 0x12);
    if (s_own[i].kind != kind) {
        s_own[i].kind = kind, s_own[i].x0 = x, s_own[i].c0 = c, s_own[i].rate = 0;
    } else if (c - s_own[i].c0 >= 16 || c - s_own[i].c0 <= -16) {
        double r = -(double)(x - s_own[i].x0) / (c - s_own[i].c0);
        s_own[i].rate = r < 0 || r > 2 ? 0 : r;
    }
    return s_own[i].rate;
}

/* f_8C01A08E(ctx, obj): an object's screen position (ctx +0x6C X, +0x6E Y): its own less its scroll layer's X
 * (obj +0x0E: 1-3; 0 fixed to the screen: the HUD's, and the stage's own_rate() ones), plus 8C188A08 x its
 * depth (obj +0x26) on the main layer. In the wide view it moves as its layer does there; an own_rate() one
 * keeps its speed against the main layer's (06's city, seen through the main layer's window) */
/* the sprite part drawer's edges for an object (hle_sprite_draw, f_8C018E9C): the picture's; but a screen-fixed
 * object's (obj +0x0E 0: the HUD's) the 4:3 screen's, as the game has them: the combo counter's banner starts its
 * slide in just past the screen's right edge, where the 4:3 game never shows it. The stage's own screen-fixed
 * objects that place themselves by the camera (pool C, own_rate(): 06's city) the picture's. */
float widescreen_sprite_cull(uint32_t obj) {
    if (!g_wide_cull || R8(obj + 0x0E)) return g_wide_cull;
    if (obj >= POOL_C && obj < POOL_C + 16 * 0xB0u && (obj - POOL_C) % 0xB0u == 0) return g_wide_cull;
    return 0;
}

void f_8C01A08E_orig(void);
void hle_scroll_object(void) {
    uint32_t ctx = g.r[4], obj = g.r[5], layer = R8(obj + 0x0E);
    f_8C01A08E_orig();
    if (!fighting() || layer > 3 || g_wide_probe) return;
    const View *v = view();
    int d;
    if (!layer) {
        if (obj < POOL_C || obj >= POOL_C + 16 * 0xB0u || (obj - POOL_C) % 0xB0u) return;
        /* seen through the main layer (06's city behind its window): its speed against the main layer's as the
         * game's */
        double r = own_rate(obj), mid = fx(MAIN_LAYER + 8);
        if (r <= 0) return;
        double rw = v->wide[1] - (v->rate[1] - r);
        double move = rw * (v->cw - mid) - r * (v->c - mid);  /* how much further right a layer of its speed is */
        d = -(int)(move < 0 ? move - 0.5 : move + 0.5);
    } else {
        int32_t x = (int32_t)R32(s_layers[layer - 1] + 0x10);
        d = -(((x + v->dx[layer - 1]) >> 16) - (x >> 16));
        int16_t depth = (int16_t)R16(obj + 0x26);
        if (layer == 2 && depth && R8(FLOOR + 4) == 1) {  /* the depth's share, with the wide view's q */
            int32_t q = (int32_t)R32(FLOOR_Q), qw = (int32_t)(v->q * 65536.0);
            d += (int16_t)((uint32_t)(qw * depth) >> 16) - (int16_t)((uint32_t)(q * depth) >> 16);
        }
    }
    W16(ctx + 0x6C, R16(ctx + 0x6C) + (uint32_t)d);
}
