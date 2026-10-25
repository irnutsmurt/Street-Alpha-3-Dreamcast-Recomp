/* PSP characters: the runtime half (see psp.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psp.h"
#include "runtime.h"

MipsCtx m;

#define SLOT_BASE 0x8C800000u
#define SLOT_SIZE 0x14000u
#define STUB_MAGIC 0x5053FFFFu  /* FFFF 5053 ("PS") */

extern uint64_t g_vblank_count;
static unsigned s_slot;  /* the player slot whose character file the running code belongs to */

/* --- fighters and effect objects: PSP offset <-> Dreamcast offset --- */

/* a fighter: Dreamcast 0x44C bytes, PSP 0x498. The PSP inserted 0x10 bytes at
 * 0x38, 0x10 at 0x130 (Dreamcast offsets), 4 at 0x32C and 0x28 at the end. */
#define F_DC 0x44Cu
#define F_PSP 0x498u
static int32_t fighter_psp(uint32_t o) {  /* Dreamcast offset -> PSP */
    if (o < 0x38) return (int32_t)o;
    if (o < 0x130) return (int32_t)o + 0x10;
    if (o < 0x32C) return (int32_t)o + 0x20;
    return (int32_t)o + 0x24;
}
static int32_t fighter_dc(uint32_t k) {  /* PSP offset -> Dreamcast, or -1: a PSP-only field */
    if (k < 0x38) return (int32_t)k;
    if (k < 0x48) return -1;
    if (k < 0x140) return (int32_t)k - 0x10;
    if (k < 0x150) return -1;
    if (k < 0x34C) return (int32_t)k - 0x20;
    if (k < 0x350) return -1;
    if (k < 0x470) return (int32_t)k - 0x24;
    return -1;
}
/* an effect object: Dreamcast 0xB0 bytes, PSP 0xC0 (0x10 inserted at 0x38) */
#define O_DC 0xB0u
#define O_PSP 0xC0u
static int32_t object_psp(uint32_t o) { return o < 0x38 ? (int32_t)o : (int32_t)o + 0x10; }
static int32_t object_dc(uint32_t k) { return k < 0x38 ? (int32_t)k : k < 0x48 ? -1 : (int32_t)k - 0x10; }

/* a projectile: Dreamcast 0x148 bytes, PSP 0x168 (laid out like a fighter's first 0x148) */
#define S_DC 0x148u
#define S_PSP 0x168u

/* the object arrays: where they are on each machine, and the offset maps */
typedef struct {
    uint32_t dc, count, dc_size;      /* Dreamcast array */
    uint32_t psp, psp_count, psp_size; /* PSP array (as its globals see it) */
    int32_t (*to_psp)(uint32_t), (*to_dc)(uint32_t);
} Array;
static const Array s_arrays[] = {
    {0x8C17FD38u, 4, F_DC, 0x0080AF70u, 4, F_PSP, fighter_psp, fighter_dc},     /* fighters */
    {0x8C180E68u, 24, S_DC, 0x00A37080u, 24, S_PSP, fighter_psp, fighter_dc},   /* projectiles */
    {0x8C1832A8u, 32, O_DC, 0x00A39840u, 32, O_PSP, object_psp, object_dc},     /* effects: the four pools */
    {0x8C1848A8u, 16, O_DC, 0x00A3B040u, 16, O_PSP, object_psp, object_dc},
    {0x8C1853A8u, 16, O_DC, 0x00A3BC40u, 16, O_PSP, object_psp, object_dc},
    {0x8C185EA8u, 16, O_DC, 0x00A3C840u, 42, O_PSP, object_psp, object_dc},
};
#define ARRAYS (sizeof s_arrays / sizeof s_arrays[0])

/* side storage for the PSP-only fields: PSP_EXT | array << 20 | index << 12 | PSP offset */
static uint8_t s_ext[ARRAYS][42][F_PSP];

static uint32_t ext_addr(unsigned arr, uint32_t index, uint32_t k) { return PSP_EXT | arr << 20 | index << 12 | k; }
static uint8_t *ext_ptr(uint32_t a) {
    uint32_t arr = (a >> 20) & 0xFF, index = (a >> 12) & 0xFF, k = a & 0xFFF;
    if (arr < ARRAYS && index < 42 && k < F_PSP) return &s_ext[arr][index][k];
    static int n;
    if (n++ < 20) logf_("psp: bad side-storage address %08X", a);
    static uint8_t junk[8];
    return junk;
}
uint32_t psp_ext_load(uint32_t a, int size) {
    uint8_t *p = ext_ptr(a);
    uint32_t v = 0;
    memcpy(&v, p, (size_t)size);
    return v;
}
void psp_ext_store(uint32_t a, uint32_t v, int size) { memcpy(ext_ptr(a), &v, (size_t)size); }

