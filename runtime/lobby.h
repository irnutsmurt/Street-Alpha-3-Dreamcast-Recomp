/* Mode Menu > NETWORK: the netplay lobby (see lobby.c). */
#pragma once
#include <stdint.h>
#include "input.h"

/* Autopilot: player 1 is driven from power-on to the lobby (netplay resets). */
void lobby_autopilot_start(void);
int lobby_autopilot_active(void);
void lobby_autopilot(PadState *p1);  /* each vblank, after host input is read */
void lobby_on_reset(void);           /* the machine was just reset */
/* VS Dramatic Battle in a session: controller 3 is the game's AI */
int lobby_third_pad(void);
void lobby_cpu_player(void);  /* each vblank, after netplay */
void lobby_boot_skip(void);   /* each vblank, after netplay: Start skips the Capcom logos */
/* The game's AI plays 3P: its "3P" tags are drawn as "CPU". Display only
 * (textures are patched as they're decoded), so it can't affect netplay. */
int lobby_cpu_label(void);
void lobby_patch_texture(uint8_t *idx, uint32_t w, uint32_t h);  /* 4bpp indices, one per byte */
