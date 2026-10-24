/* Replacing some of the game's sprites with the port's own images (see
 * sprite_swap.c). Display only: the game never sees it. */
#pragma once
#include <stdint.h>

#include "pvr.h"

/* a texture word no real texture has: the port's image `id` */
#define SPRITE_SWAP_TCW(id) (0xFFF00000u | (uint32_t)(id))
#define SPRITE_SWAP_IS_TCW(tcw) (((tcw) >> 20) == 0xFFFu)
#define SPRITE_SWAP_ID(tcw) ((tcw) & 0xFFu)

/* before a frame is drawn: hide the replaced sprites, add the images */
void sprite_swap_frame(PvrFrame *pf);
/* BUTTON CONFIG (buttoncfg.c), for the frame being built: player p's cursor
 * bar (a plain 260-pixel bar at left_px) narrowed to width_px (left_px < 0:
 * as is); and the 3-punch / 3-kick icons added under the heavy ones */
void sprite_swap_bar(int p, float left_px, float width_px);
void sprite_swap_attack_icons(int on);
/* the port's images, by id */
enum { IMG_VMU_CARD, IMG_VMU_EMPTY, IMG_PS_CROSS, IMG_PS_CIRCLE, IMG_PS_SQUARE, IMG_PS_TRIANGLE, IMG_PS_L1,
       IMG_PS_R1, IMG_PS_L2, IMG_PS_R2, IMG_XB_A, IMG_XB_B, IMG_XB_X, IMG_XB_Y, IMG_XB_LB, IMG_XB_RB, IMG_XB_LT,
       IMG_XB_RT, IMG_PANEL, IMG_BAR, IMG_COUNT,  /* PANEL / BAR: the pause menu's colours */
       IMG_CHEATS = IMG_COUNT };                  /* made at run time: Option Mode's CHEATS label */
/* Option Mode's CHEATS button is on screen this frame (option_menu.c) */
void sprite_swap_cheats_button(void);
/* an image over the game's picture for the frame being built (screen
 * pixels of the 640x480 picture); sprite_swap_images_begin() clears them */
void sprite_swap_images_begin(void);
void sprite_swap_image_at(int img, float x0, float y0, float x1, float y1);
/* the same, but behind the frame's frontmost sprites (text drawn this frame stays in front) */
void sprite_swap_image_behind(int img, float x0, float y0, float x1, float y1);
/* the image's RGBA8 pixels, or NULL */
const uint32_t *sprite_swap_image(uint32_t id, uint32_t *w, uint32_t *h);
