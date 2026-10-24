/* Option Mode: a fifth button, CHEATS, between DISPLAY and EXIT.
 *
 * Option Mode's menu (logic 0x8C07AF3C, game state 0x8C17FBC0: +3 cursor,
 * +7 the open screen + 1, +8 a screen is open) has four buttons: GAME
 * OPTION, BUTTON CONFIG, DISPLAY, EXIT. Each is a sprite object of kind 12
 * (handler 0x8C07D988; the objects are 0x90 bytes from 0x8C1870E8: +0x5A in
 * use, +0x5B kind, +0x5C button 0-3, +0x80 state; +6 x, +10 y, +12 / +24 x / y
 * offset, +14 width, +26 height, +36 / +42 / +48 colour, +0x38 and +0x88 the
 * dimmed look; +64 / +66 / +76 / +78 its picture's u0 / u1 / v0 / v1 in
 * texture 2, whose rows of 32 hold the four labels). The game highlights
 * the button whose number is the cursor.
 *
 * Here a fifth button is a second DISPLAY button, spawned as the menu opens,
 * its v1 one texel short (0x5F) so the renderer can tell it apart and draw
 * the CHEATS label instead (sprite_swap.c, made from the game's own letters).
 * The cursor moves over five rows here; the game's cursor is kept on the
 * row's own screen (CHEATS shares DISPLAY's, whose screen shows the cheats:
 * display_menu.c), with up / down hidden from the game. After each button's
 * own logic the five are stacked a little closer and highlighted for this
 * cursor. */
#include "cpu.h"
#include "gameui.h"
#include "sprite_swap.h"

#define STATE 0x8C17FBC0u
#define OBJECTS 0x8C1870E8u
#define OBJECT_STRIDE 0x90u
#define OBJECT_COUNT 32
#define KIND_BUTTON 12
#define SPAWN 0x8C025930u     /* r4 kind, r5 number: a new object (r0), or 0 */

/* rows; the game's cursor for each */
enum { ROW_GAME_OPTION, ROW_BUTTON_CONFIG, ROW_DISPLAY, ROW_CHEATS, ROW_EXIT, ROW_COUNT };
static const uint8_t s_game_cursor[ROW_COUNT] = {0, 1, 2, 2, 3};

/* layout: five buttons where there were four (192x64, 64 apart from y 96) */
#define Y0 60
#define STEP 58
#define X_EVEN 220
#define X_ODD 228
#define CLONE_V1 0x5F

void f_8C07AF3C_orig(void);
void f_8C07D988_orig(void);
void display_menu_show_cheats(int on);
extern uint64_t g_vblank_count;

static int s_row;
static uint32_t s_clone;
static uint64_t s_last_frame, s_alive_frame;
static MenuInput s_in;

static uint32_t object(int i) { return OBJECTS + (uint32_t)i * OBJECT_STRIDE; }

/* one of the menu's buttons: picture in texture 2, 192 wide, on a label row */
static int is_button(uint32_t o) {
    return R8(o + 0x5A) && R8(o + 0x5B) == KIND_BUTTON && R8(o + 0x5C) < 4 && R32(o + 60) == 2 && R16(o + 64) == 0 &&
           R16(o + 66) == 0xC0 && !(R16(o + 76) & 0x1F);
}

/* the CHEATS button: spawned when there is only the one DISPLAY button */
static void ensure_clone(void) {
    int displays = 0, buttons = 0;
    for (int i = 0; i < OBJECT_COUNT; i++) {
        uint32_t o = object(i);
        if (!is_button(o)) continue;
        buttons++;
        if (R8(o + 0x5C) == 2) displays++;
    }
    if (displays != 1 || buttons != 4) return;
    uint32_t r4 = g.r[4], r5 = g.r[5];
    g.r[4] = KIND_BUTTON;
    g.r[5] = 2;
    call_indirect(SPAWN, 0x8C07AF3Cu);
    s_clone = g.r[0];
    g.r[4] = r4;
    g.r[5] = r5;
}

/* the logic of the menu itself */
void hle_option_top(void) {
    s_alive_frame = g_vblank_count;
    if (R8(STATE + 8)) {  /* a screen is open */
        f_8C07AF3C_orig();
        return;
    }
    int opened = g_vblank_count - s_last_frame > 4 || g_vblank_count < s_last_frame;
    s_last_frame = g_vblank_count;
    if (opened) {  /* the game's cursor, coming back from a screen or from the Mode Menu */
        int c = R8(STATE + 3);
        if (!(s_row == ROW_CHEATS && c == 2)) s_row = c == 3 ? ROW_EXIT : c;
    }
    ensure_clone();
    uint32_t held = gameui_pad_held(0) | gameui_pad_held(1);
    uint32_t pressed = gameui_presses(&s_in, held, opened);
    if (pressed & (PAD_UP | PAD_DOWN)) {
        s_row = (s_row + ((pressed & PAD_UP) ? ROW_COUNT - 1 : 1)) % ROW_COUNT;
        gameui_sound(SND_CURSOR);
    }
    W8(STATE + 3, s_game_cursor[s_row]);
    display_menu_show_cheats(s_row == ROW_CHEATS);
    /* the game's logic, without the directions: A opens the row's screen, B leaves */
    uint32_t saved[3][4];
    for (uint32_t p = 0; p < 3; p++)
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t a = PADS + p * PAD_STRIDE + 0x08 + 4 * k;
            saved[p][k] = R32(a);
            W32(a, saved[p][k] & ~(uint32_t)PAD_DIRS);
        }
    f_8C07AF3C_orig();
    for (uint32_t p = 0; p < 3; p++)
        for (uint32_t k = 0; k < 4; k++) W32(PADS + p * PAD_STRIDE + 0x08 + 4 * k, saved[p][k]);
}

/* each button, after its own logic: place it and highlight it for this cursor */
void hle_option_button(void) {
    uint32_t o = g.r[4];
    f_8C07D988_orig();
    if (g_vblank_count - s_alive_frame > 2) s_clone = 0;  /* Option Mode isn't running: a new one has new objects */
    if (!is_button(o)) return;
    int clone = o == s_clone, n = R8(o + 0x5C);
    if (clone) {
        W16(o + 78, CLONE_V1);
        sprite_swap_cheats_button();
    }
    int row = clone ? ROW_CHEATS : n == 3 ? ROW_EXIT : n;
    W16(o + 6, row & 1 ? X_ODD : X_EVEN);
    W16(o + 10, Y0 + row * STEP);
    if (R8(o + 0x80) != 1) return;  /* not yet running, or leaving */
    int sel = row == s_row, c = sel ? 0xFF : 0x80;
    W8(o + 0x38, !sel);
    W8(o + 0x88, (uint32_t)c);
    W16(o + 12, sel ? 0xFFFCu : 0);
    W16(o + 24, sel ? 0xFFFCu : 0);
    W16(o + 14, sel ? 200 : 192);
    W16(o + 26, sel ? STEP + 8 : STEP);
    for (uint32_t k = 0; k < 4; k++) {
        W8(o + 36 + k, (uint32_t)c);
        W8(o + 42 + k, (uint32_t)c);
        W8(o + 48 + k, (uint32_t)c);
    }
}
