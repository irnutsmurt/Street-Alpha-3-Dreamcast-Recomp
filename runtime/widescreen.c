/* Widescreen in fights (Option Mode > DISPLAY > WIDESCREEN).
 *
 * The game draws for a 384 x 224 screen (its units; 640 x 448 pixels here).
 * With widescreen on, the picture is wider by `margin` units on each side, as
 * many as the window's shape asks for (16:9: 64 a side). The game itself is
 * unchanged: its camera, the walls (the camera's limits) and so the furthest
 * the fighters can be apart are the 4:3 game's; only what is drawn changes:
 *
 *  - only the fight's own screen is wide (fighting()); the others (menus,
 *    the win quote, the map, the VS screen, the wipes between them) are
 *    4:3 in the middle of the picture, the renderer clipping what the game
 *    draws past their edges;
 *  - the HUD stays where it is, in the middle (the renderer moves everything
 *    right by the margin);
 *  - the stage is laid out more than once a frame (hle_stage_layout): each
 *    layer once more for the margin on each side, placed by the global
 *    screen offset the layout adds (8C18EF4C), then the view itself;
 *  - the wide view is the 4:3 game's own picture with the camera d units
 *    from where the game has it (view()): 0 in the middle of the stage, a
 *    margin inward at the walls, so as not to show past them (most stages
 *    have nothing there). Each layer moves as it would with the camera
 *    there (by its rate, rate()), the floor's terms (8C188A08, 8C188A5C) are
 *    the camera's there while the layout runs, the objects on a layer move
 *    with it and the stage's objects that follow the camera themselves as
 *    they do (hle_scroll_object); the HUD doesn't. So the layers line up as
 *    the game lines them up (some are cut to fit each other);
 *  - but a layer moving less than the camera has art for less than the wide
 *    view (the 4:3 game shows it from where it is with the camera at one wall
 *    to where it is at the other, plus a screen): black bars at the sides
 *    cover what lies past it (the renderer clips the picture), and the
 *    camera's d is held back so that a bar never covers the game's own view;
 *  - the sprite drawer's edges (it leaves out the tiles past the screen's)
 *    are as far out as the picture's (g_wide_cull), and so are those past
 *    which a stage's objects aren't drawn (f_8C02472E: 64 units out, then
 *    g_wide_units more): recompile.py's REPLACE.
 *
 * Online games are 4:3: what the game draws is in its RAM (the polygons
 * waiting for the frame's end, the sprites' screen positions), and netplay.c
 * compares the two players' RAM to catch a desync. The game's state is the
 * same either way (tested: the same inputs, the fighters, game state and
 * random number the same, frame for frame).
 *
 * The background layers: 8C188818, 8C1888B4 (the main one, the camera) and
 * 8C188950 (objects' layers 1, 2 and 3), 0x9C bytes each; +0x10 the scroll X
 * (16.16, units). The main layer's +0x20 is the camera's X (16.16; +0x12 its
 * whole part), +8 the stage's middle (448), +0x44 / +0x46 the camera's limits
 * (0x100 and 0x280 on every stage). A far layer's X (f_8C02291C, f_8C022984)
 * is its +8 + its +0x50 (scrolling by itself on some stages) + its +0x58
 * (whole units) + k x (camera - middle - 8C188A5C) / 16, k from a table per
 * stage, 8C188A5C (f_8C0223AA) itself a fraction of camera - middle: all in
 * all its middle position plus a rate x (camera - middle), which rate()
 * works out from where it is. See docs/dreamcast.md, Stage backgrounds. */
#include <stdlib.h>

#include "cpu.h"
#include "netplay.h"
#include "runtime.h"
#include "settings.h"

#define MAIN_LAYER 0x8C1888B4u
#define SCREEN_OFFSET_X 0x8C18EF4Cu  /* float pixels, added to everything the stage layout and sprites place */
#define VIEW 384                     /* the game's screen width, units */
#define MARGIN_MAX 192               /* the wide view must fit between the walls: 2 x margin <= the camera's range */

