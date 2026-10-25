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
#define EXT_PER_SLOT 0x400000u

void f_8C01348E_orig(void);

void hle_pac_section(void) {
    uint32_t rec = g.r[4];
    f_8C01348E_orig();
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

/* --- the PSP characters' files --- */

#define NAMES 0x8C09895Cu      /* file number -> name */
#define PL_NAMES 13u           /* plNN.pac is file 13 + NN, but only up to 0x25 (stages follow) */
#define FIRST_NEW 0x26u
#define NEW_COUNT 4u
#define EXT_NAMES (EXT_RAM_BASE + 0xF00000u)  /* our names, where the game can read them */

static void put_name(uint32_t a, const char *s) {
    for (;; a++, s++) {
        W8(a, (uint8_t)*s);
        if (!*s) break;
    }
}

/* a character's archive (pl26.pac and on) named by a name the game indexed
 * past the characters: the stage files' */
static uint32_t new_char_name(uint32_t name) {
    static int ready;
    if (!ready) {
        ready = 1;
        for (uint32_t k = 0; k < NEW_COUNT; k++) {
            char s[16];
            snprintf(s, sizeof s, "pl%02x.pac", FIRST_NEW + k);
            put_name(EXT_NAMES + 16 * k, s);
        }
    }
    for (uint32_t k = 0; k < NEW_COUNT; k++)
        if (name == R32(NAMES + 4 * (PL_NAMES + FIRST_NEW + k))) return EXT_NAMES + 16 * k;
    return name;
}

/* where the game opens a character's archive by 13 + its number */
static int char_archive_site(uint32_t pr) { return pr == 0x8C01B648u; }

/* SFA3_FILE_LOG: log each file the game opens by name (debug) */
void f_8C012C80_orig(void);
void hle_open_file(void) {
    if (char_archive_site(g.pr)) g.r[4] = new_char_name(g.r[4]);
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
