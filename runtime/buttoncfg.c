/* Option Mode > BUTTON CONFIG, rebuilt for PC controls.
 *
 * The Dreamcast screen lists each controller button (X Y L A B R) with the
 * attack it does, then VIBRATE, COMMAND, DEFAULT and EXIT, one column per
 * player, each moved with that player's own controller. Here each column
 * starts with a grid of the attacks, punches on the left and kicks on the
 * right, the bottom row being all three at once:
 *
 *     X   L (fist)    A   L (foot)
 *     Y   M (fist)    B   M (foot)
 *     RB  H (fist)    RT  H (foot)
 *     LB  3 (fist)    LT  3 (foot)
 *     VIBRATE  OFF
 *     DEVICE   AUTO
 *     COMMAND  ARCADE
 *     DEFAULT
 *     EXIT
 *
 * Each cell shows the key or gamepad button that does it (TRI, SQR, LSH...);
 * choosing a cell and pressing a new key / button rebinds it (Esc / Back
 * cancels). DEVICE (AUTO, XBOX, PS, SWITCH, KEYBOARD) picks the device and
 * the button names; VIBRATE and COMMAND are the game's own; DEFAULT also
 * restores the player's bindings. The bindings live in controls.c (shared
 * with the F1 menu's CONTROLS page). The game's own attack per Dreamcast
 * button is kept at its default here (the input already goes through it),
 * so the game's icons are the attacks' icons.
 *
 * The screen exists twice: in Option Mode, and in the pause menu during a
 * fight (its own copy, 8 units higher, with its own cursors and objects; a
 * third player's single-column version is left as it is). For each, the
 * draw function is replaced (it printed the rows through the game's print
 * routine, and so does this one), and the logic runs as before, after the
 * presses handled here are taken out of the input. The game's cursor is kept
 * on a row whose handling applies (VIBRATE, COMMAND, DEFAULT, EXIT) or does
 * nothing; its cursor bar and the attack icons are moved to this layout, and
 * the renderer narrows the bar to a grid cell and adds the 3-punch / 3-kick
 * icons from the game's own sprite sheet (sprite_swap.c). */
#include <stdio.h>
#include <string.h>

#include "controls.h"
#include "cpu.h"
#include "gameui.h"
#include "netplay.h"
#include "sprite_swap.h"

#define BUTTON_CONFIG 0x8C09B18Cu  /* attack per Dreamcast button (16 bits, 12 bytes per player); +48 VIBRATE
                                      off bits; +57 / +58 COMMAND */
/* the DISPLAY screen's string arena (never shown at the same time) */
#define TEXT_ARENA 0x8C07BE40u
#define TEXT_ARENA_END 0x8C07C1D0u

enum { ROW_GRID = 4, ROW_VIBRATE = 4, ROW_DEVICE, ROW_COMMAND, ROW_DEFAULT, ROW_EXIT, ROW_COUNT };
/* the game's own rows: 6 VIBRATE, 7 COMMAND, 8 DEFAULT, 9 EXIT; 0 (an
 * attack row, whose presses are taken here) for the rest */
static const int s_game_row[ROW_COUNT] = {0, 0, 0, 0, 6, 0, 7, 8, 9};
#define LAYER 1
#define COLOUR 11
#define GLYPH_W 10   /* this screen's font advance, in print units */
#define ROW_STEP 16
/* print units (a 384x240 screen). Each player's rows are centred under the
 * PLAYER 1 / PLAYER 2 banner: the grid (two cells: a three-character label,
 * then the attack icon, 32 wide) and the label / value block below, as wide
 * as the game's cursor bar (156). */
#define CENTRE(p) ((p) ? 264 : 88)
#define BLOCK_W 156
#define VALUE_DX 72  /* the value column, as the game's */
/* the labels below the grid: label and value (six characters, as most are)
 * centred */
