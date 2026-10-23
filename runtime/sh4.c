/* SH-4 CPU services for recompiled code: status registers, rare ops,
 * indirect dispatch and interrupt delivery. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "dispatch.h"
#include "runtime.h"

Sh4Ctx g;
volatile int g_irq_pending;

#define SR_T 0x1u
#define SR_S 0x2u
#define SR_Q 0x100u
#define SR_M 0x200u
#define SR_FD (1u << 15)
#define SR_BL (1u << 28)
#define SR_RB (1u << 29)
#define SR_MD (1u << 30)

uint32_t sh4_get_sr(void) {
    return g.sr_rest | (g.t ? SR_T : 0) | (g.s ? SR_S : 0) | (g.q ? SR_Q : 0) | (g.m ? SR_M : 0);
}

void sh4_set_sr(uint32_t v) {
    uint32_t old = g.sr_rest;
    g.t = v & SR_T ? 1 : 0;
    g.s = v & SR_S ? 1 : 0;
    g.q = v & SR_Q ? 1 : 0;
    g.m = v & SR_M ? 1 : 0;
    g.sr_rest = v & 0x700083F0u;
    if ((old ^ g.sr_rest) & SR_RB) {
        for (int i = 0; i < 8; i++) {
            uint32_t t = g.r[i];
            g.r[i] = g.rbank[i];
            g.rbank[i] = t;
        }
    }
    /* lowering the interrupt mask may let a pending interrupt through */
    if (irq_any_pending()) g_irq_pending = 1;
}

void sh4_set_fpscr(uint32_t v) {
    if ((g.fpscr ^ v) & FPSCR_FR) {
        for (int i = 0; i < 16; i++) {
            fpreg_t t = g.fr[i];
            g.fr[i] = g.xf[i];
            g.xf[i] = t;
        }
    }
    g.fpscr = v & 0x003FFFFFu;
}

void sh4_div1(int n, int m) {
    uint32_t old_q = g.q, tmp0, tmp1;
    g.q = g.r[n] >> 31;
    g.r[n] = (g.r[n] << 1) | g.t;
    tmp0 = g.r[n];
    if (!old_q) {
        if (!g.m) {
            g.r[n] -= g.r[m];
            tmp1 = g.r[n] > tmp0;
            g.q = g.q ? (tmp1 == 0) : tmp1;
        } else {
            g.r[n] += g.r[m];
            tmp1 = g.r[n] < tmp0;
            g.q = g.q ? tmp1 : (tmp1 == 0);
        }
    } else {
        if (!g.m) {
            g.r[n] += g.r[m];
            tmp1 = g.r[n] < tmp0;
            g.q = g.q ? (tmp1 == 0) : tmp1;
        } else {
            g.r[n] -= g.r[m];
            tmp1 = g.r[n] > tmp0;
            g.q = g.q ? tmp1 : (tmp1 == 0);
        }
    }
    g.t = (g.q == g.m);
}

void sh4_macl(int n, int m) {
    int64_t a = (int64_t)(int32_t)R32(g.r[n]);
    g.r[n] += 4;
    int64_t b = (int64_t)(int32_t)R32(g.r[m]);
    g.r[m] += 4;
    int64_t acc = (int64_t)(((uint64_t)g.mach << 32) | g.macl);
    acc += a * b;
    if (g.s) {
        if (acc > 0x00007FFFFFFFFFFFLL) acc = 0x00007FFFFFFFFFFFLL;
        if (acc < -0x0000800000000000LL) acc = -0x0000800000000000LL;
    }
    g.mach = (uint32_t)((uint64_t)acc >> 32);
    g.macl = (uint32_t)acc;
}

void sh4_macw(int n, int m) {
    int32_t a = (int16_t)R16(g.r[n]);
    g.r[n] += 2;
    int32_t b = (int16_t)R16(g.r[m]);
    g.r[m] += 2;
    if (g.s) {
        int64_t r = (int64_t)(int32_t)g.macl + (int64_t)a * b;
        if (r > INT32_MAX) { r = INT32_MAX; g.mach |= 1; }
        if (r < INT32_MIN) { r = INT32_MIN; g.mach |= 1; }
        g.macl = (uint32_t)r;
    } else {
        int64_t acc = (int64_t)(((uint64_t)g.mach << 32) | g.macl) + (int64_t)a * b;
        g.mach = (uint32_t)((uint64_t)acc >> 32);
        g.macl = (uint32_t)acc;
    }
}

void sh4_pref(uint32_t a) {
    if ((a >> 26) == (0xE0000000u >> 26)) sq_flush(a);
}

void sh4_sleep(void) { host_wait_for_irq(); irq_service(); }

void sh4_trapa(uint32_t imm) {
    fatal("trapa #%u at pr=%08X", imm, g.pr);
}

