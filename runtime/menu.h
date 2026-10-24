/* In-game settings menu overlay. */
#pragma once
#include <stdint.h>

#define MENU_W 320
#define MENU_H 240

typedef enum { MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_ACCEPT, MENU_BACK } MenuAction;

int menu_active(void);
void menu_toggle(void);
void menu_action(MenuAction a);
/* RGBA8 canvas of MENU_W x MENU_H; *dirty is set when it changed since the last call */
const uint32_t *menu_canvas(int *dirty);
/* a one-line notice over the game ("" removes it) */
void menu_banner(const char *text);
/* a small tag near the bottom of the picture ("" removes it): CHEATS ON */
void menu_tag(const char *text);
int menu_overlay_visible(void);  /* the menu, a banner or a tag is showing */
void menu_refresh(void);         /* redraw (a binding changed) */