/* element i of array arr, PSP offset k -> Dreamcast (0: outside the array) */
static uint32_t in_array(unsigned arr, int64_t i, int64_t k) {
    const Array *A = &s_arrays[arr];
    while (k < 0) { k += A->psp_size; i--; }
    while (k >= A->psp_size) { k -= A->psp_size; i++; }
    if (i < 0 || i >= A->count) return 0;
    int32_t o = A->to_dc((uint32_t)k);
    if (o < 0 || (uint32_t)o >= A->dc_size) return ext_addr(arr, (uint32_t)i, (uint32_t)k);
    return A->dc + (uint32_t)i * A->dc_size + (uint32_t)o;
}

uint32_t psp_add_slow(uint32_t p, int32_t d) {
    for (unsigned arr = 0; arr < ARRAYS; arr++) {
        const Array *A = &s_arrays[arr];
        if (p - A->dc < A->count * A->dc_size) {
            uint32_t i = (p - A->dc) / A->dc_size, o = (p - A->dc) % A->dc_size;
            uint32_t r = in_array(arr, i, (int64_t)A->to_psp(o) + d);
            return r ? r : p + (uint32_t)d;
        }
    }
    return p + (uint32_t)d;
}

/* --- PSP addresses (built from constants) -> Dreamcast --- */

static uint32_t code_xa(uint32_t a) {
    unsigned lo = 0, hi = g_psp_code_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_psp_code[mid].psp < a) lo = mid + 1; else hi = mid;
    }
    if (lo < g_psp_code_count && g_psp_code[lo].psp == a) {
        uint32_t dc = g_psp_code[lo].dc;
        return dc & 1 ? SLOT_BASE + s_slot * SLOT_SIZE + (dc & ~1u) : dc;
    }
    return 0;
}

uint32_t psp_xa(uint32_t a) {
    unsigned lo = 0, hi = g_psp_region_count;
    while (lo < hi) {  /* the last region starting at or below a */
        unsigned mid = (lo + hi) / 2;
        if (g_psp_regions[mid].lo <= a) lo = mid + 1; else hi = mid;
    }
    if (lo) {
        const PspRegion *r = &g_psp_regions[lo - 1];
        if (a < r->hi) {
            uint32_t off = a - r->lo;
            switch (r->kind) {
            case PSP_R_LINEAR: return r->base + off;
            case PSP_R_SLOT: return SLOT_BASE + s_slot * SLOT_SIZE + r->base + off;
            case PSP_R_FIGHTERS:
            case PSP_R_POOLS:
                for (unsigned arr = 0; arr < ARRAYS; arr++) {
                    const Array *A = &s_arrays[arr];
                    if (a - A->psp < A->psp_count * A->psp_size) {
                        uint32_t o = a - A->psp;
                        uint32_t v = in_array(arr, o / A->psp_size, o % A->psp_size);
                        if (v) return v;
                    }
                }
                break;
            }
        }
    }
    if (a < 0x1F8354u) {
        uint32_t v = code_xa(a);
        if (v) return v;
    }
    static uint32_t seen[64];
    static unsigned nseen;
    unsigned i;
    for (i = 0; i < nseen && seen[i] != a; i++) {}
    if (i == nseen && nseen < 64) {
        seen[nseen++] = a;
        logf_("psp: no Dreamcast counterpart for PSP address %06X (slot %u)", a, s_slot);
    }
    return 0x8C000000u;  /* somewhere harmless to read */
}

/* --- calls --- */