#define COL_X(p) (CENTRE(p) - (VALUE_DX + 60) / 2)
#define ICON_DX 36   /* the attack icon after a cell's label */
#define CELL_W 84    /* from one cell to the next */
#define GRID_W (CELL_W + ICON_DX + 32)
#define BAR_W 72     /* the cursor bar on a cell */
#define UNITS_TO_PX (640.0f / 384.0f)

typedef struct {
    uint32_t cursor[2];    /* the game's cursor row, per player */
    /* The cursor bar's position per game row (x, y; y counts up from the
     * bottom, 239 - the row's text y), read by the bar object each frame */
    uint32_t bar[2];
    int top;               /* the first row's text y; rows are 16 apart */
    uint32_t str_vibrate, vibrate_values, str_command, command_values, str_default, str_exit;  /* the game's */
    uint32_t caller;       /* the replaced draw function (call site for the print routine) */
    /* state */
    int row[2], col[2];
    uint64_t last_frame;
} Screen;

static Screen s_option = {
    {0x8C17FBC5u, 0x8C17FBC6u}, {0x8C0CF37Cu, 0x8C0CF3A4u}, 52,
    0x8C0974BCu, 0x8C0CF1E0u, 0x8C0974C4u, 0x8C0CF1D4u, 0x8C0974ACu, 0x8C0974CCu, 0x8C07C430u, {0, 0}, {0, 0}, 0,
};
static Screen s_pause = {
    {0x8C1D1D18u, 0x8C1D1D19u}, {0x8C0BD8ACu, 0x8C0BD8D4u}, 44,
    0x8C097668u, 0x8C0CF28Cu, 0x8C097670u, 0x8C0CF280u, 0x8C097678u, 0x8C097680u, 0x8C07D2E0u, {0, 0}, {0, 0}, 0,
};

static int row_y(const Screen *sc, int r) { return sc->top + r * ROW_STEP; }
static int cell_x(int p, int c) { return CENTRE(p) - GRID_W / 2 + c * CELL_W; }

/* the control a grid cell binds */
static int cell_action(int r, int c) {
    if (r == 3) return c ? ACT_3K : ACT_3P;
    return (c ? ACT_LK : ACT_LP) + r;
}

extern uint64_t g_vblank_count;

static int s_hold_off[2];  /* after a capture: ignore the player until all buttons are released */
static int s_was_capturing = -1;
static MenuInput s_in[2];

static uint32_t pad_base(int p) { return PADS + (uint32_t)p * PAD_STRIDE; }

/* hide presses from the game: no new press or release edges (held stays as
 * is, so the presses found here stay right); `bits`: which buttons */
static void hide(int p, uint32_t bits) {
    uint32_t b = pad_base(p);
    W32(b + 0x0C, (R32(b + 0x0C) & ~bits) | (R32(b + 0x08) & bits));
    W32(b + 0x10, R32(b + 0x10) & ~bits);
    W32(b + 0x14, R32(b + 0x14) & ~bits);
}
static void consume(int p) { hide(p, 0xFFFFFFFFu); }

static int screen_opened(Screen *sc) {
    int opened = g_vblank_count - sc->last_frame > 4 || g_vblank_count < sc->last_frame;
    if (opened) sc->row[0] = sc->row[1] = sc->col[0] = sc->col[1] = 0;
    return opened;
}

/* this frame's row for the game's cursor, its cursor bar, and the game's
 * attack per button kept at the default */
static void place(Screen *sc, int p) {
    int r = sc->row[p], g_row = s_game_row[r];
    W8(sc->cursor[p], (uint32_t)g_row);
    for (int k = 0; k < 10; k++) {
        W16(sc->bar[p] + (uint32_t)k * 4, (uint32_t)(CENTRE(p) - BLOCK_W / 2));
        W16(sc->bar[p] + (uint32_t)k * 4 + 2, (uint32_t)(239 - (sc->top + k * 16)));
    }
    int bx = r < ROW_GRID ? cell_x(p, sc->col[p]) - 3 : CENTRE(p) - BLOCK_W / 2;
    W16(sc->bar[p] + (uint32_t)g_row * 4, (uint32_t)bx);
    W16(sc->bar[p] + (uint32_t)g_row * 4 + 2, (uint32_t)(239 - row_y(sc, r)));
    if (r < ROW_GRID) sprite_swap_bar(p, bx * UNITS_TO_PX, BAR_W * UNITS_TO_PX);
    else sprite_swap_bar(p, -1, 0);
    for (int j = 0; j < 6; j++) W16(BUTTON_CONFIG + (uint32_t)p * 12 + (uint32_t)j * 2, (uint32_t)j);
}

