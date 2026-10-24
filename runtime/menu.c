/* Overlay settings menu (F1 / Esc, or Back on a gamepad): the same display
 * options as Option Mode > DISPLAY, reachable at any time. It is drawn on a
 * small RGBA canvas that the renderer blends over the game picture; the game
 * is paused while it is open (except in a netplay session, which also adds
 * BACK TO LOBBY and LEAVE SESSION). */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#include "cheats.h"
#include "controls.h"
#include "menu.h"
#include "netplay.h"
#include "options.h"
#include "settings.h"

extern int g_quit_requested;

static uint32_t s_canvas[MENU_W * MENU_H];
static int s_open, s_sel, s_dirty = 1;

/* ---------------- 5x7 font (uppercase, digits, a little punctuation) ---------------- */

static const struct {
    char c;
    const char *rows[7];
} s_font[] = {
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
    {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"}},
    {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {'%', {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"}},
    {'/', {".....", "....#", "...#.", "..#..", ".#...", "#....", "....."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'<', {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."}},
    {'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."}},
};

/* colours are RGBA8 in memory (0xAABBGGRR) */
#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)

static void fill(int x0, int y0, int w, int h, uint32_t c) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && y >= 0 && x < MENU_W && y < MENU_H) s_canvas[y * MENU_W + x] = c;
}

static int text_width(const char *s) { return (int)strlen(s) * 6 - 1; }

static void text(int x, int y, const char *s, uint32_t c) {
    for (; *s; s++, x += 6) {
        for (size_t g = 0; g < sizeof s_font / sizeof s_font[0]; g++) {
            if (s_font[g].c != *s) continue;
            for (int r = 0; r < 7; r++)
                for (int k = 0; k < 5; k++)
                    if (s_font[g].rows[r][k] == '#') {
                        fill(x + k + 1, y + r + 1, 1, 1, RGBA(0, 0, 0, 255));  /* drop shadow */
                        fill(x + k, y + r, 1, 1, c);
                    }
            break;
        }
    }
}

/* ---------------- items ---------------- */

/* the display options, then CONTROLS, CHEATS (not in a session), DEFAULT,
 * (in a session) BACK TO LOBBY and LEAVE SESSION, RESUME, QUIT */
enum { ITEM_CONTROLS = OPT_COUNT, ITEM_CHEATS, ITEM_DEFAULT, ITEM_LOBBY, ITEM_LEAVE, ITEM_RESUME, ITEM_QUIT, ITEM_COUNT };

static const char *item_label(int i) {
    static const char *const names[] = {"CONTROLS", "CHEATS", "DEFAULT", "BACK TO LOBBY", "LEAVE SESSION", "RESUME GAME",
                                        "QUIT GAME"};
    return i < OPT_COUNT ? option_label(i) : names[i - OPT_COUNT];
}

/* the items shown, as ids */
static int s_items[ITEM_COUNT], s_nitems;

static void list_items(void) {
    s_nitems = 0;
    for (int i = 0; i < ITEM_COUNT; i++)
        if (i == ITEM_LOBBY || i == ITEM_LEAVE ? netplay_active() : i != ITEM_CHEATS || cheats_available())
            s_items[s_nitems++] = i;
    if (s_sel >= s_nitems) s_sel = s_nitems - 1;
}

/* ---------------- CONTROLS page ----------------
 * Player, controller kind, gamepad, then one row per control (directions
 * only for the keyboard: gamepads always move with the d-pad and stick),
 * DEFAULT and BACK. Choosing a control waits for the key / button to bind. */
static int s_page, s_player, s_csel;
static void redraw(void);
enum { CROW_PLAYER = -10, CROW_TYPE, CROW_PAD, CROW_DEFAULT, CROW_BACK };

static int control_rows(int *rows) {
    int n = 0;
    rows[n++] = CROW_PLAYER;
    rows[n++] = CROW_TYPE;
    if (controls_type(s_player) != CTL_KEYBOARD) rows[n++] = CROW_PAD;
    for (int a = controls_edits_keyboard(s_player) ? 0 : ACT_FIRST_BUTTON; a < ACT_COUNT; a++) rows[n++] = a;
    rows[n++] = CROW_DEFAULT;
    rows[n++] = CROW_BACK;
    return n;
}

static void panel(const char *title, int *px, int *py, int *pw, int *ph) {
    *px = 22, *py = 6, *pw = MENU_W - 44, *ph = 228;
    fill(*px - 2, *py - 2, *pw + 4, *ph + 4, RGBA(240, 200, 40, 255));   /* border */
    fill(*px, *py, *pw, *ph, RGBA(16, 20, 48, 235));                       /* panel */
    fill(*px, *py, *pw, 18, RGBA(40, 52, 120, 255));                        /* title bar */
    text(MENU_W / 2 - text_width(title) / 2, *py + 6, title, RGBA(255, 230, 90, 255));
}

static void redraw_controls(void) {
    memset(s_canvas, 0, sizeof s_canvas);
    int px, py, pw, ph, rows[32], n = control_rows(rows);
    if (s_csel >= n) s_csel = n - 1;
    panel("CONTROLS", &px, &py, &pw, &ph);
    int capturing = controls_capturing();
    /* a window of rows around the selection when they don't all fit */
    const int visible = 15;
    int first = n <= visible ? 0 : s_csel - visible / 2;
    if (first > n - visible) first = n - visible;
    if (first < 0) first = 0;
    if (first > 0) text(px + pw - 16, py + 22, "-", RGBA(150, 160, 200, 255));
    for (int k = first; k < n && k < first + visible; k++) {
        int r = rows[k], sel = k == s_csel;
        int y = py + 26 + (k - first) * 12 + (r >= CROW_DEFAULT && r < 0 ? 4 : 0);
        char v[48] = "", b[56];
        const char *label = "";
        switch (r) {
        case CROW_PLAYER: label = "PLAYER"; snprintf(v, sizeof v, "%d", s_player + 1); break;
        case CROW_TYPE: label = "CONTROLLER"; snprintf(v, sizeof v, "%s", controls_type_name(controls_type(s_player))); break;
        case CROW_PAD: {
            int c = controls_pad_choice(s_player);
            label = "GAMEPAD";
            if (c) snprintf(v, sizeof v, "%d", c); else snprintf(v, sizeof v, "AUTO");
            if (!controls_pad_count()) snprintf(v + strlen(v), sizeof v - strlen(v), " (NONE FOUND)");
            break;
        }
        case CROW_DEFAULT: label = "DEFAULT"; break;
        case CROW_BACK: label = "BACK"; break;
        default:
            label = controls_action_name(r);
            if (capturing && sel) snprintf(v, sizeof v, "...");
            else controls_binding_name(s_player, r, v, sizeof v);
        }
        if (sel) fill(px + 4, y - 3, pw - 8, 13, capturing ? RGBA(40, 120, 60, 255) : RGBA(200, 60, 40, 255));
        uint32_t c = sel ? RGBA(255, 255, 255, 255) : RGBA(180, 190, 230, 255);
        if (sel) text(px + 8, y, ">", c);
        text(px + 18, y, label, c);
        if (v[0]) {
            if (sel && r < CROW_DEFAULT) snprintf(b, sizeof b, "< %s >", v); else snprintf(b, sizeof b, "%s", v);
            text(px + pw - 10 - text_width(b), y, b, RGBA(255, 230, 90, 255));
        }
    }
    const char *help = "ENTER: CHANGE   ESC: BACK";
    if (capturing) help = controls_edits_keyboard(s_player) ? "PRESS A KEY  (ESC: CANCEL)" : "PRESS A BUTTON  (BACK: CANCEL)";
    text(MENU_W / 2 - text_width(help) / 2, py + ph - 11, help, RGBA(150, 160, 200, 255));
    s_dirty = 1;
}

/* ---------------- CHEATS page ----------------
 * The cheats (the same as Option Mode > CHEATS), DEFAULT and BACK. */
static int s_hsel;
enum { HROW_DEFAULT = CHEAT_COUNT, HROW_BACK, HROW_COUNT };

static void redraw_cheats(void) {
    memset(s_canvas, 0, sizeof s_canvas);
    int px, py, pw, ph;
    panel("CHEATS", &px, &py, &pw, &ph);
    for (int r = 0; r < HROW_COUNT; r++) {
        int sel = r == s_hsel, y = py + 26 + r * 12 + (r >= HROW_DEFAULT ? 4 : 0);
        if (sel) fill(px + 4, y - 3, pw - 8, 12, RGBA(200, 60, 40, 255));
        uint32_t c = sel ? RGBA(255, 255, 255, 255) : RGBA(180, 190, 230, 255);
        if (sel) text(px + 8, y, ">", c);
        text(px + 18, y, r == HROW_DEFAULT ? "DEFAULT" : r == HROW_BACK ? "BACK" : cheat_label(r), c);
        if (r < CHEAT_COUNT) {
            char v[32], b[40];
            cheat_value(r, v, sizeof v);
            if (sel) snprintf(b, sizeof b, "< %s >", v); else snprintf(b, sizeof b, "%s", v);
            text(px + pw - 10 - text_width(b), y, b, RGBA(255, 230, 90, 255));
        }
    }
    static const char *const notes[] = {"AGAINST THE CPU ONLY", "FINAL BISON: HOLD START ON M.BISON"};
    for (int k = 0; k < 2; k++)
        text(MENU_W / 2 - text_width(notes[k]) / 2, py + 26 + HROW_COUNT * 12 + 6 + k * 9, notes[k],
             RGBA(150, 160, 200, 255));
    const char *help = "ENTER: CHANGE   ESC: BACK";
    text(MENU_W / 2 - text_width(help) / 2, py + ph - 11, help, RGBA(150, 160, 200, 255));
    s_dirty = 1;
}

static void cheats_action(MenuAction a) {
    switch (a) {
    case MENU_UP: s_hsel = (s_hsel + HROW_COUNT - 1) % HROW_COUNT; break;
    case MENU_DOWN: s_hsel = (s_hsel + 1) % HROW_COUNT; break;
    case MENU_BACK: s_page = 0; break;
    case MENU_LEFT: case MENU_RIGHT: case MENU_ACCEPT:
        if (s_hsel < CHEAT_COUNT) cheat_change(s_hsel, a == MENU_LEFT ? -1 : +1);
        else if (a != MENU_ACCEPT) break;
        else if (s_hsel == HROW_DEFAULT) cheats_reset();
        else s_page = 0;
        break;
    }
}

static void controls_action(MenuAction a) {
    int rows[32], n = control_rows(rows), r = rows[s_csel < n ? s_csel : n - 1];
    int dir = a == MENU_LEFT ? -1 : +1;
    switch (a) {
    case MENU_UP: s_csel = (s_csel + n - 1) % n; break;
    case MENU_DOWN: s_csel = (s_csel + 1) % n; break;
    case MENU_BACK: s_page = 0; break;
    case MENU_LEFT: case MENU_RIGHT: case MENU_ACCEPT:
        if (r == CROW_PLAYER) { s_player ^= 1; s_csel = 0; }
        else if (r == CROW_TYPE) controls_set_type(s_player, controls_type(s_player) + dir);
        else if (r == CROW_PAD) controls_set_pad_choice(s_player, controls_pad_choice(s_player) + dir);
        else if (a != MENU_ACCEPT) break;
        else if (r == CROW_DEFAULT) { controls_reset(s_player); settings_save(); }
        else if (r == CROW_BACK) s_page = 0;
        else controls_capture_begin(s_player, r);
        break;
    }
}

void menu_refresh(void) {
    if (s_open) redraw();
}

static void redraw(void) {
    if (s_page == 1) { redraw_controls(); return; }
    if (s_page == 2) { redraw_cheats(); return; }
    memset(s_canvas, 0, sizeof s_canvas);
    list_items();
    const int px = 22, py = 6, pw = MENU_W - 44, ph = 228, row = s_nitems > 15 ? 11 : s_nitems > 13 ? 12 : 13;
    fill(px - 2, py - 2, pw + 4, ph + 4, RGBA(240, 200, 40, 255));   /* border */
    fill(px, py, pw, ph, RGBA(16, 20, 48, 235));                     /* panel */
    fill(px, py, pw, 18, RGBA(40, 52, 120, 255));                     /* title bar */
    text(MENU_W / 2 - text_width("DISPLAY") / 2, py + 6, "DISPLAY", RGBA(255, 230, 90, 255));
    for (int k = 0; k < s_nitems; k++) {
        int i = s_items[k];
        int y = py + 26 + k * row + (i >= ITEM_CONTROLS ? 4 : 0);
        int sel = k == s_sel, on = i >= OPT_COUNT || option_enabled(i);
        if (sel) fill(px + 4, y - 3, pw - 8, 13, RGBA(200, 60, 40, 255));
        uint32_t c = sel ? RGBA(255, 255, 255, 255) : on ? RGBA(180, 190, 230, 255) : RGBA(90, 95, 120, 255);
        if (sel) text(px + 8, y, ">", c);
        text(px + 18, y, item_label(i), c);
        if (i < OPT_COUNT) {
            char v[32], b[40];
            option_value(i, v, sizeof v);
            if (sel && on) snprintf(b, sizeof b, "< %s >", v); else snprintf(b, sizeof b, "%s", v);
            text(px + pw - 10 - text_width(b), y, b, on ? RGBA(255, 230, 90, 255) : RGBA(110, 100, 60, 255));
        }
    }
    const char *help = "ARROWS: CHOOSE   ESC: CLOSE";
    text(MENU_W / 2 - text_width(help) / 2, py + ph - 11, help, RGBA(150, 160, 200, 255));
    s_dirty = 1;
}

static void activate(int dir) {
    int i = s_items[s_sel];
    if (i < OPT_COUNT) option_change(i, dir);
    else if (i == ITEM_CONTROLS) { s_page = 1; s_csel = 0; }
    else if (i == ITEM_CHEATS) { s_page = 2; s_hsel = 0; }
    else if (i == ITEM_DEFAULT) options_reset();
    else if (i == ITEM_LOBBY) { netplay_request_lobby(); s_open = 0; }
    else if (i == ITEM_LEAVE) { netplay_leave(); s_open = 0; }
    else if (i == ITEM_RESUME) s_open = 0;
    else if (i == ITEM_QUIT) { g_quit_requested = 1; s_open = 0; }
}

/* ---------------- banner ----------------
 * A one-line notice over the game (netplay: joining, waiting for the other
 * player), drawn on the same canvas when the menu is closed. */
static char s_banner[48], s_tag[16];

static void redraw_banner(void) {
    memset(s_canvas, 0, sizeof s_canvas);
    if (s_tag[0]) {  /* a small tag above the bottom of the picture */
        int w = text_width(s_tag) + 6, x = MENU_W / 2 - w / 2, y = MENU_H - 44;
        fill(x, y, w, 11, RGBA(16, 20, 48, 160));
        text(x + 3, y + 2, s_tag, RGBA(255, 230, 90, 255));
    }
    if (s_banner[0]) {
        int w = text_width(s_banner) + 24, x = MENU_W / 2 - w / 2, y = MENU_H / 2 - 12;
        fill(x - 2, y - 2, w + 4, 24 + 4, RGBA(240, 200, 40, 255));
        fill(x, y, w, 24, RGBA(16, 20, 48, 235));
        text(x + 12, y + 8, s_banner, RGBA(255, 255, 255, 255));
    }
    s_dirty = 1;
}

void menu_banner(const char *t) {
    if (!strcmp(t, s_banner)) return;
    snprintf(s_banner, sizeof s_banner, "%s", t);
    if (!s_open) redraw_banner();
}

void menu_tag(const char *t) {
    if (!strcmp(t, s_tag)) return;
    snprintf(s_tag, sizeof s_tag, "%s", t);
    if (!s_open) redraw_banner();
}

int menu_overlay_visible(void) { return s_open || s_banner[0] || s_tag[0]; }

/* ---------------- public ---------------- */

int menu_active(void) { return s_open; }

void menu_toggle(void) {
    s_open = !s_open;
    controls_capture_cancel();
    if (s_open) { s_sel = 0; s_page = 0; redraw(); }
    else redraw_banner();
}

void menu_action(MenuAction a) {
    if (!s_open || controls_capturing()) return;
    if (s_page) {
        if (s_page == 1) controls_action(a);
        else cheats_action(a);
        redraw();
        return;
    }
    switch (a) {
    case MENU_UP: s_sel = (s_sel + s_nitems - 1) % s_nitems; break;
    case MENU_DOWN: s_sel = (s_sel + 1) % s_nitems; break;
    case MENU_LEFT: if (s_items[s_sel] < OPT_COUNT) activate(-1); break;
    case MENU_RIGHT: if (s_items[s_sel] < OPT_COUNT) activate(+1); break;
    case MENU_ACCEPT: activate(+1); break;
    case MENU_BACK: s_open = 0; break;
    }
    if (s_open) redraw();
    else redraw_banner();
}

const uint32_t *menu_canvas(int *dirty) {
    *dirty = s_dirty;
    s_dirty = 0;
    return s_canvas;
}