void psp_call_dc(uint32_t addr) {
    static int trace = -1;  /* SFA3_PSP_TRACE=<hex address>: log calls to it (debug) */
    static uint32_t trace_addr;
    if (trace < 0) {
        const char *t = getenv("SFA3_PSP_TRACE");
        trace = t != NULL;
        trace_addr = t ? (uint32_t)strtoul(t, NULL, 16) : 0;
    }
    for (int i = 4; i < 8; i++) g.r[i] = m.r[i];
    /* EABI passes arguments 5-8 in t0-t3; the SH-4 takes them on the stack */
    g.r[15] -= 16;
    for (int i = 0; i < 4; i++) W32(g.r[15] + 4u * (uint32_t)i, m.r[8 + i]);
    uint32_t pr = g.pr;
    call_indirect(addr, 0x8C800000u);
    g.pr = pr;
    g.r[15] += 16;
    m.r[2] = g.r[0];
    m.r[3] = g.r[1];
    m.f[0] = g.fr[0];
    if (trace && !trace_addr) {  /* SFA3_PSP_TRACE=0: every call from SFA3_WATCH_FROM's frame on */
        static long long from = -1;
        static int n;
        if (from < 0) from = getenv("SFA3_WATCH_FROM") ? atoll(getenv("SFA3_WATCH_FROM")) : 0;
        if ((long long)g_vblank_count >= from && n++ < 4000)
            logf_("psp call %08X(%08X, %08X, %08X, %08X) = %08X at frame %llu", addr, m.r[4], m.r[5], m.r[6], m.r[7],
                  m.r[2], (unsigned long long)g_vblank_count);
    }
    if (trace && addr == trace_addr) {
        static int n, calls;
        if ((calls++ < 30 || m.r[2]) && n++ < 2000)
            logf_("psp trace %08X(%08X, %08X, %08X, %08X) = %08X at frame %llu", addr, m.r[4], m.r[5], m.r[6], m.r[7],
                  m.r[2], (unsigned long long)g_vblank_count);
    }
}

static void run(unsigned index, unsigned slot) {
    if (index >= g_psp_fn_count || !g_psp_fns[index]) fatal("psp: bad stub index %u", index);
    unsigned saved = s_slot;
    s_slot = slot;
    g_psp_fns[index]();
    s_slot = saved;
}

/* a stub: FFFF 5053 <index> FFFF in a character file */
static int stub_index(uint32_t addr) {
    uint32_t off = addr & 0xFFFFFFu;
    if (!is_ram(addr) || off < (SLOT_BASE & 0xFFFFFF) || off >= (SLOT_BASE & 0xFFFFFF) + 3 * SLOT_SIZE || addr & 1)
        return -1;
    if (R32(addr) != STUB_MAGIC || R16(addr + 6) != 0xFFFF) return -1;
    return (int)R16(addr + 4);
}

int psp_stub_call(uint32_t addr) {
    int index = stub_index(addr);
    if (index < 0) return 0;
    if (getenv("SFA3_PSP_STUBS")) {  /* debug: log the engine's calls into PSP code */
        static long long from = -1;
        static int n;
        if (from < 0) from = atoll(getenv("SFA3_PSP_STUBS"));
        if ((long long)g_vblank_count >= from && n++ < 3000)
            logf_("psp stub %06X(%08X, %08X) at frame %llu", (unsigned)index < g_psp_fn_count ? g_psp_fn_addr[index] : 0u,
                  g.r[4], g.r[5], (unsigned long long)g_vblank_count);
    }
    unsigned slot = ((addr & 0xFFFFFFu) - (SLOT_BASE & 0xFFFFFF)) / SLOT_SIZE;
    /* from the Dreamcast side: arguments r4-r7, then the stack */
    for (int i = 4; i < 8; i++) m.r[i] = g.r[i];
    for (int i = 0; i < 4; i++) m.r[8 + i] = R32(g.r[15] + 4u * (uint32_t)i);
    run((unsigned)index, slot);
    g.r[0] = m.r[2];
    g.r[1] = m.r[3];
    return 1;
}

void psp_icall(uint32_t addr) {
    int index = stub_index(addr);
    if (index >= 0) {  /* one of ours: stay in MIPS registers */
        unsigned slot = ((addr & 0xFFFFFFu) - (SLOT_BASE & 0xFFFFFF)) / SLOT_SIZE;
        run((unsigned)index, slot);
        return;
    }
    psp_call_dc(addr);
}

void psp_unmapped(uint32_t a) {
    static int n;
    if (n++ < 50) logf_("psp: call to PSP function %06X, which has no Dreamcast counterpart", a);
}

void psp_bad(uint32_t a) { fatal("psp: unhandled code at PSP %06X", a); }

/* unaligned word halves (big pieces of a little-endian word) */
uint32_t psp_lwl(uint32_t a, uint32_t old) {
    uint32_t sh = (a & 3) * 8, w = psp_lw(a & ~3u);
    return (old & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
}
uint32_t psp_lwr(uint32_t a, uint32_t old) {
    uint32_t sh = (a & 3) * 8, w = psp_lw(a & ~3u);
    return (old & ~(0xFFFFFFFFu >> sh)) | (w >> sh);
}
void psp_swl(uint32_t a, uint32_t v) {
    for (uint32_t i = 0; i <= (a & 3); i++) psp_sb((a & ~3u) + i, v >> (8 * (3 - (a & 3) + i)));
}
void psp_swr(uint32_t a, uint32_t v) {
    for (uint32_t i = a & 3; i < 4; i++) psp_sb((a & ~3u) + i, v >> (8 * (i - (a & 3))));
}
