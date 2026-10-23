/* Character code overlays (PLnnX.BIN at 0x8C800000). Several overlays share
 * the same addresses, so a call is matched to the overlay whose bytes are
 * currently in RAM. */
#include <string.h>

#include "cpu.h"
#include "overlay.h"
#include "runtime.h"

GuestFn overlay_lookup(uint32_t addr) {
    addr = (addr & 0x1FFFFFFF) | 0x80000000u;
    /* entries are sorted by address; scan candidates at this address */
    unsigned lo = 0, hi = g_overlay_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_overlay_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    for (unsigned i = lo; i < g_overlay_count && g_overlay_funcs[i].addr == addr; i++) {
        const OverlayEntry *e = &g_overlay_funcs[i];
        if (!memcmp(ram_ptr(addr), e->sig, e->sig_len)) return e->fn;
    }
    return NULL;
}
