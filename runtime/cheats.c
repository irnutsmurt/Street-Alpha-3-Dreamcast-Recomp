/* Cheats: values, cycling, and applying them every frame.
 *
 * The fighters are the 4 structures at 0x8C17FD38 (lobby.c): +0 1 = in play,
 * +0x60 health (16 bits, signed; below 0 is a knockout), +0x62 the round's
 * full health (144), +0x14E super meter (144 = full in every -ism), +0x155
 * 1 = played by the CPU, +0x20C the guard meter's size (60, smaller after
 * each guard crush), +0x20D the guard damage taken (a crush when it reaches
 * the size; it wears off between blocks), +0x140 World Tour EXP (a copy of
 * the entry's, shown over the health bar). A round is being fought while the game state
 * (0x8C17FBC0) reads 2 at +2 (a fight) and 3 at +6 (4 = round over, 2 = the
 * round's intro); +0x72 is the game mode (3 = World Tour). Health and meters
 * are rewritten once a frame, after the game's own logic: full health refills
 * between hits. One hit KO keeps the CPU at 1 health point, so the game's own
 * damage takes it below 0 and the knockout plays as usual (setting health
 * below 0 without a hit leaves the round stuck at KNOCK OUT).
 *
 * World Tour keeps an entry per player side, 0x354 bytes from 0x8C188D48:
 * +0 1 = in use, +8 EXP, +0x0C / +0x10 / +0x14 X-, A-, V-ism EXP. The game
 * works the levels out from these after each fight and saves the entry.
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
#define MODE_WORLD_TOUR 3
#define FIGHTERS 0x8C17FD38u
#define FIGHTER_STRIDE 0x44Cu
#define F_ACTIVE 0x000u
#define F_HEALTH 0x060u
#define F_HEALTH_FULL 0x062u
#define F_WT_EXP 0x140u
#define F_SUPER 0x14Eu
#define F_CPU 0x155u
#define F_GUARD_SIZE 0x20Cu
#define F_GUARD_DAMAGE 0x20Du
#define SUPER_FULL 0x90u
#define WT_ENTRIES 0x8C188D48u
#define WT_ENTRY_STRIDE 0x354u
#define WT_EXP 0x08u
#define WT_ISM_EXP 0x0Cu  /* X, A, V: 4 bytes each */
#define EXP_MOST 9999999u
#define UNLOCKS 0x8C09B1CAu
#define UNLOCK_FINAL_BISON 0x0800u

enum { HALF_YOU = 1, HALF_CPU = 2 };  /* cheat_half_health bits */

static const char *const s_labels[CHEAT_COUNT] = {
    "INFINITE HEALTH", "INFINITE SUPER", "INFINITE GUARD", "HALF HEALTH", "ONE HIT KO ON CPU", "GUARD CRUSH ON CPU",
    "NO SUPER FOR CPU", "FINAL BISON",
    "WORLD TOUR MAX EXP", "WORLD TOUR X-ISM EXP", "WORLD TOUR A-ISM EXP", "WORLD TOUR V-ISM EXP",
};

const char *cheat_label(int i) { return i >= 0 && i < CHEAT_COUNT ? s_labels[i] : ""; }

static int *setting(int i) {
    switch (i) {
    case CHEAT_HEALTH: return &g_settings.cheat_health;
    case CHEAT_SUPER: return &g_settings.cheat_super;
    case CHEAT_GUARD: return &g_settings.cheat_guard;
    case CHEAT_CPU_GUARD_CRUSH: return &g_settings.cheat_cpu_guard_crush;
    case CHEAT_HALF_HEALTH: return &g_settings.cheat_half_health;
    case CHEAT_CPU_KO: return &g_settings.cheat_cpu_ko;
    case CHEAT_CPU_SUPER: return &g_settings.cheat_cpu_super;
    case CHEAT_FINAL_BISON: return &g_settings.cheat_final_bison;
    case CHEAT_WT_EXP: return &g_settings.cheat_wt_exp;
    case CHEAT_WT_X: return &g_settings.cheat_wt_x;
    case CHEAT_WT_A: return &g_settings.cheat_wt_a;
    case CHEAT_WT_V: return &g_settings.cheat_wt_v;
    default: return NULL;
    }
}

void cheat_value(int i, char *out, size_t n) {
    static const char *const half[] = {"OFF", "YOU", "CPU", "BOTH"};
    int *v = setting(i);
    if (!v) out[0] = 0;
    else if (i == CHEAT_FINAL_BISON) snprintf(out, n, "%s", *v || (R16(UNLOCKS) & UNLOCK_FINAL_BISON) ? "UNLOCKED" : "LOCKED");
    else if (i == CHEAT_HALF_HEALTH) snprintf(out, n, "%s", half[*v & 3]);
    else snprintf(out, n, "%s", *v ? "ON" : "OFF");
}

