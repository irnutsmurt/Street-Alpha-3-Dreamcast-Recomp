#pragma once
#include <stdint.h>

/* Dreamcast controller button bits (active-high here; Maple inverts them) */
enum {
    BTN_C = 1 << 0, BTN_B = 1 << 1, BTN_A = 1 << 2, BTN_START = 1 << 3,
    BTN_UP = 1 << 4, BTN_DOWN = 1 << 5, BTN_LEFT = 1 << 6, BTN_RIGHT = 1 << 7,
    BTN_Z = 1 << 8, BTN_Y = 1 << 9, BTN_X = 1 << 10, BTN_D = 1 << 11,
};

typedef struct {
    uint16_t buttons;
    uint8_t ltrig, rtrig;
    uint8_t ax, ay;
} PadState;

int input_connected(int port);
int input_pad_present(int port);  /* a real gamepad drives this port */
void input_poll(int port, PadState *out);
PadState *input_state(int port);  /* this frame's state of a port (netplay replaces it) */
void input_update(void);  /* pump host events, once per frame */
void input_init(void);
/* Text entry (lobby name / address): typed characters instead of the
 * keyboard's controller keys. input_text_get(): next character, '\b'
 * (Backspace), '\n' (Enter), 27 (Esc), or -1. */
void input_text_begin(void);
void input_text_end(void);
int input_text_get(void);
void maple_init(const char *save_dir);
void maple_flush_saves(void);