/* The game plays its cursor sound on its own: whenever its cursor row
 * changes (which this layout does behind the scenes) and, on the pause
 * screen, on up / down. On these screens the cursor sound is this file's
 * alone: one per move. */
static int s_own_sound;
static void sound(int id) {
    s_own_sound = 1;
    gameui_sound(id);
    s_own_sound = 0;
}
static int screen_active(const Screen *sc) { return sc->last_frame && g_vblank_count - sc->last_frame <= 4; }
int buttoncfg_mutes_sound(void) {
    return g.r[5] == SND_CURSOR && !s_own_sound && (screen_active(&s_option) || screen_active(&s_pause));
}

/* the capture that just ended (bound or cancelled), once per frame */
static void capture_ended(void) {
    int cap = controls_capture_player();
    if (s_was_capturing >= 0 && cap < 0) {
        gameui_sound(SND_OK);
        s_hold_off[s_was_capturing] = 1;
    }
    s_was_capturing = cap;
}

/* one player's presses on this screen */
static void handle_player(Screen *sc, int p, int opened) {
    int cap = controls_capture_player();
    /* presses found here from the held buttons: hiding presses from the game
     * makes its own "newly pressed" bits fire again next frame */
    uint32_t held = R32(pad_base(p) + 0x08);
    uint32_t pressed = gameui_presses(&s_in[p], held, opened);
    hide(p, PAD_UP | PAD_DOWN);  /* rows are moved here */
    int row = sc->row[p];
    if (cap == p) { consume(p); place(sc, p); return; }
    if (s_hold_off[p]) {
        if (held) { consume(p); place(sc, p); return; }
        s_hold_off[p] = 0;
    }
    if (pressed & (PAD_UP | PAD_DOWN)) {
        sc->row[p] = (row + ((pressed & PAD_UP) ? ROW_COUNT - 1 : 1)) % ROW_COUNT;
        sound(SND_CURSOR);
        consume(p);
    } else if (row < ROW_GRID) {
        if (pressed & (PAD_LEFT | PAD_RIGHT)) {
            sc->col[p] ^= 1;
            sound(SND_CURSOR);
        } else if ((pressed & PAD_OK) && cap < 0 && !netplay_active()) {  /* a session's games must not differ */
            controls_capture_begin(p, cell_action(row, sc->col[p]));
            s_was_capturing = p;
            gameui_sound(SND_OK);
        }
        consume(p);
    } else if (row == ROW_DEVICE) {
        if (pressed & (PAD_LEFT | PAD_RIGHT | PAD_OK)) {
            controls_set_type(p, controls_type(p) + ((pressed & PAD_LEFT) ? -1 : 1));
            sound(SND_CURSOR);
        }
        consume(p);
    } else if ((row == ROW_VIBRATE || row == ROW_COMMAND) && (pressed & (PAD_LEFT | PAD_RIGHT))) {
        sound(SND_CURSOR);  /* the game changes the value (its own sound is muted here) */
    } else if (row == ROW_DEFAULT && (pressed & PAD_OK)) {
        controls_reset(p);  /* the game restores its own settings itself */
        controls_set_type(p, CTL_AUTO);
    }
    place(sc, p);
}

/* ---------------- Option Mode > BUTTON CONFIG ---------------- */

