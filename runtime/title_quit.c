/* QUIT GAME? on the title screen.
 *
 * On the title screen (PRESS START BUTTON: the mode menu task's state 1,
 * 0x8C0625E0), B, the gamepad's Back button or Esc asks "QUIT GAME?" with YES /
 * NO (NO first), drawn like the pause menu's prompt: a purple panel, the
 * game's font and colours, the cursor bar and sounds. YES closes the game,
 * as closing the window does. While it is up the title sees no presses and
 * its attract-demo timer stands still. Not in a netplay session (there is no
 * title screen there).
 *
 * The intro's last scene is the same title, drawn by the boot task (where the
 * game starts up and goes back to from the Mode Menu; SKIP INTRO passes it):
 * there those buttons press Start for the game, which goes on to the mode
 * menu's title, and QUIT GAME? opens on it. */
#include "cpu.h"
#include "gameui.h"
#include "input.h"
#include "netplay.h"
#include "sprite_swap.h"

extern uint64_t g_vblank_count;
extern int g_quit_requested;
void f_8C0625E0_orig(void);
void f_8C062350_orig(void);

#define TITLE_TIMER 14  /* the title's countdown to the attract demo (16 bits at +14 of its state) */
/* the DISPLAY screen's string arena (never shown at the same time) */
#define TEXT_ARENA 0x8C07BE40u
#define TEXT_ARENA_END 0x8C07C1D0u

/* layout, in the print routine's units (384x240) */
#define PANEL_X 112
#define PANEL_W 160
#define PANEL_TOP 92
#define PANEL_BOTTOM 160
#define QUESTION_Y 100
#define YES_Y 120
#define NO_Y 136
#define LAYER 3  /* the pause menu's font (sprite_swap.c lifts it over the logo onto the panel) */

static uint64_t s_title_frame;
static int s_open, s_choice;  /* 0 NO, 1 YES */
static MenuInput s_in;
/* the intro's title (hle_title_text): when it was last drawn; Back there: Start pressed for the game for these
 * frames, and the mode menu's title opens QUIT GAME? if it comes within 5 seconds of then */
static uint64_t s_boot_title_frame, s_open_next;
static int s_start_frames;
static MenuInput s_boot_in;

static int recent(uint64_t frame) { return frame && g_vblank_count - frame <= 4; }
int title_quit_available(void) { return recent(s_title_frame) || recent(s_boot_title_frame); }
int title_quit_open(void) { return s_open; }

static void boot_title_back(void) {
    if (s_start_frames || netplay_active()) return;
    s_start_frames = 2;
    s_open_next = g_vblank_count;
}

/* Back / Esc (input_sdl.c) */
void title_quit_toggle(void) {
    if (!recent(s_title_frame)) {  /* the intro's title */
        boot_title_back();
        return;
    }
    s_open = !s_open;
    s_choice = 0;
    s_in = (MenuInput){0};
}

/* main.c, the frame's input read: Start, held two frames, for Back on the intro's title */
void title_quit_input(PadState *p1) {
    if (s_start_frames <= 0) return;
    s_start_frames--;
    *p1 = (PadState){BTN_START, 0, 0, 0x80, 0x80};
}

/* f_8C062350: a title's PRESS START BUTTON and copyright lines, each frame: the mode menu's title (f_8C0625E0),
 * the intro's last scene (f_8C073270, returning to 8C073292), the boot's loading screen (copyright lines only) */
void hle_title_text(void) {
    if (g.pr == 0x8C073292u) {
        uint32_t r[4] = {g.r[4], g.r[5], g.r[6], g.r[7]}, pr = g.pr;
        s_boot_title_frame = g_vblank_count;
        if (gameui_presses(&s_boot_in, gameui_pad_held(0) | gameui_pad_held(1), 0) & PAD_CANCEL) boot_title_back();
        g.r[4] = r[0], g.r[5] = r[1], g.r[6] = r[2], g.r[7] = r[3], g.pr = pr;
    }
    f_8C062350_orig();
}

static void no_presses(void) {
    for (uint32_t p = 0; p < 3; p++) {
        uint32_t pad = PADS + p * PAD_STRIDE;
        W32(pad + 0x0C, R32(pad + 0x08));
        W32(pad + 0x10, 0);
        W32(pad + 0x14, 0);
    }
}

static void draw(void) {
    const float sx = 640.0f / 384.0f;
    sprite_swap_image_behind(IMG_PANEL, PANEL_X * sx, PANEL_TOP * 2.0f, (PANEL_X + PANEL_W) * sx, PANEL_BOTTOM * 2.0f);
    int y = s_choice ? YES_Y : NO_Y;
    sprite_swap_image_behind(IMG_BAR, PANEL_X * sx, 2.0f * y + 10, (PANEL_X + PANEL_W) * sx, 2.0f * y + 34);
    gameui_print_layer("QUIT GAME?", LAYER, SCREEN_W / 2 - gameui_text_w("QUIT GAME?") / 2, QUESTION_Y, COL_WHITE);
    gameui_print_layer("YES", LAYER, SCREEN_W / 2 - gameui_text_w("YES") / 2, YES_Y, s_choice ? COL_GOLD : COL_WHITE);
    gameui_print_layer("NO", LAYER, SCREEN_W / 2 - gameui_text_w("NO") / 2, NO_Y, s_choice ? COL_WHITE : COL_GOLD);
}

void hle_title_logic(void) {
    uint32_t state = g.r[4], r5 = g.r[5];  /* the routine's arguments (the sounds below clobber them) */
    s_title_frame = g_vblank_count;
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, 0x8C0625E0u);
    uint32_t pressed = gameui_presses(&s_in, gameui_pad_held(0) | gameui_pad_held(1), 0);
    if (s_open_next) {  /* Back on the intro's title, which Start brought here */
        if (g_vblank_count - s_open_next <= 300 && !s_open) {
            s_open = 1, s_choice = 0;
            s_in.prev_held = gameui_pad_held(0) | gameui_pad_held(1);
            pressed = 0;
        }
        s_open_next = 0;
    }
    if (!s_open) {
        /* B (the gamepad's usual "back") asks too; the title itself has no use for it */
        if (pressed & PAD_CANCEL) {
            title_quit_toggle();
            gameui_sound(SND_CURSOR);
            s_in.prev_held = gameui_pad_held(0) | gameui_pad_held(1);
            no_presses();
            g.r[4] = state;
            g.r[5] = r5;
        }
        f_8C0625E0_orig();
        if (s_open) draw();
        return;
    }
    if (pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) {
        s_choice ^= 1;
        gameui_sound(SND_CURSOR);
    } else if (pressed & PAD_CANCEL) {
        s_open = 0;
        gameui_sound(SND_CANCEL);
    } else if (pressed & PAD_OK) {
        if (s_choice) g_quit_requested = 1;
        s_open = 0;
        gameui_sound(s_choice ? SND_OK : SND_CANCEL);
    }
    no_presses();
    uint16_t timer = R16(state + TITLE_TIMER);
    g.r[4] = state;
    g.r[5] = r5;
    f_8C0625E0_orig();
    W16(state + TITLE_TIMER, timer);  /* the attract demo waits */
    if (s_open) draw();
}