static const uint32_t s_layers[3] = {0x8C188818u, MAIN_LAYER, 0x8C188950u};  /* objects' layers 1, 2, 3 */
static int s_wide;         /* the picture's margin, units each side (0: 4:3): the renderer's */
static int s_margin;       /* the margin the game is drawn with: s_wide, 0 in an online game */
static int s_stage_frame;  /* the stage was laid out since the renderer last asked */
static double s_bar_l, s_bar_r;  /* this frame's black bars, units (view()) */
static double s_view_d;          /* this frame's view: the game's 4:3 view d units right in the picture (view()) */

int widescreen_margin(void) { return s_wide; }
float widescreen_margin_px(void) { return (float)s_wide * 640.0f / (float)VIEW; }

/* the margin (units) that fills a window (or screen) of this shape, width / height (the renderer asks each
 * frame, and applies a new one with widescreen_set between frames) */
int widescreen_wanted(float aspect) {
    int m = 0;
    if (netplay_active()) return 0;
    static int forced = -2;  /* SFA3_WIDE=<units>: a margin whatever the window (testing headless) */
    if (forced == -2) forced = getenv("SFA3_WIDE") ? atoi(getenv("SFA3_WIDE")) : -1;
    if (forced >= 0) m = forced;
    else if (g_settings.widescreen && aspect > 4.0f / 3.0f) m = (int)((aspect * 480.0f - 640.0f) / 2 * VIEW / 640.0f + 0.5f);
    return m < 0 ? 0 : m > MARGIN_MAX ? MARGIN_MAX : m;
}

float g_wide_cull;
int g_wide_units;
/* each frame (video_vblank), and when the renderer changes the picture's margin */
void widescreen_frame(void) {
    s_margin = netplay_active() ? 0 : s_wide;
    g_wide_cull = s_margin ? (float)(int)(widescreen_margin_px() + 0.5f) : 0.0f;
    g_wide_units = 2 * s_margin;  /* the margin and the most a layer slides */
}

void widescreen_set(int units) {
    s_wide = units;
    widescreen_frame();
}

/* whether the stage was laid out since the last call (a fight's frame, drawn wide), its black bars at the
 * picture's sides and where the game's own 4:3 view is (its left edge, from where the HUD's is), pixels */
int widescreen_stage_frame(float *bar_l, float *bar_r, float *view) {
    int f = s_stage_frame;
    s_stage_frame = 0;
    *view = f && s_margin ? (float)(s_view_d * 640 / VIEW) : 0.0f;
    float all = (float)(int)(widescreen_margin_px() + 0.5f);  /* the picture's margin, whole pixels */
    *bar_l = !f || !s_margin ? 0.0f : s_bar_l >= s_margin ? all : (float)(s_bar_l * 640 / VIEW);
    *bar_r = !f || !s_margin ? 0.0f : s_bar_r >= s_margin ? all : (float)(s_bar_r * 640 / VIEW);
    return f;
}

static double fx(uint32_t a) { return (int32_t)R32(a) / 65536.0; }

/* a fight's own screen (game state +2 = 2, +4 = 3: the round's intro, the round, the KO and the win pose); the
 * stage is laid out on the screens either side of it too (the win quote, the wipe into the next fight), which
 * stay 4:3 */
static int fighting(void) { return s_margin && R16(0x8C17FBC2u) == 2 && R16(0x8C17FBC4u) == 3; }

#define FLOOR_Q 0x8C188A08u  /* (camera - middle) / a stage's constant, 16.16 (f_8C0223AA: the floor's block +0x1C) */
#define FLOOR_A 0x8C188A5Cu  /* that x a second constant (the floor's block +0x70) */
#define FLOOR 0x8C1889ECu

static double cam_from_middle(void) { return fx(MAIN_LAYER + 0x20) - fx(MAIN_LAYER + 8); }

