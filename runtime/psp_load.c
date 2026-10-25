/* PSP characters: sprite tiles too big for a player's share of the character
 * data area.
 *
 * The game loads a character's archive sections one after another into an
 * area of 0x140000 bytes per player from 0x8C347300 (0x8C1A2170 is where the
 * next one goes; a player's share starts at slot * 0x140000). The PSP had more
 * memory: some of its characters' sprite tiles (section 0x200000) are bigger
 * than a share. Those go to the extension memory instead (4 MB per player
 * slot), and the running offset is set back so the rest stays in the share.
 * f_8C01348E reads the next section's header into the load record (r4): +100
 * tag, +48 size, +44 where to put it, +104 / +105 the sprite bank and slot. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

#include "runtime.h"

#define AREA 0x8C347300u
#define SHARE 0x140000u
#define NEXT 0x8C1A2170u
#define TILE_BY_SLOT 0x8C1A2150u
#define TILE_BY_BANK 0x8C1A1FE8u
#define PALETTE_ROOM 0x800u  /* the palettes still to come */
#define EXT_PER_SLOT 0x200000u  /* the biggest, Ingrid's, is 0x1661DB */
#define PARTS_BY_BANK 0x8C1A1F48u
#define PALETTES_BY_BANK 0x8C1A2088u

/* SELECT.PAC's sprite banks (8 and B) go to fixed places with little room
 * between them; the add-on's, with the PSP characters' sprites added, are
 * bigger: a section that doesn't fit its place goes to the extension memory
 * instead (the bank's table points there) */
#define EXT_SELECT (EXT_RAM_BASE + 0x600000u)
static const struct { uint32_t tag, room, at, table; } s_select[] = {
    {0x02000008u, 0x8CB94C00u - 0x8CAF4800u, 0x000000u, TILE_BY_BANK},   /* bank 8 tiles */
    {0x04000008u, 0x8CBA1C00u - 0x8CB94C00u, 0x200000u, PALETTES_BY_BANK},
    {0x01000008u, 0x8CBB4800u - 0x8CBA1C00u, 0x220000u, PARTS_BY_BANK},
    {0x0200000Bu, 0x8CBFC800u - 0x8CBB4800u, 0x260000u, TILE_BY_BANK},  /* bank B tiles */
    {0x0100000Bu, 0x00001C00u, 0x2E0000u, PARTS_BY_BANK},
};

static void select_section(uint32_t rec) {
    uint32_t tag = R32(rec + 100), size = R32(rec + 48);
    for (unsigned i = 0; i < sizeof s_select / sizeof s_select[0]; i++) {
        if (s_select[i].tag != tag || size <= s_select[i].room) continue;
        uint32_t dest = EXT_SELECT + s_select[i].at;
        W32(rec + 44, dest);
        W32(s_select[i].table + 4 * (tag & 0xFF), dest);
        logf_("select sprites: section %08X of %u bytes to extension memory at %08X", tag, size, dest);
    }
}

void f_8C01348E_orig(void);

void hle_pac_section(void) {
    uint32_t rec = g.r[4];
    f_8C01348E_orig();
    { static int log = -1; if (log < 0) log = getenv("SFA3_PAC_LOG") != NULL;
      if (log) logf_("pac section: tag %08X size %u to %08X (bank %u slot %u, next %08X)", R32(rec + 100), R32(rec + 48),
                     R32(rec + 44), R8(rec + 104), R8(rec + 105), R32(NEXT)); }
    if ((R32(rec + 100) & 0xF0FFFF00u) == 0) select_section(rec);
    if ((R32(rec + 100) & 0xFFFF0000u) != 0x00200000u) return;
    uint32_t size = R32(rec + 48), next = R32(NEXT);
    uint32_t slot = next / SHARE, used = next % SHARE;
    if (used + size + PALETTE_ROOM <= SHARE || slot >= 3 || size > EXT_PER_SLOT) return;
    uint32_t dest = EXT_RAM_BASE + slot * EXT_PER_SLOT;
    W32(rec + 44, dest);
    W32(TILE_BY_SLOT + 4 * R8(rec + 105), dest);
    W32(TILE_BY_BANK + 4 * R8(rec + 104), dest);
    W32(NEXT, next - ((size + 3) & ~3u));  /* the loader adds the size when the section is in */
    logf_("sprite tiles of %u bytes: to extension memory at %08X (player slot %u)", size, dest, slot);
}

