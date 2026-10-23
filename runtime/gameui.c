#include <string.h>

#include "cpu.h"
#include "gameui.h"

#define GAME_PRINT 0x8C01532Au  /* (text, layer, x, y, [sp] colour); keeps the text pointer */
#define GAME_SOUND 0x8C059F8Cu  /* (0, sound, 0xC0) */

extern uint64_t g_vblank_count;

static uint32_t s_arena, s_arena_end, s_caller;

void gameui_begin(uint32_t arena, uint32_t arena_end, uint32_t caller) {
    s_arena = arena;
    s_arena_end = arena_end;
    s_caller = caller;
}

/* The game's font is ASCII-like for letters and digits, but most punctuation
 * codes hold arrows, kana and symbols; its full stop is at '#'. */
int gameui_printable(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr(" .-:+/?()'", c));
}

static uint32_t guest_string(const char *s) {
    size_t n = strlen(s) + 1;
    if (s_arena + n > s_arena_end) return 0;
    uint32_t a = s_arena;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '.') c = '#';
        else if (c == GAMEUI_BLOCK) c = '!';  /* a solid block (text cursor) */
        W8(a + i, c);
    }
    s_arena = (s_arena + n + 3) & ~3u;
    return a;
}

void gameui_print_layer(const char *s, int layer, int x, int y, int colour) {
    uint32_t a = guest_string(s);
    if (!a) return;
    g.r[4] = a;
    g.r[5] = (uint32_t)layer;
    g.r[6] = (uint32_t)x;
    g.r[7] = (uint32_t)y;
    g.r[15] -= 4;
    W32(g.r[15], (uint32_t)colour);
    call_indirect(GAME_PRINT, s_caller);
    g.r[15] += 4;
}

void gameui_print(const char *s, int x, int y, int colour) { gameui_print_layer(s, 3, x, y, colour); }

int gameui_text_w(const char *s) { return (int)strlen(s) * CHAR_W; }

void gameui_center(const char *s, int y, int colour) {
    gameui_print(s, SCREEN_W / 2 - gameui_text_w(s) / 2, y, colour);
}

void gameui_sound(int id) {
    g.r[4] = 0;
    g.r[5] = (uint32_t)id;
    g.r[6] = 0xC0;
    call_indirect(GAME_SOUND, s_caller);
}

uint32_t gameui_pad_held(int player) { return R32(PADS + (uint32_t)player * PAD_STRIDE + PAD_HELD); }

uint32_t gameui_presses(MenuInput *m, uint32_t held, int reset) {
    if (reset || g_vblank_count - m->last_frame > 4 || g_vblank_count < m->last_frame) {
        m->prev_held = held;
        m->hold_start = 0;
    }
    m->last_frame = g_vblank_count;
    uint32_t pressed = held & ~m->prev_held;
    m->prev_held = held;
    /* auto-repeat like the game's own menus: 20 frames, then every 6 */
    if (!(held & PAD_DIRS)) {
        m->hold_start = 0;
    } else if (pressed & PAD_DIRS) {
        m->hold_start = m->last_repeat = g_vblank_count;
    } else if (m->hold_start && g_vblank_count - m->hold_start >= 20 && g_vblank_count - m->last_repeat >= 6) {
        pressed |= held & PAD_DIRS;
        m->last_repeat = g_vblank_count;
    }
    return pressed;
}
