/* Guest CPU context and primitives used by recompiled SH-4 code. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef union {
    float f;
    uint32_t u;
    int32_t i;
} fpreg_t;

typedef struct {
    uint32_t r[16];
    uint32_t rbank[8];
    uint32_t t, s, q, m;      /* SR bits kept unpacked */
    uint32_t sr_rest;         /* other SR bits (MD, RB, BL, FD, IMASK) */
    uint32_t gbr, vbr, ssr, spc, sgr, dbr;
    uint32_t mach, macl, pr;
    fpreg_t fr[16];           /* current FPU bank */
    fpreg_t xf[16];           /* other FPU bank */
    uint32_t fpscr;
    fpreg_t fpul;
} Sh4Ctx;

extern Sh4Ctx g;
extern float g_wide_cull;  /* widescreen: the picture's margin in pixels, the sprite drawer's edges widened by it (widescreen.c) */
extern int g_wide_units;   /* widescreen: how much further out a stage's objects are drawn, units (widescreen.c) */

#define FPSCR_PR (1u << 19)
#define FPSCR_SZ (1u << 20)
#define FPSCR_FR (1u << 21)

/* ---------------- memory ---------------- */
extern uint8_t *g_ram;  /* 16 MB main RAM */

uint32_t mmio_read8(uint32_t a);
uint32_t mmio_read16(uint32_t a);
uint32_t mmio_read32(uint32_t a);
void mmio_write8(uint32_t a, uint32_t v);
void mmio_write16(uint32_t a, uint32_t v);
void mmio_write32(uint32_t a, uint32_t v);

/* Main RAM lives at physical 0x0C000000-0x0FFFFFFF (16 MB, mirrored),
 * reachable through U0/P0, P1 (0x8C..) and P2 (0xAC..). */
static inline int is_ram(uint32_t a) {
    return (a & 0x1C000000u) == 0x0C000000u && a < 0xE0000000u;
}
static inline uint8_t *ram_ptr(uint32_t a) { return g_ram + (a & 0x00FFFFFFu); }

/* debug build (SFA3_READ_TRACE, tools/read_trace.py): the recompiled code
 * keeps the running instruction's address in g_pc, and reads of the game's
 * data and work RAM are recorded per instruction (read_trace.c) */
#ifdef SFA3_READ_TRACE
extern uint32_t g_pc;
void read_trace_note(uint32_t a);
#define READ_NOTE(a) do { if (((a) & 0x00FFFFFFu) - 0x00094000u < 0x0026C000u) read_trace_note(a); } while (0)
#else
#define READ_NOTE(a) ((void)0)
#endif
static inline uint32_t R8(uint32_t a) {
    if (is_ram(a)) { READ_NOTE(a); return *ram_ptr(a); }
    return mmio_read8(a);
}
static inline uint32_t R16(uint32_t a) {
    if (is_ram(a)) { READ_NOTE(a); uint16_t v; memcpy(&v, ram_ptr(a), 2); return v; }
    return mmio_read16(a);
}
static inline uint32_t R32(uint32_t a) {
    if (is_ram(a)) { READ_NOTE(a); uint32_t v; memcpy(&v, ram_ptr(a), 4); return v; }
    return mmio_read32(a);
}
/* debug write watchpoint (SFA3_WATCH_WRITE=<address>): logs who writes a RAM byte */
extern uint32_t g_watch_ram, g_watch_len;
void watch_hit(uint32_t a, uint32_t v, int size);
#define WATCH(a, n, v) do { if (__builtin_expect(((a) & 0xFFFFFFu) + (n) - 1 - g_watch_ram < g_watch_len + (n) - 1, 0)) watch_hit(a, v, n); } while (0)
static inline void W8(uint32_t a, uint32_t v) {
    if (is_ram(a)) { WATCH(a, 1, v); *ram_ptr(a) = (uint8_t)v; } else mmio_write8(a, v);
}
static inline void W16(uint32_t a, uint32_t v) {
    if (is_ram(a)) { WATCH(a, 2, v); uint16_t x = (uint16_t)v; memcpy(ram_ptr(a), &x, 2); }
    else mmio_write16(a, v);
}
static inline void W32(uint32_t a, uint32_t v) {
    if (is_ram(a)) { WATCH(a, 4, v); memcpy(ram_ptr(a), &v, 4); } else mmio_write32(a, v);
}

/* ---------------- FPU ---------------- */
#define FPR() (g.fpscr & FPSCR_PR)
#define FSZ() ((g.fpscr & FPSCR_SZ) ? 8u : 4u)

