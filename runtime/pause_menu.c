/* The pause menu during a fight, with DISPLAY added.
 *
 * The game's menu (state at 0x8C17FD00: +0 phase, +1 opened, +8 cursor, +9
 * chosen item) lists CONTINUE, BUTTON CONFIG, ATTACK LIST and MODE MENU on a
 * purple panel with a cursor bar; only the player who paused moves it. Here
 * it has five rows, DISPLAY before MODE MENU (and, in a netplay session,
 * LEAVE SESSION last: the paused player's game leaves, the other is told and
 * returns to its lobby too; MODE MENU already brings both back to the
 * session lobby). DISPLAY opens the PC
 * display settings (the same as Option Mode > DISPLAY) on the same panel,
 * enlarged: up / down choose, left / right (or A) change, B / Start or BACK
 * return to the list.
 *
 * The menu's logic (0x8C02EB8C) runs as before, with this file's cursor:
 * up / down are taken here and the game's cursor (0-3) is kept on the row's
 * own item, MODE MENU sharing 3 with DISPLAY (whose A is taken here). The
 * list's drawing (0x8C02EE26) is replaced; its MODE MENU prompt is the
 * game's. The panel and the bar are the game's sprite objects (16.16 fixed
 * point: whole parts at +6 x, +10 y counting up from 244, +14 width, +26
 * height), resized here; the bar's y
 * comes from a table indexed by the game's cursor, whose last entry follows
 * the row. All of it depends only on the synced game and inputs, so in a
 * netplay session both games show the same menu; display changes apply only
 * on the paused player's own machine. */
#include <stdio.h>

#include "cpu.h"
#include "gameui.h"
#include "netplay.h"
#include "options.h"

#define PAUSE 0x8C17FD00u
#define GAME_STATE 0x8C17FBC0u     /* +89: the paused player's bit */
#define BAR_Y_TABLE 0x8C0BD858u    /* 16-bit bar y per cursor row (239 - the row's text y) */
#define OBJECTS 0x8C1870E8u
#define OBJECT_STRIDE 0x90u
#define TITLE_1P 0x8C0962DCu       /* "1P PAUSE", then 2P and 3P, 12 bytes apart */
#define ITEMS 0x8C0A8C88u          /* the game's items: string, layer (byte +4), x (+6), y (+8); 12 bytes each */
/* the DISPLAY screen's string arena (never shown at the same time) */
#define TEXT_ARENA 0x8C07BE40u
#define TEXT_ARENA_END 0x8C07C1D0u

enum { ROW_CONTINUE, ROW_BUTTON_CONFIG, ROW_ATTACK_LIST, ROW_DISPLAY, ROW_MODE_MENU, ROW_LEAVE };
/* LEAVE SESSION only in a netplay session (synced, so both games agree) */
static int rows_n(void) { return netplay_active() ? ROW_LEAVE + 1 : ROW_LEAVE; }
#define LAYER 3
#define COL_TITLE 2
#define COL_SELECTED 3
#define COL_NORMAL 9
#define LIST_TOP 104      /* the first row's text y */
#define TITLE_Y 88
#define PANEL_X 112
#define PANEL_W 160
#define PANEL_TOP 84
/* the DISPLAY page: a larger panel */
#define D_PANEL_X 20
#define D_PANEL_W 344
#define D_TITLE_Y 20
#define D_TOP 40
#define D_LABEL_X 32
#define D_VALUE_RIGHT 352

void f_8C02EB8C_orig(void);
void f_8C02EE26_orig(void);
extern uint64_t g_vblank_count;

static int s_row, s_display, s_drow;  /* list row; the DISPLAY page and its row */
static int s_return_row;              /* the row to come back to (from the MODE MENU prompt) */
static MenuInput s_in;
static uint64_t s_last_frame;

