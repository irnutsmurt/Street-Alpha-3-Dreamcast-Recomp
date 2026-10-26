/* PSP characters: support for character code recompiled from the PSP's MIPS
 * (tools/psp/mipsgen.py), running inside the Dreamcast game.
 *
 * The PSP game is the Dreamcast one extended, but it lays some things out
 * differently: a fighter is 0x498 bytes there (0x44C here), an effect object
 * 0xC0 (0xB0), and globals sit at other addresses. The recompiled code keeps
 * Dreamcast addresses in its registers; offsets it adds to a fighter or an
 * object pointer are PSP offsets and are converted as they're added (psp_add),
 * and addresses it builds from constants are looked up (psp_xa). Fields only
 * the PSP has live in side storage (addresses from PSP_EXT up).
 *
 * The add-on's character files (PLnnA/B/C.BIN, built by tools/psp/build_char.py)
 * hold the character's data in Dreamcast form and a stub for each function the
 * engine may call: 8 bytes, FFFF 5053 <index> FFFF. A call to a stub runs the
 * recompiled function. Which player slot's file the stub is in says whose data
 * the code's constants refer to (psp_xa). */
#pragma once
#include <stdint.h>

#include "cpu.h"

typedef struct {
    uint32_t r[32];  /* r[29] is unused: $sp is the SH-4's r15 */
    uint32_t hi, lo;
    fpreg_t f[32];
    int fcc;
} MipsCtx;

extern MipsCtx m;

typedef void (*PspFn)(void);

/* generated per build (tools/psp/build_char.py) */
typedef struct {
    uint32_t lo, hi;   /* PSP addresses [lo, hi) */
    uint32_t kind;     /* PSP_R_* */
    uint32_t base;     /* Dreamcast address, or offset in the slot's character file */
} PspRegion;
enum { PSP_R_LINEAR, PSP_R_SLOT, PSP_R_FIGHTERS, PSP_R_POOLS };
typedef struct {
    uint32_t psp;      /* a PSP function's address */
    uint32_t dc;       /* the Dreamcast function, or (stub offset | 1) in the slot's file */
} PspCode;
extern const PspRegion g_psp_regions[];
extern const unsigned g_psp_region_count;
extern const PspCode g_psp_code[];
extern const unsigned g_psp_code_count;
extern const PspFn g_psp_fns[];
extern const unsigned g_psp_fn_count;
extern const uint32_t g_psp_fn_addr[];  /* each function's PSP address (logs) */

#define PSP_EXT 0xF0000000u

/* --- for the generated code --- */
uint32_t psp_add_slow(uint32_t p, int32_t d);
uint32_t psp_xa(uint32_t psp_addr);
/* an address built for segment 1 (.bss): its offsets below the .bss's start (0x2D4FC0) would read as
 * segment 0's; the tools have them at 0x1000000 + offset (tools/psp/mipsgen.py SEG1_LOW) */
static inline uint32_t psp_xa1(uint32_t a) { return psp_xa(a < 0x2D4FC0u ? a + 0x1000000u : a); }
void psp_call_dc(uint32_t dc_addr);
void psp_icall(uint32_t addr);
void psp_unmapped(uint32_t psp_addr);
void psp_bad(uint32_t psp_addr);
uint32_t psp_ext_load(uint32_t a, int size);
void psp_ext_store(uint32_t a, uint32_t v, int size);
uint32_t psp_lwl(uint32_t a, uint32_t old);
uint32_t psp_lwr(uint32_t a, uint32_t old);
void psp_swl(uint32_t a, uint32_t v);
void psp_swr(uint32_t a, uint32_t v);

/* the Dreamcast memory holding the object arrays the PSP lays out differently
 * (fighters, projectiles, effects; psp.c has the details) */
#define PSP_STRUCTS 0x8C17FD38u
#define PSP_STRUCTS_SIZE (0x8C1869A8u - 0x8C17FD38u)

static inline uint32_t psp_add(uint32_t p, int32_t d) {
    if (p - PSP_STRUCTS < PSP_STRUCTS_SIZE) return psp_add_slow(p, d);
    return p + (uint32_t)d;
}
static inline uint32_t psp_addu(uint32_t x, uint32_t y) {
    if (x - PSP_STRUCTS < PSP_STRUCTS_SIZE && y + 0x8000u < 0x10000u) return psp_add_slow(x, (int32_t)y);
    if (y - PSP_STRUCTS < PSP_STRUCTS_SIZE && x + 0x8000u < 0x10000u) return psp_add_slow(y, (int32_t)x);
    return x + y;
}
static inline uint32_t psp_lw(uint32_t a) { return a >= PSP_EXT ? psp_ext_load(a, 4) : R32(a); }
static inline uint32_t psp_lh(uint32_t a) { return (uint32_t)(int16_t)(a >= PSP_EXT ? psp_ext_load(a, 2) : R16(a)); }
static inline uint32_t psp_lhu(uint32_t a) { return a >= PSP_EXT ? psp_ext_load(a, 2) : R16(a); }
static inline uint32_t psp_lb(uint32_t a) { return (uint32_t)(int8_t)(a >= PSP_EXT ? psp_ext_load(a, 1) : R8(a)); }
static inline uint32_t psp_lbu(uint32_t a) { return a >= PSP_EXT ? psp_ext_load(a, 1) : R8(a); }
static inline void psp_sw(uint32_t a, uint32_t v) { if (a >= PSP_EXT) psp_ext_store(a, v, 4); else W32(a, v); }
static inline void psp_sh(uint32_t a, uint32_t v) { if (a >= PSP_EXT) psp_ext_store(a, v, 2); else W16(a, v); }
static inline void psp_sb(uint32_t a, uint32_t v) { if (a >= PSP_EXT) psp_ext_store(a, v, 1); else W8(a, v); }

/* --- for the runtime --- */
int psp_stub_call(uint32_t addr);  /* call_indirect: a stub of ours? runs it (1) */