/* DRn: FR[n] holds the high word, FR[n+1] the low word. */
static inline double DR(int n) {
    uint64_t v = ((uint64_t)g.fr[n].u << 32) | g.fr[n + 1].u;
    double d;
    memcpy(&d, &v, 8);
    return d;
}
static inline void DR_SET(int n, double d) {
    uint64_t v;
    memcpy(&v, &d, 8);
    g.fr[n].u = (uint32_t)(v >> 32);
    g.fr[n + 1].u = (uint32_t)v;
}
/* With FPSCR.SZ=1, register numbers name pairs: even -> DRn, odd -> XD(n-1). */
static inline fpreg_t *fpair(int n) { return (n & 1) ? &g.xf[n & ~1] : &g.fr[n]; }

static inline void FMOV_RR(int n, int m) {
    if (g.fpscr & FPSCR_SZ) { fpreg_t *d = fpair(n), *s = fpair(m); d[0] = s[0]; d[1] = s[1]; }
    else g.fr[n] = g.fr[m];
}
static inline void FMOV_LD(int n, uint32_t a) {
    if (g.fpscr & FPSCR_SZ) { fpreg_t *d = fpair(n); d[0].u = R32(a); d[1].u = R32(a + 4); }
    else g.fr[n].u = R32(a);
}
static inline void FMOV_ST(uint32_t a, int m) {
    if (g.fpscr & FPSCR_SZ) { fpreg_t *s = fpair(m); W32(a, s[0].u); W32(a + 4, s[1].u); }
    else W32(a, g.fr[m].u);
}

static inline int32_t sh4_ftrc_f(float f) {
    if (f != f) return INT32_MIN;
    if (f >= 2147483648.0f) return INT32_MAX;
    if (f <= -2147483648.0f) return INT32_MIN;
    return (int32_t)f;
}
static inline int32_t sh4_ftrc_d(double f) {
    if (f != f) return INT32_MIN;
    if (f >= 2147483648.0) return INT32_MAX;
    if (f <= -2147483648.0) return INT32_MIN;
    return (int32_t)f;
}

static inline void sh4_fipr(int m, int n) {
    g.fr[n + 3].f = g.fr[m].f * g.fr[n].f + g.fr[m + 1].f * g.fr[n + 1].f +
                    g.fr[m + 2].f * g.fr[n + 2].f + g.fr[m + 3].f * g.fr[n + 3].f;
}
static inline void sh4_ftrv(int n) {
    float v0 = g.fr[n].f, v1 = g.fr[n + 1].f, v2 = g.fr[n + 2].f, v3 = g.fr[n + 3].f;
    for (int i = 0; i < 4; i++)
        g.fr[n + i].f = g.xf[i].f * v0 + g.xf[4 + i].f * v1 + g.xf[8 + i].f * v2 +
                        g.xf[12 + i].f * v3;
}
static inline void sh4_fsca(int n) {
    double a = (double)(g.fpul.u & 0xFFFF) * (2.0 * 3.14159265358979323846 / 65536.0);
    g.fr[n].f = (float)sin(a);
    g.fr[n + 1].f = (float)cos(a);
}

/* ---------------- runtime services ---------------- */
void sh4_set_fpscr(uint32_t v);
uint32_t sh4_get_sr(void);
void sh4_set_sr(uint32_t v);
void sh4_div1(int n, int m);
void sh4_macl(int n, int m);
void sh4_macw(int n, int m);
void sh4_pref(uint32_t a);
void sh4_sleep(void);
void sh4_trapa(uint32_t imm);
void sh4_rte(void);

void call_indirect(uint32_t addr, uint32_t from);
void bad_jump(uint32_t addr, uint32_t from);

/* Interrupts are delivered at backward branches and on sleep. */
extern volatile int g_irq_pending;
void irq_service(void);
/* Loop back-edges: deliver pending interrupts, and in deterministic mode
 * advance the guest clock (see irq.c). */
#define DET_TICK_CHECKS 256
#define DET_NS_PER_CHECK 50
extern int32_t g_tick_countdown;
void irq_slowpath(void);
#define IRQ_CHECK() do { if (__builtin_expect(g_irq_pending | (--g_tick_countdown < 0), 0)) irq_slowpath(); } while (0)

#define RETURN() return

/* setjmp call sites: capture a host landing point in the caller's frame */
void **sjlj_site(uint32_t buf);
void sjlj_record(void);
void f_8C094584(void);
#define SETJMP_ADDR 0x8C094584u
#define SJLJ_SITE() do { \
        if (__builtin_setjmp(sjlj_site(g.r[4])) == 0) { f_8C094584(); sjlj_record(); } \
    } while (0)