/* --- the PSP characters' files ---
 * The game names its files by number: names[] (8C09895C, 271 names; moved
 * by chartables.py with room for the add-on's after them). A character's
 * files are numbered by family: pl 13 + n, rk 90 + n, op 126 + n, with n the
 * character number (one less above 0x12, whose files are 0x11's), the others
 * through tables (win, bd, end: moved, the add-on fills them). For 0x26 and
 * up the numbers run into the next family; the openers below get the
 * add-on's names instead: NAME_COUNT + 4 x family + (character - 0x26), the
 * families in chartables.py's FAMILIES order. */
#include "chartables.h"

#define NAMES 0x8C09895Cu
#define NAME_COUNT 271u
enum { F_PL, F_OP, F_RK };  /* chartables.py FAMILIES: pl op rk win bd end */
#define PL_NAMES 13u        /* plNN.pac is file 13 + NN, but only up to 0x25 (stages follow) */
#define FIRST_NEW 0x26u
#define NEW_COUNT 4u

/* the add-on's name for a new character's file (0 when it has none: the
 * moved table's spare entries are copies of names[0]) */
static uint32_t new_name(unsigned family, uint32_t c) {
    uint32_t names = char_table_moved(NAMES);
    uint32_t p = R32(names + 4u * (NAME_COUNT + 4u * family + (c - FIRST_NEW)));
    return p == R32(names) ? 0 : p;
}

/* a family numbered by n (see above): the character of n */
static uint32_t char_of_position(uint32_t n) { return n > 0x12u ? n + 1u : n; }

/* f_8C012C80(name, ...) opens a file by name; the callers that index names[]
 * by character: f_8C01B598 the archive (pl), f_8C01C8D0 the ranking's
 * portraits (rk, the position in r14) */
static uint32_t char_file_name(uint32_t pr, uint32_t name) {
    uint32_t c = 0, family = 0;
    if (pr == 0x8C01B648u) {  /* the archive: names[13 + n], n read past the pl names */
        for (uint32_t k = 0; k < NEW_COUNT; k++)
            if (name == R32(NAMES + 4 * (PL_NAMES + FIRST_NEW + k))) c = FIRST_NEW + k, family = F_PL;
    } else if (pr == 0x8C01C9F6u) {
        c = char_of_position(g.r[14] & 0xFFu), family = F_RK;
    }
    uint32_t p = c - FIRST_NEW < NEW_COUNT ? new_name(family, c) : 0;
    return p ? p : name;
}

/* f_8C0126F4(number): opens names[number]; f_8C01B904 opens the prologue
 * (op, 126 + n) */
void f_8C0126F4_orig(void);
void hle_open_numbered(void) {
    if (g.pr == 0x8C01BA44u && g.r[4] >= 126u) {
        uint32_t c = char_of_position(g.r[4] - 126u);
        if (c - FIRST_NEW < NEW_COUNT && new_name(F_OP, c))
            g.r[4] = NAME_COUNT + 4u * F_OP + (c - FIRST_NEW);
    }
    f_8C0126F4_orig();
}

/* SFA3_FILE_LOG: log each file the game opens by name (debug) */
void f_8C012C80_orig(void);
void hle_open_file(void) {
    g.r[4] = char_file_name(g.pr, g.r[4]);
    static int on = -1;
    if (on < 0) on = getenv("SFA3_FILE_LOG") != NULL;
    if (on) {
        char name[24];
        uint32_t p = g.r[4];
        int k = 0;
        for (; k < 23 && R8(p + (uint32_t)k); k++) name[k] = (char)R8(p + (uint32_t)k);
        name[k] = 0;
        uint32_t pr = g.pr, arg = g.r[5];
        f_8C012C80_orig();
        logf_("file: open %s (%08X) from %08X = %08X", name, arg, pr, g.r[0]);
        return;
    }
    f_8C012C80_orig();
}

/* --- the select screen's big portraits: their palettes ---
 * f_8C06BA42 & co. load a portrait's palette, 0x4C0 + character x 6 +
 * colour, from bank 8's palettes (f_8C069D64(index, slot, count)); for 0x26
 * and on that's other palettes (0x5A4 is Sodom's extra one). The add-on puts
 * the PSP characters' after the Dreamcast's: from g_psp_portrait_palettes. */
