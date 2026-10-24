/* Cheats, as a list of menu items shared by Option Mode > CHEATS and the F1
 * overlay (like options.h for the display settings). Changing an item saves
 * sfa3.cfg. The fight cheats work only in a fight against the CPU, the World
 * Tour ones only in World Tour: never in a netplay session or a fight
 * between two people. */
#pragma once
#include <stddef.h>

enum {
    CHEAT_HEALTH,       /* your health stays full */
    CHEAT_SUPER,        /* your super meter stays full */
    CHEAT_GUARD,        /* your guard meter never wears down */
    CHEAT_HALF_HEALTH,  /* half a health bar: off, you, the CPU or both */
    CHEAT_CPU_KO,       /* the CPU is knocked out by any damage */
    CHEAT_CPU_GUARD_CRUSH, /* the CPU's guard breaks on the next hit it blocks */
    CHEAT_CPU_SUPER,    /* the CPU's super meter stays empty */
    CHEAT_FINAL_BISON,  /* Final Bison unlocked (hold Start on M. Bison) */
    CHEAT_WT_EXP,       /* World Tour: your EXP at the most (9999999) */
    CHEAT_WT_X,         /* World Tour: X-ism EXP at the most */
    CHEAT_WT_A,         /* World Tour: A-ism EXP at the most */
    CHEAT_WT_V,         /* World Tour: V-ism EXP at the most */
    CHEAT_COUNT
};

const char *cheat_label(int i);
void cheat_value(int i, char *out, size_t n);  /* upper-case, game-font safe */
void cheat_change(int i, int dir);             /* dir = -1 / +1 */
void cheats_reset(void);                       /* DEFAULT: the fight cheats off (an unlock stays) */
int cheats_available(void);                    /* not in a netplay session */
int cheats_active(void);                       /* a cheat changed the game this frame (CHEATS ON) */
void cheats_frame(void);                       /* every vblank: apply them */