static int paused_player(void) {
    uint8_t bits = R8(GAME_STATE + 89);
    for (int i = 0; i < 3; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

/* display changes only on the paused player's machine */
static int local_player(int p) {
    if (!netplay_active()) return 1;
    if (p < 2) return p == (netplay_is_host() ? 0 : 1);
    return netplay_third_player() == (netplay_is_host() ? 1 : 2);
}

/* hide presses from the game (as in buttoncfg.c) */
static void hide(uint32_t pad, uint32_t bits) {
    W32(pad + 0x0C, (R32(pad + 0x0C) & ~bits) | (R32(pad + 0x08) & bits));
    W32(pad + 0x10, R32(pad + 0x10) & ~bits);
    W32(pad + 0x14, R32(pad + 0x14) & ~bits);
}

/* the DISPLAY page's rows: the options that apply now, then BACK */
static int display_rows(int *rows) {
    int n = 0;
    for (int i = 0; i < OPT_COUNT; i++)
        if (option_enabled(i) && i != OPT_SKIP_INTRO) rows[n++] = i;
    rows[n++] = OPT_COUNT;  /* BACK */
    return n;
}

static int list_y(int r) { return LIST_TOP + r * 16; }
static int display_y(int r) { return D_TOP + r * 16; }

static void set_panel(int x, int w, int top, int h, int bar_x, int bar_w) {
    for (uint32_t i = 0; i < 32; i++) {
        uint32_t o = OBJECTS + i * OBJECT_STRIDE;
        if (!R8(o + 90) || R8(o + 91) != 3) continue;
        if (R8(o + 92) == 0) {  /* the panel */
            W16(o + 6, (uint32_t)x);
            W16(o + 14, (uint32_t)w);
            W16(o + 10, (uint32_t)(244 - top));
            W16(o + 26, (uint32_t)h);
        } else if (R8(o + 92) == 1) {  /* the cursor bar */
            W16(o + 6, (uint32_t)bar_x);
            W16(o + 14, (uint32_t)bar_w);
        }
    }
}

/* this frame's panel, bar and game cursor */
static void place(void) {
    int rows[OPT_COUNT + 1], n = display_rows(rows), y;
    if (s_display) {
        if (s_drow >= n) s_drow = n - 1;
        set_panel(D_PANEL_X, D_PANEL_W, D_TITLE_Y - 4, display_y(n - 1) + 20 - (D_TITLE_Y - 4), D_PANEL_X, D_PANEL_W);
        y = display_y(s_drow);
    } else {
        set_panel(PANEL_X, PANEL_W, PANEL_TOP, list_y(rows_n() - 1) + 24 - PANEL_TOP, PANEL_X, PANEL_W);
        y = list_y(s_row);
    }
    int cursor = s_display ? 3 : s_row < ROW_DISPLAY ? s_row : 3;
    W8(PAUSE + 8, (uint32_t)cursor);
    for (int k = 0; k < 3; k++) W16(BAR_Y_TABLE + (uint32_t)k * 2, (uint32_t)(239 - list_y(k)));
    W16(BAR_Y_TABLE + 6, (uint32_t)(239 - (cursor == 3 ? y : list_y(3))));
}

static void display_input(uint32_t pressed, int p) {
    int rows[OPT_COUNT + 1], n = display_rows(rows), r = rows[s_drow < n ? s_drow : n - 1];
    if (pressed & (PAD_CANCEL | 0x8000u)) {  /* B or Start: back to the list */
        s_display = 0;
        gameui_sound(SND_CANCEL);
    } else if (pressed & (PAD_UP | PAD_DOWN)) {
        s_drow = (s_drow + ((pressed & PAD_UP) ? n - 1 : 1)) % n;
        gameui_sound(SND_CURSOR);
    } else if (r == OPT_COUNT) {
        if (pressed & 0x80u) { s_display = 0; gameui_sound(SND_CANCEL); }
    } else if (pressed & (PAD_LEFT | PAD_RIGHT | 0x80u)) {
        if (local_player(p)) option_change(r, (pressed & PAD_LEFT) ? -1 : 1);
        gameui_sound(SND_CURSOR);
    }
}

void hle_pause_logic(void) {
    uint32_t r4 = g.r[4], r5 = g.r[5], r6 = g.r[6];
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, 0x8C02EB8Cu);
    int opened = g_vblank_count - s_last_frame > 4 || g_vblank_count < s_last_frame;
    if (opened) {
        s_row = s_return_row;
        s_display = s_drow = s_return_row = 0;
    }
    if (R8(PAUSE + 10) == 1) s_row = s_display = s_return_row = 0;  /* a new pause, opening */
    s_last_frame = g_vblank_count;
    if (R8(PAUSE + 1)) {  /* the menu is up (not still opening) */
        int p = paused_player();
        uint32_t pad = PADS + (uint32_t)p * PAD_STRIDE;
        uint32_t pressed = gameui_presses(&s_in, R32(pad + PAD_HELD), opened);
        if (s_display) {
            display_input(pressed, p);
            if (s_display) hide(pad, 0xFFFFFFFFu);
            else hide(pad, ~0u), s_row = ROW_DISPLAY;
        } else {
            hide(pad, PAD_UP | PAD_DOWN);  /* rows are moved here */
            if (pressed & (PAD_UP | PAD_DOWN)) {
                s_row = (s_row + ((pressed & PAD_UP) ? rows_n() - 1 : 1)) % rows_n();
                gameui_sound(SND_CURSOR);
            } else if (s_row == ROW_LEAVE && (pressed & 0x80u)) {
                gameui_sound(SND_OK);
                hide(pad, 0xFFFFFFFFu);
                if (local_player(p)) netplay_leave();
            } else if (s_row == ROW_MODE_MENU && (pressed & 0x80u)) {
                s_return_row = ROW_MODE_MENU;  /* its prompt's NO comes back here */
            } else if (s_row == ROW_DISPLAY && (pressed & 0x80u)) {
                s_display = 1;
                s_drow = 0;
                gameui_sound(SND_OK);
                hide(pad, 0xFFFFFFFFu);
            }
        }
    }
    place();
    g.r[4] = r4;
    g.r[5] = r5;
    g.r[6] = r6;
    f_8C02EB8C_orig();
    if (R8(PAUSE) != 1) {  /* the menu was left or an item chosen: the game's own bar table */
        for (int k = 0; k < 4; k++) W16(BAR_Y_TABLE + (uint32_t)k * 2, (uint32_t)(239 - list_y(k)));
    }
}

/* ---------------- drawing ---------------- */

static void print_guest(uint32_t str, int x, int y, int colour) {
    g.r[4] = str;
    g.r[5] = LAYER;
    g.r[6] = (uint32_t)x;
    g.r[7] = (uint32_t)y;
    g.r[15] -= 4;
    W32(g.r[15], (uint32_t)colour);
    call_indirect(0x8C01532Au, 0x8C02EE26u);
    g.r[15] += 4;
}

static int centre_x(const char *s) { return SCREEN_W / 2 - gameui_text_w(s) / 2; }

static void draw_display(void) {
    int rows[OPT_COUNT + 1], n = display_rows(rows);
    gameui_print_layer("DISPLAY", LAYER, centre_x("DISPLAY"), D_TITLE_Y, COL_TITLE);
    for (int k = 0; k < n; k++) {
        int r = rows[k], sel = k == s_drow, col = sel ? COL_SELECTED : COL_NORMAL, y = display_y(k);
        if (r == OPT_COUNT) {
            gameui_print_layer("BACK", LAYER, centre_x("BACK"), y, col);
            continue;
        }
        const char *label = option_label(r);
        gameui_print_layer(label, LAYER, D_LABEL_X, y, col);
        char v[40], b[48];
        option_value(r, v, sizeof v);
        snprintf(b, sizeof b, GAMEUI_LEFT " %s " GAMEUI_RIGHT, v);
        /* arrows on the selected value, if they fit beside the label */
        const char *shown = sel && D_VALUE_RIGHT - gameui_text_w(b) >= D_LABEL_X + gameui_text_w(label) + CHAR_W ? b : v;
        gameui_print_layer(shown, LAYER, D_VALUE_RIGHT - gameui_text_w(shown), y, col);
    }
}

void hle_pause_draw(void) {
    if (g.r[7]) { f_8C02EE26_orig(); return; }  /* the MODE MENU prompt */
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, 0x8C02EE26u);
    if (s_display) { draw_display(); return; }
    print_guest(TITLE_1P + (uint32_t)paused_player() * 12, 0x90, TITLE_Y, COL_TITLE);
    for (int r = 0; r < rows_n(); r++) {
        int col = r == s_row ? COL_SELECTED : COL_NORMAL;
        if (r == ROW_DISPLAY || r == ROW_LEAVE) {
            const char *t = r == ROW_DISPLAY ? "DISPLAY" : "LEAVE SESSION";
            gameui_print_layer(t, LAYER, centre_x(t), list_y(r), col);
            continue;
        }
        uint32_t item = ITEMS + (uint32_t)(r < ROW_DISPLAY ? r : 3) * 12;
        print_guest(R32(item), (int16_t)R16(item + 6), list_y(r), col);
    }
}