/* Exception handlers are entered as C calls from irq_service(); rte
 * restores SR and the generated code then returns to irq_service(). */
void sh4_rte(void) { sh4_set_sr(g.ssr); }

/* ---------------- indirect dispatch ---------------- */

/* Direct-mapped table over main RAM: index = (addr & 0xFFFFFF) >> 1 */
static GuestFn *s_table;

void dispatch_init(void) {
    s_table = calloc(0x800000, sizeof(GuestFn));
    for (unsigned i = 0; i < g_dispatch_main_count; i++)
        s_table[(g_dispatch_main[i].addr & 0xFFFFFF) >> 1] = g_dispatch_main[i].fn;
}

/* Code copied at runtime (e.g. Shinobi's exception trampolines into the VBR
 * page) is matched by content against the original executable image and
 * dispatched to the recompiled original. Results are cached per address. */
extern uint8_t *g_main_image;
extern uint32_t g_main_image_size;

static GuestFn relocated_lookup(uint32_t addr, uint32_t from) {
    const uint8_t *code = ram_ptr(addr);
    for (uint32_t off = 0; off + 32 <= g_main_image_size; off += 2) {
        if (g_main_image[off] != code[0] || memcmp(g_main_image + off, code, 32)) continue;
        uint32_t src = 0x8C010000u + off;
        GuestFn f = s_table[(src & 0xFFFFFF) >> 1];
        if (f) {
            logf_("relocated code at %08X is a copy of %08X", addr, src);
            s_table[(addr & 0xFFFFFF) >> 1] = f;
            return f;
        }
        fatal("call to %08X (from %08X): copy of %08X, which is not a known function entry; "
              "add it to tools/recomp/extra_entries.txt", addr, from, src);
    }
    return NULL;
}

Stats g_stats;

int buttoncfg_mutes_sound(void);

void call_indirect(uint32_t addr, uint32_t from) {
    g_stats.indirect_calls++;
    g_stats.last_indirect[g_stats.last_idx++ & 15] = addr;
    if (g_call_hist_on) call_hist_add(addr);
    if (hle_call(addr)) return;
    if (addr == 0x8C059F8Cu && buttoncfg_mutes_sound()) return;  /* see buttoncfg.c */
    /* tiny RAM stubs like "rte; nop" written by the boot code */
    if (is_ram(addr) && R16(addr) == 0x002B) { sh4_rte(); return; }
    if (is_ram(addr) && !(addr & 1)) {
        uint32_t off = addr & 0xFFFFFF;
        if (off >= OVERLAY_START && off < OVERLAY_END) {
            GuestFn f = overlay_lookup(addr);
            if (f) { f(); return; }
        } else {
            GuestFn f = s_table[off >> 1];
            if (!f && (off < 0x10000 || off >= 0x10000 + g_main_image_size)) f = relocated_lookup(addr, from);
            if (f) { f(); return; }
        }
    }
    if (is_ram(addr)) {
        uint32_t p1 = (addr & 0x1FFFFFFF) | 0x80000000u;
        if (p1 >= 0x8C010000u && p1 < 0x8C010000u + g_main_image_size) {
            FILE *f = fopen("build/missing_entries.txt", "a");
            if (f) { fprintf(f, "%08X  # called from %08X\n", p1, from); fclose(f); }
        }
    }
    if (is_ram(addr)) {
        char hex[200];
        int k = 0;
        for (int i = 0; i < 32; i++) k += snprintf(hex + k, sizeof hex - k, "%02X", R8(addr + i));
        logf_("bytes at %08X: %s", addr, hex);
    }
    fatal("call to unrecompiled address %08X from %08X (pr=%08X)", addr, from, g.pr);
}

/* Debug aid: histogram of indirect call targets between "hist start" and
 * "hist stop" control commands (see input_sdl.c). */
int g_call_hist_on;
static struct { uint32_t addr, n; } s_hist[4096];
void call_hist_add(uint32_t a) {
    uint32_t h = (a >> 1) * 2654435761u >> 20;
    for (int i = 0; i < 4096; i++, h = (h + 1) & 4095) {
        if (s_hist[h].addr == a) { s_hist[h].n++; return; }
        if (!s_hist[h].addr) { s_hist[h].addr = a; s_hist[h].n = 1; return; }
    }
}
void call_hist_dump(void) {
    for (int i = 0; i < 4096; i++)
        if (s_hist[i].addr) logf_("hist %08X %u", s_hist[i].addr, s_hist[i].n);
    memset(s_hist, 0, sizeof s_hist);
}

void bad_jump(uint32_t addr, uint32_t from) {
    fatal("jump to unresolved target %08X from %08X", addr, from);
}
