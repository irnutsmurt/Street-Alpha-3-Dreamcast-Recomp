/* Option Mode > DISPLAY, rebuilt as the PC display settings screen.
 *
 * On the Dreamcast this screen ("DISPLAY ADJUST") moved and shrank the picture
 * for CRT televisions. Its per-frame logic (0x8C07BC42) and draw (0x8C07C6B0)
 * functions are replaced here; text is drawn with the game's own print routine
 * and menu sounds, so the screen looks and behaves like the rest of Option Mode.
 * Leaving the screen is handed back to the original logic so the game's own
 * exit transition runs unchanged. */
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "runtime.h"
#include "gameui.h"
#include "options.h"

/* Option Mode state: +8 sub-screen active, +9 cursor, +13 sub-state
 * (0 choosing, 1 item selected, 2/3 leaving). */
#define OPT_STATE 0x8C17FBC0u

/* Text is drawn after this function returns, so strings must stay in guest
 * memory. The replaced CRT position/size handlers' code bytes are never
 * executed in the recompiled program, so they hold this screen's strings. */
#define TEXT_ARENA 0x8C07BE40u
#define TEXT_ARENA_END 0x8C07C1D0u

/* rows: the display options, then DEFAULT and EXIT */
enum { ROW_DEFAULT = OPT_COUNT, ROW_EXIT, ROW_COUNT };

/* Layout in the print routine's coordinates: a 384x240 grid, 12 units per
 * character (gameui.h). The scrolling banner covers everything below y = 165. */
#define TITLE_Y 30
#define LABEL_X 24
#define VALUE_RIGHT 360
#define ROW_Y0 48
#define ROW_STEP 10
/* the game's row-highlight object only knows the original screen's rows;
 * pointing the cursor past them hides it */
#define NO_HIGHLIGHT_ROW 7

void f_8C07BC42_orig(void);
extern uint64_t g_vblank_count;

static int s_cursor;
static uint64_t s_last_frame;

/* Rows currently shown: options that apply in the current mode (e.g. window
 * size only for a window), then DEFAULT and EXIT. s_cursor holds a row id. */
static int visible_rows(int *rows) {
    int n = 0;
    for (int r = 0; r < ROW_COUNT; r++)
        if (r >= OPT_COUNT || option_enabled(r)) rows[n++] = r;
    return n;
}

static int cursor_pos(const int *rows, int n) {
    for (int i = 0; i < n; i++)
        if (rows[i] == s_cursor) return i;
    return 0;
}


void hle_display_draw(void) {
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, 0x8C07C6B0u);
    gameui_print("DISPLAY OPTION", SCREEN_W / 2 - gameui_text_w("DISPLAY OPTION") / 2, TITLE_Y, COL_GOLD);
    int rows[ROW_COUNT], n = visible_rows(rows);
    for (int i = 0; i < n; i++) {
        int r = rows[i], sel = r == s_cursor;
        int y = ROW_Y0 + i * ROW_STEP + (r >= ROW_DEFAULT ? 4 : 0);
        if (r >= OPT_COUNT) {
            /* centred like the game's own DEFAULT / EXIT */
            const char *label = r == ROW_DEFAULT ? "DEFAULT" : "EXIT";
            gameui_print(label, SCREEN_W / 2 - gameui_text_w(label) / 2, y, sel ? COL_GOLD : COL_WHITE);
            continue;
        }
        const char *label = option_label(r);
        gameui_print(label, LABEL_X, y, sel ? COL_GOLD : COL_WHITE);
        char v[40], b[48];
        option_value(r, v, sizeof v);
        snprintf(b, sizeof b, GAMEUI_LEFT " %s " GAMEUI_RIGHT, v);
        /* arrows on the selected value, if they fit beside the label */
        const char *shown = sel && VALUE_RIGHT - gameui_text_w(b) >= LABEL_X + gameui_text_w(label) + CHAR_W ? b : v;
        gameui_print(shown, VALUE_RIGHT - gameui_text_w(shown), y, sel ? COL_GOLD : COL_BLUE);
    }
}

static void move(int dir) {
    int rows[ROW_COUNT], n = visible_rows(rows);
    s_cursor = rows[(cursor_pos(rows, n) + dir + n) % n];
    gameui_sound(SND_CURSOR);
}

static void leave(int snd) {
    gameui_sound(snd);
    W8(OPT_STATE + 13, 2);  /* the original logic runs the exit transition from here */
}

void hle_display_logic(void) {
    int sub = (int8_t)R8(OPT_STATE + 13);
    if (sub >= 2) { f_8C07BC42_orig(); return; }
    static MenuInput in;
    uint32_t held = gameui_pad_held(0) | gameui_pad_held(1);
    int opened = g_vblank_count - s_last_frame > 4 || g_vblank_count < s_last_frame;
    if (opened) s_cursor = 0;
    s_last_frame = g_vblank_count;
    W8(OPT_STATE + 9, NO_HIGHLIGHT_ROW);
    W8(OPT_STATE + 13, 0);
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, 0x8C07BC42u);  /* for the sounds */
    uint32_t pressed = gameui_presses(&in, held, opened);

    if (pressed & PAD_CANCEL) { leave(SND_CANCEL); return; }
    if (pressed & PAD_UP) { move(-1); return; }
    if (pressed & PAD_DOWN) { move(+1); return; }
    if (s_cursor < OPT_COUNT && (pressed & (PAD_LEFT | PAD_RIGHT))) {
        option_change(s_cursor, (pressed & PAD_LEFT) ? -1 : +1);
        gameui_sound(SND_CURSOR);
        return;
    }
    if (pressed & PAD_OK) {
        if (s_cursor < OPT_COUNT) { option_change(s_cursor, +1); gameui_sound(SND_OK); }
        else if (s_cursor == ROW_DEFAULT) { options_reset(); gameui_sound(SND_OK); }
        else leave(SND_OK);
    }
}

/* Option Mode sprite objects (r4 = object; byte +3 = object type). */
void f_8C07E17C_orig(void);
int buttoncfg_active(void);
void buttoncfg_place_icon(uint32_t obj, int type);
void hle_option_object(void) {
    uint32_t obj = g.r[4];
    int type = R8(obj + 3);
    /* type 48 is the old screen's big "100%" size readout */
    if (type == 48 && g_vblank_count - s_last_frame <= 4) return;
    if ((type == 0x1E || type == 0x21 || (type >= 0x24 && type <= 0x2F)) && buttoncfg_active())
        buttoncfg_place_icon(obj, type);
    f_8C07E17C_orig();
}
