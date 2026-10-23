/* Controller setup for players 1 and 2: which device each one uses
 * (keyboard or a gamepad), the button names to show for it, and the
 * bindings from keys / gamepad buttons to the game's controls. Edited on the
 * F1 / Esc menu's CONTROLS page; saved in sfa3.cfg. */
#pragma once
#include <stdio.h>

#include "input.h"

/* the game's controls; an attack presses whichever Dreamcast button the
 * game's own button setting gives it */
enum { ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_LP, ACT_MP, ACT_HP, ACT_LK, ACT_MK, ACT_HK, ACT_START,
       ACT_3P, ACT_3K, ACT_COUNT };  /* 3P / 3K: all three punches / kicks at once */
#define ACT_FIRST_BUTTON ACT_LP  /* gamepads: directions are always the d-pad and left stick */

/* CONTROLLER setting */
enum { CTL_AUTO, CTL_XBOX, CTL_PLAYSTATION, CTL_SWITCH, CTL_KEYBOARD, CTL_COUNT };

const char *controls_action_name(int act);
const char *controls_type_name(int type);
int controls_type(int player);
void controls_set_type(int player, int type);
int controls_pad_choice(int player);         /* 0 = auto, 1-4 = that gamepad */
void controls_set_pad_choice(int player, int n);
int controls_pad_count(void);
/* which binding set the CONTROLS page shows for this player: 1 keyboard, 0 gamepad */
int controls_edits_keyboard(int player);
/* the binding's name as shown (keyboard key or gamepad button), "-" if none */
void controls_binding_name(int player, int act, char *out, size_t n);
void controls_reset(int player);
/* a short name (at most `max` characters, game-font safe) for in-game screens */
void controls_short_name(int player, int act, int max, char *out, size_t n);
const char *controls_short_type_name(int type);  /* AUTO XBOX PS SWITCH KEYBOARD */
/* the picture of the gamepad button a control is bound to: an image id
 * (sprite_swap.h) for PlayStation and Xbox face / shoulder buttons, else -1 */
int controls_button_icon(int player, int act);

/* binding capture: the next key / button pressed is bound to this action */
void controls_capture_begin(int player, int act);
int controls_capturing(void);
int controls_capture_player(void);  /* -1 when not capturing */
int controls_capture_action(void);
void controls_capture_cancel(void);
/* raw events while capturing: 1 if taken (bound or cancelled) */
int controls_capture_key(int scancode);
int controls_capture_pad(int pad_index, int binding);  /* SDL button, or PADB_LT / PADB_RT */
enum { PADB_LT = 100, PADB_RT = 101 };

/* input: this port's state from its device(s); pads = the open gamepads */
struct SDL_Gamepad;
void controls_read(int port, struct SDL_Gamepad *const *pads, int text_entry, PadState *st);
int controls_port_has_device(int port, struct SDL_Gamepad *const *pads);

/* a Jump Pack condition (4 bytes as sent) for this controller port: rumble its gamepad */
void controls_rumble(int port, const uint8_t *cond);

/* sfa3.cfg: "pN_..." lines */
int controls_cfg_read(const char *key, const char *val);
void controls_cfg_write(FILE *f);