void f_8C07B7B4_orig(void);
void hle_buttoncfg_logic(void) {
    uint32_t r4 = g.r[4], r5 = g.r[5];
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, 0x8C07B7B4u);  /* for the sounds */
    int opened = screen_opened(&s_option);
    capture_ended();
    for (int p = 0; p < 2; p++) handle_player(&s_option, p, opened);
    s_option.last_frame = g_vblank_count;
    g.r[4] = r4;
    g.r[5] = r5;
    f_8C07B7B4_orig();
    for (int p = 0; p < 2; p++) place(&s_option, p);  /* in case the game moved its cursor */
}

int buttoncfg_active(void) { return g_vblank_count - s_option.last_frame <= 4; }

/* an attack icon's place: Dreamcast button k (attack k) in its grid cell */
static void icon_xy(const Screen *sc, int p, int k, int *x, int *y) {
    *x = cell_x(p, k >= 3) + ICON_DX;
    *y = 240 - row_y(sc, k % 3);  /* counting up */
}

/* Option Mode objects on this screen (+0x12 x, +0x16 y counting up): the
 * attack icons (0x24-0x29 player 1, 0x2A-0x2F player 2, X Y L A B R) */
void buttoncfg_place_icon(uint32_t obj, int type) {
    if (type < 0x24 || type > 0x2F) return;
    int x, y;
    icon_xy(&s_option, type >= 0x2A, (type - 0x24) % 6, &x, &y);
    W16(obj + 0x12, (uint32_t)x);
    W16(obj + 0x16, (uint32_t)y);
}

/* ---------------- pause menu > BUTTON CONFIG ----------------
 * Players 1 and 2 have their own logic (0x8C07CD5A, 0x8C07CE14); the screen
 * shows a single column (0x8C1D1D1C set) for a third player, left as is. */
#define PAUSE_SINGLE 0x8C1D1D1Cu

static void pause_player(int p, void (*orig)(void)) {
    uint32_t r4 = g.r[4], r5 = g.r[5], r6 = g.r[6];
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, 0x8C07CBBCu);
    int opened = screen_opened(&s_pause);
    if (p == 0) capture_ended();
    handle_player(&s_pause, p, opened);
    g.r[4] = r4;
    g.r[5] = r5;
    g.r[6] = r6;
    orig();
    place(&s_pause, p);
    if (p == 1 || !R8(PAUSE_SINGLE)) s_pause.last_frame = g_vblank_count;
}
void f_8C07CD5A_orig(void);
void f_8C07CE14_orig(void);
void hle_pause_buttoncfg_p1(void) { pause_player(0, f_8C07CD5A_orig); }
void hle_pause_buttoncfg_p2(void) { pause_player(1, f_8C07CE14_orig); }

/* The pause screen's attack icons: sprite objects pointing at player 1's
 * cursor byte (+124), x at +0x46 (112 player 1, 288 player 2), y at +0x4A
 * (counting up: 240 - the row's text y). Their buttons are noted when the
 * screen opens (from the game's positions), then they follow the grid. */
#define OBJ_POOL 0x8C180000u
#define OBJ_POOL_END 0x8C190000u
static struct { uint32_t obj; int p, k; } s_icons[12];
static int s_nicons;
static uint64_t s_icons_frame;

static void pause_icons(void) {
    if (g_vblank_count - s_icons_frame > 4 || g_vblank_count < s_icons_frame) {
        s_nicons = 0;
        for (uint32_t a = OBJ_POOL + 124; a < OBJ_POOL_END && s_nicons < 12; a += 4) {
            if (R32(a) != s_pause.cursor[0]) continue;
            uint32_t obj = a - 124;
            int x = (int16_t)R16(obj + 0x46), y = (int16_t)R16(obj + 0x4A);
            int k = (240 - y - s_pause.top) / 16;
            if ((x == 112 || x == 288) && k >= 0 && k < 6) {
                s_icons[s_nicons].obj = obj;
                s_icons[s_nicons].p = x == 288;
                s_icons[s_nicons++].k = k;
            }
        }
    }
    s_icons_frame = g_vblank_count;
    for (int i = 0; i < s_nicons; i++) {
        int x, y;
        icon_xy(&s_pause, s_icons[i].p, s_icons[i].k, &x, &y);
        W16(s_icons[i].obj + 0x46, (uint32_t)x);
        W16(s_icons[i].obj + 0x4A, (uint32_t)y);
    }
}

