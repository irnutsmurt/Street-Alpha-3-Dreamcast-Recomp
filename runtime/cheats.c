/* Cheats: values, cycling, and applying them every frame.
 *
 * The fighters are the 4 structures at 0x8C17FD38 (lobby.c): +0 1 = in play,
 * +0x60 health (16 bits, signed; a negative value is a knockout), +0x62 the
 * round's full health (144), +0x14E super meter (144 = full in every -ism),
 * +0x155 1 = played by the CPU. A round is being fought while the game state
 * (0x8C17FBC0) reads 2 at +2 (a fight) and 3 at +6 (4 = round over, 2 = the
 * round's intro). Health and meters are rewritten once a frame, after the
 * game's own logic: full health refills between hits, and the CPU's health
 * going below full is set negative, which the game takes as a knockout.
 *
 * Final Bison is unlocked by bit 0x800 of 0x8C09B1CA (part of the system
 * data the game saves to the memory card), which the character select checks
 * when Start is held on M. Bison. It is set every frame while the cheat is on
 * and goes into the save the next time the game writes it. */
#include <stdio.h>

#include "cheats.h"
#include "cpu.h"
#include "netplay.h"
#include "settings.h"

#define STATE 0x8C17FBC0u
#define FIGHTERS 0x8C17FD38u
#define FIGHTER_STRIDE 0x44Cu
#define F_ACTIVE 0x000u
#define F_HEALTH 0x060u
#define F_HEALTH_FULL 0x062u
#define F_SUPER 0x14Eu
#define F_CPU 0x155u
#define SUPER_FULL 0x90u
#define UNLOCKS 0x8C09B1CAu
#define UNLOCK_FINAL_BISON 0x0800u

static const char *const s_labels[CHEAT_COUNT] = {
    "INFINITE HEALTH", "INFINITE SUPER", "ONE HIT KO ON CPU", "NO SUPER FOR CPU", "FINAL BISON",
};

const char *cheat_label(int i) { return i >= 0 && i < CHEAT_COUNT ? s_labels[i] : ""; }

static int *setting(int i) {
    switch (i) {
    case CHEAT_HEALTH: return &g_settings.cheat_health;
    case CHEAT_SUPER: return &g_settings.cheat_super;
    case CHEAT_CPU_KO: return &g_settings.cheat_cpu_ko;
    case CHEAT_CPU_SUPER: return &g_settings.cheat_cpu_super;
    case CHEAT_FINAL_BISON: return &g_settings.cheat_final_bison;
    default: return NULL;
    }
}

void cheat_value(int i, char *out, size_t n) {
    int *v = setting(i);
    if (i == CHEAT_FINAL_BISON) snprintf(out, n, "%s", *v || (R16(UNLOCKS) & UNLOCK_FINAL_BISON) ? "UNLOCKED" : "LOCKED");
    else snprintf(out, n, "%s", v && *v ? "ON" : "OFF");
}

void cheat_change(int i, int dir) {
    (void)dir;
    int *v = setting(i);
    if (!v) return;
    if (i == CHEAT_FINAL_BISON) {
        /* an unlock: on for good, like earning it (it is in the game's save) */
        *v = 1;
        if (cheats_available()) W16(UNLOCKS, R16(UNLOCKS) | UNLOCK_FINAL_BISON);
    } else {
        *v = !*v;
    }
    settings_save();
}

void cheats_reset(void) {
    g_settings.cheat_health = g_settings.cheat_super = 0;
    g_settings.cheat_cpu_ko = g_settings.cheat_cpu_super = 0;
    settings_save();
}

int cheats_available(void) { return !netplay_active(); }

static uint32_t fighter(int i) { return FIGHTERS + (uint32_t)i * FIGHTER_STRIDE; }

static int fight_cheats_on(void) {
    return g_settings.cheat_health || g_settings.cheat_super || g_settings.cheat_cpu_ko || g_settings.cheat_cpu_super;
}

/* a round against the CPU is being fought: one person playing, the rest the CPU */
static int vs_cpu_round(void) {
    if (R16(STATE + 2) != 2 || R16(STATE + 6) != 3) return 0;
    int people = 0, cpus = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t f = fighter(i);
        if (R8(f + F_ACTIVE) != 1) continue;
        if (R8(f + F_CPU)) cpus++;
        else people++;
    }
    return people == 1 && cpus >= 1;
}

int cheats_active(void) { return cheats_available() && fight_cheats_on() && vs_cpu_round(); }

void cheats_frame(void) {
    if (!cheats_available()) return;
    if (g_settings.cheat_final_bison && !(R16(UNLOCKS) & UNLOCK_FINAL_BISON))
        W16(UNLOCKS, R16(UNLOCKS) | UNLOCK_FINAL_BISON);
    if (!fight_cheats_on() || !vs_cpu_round()) return;
    for (int i = 0; i < 4; i++) {
        uint32_t f = fighter(i);
        if (R8(f + F_ACTIVE) != 1) continue;
        int16_t health = (int16_t)R16(f + F_HEALTH), full = (int16_t)R16(f + F_HEALTH_FULL);
        if (!R8(f + F_CPU)) {
            if (g_settings.cheat_health && health < full) W16(f + F_HEALTH, (uint16_t)full);
            if (g_settings.cheat_super) W16(f + F_SUPER, SUPER_FULL);
        } else {
            if (g_settings.cheat_cpu_ko && health >= 0 && health < full) W16(f + F_HEALTH, 0xFFFFu);
            if (g_settings.cheat_cpu_super) W16(f + F_SUPER, 0);
        }
    }
}
