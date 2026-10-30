/* Character code overlays (PLnnX.BIN at 0x8C800000). Several overlays share
 * the same addresses, so a call is matched to the overlay whose bytes are
 * currently in RAM. */
#include <string.h>

#include "cpu.h"
#include "overlay.h"
#include "runtime.h"

/* The file loaded in each player slot (PLnnA / B / C, 0x14000 apart from 0x8C800000; an ending, ENDnn, in the
 * first): many functions are the same bytes in several files (all 33 endings' f_8C8077AC), and a recompiled
 * function calls its own file's others directly, so the one matched by its bytes alone (the first file's) can call
 * code that isn't the loaded file's: R. Mika's ending ran Ryu's f_8C8077E8, which called into her ending's text. The
 * loaded file's function is taken when it matches; any file's otherwise (a file not recompiled). */
static int s_slot_tag[3] = {-1, -1, -1};

static int tag_of(const char *t) {
    for (unsigned i = 0; i < g_overlay_tag_count; i++)
        if (!strcmp(g_overlay_tags[i], t)) return (int)i;
    return -1;
}

void overlay_file_opened(const char *name) {
    const char *b = name;
    for (const char *q = name; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    char t[8];
    int n = 0;
    for (; b[n] && b[n] != '.' && n < 7; n++) t[n] = (char)(b[n] >= 'a' && b[n] <= 'z' ? b[n] - 32 : b[n]);
    t[n] = 0;
    if (strncmp(b + n, ".BIN", 4) && strncmp(b + n, ".bin", 4)) return;
    if (n == 5 && !strncmp(t, "END", 3)) {  /* the PSP characters' endings (26-29) are END06's code */
        if (!strcmp(t, "END26") || !strcmp(t, "END27") || !strcmp(t, "END28") || !strcmp(t, "END29")) strcpy(t, "END06");
        s_slot_tag[0] = tag_of(t);
    } else if (n == 5 && t[0] == 'P' && t[1] == 'L' && t[4] >= 'A' && t[4] <= 'C') {
        s_slot_tag[t[4] - 'A'] = tag_of(t);
    }
}

GuestFn overlay_lookup(uint32_t addr) {
    addr = (addr & 0x1FFFFFFF) | 0x80000000u;
    /* entries are sorted by address; scan candidates at this address */
    unsigned lo = 0, hi = g_overlay_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_overlay_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    unsigned slot = (addr - 0x8C800000u) / 0x14000u;
    int want = slot < 3 ? s_slot_tag[slot] : -1;
    GuestFn first = NULL;
    for (unsigned i = lo; i < g_overlay_count && g_overlay_funcs[i].addr == addr; i++) {
        const OverlayEntry *e = &g_overlay_funcs[i];
        if (memcmp(ram_ptr(addr), e->sig, e->sig_len)) continue;
        if (want < 0 || (int)e->tag == want) return e->fn;
        if (!first) first = e->fn;
    }
    return first;
}
