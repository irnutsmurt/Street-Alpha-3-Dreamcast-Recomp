/* Debug: which sprite parts the game draws (SFA3_SPRITE_LOG=<frame>); and
 * the PSP characters' endings' fighter sprites (ending_draw, psp_load.c).
 *
 * f_8C018E9C draws one sprite part: r5 the object, r6 the part's 12-byte
 * record, r7 the start of the object's bank of records (8C1A1F48[obj+0x46]).
 * Every part drawn in that frame (or from it on: <frame>+) goes to the log
 * with its object, bank, part number and the caller, to find which object
 * draws what on a screen.
 * SFA3_SPRITE_FIND=<part>: where a part number is drawn (any bank, any frame).
 * SFA3_SPRITE_NEW=1: each (object type, sub-type, bank, part) the first time it's drawn.
 * Always: a part number past its bank's records is logged (the first 20). */
#include <stdlib.h>

#include "cpu.h"
#include "runtime.h"

void f_8C018E9C_orig(void);
extern uint64_t g_vblank_count;

void ending_draw(uint32_t obj);
void hle_sprite_draw(void) {
    static int init, onward, logged;
    ending_draw(g.r[5]);  /* the PSP characters' endings (psp_load.c) */
    static uint64_t frame;
    if (!init) {  /* SFA3_SPRITE_LOG=<frame>: that frame's draws; <frame>+: from that frame on (400 at most) */
        init = 1;
        const char *e = getenv("SFA3_SPRITE_LOG");
        char *end = NULL;
        frame = e ? strtoull(e, &end, 0) : 0;
        onward = end && *end == '+';
    }
    {   /* SFA3_SPRITE_FIND=<hex part>: the first 20 draws of that part number, any bank, with the frame */
        static int find = -2, found;
        if (find == -2) { const char *e = getenv("SFA3_SPRITE_FIND"); find = e ? (int)strtol(e, NULL, 16) : -1; }
        uint32_t obj = g.r[5], rec = g.r[6], base = g.r[7];
        if (find >= 0 && rec >= base && (rec - base) / 12 == (uint32_t)find && found < 20) {
            found++;
            logf_("sprite find: frame %llu obj %08X bank %X part %X pos %d,%d anim %08X caller %08X",
                  (unsigned long long)g_vblank_count, obj, R16(obj + 0x46), find, (int16_t)R16(obj + 0x12),
                  (int16_t)R16(obj + 0x16), R32(obj + 0x1C), g.pr);
        }
    }
    {   /* a part past its bank's records (the first record's offset is their size): the first 20 */
        static int past;
        uint32_t obj = g.r[5], rec = g.r[6], base = g.r[7];
        if (past < 20 && rec >= base && (rec - base) / 12 >= R32(base) / 12) {
            past++;
            logf_("sprite: part %X past bank %X's %u records: frame %llu obj %08X (+2 %02X +3 %02X +39 %02X +79 %02X "
                  "+94 %X) anim %08X caller %08X",
                  (rec - base) / 12, R16(obj + 0x46), R32(base) / 12, (unsigned long long)g_vblank_count, obj,
                  R8(obj + 2), R8(obj + 3), R8(obj + 0x39), R8(obj + 0x79), R32(obj + 0x94), R32(obj + 0x1C), g.pr);
        }
    }
    {   /* SFA3_SPRITE_NEW=1: each (object type, sub-type, bank, part) the first time it's drawn, with the frame */
        static int on = -1, n;
        static uint32_t seen[4096];
        if (on < 0) on = getenv("SFA3_SPRITE_NEW") != NULL;
        uint32_t obj = g.r[5], rec = g.r[6], base = g.r[7];
        if (on && n < 4096 && rec >= base) {
            uint32_t key = R8(obj + 2) << 24 | R8(obj + 3) << 16 | (R16(obj + 0x46) & 0xFF) << 12 | ((rec - base) / 12 & 0xFFF);
            int i = 0;
            while (i < n && seen[i] != key) i++;
            if (i == n) {
                seen[n++] = key;
                logf_("sprite new: frame %llu obj %08X type %02X/%02X bank %X part %X char %02X pal %X pos %d,%d",
                      (unsigned long long)g_vblank_count, obj, R8(obj + 2), R8(obj + 3), R16(obj + 0x46), (rec - base) / 12,
                      R8(obj + 0x39), R8(obj + 0xD), (int16_t)R16(obj + 0x12), (int16_t)R16(obj + 0x16));
            }
        }
    }
    if (frame && (onward ? g_vblank_count >= frame && logged++ < 400 : g_vblank_count == frame)) {
        uint32_t obj = g.r[5], rec = g.r[6], base = g.r[7];
        logf_("sprite: obj %08X type %02X/%02X layer %u bank %X part %X rec %08X pos %d,%d anim %08X caller %08X", obj,
              R8(obj + 2), R8(obj + 3), R8(obj + 0x0E), R16(obj + 0x46), rec >= base ? (rec - base) / 12 : 0xFFFFu, rec,
              (int16_t)R16(obj + 0x12), (int16_t)R16(obj + 0x16), R32(obj + 0x1C), g.pr);
    }
    /* the edges past which it leaves out tile columns: the HUD's the 4:3 screen's (widescreen.c) */
    extern float g_wide_cull;
    float widescreen_sprite_cull(uint32_t obj);
    float cull = g_wide_cull;
    g_wide_cull = widescreen_sprite_cull(g.r[5]);
    f_8C018E9C_orig();
    g_wide_cull = cull;
}

