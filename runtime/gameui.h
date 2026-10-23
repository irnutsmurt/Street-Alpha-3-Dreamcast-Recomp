/* Drawing and input helpers for screens added to the game (Option Mode >
 * DISPLAY, the NETWORK lobby): text through the game's own print routine,
 * the game's menu sounds, and menu-style controller handling. */
#pragma once
#include <stdint.h>

/* Controller state, 0x94 bytes per player: +8 held, +0x0C held last frame,
 * +0x10 newly pressed, +0x14 released. Bits: up 8, down 4, left 2, right 1,
 * A 0x80, B 0x100. */
#define PADS 0x8C1A1C54u
#define PAD_STRIDE 0x94u
#define PAD_HELD 0x08u
#define PAD_UP 0x0008u
#define PAD_DOWN 0x0004u
#define PAD_LEFT 0x0002u
#define PAD_RIGHT 0x0001u
#define PAD_OK 0x8080u
#define PAD_CANCEL 0x0100u
#define PAD_START 0x1000u
#define PAD_DIRS (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)

enum { SND_CURSOR = 56, SND_OK = 57, SND_CANCEL = 47 };
/* the print routine's colour codes (0-11) */
enum { COL_GOLD = 3, COL_BLUE = 5, COL_GREY = 8, COL_WHITE = 9, COL_TITLE = 11 };

/* Layout in the print routine's coordinates: a 384x240 grid, 12 units per character. */
#define SCREEN_W 384
#define CHAR_W 12

/* Strings must stay in guest memory until the frame is drawn, so each screen
 * owns an arena of guest bytes the recompiled program never reads (code of
 * functions it replaces). gameui_begin() starts a frame's strings. */
void gameui_begin(uint32_t arena, uint32_t arena_end, uint32_t caller);
void gameui_print(const char *s, int x, int y, int colour);
void gameui_print_layer(const char *s, int layer, int x, int y, int colour);
void gameui_center(const char *s, int y, int colour);
int gameui_text_w(const char *s);
void gameui_sound(int id);
int gameui_printable(int c);  /* characters the game's font has (upper case) */
#define GAMEUI_BLOCK '\x01'   /* prints as a solid block */
#define GAMEUI_LEFT "$"       /* the font's arrows */
#define GAMEUI_RIGHT ","

/* Menu-style presses for one or more players: edges from the held bits (the
 * game can run a screen's logic twice per input read) plus auto-repeat for
 * held directions, timed in vblanks. */
typedef struct {
    uint32_t prev_held;
    uint64_t hold_start, last_repeat, last_frame;
} MenuInput;
uint32_t gameui_pad_held(int player);
/* reset: the screen just opened (the button that opened it isn't a press) */
uint32_t gameui_presses(MenuInput *m, uint32_t held, int reset);