extern const uint32_t g_psp_portrait_palettes;
void f_8C069D64_orig(void);
void hle_select_palette(void) {
    uint32_t first = 0x4C0u + FIRST_NEW * 6u;
    if (g.pr == 0x8C06BB96u && g_psp_portrait_palettes && g.r[4] - first < NEW_COUNT * 6u)
        g.r[4] = g_psp_portrait_palettes + (g.r[4] - first);
    f_8C069D64_orig();
}

/* --- the VS screen's portraits: their palettes ---
 * f_8C069D34(character, -, colour, slot) loads 4 palettes from bank 8's
 * 0x148 + character x 24 + colour x 4; for 0x26 and on that's the big
 * portraits' (0x4C0 on). The add-on puts the PSP characters' at an index this
 * formula reaches with the character number + g_psp_vs_palette_shift. */
extern const uint32_t g_psp_vs_palette_shift;
void f_8C069D34_orig(void);
void hle_vs_palette(void) {
    if (g_psp_vs_palette_shift && (g.r[4] & 0xFFFFu) - FIRST_NEW < NEW_COUNT) g.r[4] += g_psp_vs_palette_shift;
    f_8C069D34_orig();
}

/* --- the select screens' face layouts: the PSP characters' extra cells ---
 * f_8C018084 draws an object's layout: a list of (x, y, part) records from
 * obj+0x8C + pose x 6 (the pose from the object's animation). Objects share a
 * base and pick lists by pose: arcade's 0x8C0BF610 (faces pose 0, frames 43),
 * versus' 0x8C0C0714 (faces 0, frames 41). The PSP characters' cells that
 * aren't in those lists are drawn from extra lists (base, pose, list): after
 * the object's own, its base pointed so its pose finds the extra list. */
extern const uint32_t g_psp_layout_extras[][3];
extern const unsigned g_psp_layout_extra_count;
void f_8C018084_orig(void);
void hle_layout_draw(void) {
    uint32_t r4 = g.r[4], obj = g.r[5];
    f_8C018084_orig();
    if (!g_psp_layout_extra_count || !R32(obj + 0x1C)) return;
    uint32_t base = R32(obj + 0x8C), pose = R16(R32(obj + 0x1C) + 4);
    for (unsigned i = 0; i < g_psp_layout_extra_count; i++) {
        if (base != g_psp_layout_extras[i][0] || pose != g_psp_layout_extras[i][1]) continue;
        W32(obj + 0x8C, g_psp_layout_extras[i][2] - 6u * pose);
        g.r[4] = r4;
        g.r[5] = obj;
        f_8C018084_orig();
        W32(obj + 0x8C, base);
    }
}

/* f_8C017900 draws a layout the same way, its base from a table of them,
 * 8C188810[obj+0x84 - 1] (versus' select screen: 0x8C0C0714) */
#define LAYOUT_SETS 0x8C188810u
void f_8C017900_orig(void);
void hle_layout_draw_set(void) {
    uint32_t r4 = g.r[4], obj = g.r[5];
    f_8C017900_orig();
    if (!g_psp_layout_extra_count || !R32(obj + 0x1C) || !R32(obj + 0x84)) return;
    uint32_t set = LAYOUT_SETS + 4u * (R32(obj + 0x84) - 1u);
    uint32_t base = R32(set), pose = R16(R32(obj + 0x1C) + 4);
    for (unsigned i = 0; i < g_psp_layout_extra_count; i++) {
        if (base != g_psp_layout_extras[i][0] || pose != g_psp_layout_extras[i][1]) continue;
        W32(set, g_psp_layout_extras[i][2] - 6u * pose);
        g.r[4] = r4;
        g.r[5] = obj;
        f_8C017900_orig();
        W32(set, base);
    }
}

/* --- the use counters ---
 * f_8C069DCC(fighter) counts a character's games by -ism: 8C09B20C, 8 bytes a
 * character (u16 per -ism), part of the system data saved to the memory
 * card, with the ranking right after it (8C09B33C). The PSP characters have
 * no room there: not counted. */
void f_8C069DCC_orig(void);
void hle_count_use(void) {
    if ((uint32_t)R8(g.r[4] + 0x132u) - FIRST_NEW < NEW_COUNT) return;
    f_8C069DCC_orig();
}