/* How fast each layer moves across the screen against the camera (the fighters move 1): layers 1 and 3 their
 * X's rate (their X less their middle position, over the camera's distance from the middle); the main layer 1
 * less what its layout takes off its X (f_8C020804: 8C188A5C, or 8 x 8C188A08 on stage 00 (+3 0xA0), or
 * 8C188A08 x (floor +0x84 - floor +0x82 + 2) on stage 04 (0xA4); f_8C01F57C (stages 15, 20) 8C188A5C;
 * f_8C020AD8 (0A, 1E) nothing), all of them some fraction of camera - middle. With the camera in the middle
 * the last ones worked out on this background (8C17FC60); 1 for what isn't a layer's usual motion. */
static double s_rates[3] = {1, 1, 1};
static int s_rates_bg = -1;
static double rate(int k) {
    int bg = R16(0x8C17FC60);
    if (bg != s_rates_bg) s_rates_bg = bg, s_rates[0] = s_rates[1] = s_rates[2] = 1;
    double cam = cam_from_middle(), r;
    if (cam > -1 && cam < 1) return s_rates[k];
    if (s_layers[k] == MAIN_LAYER) {
        int kind = R8(MAIN_LAYER + 3);
        double off = fx(FLOOR_A);
        if (kind == 0xA0) off = 8 * fx(FLOOR_Q);
        else if (kind == 0xA4) off = fx(FLOOR_Q) * ((int16_t)R16(FLOOR + 0x84) - (int16_t)R16(FLOOR + 0x82) + 2);
        else if (kind == 0xAA || kind == 0xBE) off = 0;
        r = 1 - off / cam;
    } else {
        double home = fx(s_layers[k] + 8) + fx(s_layers[k] + 0x50) + (int16_t)R16(s_layers[k] + 0x58);
        r = (fx(s_layers[k] + 0x10) - home) / cam;
    }
    return s_rates[k] = r < -0.05 || r > 2 ? 1 : r;
}

/* Where the wide view is: the camera d units right of the game's (the fighters slide d, each layer its rate x
 * d), and how many units of each margin are black (bars).
 *
 * The 4:3 game shows a layer from where it is with the camera at the left wall (lo) to where it is with the
 * camera at the right one (hi), plus a screen: that is its art, for all we know. The camera stays a margin
 * from the walls so as not to show past them: d is 0 in the middle, -margin at the left wall (the wall at the
 * picture's left edge), +margin at the right one. A layer moving less than the camera has art for less than
 * the wide view: in the middle it runs out margin - rate x 192 units into each margin, at a wall (1 - rate) x
 * margin into the wall's side after the slide. Those are black; and d is held back so that a bar never covers
 * the game's own view (where the fighters are): at the wall itself it stays 0 for any layer slower than the
 * camera, its margin all bar. Layers scrolling by themselves (+0x50) go round (the clouds); for a layer moving
 * nearly with the camera (0A's, 0.95) up to 4 units past its art are let through (it is painted on there; the
 * slower ones aren't, as a rule: the caves' are black) */
static double spare(double r) { return r >= 0.9 ? 4 : 0; }
static double view(double *bar_l, double *bar_r) {
    double lo = (int16_t)R16(MAIN_LAYER + 0x44), hi = (int16_t)R16(MAIN_LAYER + 0x46), m = s_margin;
    double c = fx(MAIN_LAYER + 0x10), v = c - m;
    if (hi - lo < 2 * m) v = (lo + hi) / 2 - m;  /* a stage narrower than the wide view: centred */
    else if (v < lo) v = lo;
    else if (v > hi - 2 * m) v = hi - 2 * m;
    double d = c - m - v, dmin = -m, dmax = m, r[3];
    uint8_t layers = R8(MAIN_LAYER + 0x7B);
    for (int k = 0; k < 3; k++) {
        r[k] = 1;
        if (!(layers & (1 << k)) || (k != 1 && R32(s_layers[k] + 0x50)) || (r[k] = rate(k)) >= 1) continue;
        double a = -(r[k] * (c - lo) + spare(r[k])) / (1 - r[k]), b = (r[k] * (hi - c) + spare(r[k])) / (1 - r[k]);
        dmin = a > dmin ? a : dmin;
        dmax = b < dmax ? b : dmax;
    }
    d = d < dmin ? dmin : d > dmax ? dmax : d;
    if (bar_l) {
        double l = m - (c - d - lo), rr = m - (hi - c + d);  /* the walls' */
        for (int k = 0; k < 3; k++) {
            if (r[k] >= 1) continue;
            double lk = m - r[k] * (c - d - lo) - spare(r[k]), rk = m - r[k] * (hi - c + d) - spare(r[k]);
            l = lk > l ? lk : l;
            rr = rk > rr ? rk : rr;
        }
        *bar_l = l < 0 ? 0 : l > m ? m : l;
        *bar_r = rr < 0 ? 0 : rr > m ? m : rr;
    }
    return d;
}

