/* ARM7 context for the recompiled AICA sound driver. */
#pragma once
#include <stdint.h>

#define ARM_F (1u << 6)
#define ARM_I (1u << 7)
enum { ARM_USR = 0x10, ARM_FIQ = 0x11, ARM_IRQ = 0x12, ARM_SVC = 0x13, ARM_ABT = 0x17, ARM_UND = 0x1B,
       ARM_SYS = 0x1F };

typedef struct {
    uint32_t r[16];
    uint32_t n, z, c, v;      /* flags kept unpacked */
    uint32_t cpsr_rest;       /* I, F, T, mode */
    uint32_t pc;              /* next instruction at dispatch points */
    int32_t cycles;
    int fiq_pending;
    /* banked registers */
    uint32_t usr_r8_14[7], fiq_r8_14[7];
    uint32_t irq_r13_14[2], svc_r13_14[2], abt_r13_14[2], und_r13_14[2];
    uint32_t spsr_fiq, spsr_irq, spsr_svc, spsr_abt, spsr_und;
} ArmCtx;

extern ArmCtx A;

uint32_t arm_get_cpsr(void);
void arm_set_cpsr(uint32_t v, uint32_t mask);
uint32_t arm_get_spsr(void);
void arm_set_spsr(uint32_t v, uint32_t mask);
void arm_restore_spsr(void);
void arm_enter_fiq(void);
void arm_swi(uint32_t ret);
void arm_reset(void);
void arm_bad_pc(uint32_t pc);
void arm_undefined(uint32_t pc, uint32_t op);
uint32_t arm_get_user_reg(int r);
void arm_set_user_reg(int r, uint32_t v);
void arm_execute(int32_t budget);
extern const uint32_t g_arm_code_size, g_arm_code_sum;

/* memory: sound RAM at 0x000000, AICA registers at 0x800000 */
extern uint8_t *g_aica_ram;
uint32_t aica_arm_reg_read(uint32_t off, int size);
void aica_arm_reg_write(uint32_t off, uint32_t v, int size);

static inline uint32_t arm_read32(uint32_t a) {
    if (a < 0x800000) { uint32_t v; __builtin_memcpy(&v, g_aica_ram + (a & 0x1FFFFC), 4); return v; }
    return aica_arm_reg_read(a & 0xFFFC, 4);
}
static inline uint32_t arm_read32_rot(uint32_t a) {
    uint32_t v = arm_read32(a & ~3u), r = (a & 3) * 8;
    return r ? (v >> r) | (v << (32 - r)) : v;
}
static inline uint32_t arm_read16(uint32_t a) {
    if (a < 0x800000) { uint16_t v; __builtin_memcpy(&v, g_aica_ram + (a & 0x1FFFFE), 2); return v; }
    return aica_arm_reg_read(a & 0xFFFE, 2) & 0xFFFF;
}
static inline uint32_t arm_read8(uint32_t a) {
    if (a < 0x800000) return g_aica_ram[a & 0x1FFFFF];
    return aica_arm_reg_read(a & 0xFFFF, 1) & 0xFF;
}
static inline void arm_write32(uint32_t a, uint32_t v) {
    if (a < 0x800000) { __builtin_memcpy(g_aica_ram + (a & 0x1FFFFC), &v, 4); return; }
    aica_arm_reg_write(a & 0xFFFC, v, 4);
}
static inline void arm_write16(uint32_t a, uint32_t v) {
    if (a < 0x800000) { uint16_t x = (uint16_t)v; __builtin_memcpy(g_aica_ram + (a & 0x1FFFFE), &x, 2); return; }
    aica_arm_reg_write(a & 0xFFFE, v & 0xFFFF, 2);
}
static inline void arm_write8(uint32_t a, uint32_t v) {
    if (a < 0x800000) { g_aica_ram[a & 0x1FFFFF] = (uint8_t)v; return; }
    aica_arm_reg_write(a & 0xFFFF, v & 0xFF, 1);
}

/* barrel shifter; *c receives the shifter carry-out */
static inline uint32_t arm_shift_imm(uint32_t v, int type, int amt, uint32_t *c) {
    switch (type) {
    case 0: /* LSL */
        if (!amt) return v;
        *c = (v >> (32 - amt)) & 1;
        return v << amt;
    case 1: /* LSR (#0 means #32) */
        if (!amt) { *c = v >> 31; return 0; }
        *c = (v >> (amt - 1)) & 1;
        return v >> amt;
    case 2: /* ASR (#0 means #32) */
        if (!amt) { *c = v >> 31; return (uint32_t)((int32_t)v >> 31); }
        *c = (v >> (amt - 1)) & 1;
        return (uint32_t)((int32_t)v >> amt);
    default: /* ROR (#0 means RRX) */
        if (!amt) { uint32_t r = (A.c << 31) | (v >> 1); *c = v & 1; return r; }
        *c = (v >> (amt - 1)) & 1;
        return (v >> amt) | (v << (32 - amt));
    }
}
static inline uint32_t arm_shift_reg(uint32_t v, int type, uint32_t amt, uint32_t *c) {
    if (!amt) return v;
    switch (type) {
    case 0:
        if (amt < 32) { *c = (v >> (32 - amt)) & 1; return v << amt; }
        *c = amt == 32 ? v & 1 : 0;
        return 0;
    case 1:
        if (amt < 32) { *c = (v >> (amt - 1)) & 1; return v >> amt; }
        *c = amt == 32 ? v >> 31 : 0;
        return 0;
    case 2:
        if (amt < 32) { *c = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
        *c = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default: {
        uint32_t r = amt & 31;
        if (!r) { *c = v >> 31; return v; }
        *c = (v >> (r - 1)) & 1;
        return (v >> r) | (v << (32 - r));
    }
    }
}

#define JUMP_DYN goto dispatch
#define JUMP(t, label) do { \
        A.pc = (t); \
        if (A.cycles > 0 && !(A.fiq_pending && !(A.cpsr_rest & ARM_F))) goto label; \
        goto dispatch; \
    } while (0)