/* --- throws on the PSP characters ---
 * A character's descriptor word 9 (+36) is a table by the opponent's
 * character number (38 entries, in its character file) of where a held
 * opponent goes, frame by frame: f_8C05206E, f_8C0520F0, f_8C052180 and
 * f_8C052268 read it. For a PSP character as the opponent the add-on has the
 * PSP's lists for every Dreamcast character (tools/psp/throws.py): u32
 * [character file][4] at THROW_LISTS. When the opponent is one, the
 * attacker's descriptor (fighter +0x180: the fighter's own copy in RAM, made
 * when its character loads) gets a 42-entry copy of its table in the
 * extension RAM. */
#define THROW_LISTS 0x98E20000u                       /* chartables.EXT_THROWS */
#define THROW_TABLES (EXT_RAM_BASE + 0xC00000u)       /* a copy per fighter */
#define FIGHTERS 0x8C17FD38u
#define FIGHTER_SIZE 0x44Cu

static void throw_table(uint32_t fighter) {
    uint32_t opponent = R32(fighter + 76u);
    if (!opponent || (uint32_t)R8(opponent + 0x132u) - FIRST_NEW >= NEW_COUNT) return;
    uint32_t c = R8(fighter + 0x132u), desc = R32(fighter + 0x180u);
    if (c == 0x13u) c = 0x11u;  /* Gen's second file uses the first's code */
    uint32_t n = (fighter - FIGHTERS) / FIGHTER_SIZE;
    if (c >= FIRST_NEW || n > 3 || !R32(THROW_LISTS + 16u * c)) return;
    uint32_t table = R32(desc + 36u), copy = THROW_TABLES + n * 0x100u;
    if (table == copy) return;
    for (uint32_t k = 0; k < FIRST_NEW; k++) W32(copy + 4u * k, R32(table + 4u * k));
    for (uint32_t k = 0; k < NEW_COUNT; k++) W32(copy + 4u * (FIRST_NEW + k), R32(THROW_LISTS + 16u * c + 4u * k));
    W32(desc + 36u, copy);
}

void f_8C05206E_orig(void);
void f_8C0520F0_orig(void);
void f_8C052180_orig(void);
void f_8C052268_orig(void);
void hle_throw_a(void) { throw_table(g.r[4]); f_8C05206E_orig(); }
void hle_throw_b(void) { throw_table(g.r[4]); f_8C0520F0_orig(); }
void hle_throw_c(void) { throw_table(g.r[4]); f_8C052180_orig(); }
void hle_throw_d(void) { throw_table(g.r[4]); f_8C052268_orig(); }

/* --- the fight's HUD: the PSP characters' faces and names ---
 * f_8C06443C(hud) sets the HUD's parts each frame: player 1's face (bank
 * 0x10) 0x17D + character at hud +0x30C, player 2's 0x1A6 + character at
 * +0x94C, the names (bank 9) 0xF6 / 0x121 + 8C0BE266[character] at +0x330 /
 * +0x970, from the fighters it keeps at +12 / +32. For the PSP characters
 * those are other parts: the add-on's (tools/psp/hud.py) go in instead. */
extern const uint16_t g_psp_hud_parts[4][4];  /* per new character: face 1, face 2, name 1, name 2 */
void f_8C06443C_orig(void);
void hle_hud(void) {
    uint32_t hud = g.r[4];
    f_8C06443C_orig();
    if (!g_psp_hud_parts[0][0]) return;
    static const uint32_t face_at[2] = {52 + 0x280 + 60, 52 + 0x8C0 + 60}, name_at[2] = {0x330, 0x970};
    static const uint32_t face_base[2] = {0x17D, 0x1A6}, name_base[2] = {0xF6, 0x121};
    uint32_t names = char_table_moved(0x8C0BE266u);
    for (int side = 0; side < 2; side++) {
        uint32_t f = R32(hud + (side ? 32u : 12u));
        if (f - FIGHTERS >= 4 * FIGHTER_SIZE) continue;
        uint32_t c = R8(f + 0x132u);
        if (c - FIRST_NEW >= NEW_COUNT) continue;
        const uint16_t *p = g_psp_hud_parts[c - FIRST_NEW];
        if (R16(hud + face_at[side]) == face_base[side] + c) W16(hud + face_at[side], p[side]);
        if (R16(hud + name_at[side]) == name_base[side] + R8(names + c)) W16(hud + name_at[side], p[2 + side]);
    }
}