/* ---------------- drawing ---------------- */

static uint32_t s_caller;

static void print_guest(uint32_t str, int x, int y) {
    g.r[4] = str;
    g.r[5] = LAYER;
    g.r[6] = (uint32_t)x;
    g.r[7] = (uint32_t)y;
    g.r[15] -= 4;
    W32(g.r[15], COLOUR);
    call_indirect(0x8C01532Au, s_caller);
    g.r[15] += 4;
}

static void text(const char *s, int x, int y) { gameui_print_layer(s, LAYER, x, y, COLOUR); }

static void draw(const Screen *sc) {
    s_caller = sc->caller;
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, sc->caller);
    sprite_swap_attack_icons(1);  /* the 3-punch / 3-kick icons */
    sprite_swap_images_begin();
    int cap = controls_capture_player(), cap_act = controls_capture_action();
    for (int p = 0; p < 2; p++) {
        int x = COL_X(p), vx = x + VALUE_DX;
        for (int r = 0; r < ROW_GRID; r++)
            for (int c = 0; c < 2; c++) {
                int act = cell_action(r, c);
                char name[8];
                if (cap == p && cap_act == act) {
                    if ((g_vblank_count / 16) % 2) continue;  /* blinking while waiting for the press */
                    snprintf(name, sizeof name, "?");
                } else if (controls_button_icon(p, act) >= 0) {
                    /* the gamepad button's picture where its name would be:
                     * square, about a line of text tall (text at y is drawn
                     * from 2y + 8, 32 pixels tall) */
                    /* in its own proportions, 30 pixels tall, at most 56 wide
                     * (the room before the attack icon), centred on the line */
                    int img = controls_button_icon(p, act);
                    uint32_t iw = 1, ih = 1;
                    sprite_swap_image((uint32_t)img, &iw, &ih);
                    float h = 30, w = h * (float)iw / (float)ih;
                    if (w > 56) { h *= 56 / w; w = 56; }
                    float x0 = cell_x(p, c) * UNITS_TO_PX, y0 = 2.0f * row_y(sc, r) + 9 + (30 - h) / 2;
                    sprite_swap_image_at(img, x0, y0, x0 + w, y0 + h);
                    continue;
                } else {
                    controls_short_name(p, act, 3, name, sizeof name);
                }
                text(name, cell_x(p, c), row_y(sc, r));
            }
        print_guest(sc->str_vibrate, x, row_y(sc, ROW_VIBRATE));
        print_guest(R32(sc->vibrate_values + ((R8(BUTTON_CONFIG + 48) >> p) & 1) * 4), vx, row_y(sc, ROW_VIBRATE));
        text("DEVICE", x, row_y(sc, ROW_DEVICE));
        text(controls_short_type_name(controls_type(p)), vx, row_y(sc, ROW_DEVICE));
        print_guest(sc->str_command, x, row_y(sc, ROW_COMMAND));
        int command = (int8_t)R8(BUTTON_CONFIG + 57 + (uint32_t)p);
        print_guest(R32(sc->command_values + (uint32_t)(command >= 0 && command < 3 ? command : 0) * 4), vx,
                    row_y(sc, ROW_COMMAND));
        print_guest(sc->str_default, x, row_y(sc, ROW_DEFAULT));
        print_guest(sc->str_exit, x, row_y(sc, ROW_EXIT));
    }
}

void hle_buttoncfg_draw(void) { draw(&s_option); }

void f_8C07D2E0_orig(void);
void hle_pause_buttoncfg_draw(void) {
    if (R8(PAUSE_SINGLE)) { f_8C07D2E0_orig(); return; }  /* a third player's column */
    pause_icons();
    draw(&s_pause);
}