static int whole(double d) { return (int)(d < 0 ? d - 0.5 : d + 0.5); }

/* how far right layer k's X moves, 16.16 units: the camera's d for the main layer (its layout takes the
 * floor's share off itself, as 8C188A5C / 8C188A08 are set for the wide view's camera while it runs), a far
 * layer's rate x d. The layout uses the whole part, as the game does, so the layers line up exactly as the
 * game's at the wide view's camera */
static int32_t slide(int k) {
    double d = view(NULL, NULL);
    return (int32_t)whole((s_layers[k] == MAIN_LAYER ? d : d * rate(k)) * 65536.0);
}

/* The stage program's objects (effect pool C, 8C1853A8, 16 x 0xB0) that are fixed to the screen (layer 0) but
 * place themselves by the camera, as a far layer moves (stage 06's city behind the window, 0.875 as fast as the
 * camera): how fast each moves, from where it was as it appeared (the camera then in the middle, as a rule);
 * 0 (fixed) until the camera has moved 16 units from there. The HUD's objects are elsewhere. */
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
        s_own[i].rate = r < 0 || r > 1.5 ? 0 : r;
    }
    return s_own[i].rate;
}

/* f_8C01A08E(ctx, obj): an object's screen position (ctx +0x6C X, +0x6E Y) less its scroll layer's (obj +0x0E:
 * 1-3; 0 fixed to the screen: the HUD's, and the stage's own_rate() ones). One on the main layer with a depth
 * (obj +0x26) also moves by 8C188A08 x depth: 8C188A08 is (camera - middle) / a stage's constant (f_8C0223AA),
 * so such an object moves 1 - 8C188A08 x depth / (camera - middle) as fast as the camera */
void f_8C01A08E_orig(void);
void hle_scroll_object(void) {
    uint32_t ctx = g.r[4], obj = g.r[5], layer = R8(obj + 0x0E);
    f_8C01A08E_orig();
    if (!fighting() || layer > 3) return;
    double cam = cam_from_middle();
    int16_t depth = (int16_t)R16(obj + 0x26);
    int d;
    if (!layer) {
        if (obj < POOL_C || obj >= POOL_C + 16 * 0xB0u || (obj - POOL_C) % 0xB0u) return;
        d = whole(slide(1) / 65536.0 * own_rate(obj));
    } else if (layer == 2 && depth && (cam <= -1 || cam >= 1)) {  /* + the depth's share of the camera's move */
        int32_t x = (int32_t)R32(MAIN_LAYER + 0x10), s = slide(1), q = (int32_t)R32(FLOOR_Q);
        int32_t q2 = (int32_t)whole(q * (cam - s / 65536.0) / cam);
        d = (x >> 16) - ((x - s) >> 16) + (int16_t)((uint32_t)(q2 * depth) >> 16) - (int16_t)((uint32_t)(q * depth) >> 16);
    } else {  /* by as much as its layer's whole part (what the object's position is less) moves */
        int32_t x = (int32_t)R32(s_layers[layer - 1] + 0x10);
        d = (x >> 16) - ((x - slide((int)layer - 1)) >> 16);
    }
    W16(ctx + 0x6C, R16(ctx + 0x6C) + (uint32_t)d);
}

/* f_8C01EBA8: lays out the background layers for the frame, those main +0x7B says (1 layer 1, 2 the main one,
 * 4 layer 3, 8 the stage's own drawing: the floor, f_8C02108C...), the polygons placed now and drawn later.
 * A layer's map is 1024 units wide (its X & 0x3FF); most layers' layouts stop at its end instead of going round
 * to its start (the main layer's does go round) */