void cheat_change(int i, int dir) {
    int *v = setting(i);
    if (!v) return;
    if (i == CHEAT_FINAL_BISON) {
        /* an unlock: on for good, like earning it (it is in the game's save) */
        *v = 1;
        if (cheats_available()) W16(UNLOCKS, R16(UNLOCKS) | UNLOCK_FINAL_BISON);
    } else if (i == CHEAT_HALF_HEALTH) {
        *v = (*v + (dir < 0 ? 3 : 1)) & 3;
    } else {
        *v = !*v;
    }
    settings_save();
}

void cheats_reset(void) {
    g_settings.cheat_health = g_settings.cheat_super = g_settings.cheat_half_health = 0;
    g_settings.cheat_cpu_ko = g_settings.cheat_cpu_super = 0;
    g_settings.cheat_guard = g_settings.cheat_cpu_guard_crush = 0;
    g_settings.cheat_wt_exp = g_settings.cheat_wt_x = g_settings.cheat_wt_a = g_settings.cheat_wt_v = 0;
    settings_save();
}

int cheats_available(void) { return !netplay_active(); }

static uint32_t fighter(int i) { return FIGHTERS + (uint32_t)i * FIGHTER_STRIDE; }

static int fight_cheats_on(void) {
    return g_settings.cheat_health || g_settings.cheat_super || g_settings.cheat_half_health || g_settings.cheat_cpu_ko ||
           g_settings.cheat_cpu_super || g_settings.cheat_guard || g_settings.cheat_cpu_guard_crush;
}

static int wt_cheats_on(void) {
    return g_settings.cheat_wt_exp || g_settings.cheat_wt_x || g_settings.cheat_wt_a || g_settings.cheat_wt_v;
}

/* one person playing, the rest the CPU */
static int vs_cpu(void) {
    int people = 0, cpus = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t f = fighter(i);
        if (R8(f + F_ACTIVE) != 1) continue;
        if (R8(f + F_CPU)) cpus++;
        else people++;
    }
    return people == 1 && cpus >= 1;
}

static int round_on(void) { return R16(STATE + 2) == 2 && R16(STATE + 6) == 3; }

static int s_applied;  /* a cheat is changing the current fight */
int cheats_active(void) { return s_applied; }

static void world_tour(int in_round) {
    for (uint32_t side = 0; side < 2; side++) {
        uint32_t e = WT_ENTRIES + side * WT_ENTRY_STRIDE;
        if (R8(e) != 1) continue;  /* not in use */
        if (g_settings.cheat_wt_exp) W32(e + WT_EXP, EXP_MOST);
        const int ism[3] = {g_settings.cheat_wt_x, g_settings.cheat_wt_a, g_settings.cheat_wt_v};
        for (uint32_t k = 0; k < 3; k++)
            if (ism[k]) W32(e + WT_ISM_EXP + 4 * k, EXP_MOST);
    }
    if (in_round && g_settings.cheat_wt_exp)  /* the fight's own copy, shown over the health bar */
        for (int i = 0; i < 4; i++)
            if (R8(fighter(i) + F_ACTIVE) == 1 && !R8(fighter(i) + F_CPU)) W32(fighter(i) + F_WT_EXP, EXP_MOST);
}

void cheats_frame(void) {
    s_applied = 0;
    if (!cheats_available()) return;
    if (g_settings.cheat_final_bison && !(R16(UNLOCKS) & UNLOCK_FINAL_BISON))
        W16(UNLOCKS, R16(UNLOCKS) | UNLOCK_FINAL_BISON);
    int in_round = round_on() && vs_cpu();
    if (wt_cheats_on() && R8(STATE + 0x72) == MODE_WORLD_TOUR && (in_round || R16(STATE + 2) != 2)) {
        world_tour(in_round);
        s_applied = in_round;
    }
    if (!fight_cheats_on() || !in_round) return;
    s_applied = 1;
    for (int i = 0; i < 4; i++) {
        uint32_t f = fighter(i);
        if (R8(f + F_ACTIVE) != 1) continue;
        int cpu = R8(f + F_CPU) != 0;
        int16_t health = (int16_t)R16(f + F_HEALTH), full = (int16_t)R16(f + F_HEALTH_FULL);
        if (g_settings.cheat_half_health & (cpu ? HALF_CPU : HALF_YOU)) full /= 2;
        if (!cpu) {
            if (g_settings.cheat_health && health < full) health = full;
            if (g_settings.cheat_super) W16(f + F_SUPER, SUPER_FULL);
            if (g_settings.cheat_guard) W8(f + F_GUARD_DAMAGE, 0);
        } else {
            if (g_settings.cheat_cpu_ko && health > 1) health = 1;
            if (g_settings.cheat_cpu_super) W16(f + F_SUPER, 0);
            uint32_t size = R8(f + F_GUARD_SIZE);
            if (g_settings.cheat_cpu_guard_crush && size > 1 && R8(f + F_GUARD_DAMAGE) < size - 1)
                W8(f + F_GUARD_DAMAGE, size - 1);
        }
        if (health > full) health = full;  /* half health */
        if (health != (int16_t)R16(f + F_HEALTH) && R16(f + F_HEALTH) < 0x8000u) W16(f + F_HEALTH, (uint16_t)health);
    }
}