void f_8C01EBA8_orig(void);
#define MAP 1024
static const uint8_t s_bits[3] = {1, 2, 4};

/* lays out `which` layers (main +0x7B bits), layer k's X moved by add[k] (16.16 units), all drawn `units`
 * further right, with the camera `moved` (16.16) right of the game's */
static void layout(uint8_t which, const int32_t *add, int units, int32_t moved) {
    uint32_t x[3], off = R32(SCREEN_OFFSET_X), q = R32(FLOOR_Q), a = R32(FLOOR_A);
    uint8_t enabled = R8(MAIN_LAYER + 0x7B);
    double cam = cam_from_middle(), at = cam - moved / 65536.0;  /* the camera's distance from the middle there */
    if (cam <= -1 || cam >= 1) {
        W32(FLOOR_Q, (uint32_t)whole((int32_t)q * at / cam));
        W32(FLOOR_A, (uint32_t)whole((int32_t)a * at / cam));
    }
    for (int k = 0; k < 3; k++) {
        x[k] = R32(s_layers[k] + 0x10);
        W32(s_layers[k] + 0x10, x[k] + (uint32_t)add[k]);
    }
    fpreg_t f;
    f.u = off;
    f.f += (float)units * 640.0f / VIEW;
    W32(SCREEN_OFFSET_X, f.u);
    W8(MAIN_LAYER + 0x7B, (enabled & ~0x0Fu) | which);
    f_8C01EBA8_orig();
    W8(MAIN_LAYER + 0x7B, enabled);
    W32(FLOOR_Q, q);
    W32(FLOOR_A, a);
    W32(SCREEN_OFFSET_X, off);
    for (int k = 0; k < 3; k++) W32(s_layers[k] + 0x10, x[k]);
}

void hle_stage_layout(void) {
    if (!fighting()) {
        f_8C01EBA8_orig();
        return;
    }
    s_stage_frame = 1;
    /* +7: a layer's set-up step, 0 until the layout's first call has started it: a stage's first frame is laid
     * out once, as the game does it */
    if (!R8(s_layers[0] + 7) || !R8(MAIN_LAYER + 7) || !R8(s_layers[2] + 7)) {
        s_bar_l = s_bar_r = s_margin;
        s_view_d = 0;
        f_8C01EBA8_orig();
        return;
    }
    double d = s_view_d = view(&s_bar_l, &s_bar_r);
    int32_t e[3], add[3];
    for (int k = 0; k < 3; k++) e[k] = (int32_t)whole((k == 1 ? d : d * rate(k)) * 65536.0);
    uint8_t enabled = R8(MAIN_LAYER + 0x7B);
    Sh4Ctx saved = g;
    /* each side's margin, layer by layer: what lies a screen (VIEW) left or right of the view's X; less than a
     * screen when that would take the layer's X past its map's ends, where its layout stops (so the
     * layout reaches the margin; the view's own part of it is laid out twice, the same) */
    for (int side = -1; side <= 1; side += 2)
        for (int k = 0; k < 3; k++) {
            if (!(enabled & s_bits[k])) continue;
            int view = ((int32_t)R32(s_layers[k] + 0x10) - e[k]) >> 16, shift = VIEW;
            if (side < 0 && view < VIEW && view >= s_margin) shift = view;
            if (side > 0 && view + VIEW + s_margin > MAP && MAP - s_margin - view >= s_margin) shift = MAP - s_margin - view;
            for (int j = 0; j < 3; j++) add[j] = -e[j] + (j == k ? side * shift * 0x10000 : 0);
            layout((uint8_t)(s_bits[k] | (k == 1 ? enabled & 8 : 0)), add, side * shift, e[1]);
            g = saved;
        }
    for (int k = 0; k < 3; k++) add[k] = -e[k];
    layout(enabled & 0x0F, add, 0, e[1]);  /* the view itself last: what the layout leaves behind is the view's, as the game expects */
}
