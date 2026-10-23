/* ARM7 mode/bank handling for the recompiled sound driver. */
#include <stddef.h>
#include <stdio.h>

#include "arm.h"
#include "runtime.h"

ArmCtx A;

static uint32_t *spsr_ptr(void) {
    switch (A.cpsr_rest & 0x1F) {
    case ARM_FIQ: return &A.spsr_fiq;
    case ARM_IRQ: return &A.spsr_irq;
    case ARM_SVC: return &A.spsr_svc;
    case ARM_ABT: return &A.spsr_abt;
    case ARM_UND: return &A.spsr_und;
    }
    return NULL;
}

static uint32_t *bank13(int mode) {
    switch (mode) {
    case ARM_IRQ: return A.irq_r13_14;
    case ARM_SVC: return A.svc_r13_14;
    case ARM_ABT: return A.abt_r13_14;
    case ARM_UND: return A.und_r13_14;
    }
    return NULL;
}

/* Switch register banks from the current mode to new_mode. */
static void switch_mode(int new_mode) {
    int old = A.cpsr_rest & 0x1F;
    if (old == new_mode) return;
    /* save current banked registers */
    if (old == ARM_FIQ) {
        for (int i = 0; i < 7; i++) A.fiq_r8_14[i] = A.r[8 + i];
        for (int i = 0; i < 7; i++) A.r[8 + i] = A.usr_r8_14[i];
    } else {
        uint32_t *b = bank13(old);
        if (b) {
            b[0] = A.r[13];
            b[1] = A.r[14];
            A.r[13] = A.usr_r8_14[5];
            A.r[14] = A.usr_r8_14[6];
        }
    }
    /* now r8-r14 hold user values; load the new mode's bank */
    if (new_mode == ARM_FIQ) {
        for (int i = 0; i < 7; i++) A.usr_r8_14[i] = A.r[8 + i];
        for (int i = 0; i < 7; i++) A.r[8 + i] = A.fiq_r8_14[i];
    } else {
        uint32_t *b = bank13(new_mode);
        if (b) {
            A.usr_r8_14[5] = A.r[13];
            A.usr_r8_14[6] = A.r[14];
            A.r[13] = b[0];
            A.r[14] = b[1];
        }
    }
    A.cpsr_rest = (A.cpsr_rest & ~0x1Fu) | (uint32_t)new_mode;
}

uint32_t arm_get_cpsr(void) {
    return A.n << 31 | A.z << 30 | A.c << 29 | A.v << 28 | A.cpsr_rest;
}

static void put_cpsr(uint32_t v) {
    switch_mode(v & 0x1F);
    A.n = v >> 31;
    A.z = (v >> 30) & 1;
    A.c = (v >> 29) & 1;
    A.v = (v >> 28) & 1;
    A.cpsr_rest = v & 0xFF;
}

void arm_set_cpsr(uint32_t v, uint32_t mask) {
    uint32_t cur = arm_get_cpsr();
    uint32_t m = 0;
    if (mask & 1) m |= 0x000000FF;
    if (mask & 8) m |= 0xF0000000;
    if ((A.cpsr_rest & 0x1F) == ARM_USR) m &= 0xF0000000;  /* user mode can only change flags */
    put_cpsr((cur & ~m) | (v & m));
}

uint32_t arm_get_spsr(void) {
    uint32_t *s = spsr_ptr();
    return s ? *s : arm_get_cpsr();
}

void arm_set_spsr(uint32_t v, uint32_t mask) {
    uint32_t *s = spsr_ptr();
    if (!s) return;
    uint32_t m = 0;
    if (mask & 1) m |= 0x000000FF;
    if (mask & 8) m |= 0xF0000000;
    *s = (*s & ~m) | (v & m);
}

void arm_restore_spsr(void) {
    uint32_t *s = spsr_ptr();
    if (s) put_cpsr(*s);
}

uint64_t g_aica_fiqs;
void arm_enter_fiq(void) {
    g_aica_fiqs++;
    uint32_t old = arm_get_cpsr();
    switch_mode(ARM_FIQ);
    A.spsr_fiq = old;
    A.cpsr_rest |= ARM_F | ARM_I;
    A.r[14] = A.pc + 4;  /* handler returns with subs pc, lr, #4 */
    A.pc = 0x1C;
}

void arm_swi(uint32_t ret) {
    uint32_t old = arm_get_cpsr();
    switch_mode(ARM_SVC);
    A.spsr_svc = old;
    A.cpsr_rest |= ARM_I;
    A.r[14] = ret;
    A.pc = 0x08;
}

void arm_reset(void) {
    for (int i = 0; i < 16; i++) A.r[i] = 0;
    A.cpsr_rest = ARM_SVC | ARM_I | ARM_F;
    A.n = A.z = A.c = A.v = 0;
    A.pc = 0;
    A.cycles = 0;
    A.fiq_pending = 0;
}

uint32_t arm_get_user_reg(int r) {
    int mode = A.cpsr_rest & 0x1F;
    if (r >= 8 && r <= 14 && mode == ARM_FIQ) return A.usr_r8_14[r - 8];
    if (r >= 13 && r <= 14 && mode != ARM_USR && mode != ARM_SYS) return A.usr_r8_14[r - 8];
    return A.r[r];
}

void arm_set_user_reg(int r, uint32_t v) {
    int mode = A.cpsr_rest & 0x1F;
    if ((r >= 8 && r <= 14 && mode == ARM_FIQ) ||
        (r >= 13 && r <= 14 && mode != ARM_USR && mode != ARM_SYS)) {
        A.usr_r8_14[r - 8] = v;
        return;
    }
    A.r[r] = v;
}

void arm_bad_pc(uint32_t pc) {
    logf_("arm: jump to unrecompiled address %08X (lr=%08X cpsr=%08X)", pc, A.r[14], arm_get_cpsr());
    for (uint32_t a = (pc & ~3u) - 0x10; a < (pc & ~3u) + 0x10; a += 4)
        logf_("arm:   %06X: %08X", a, arm_read32(a));
    for (int i = 0; i < 16; i++) logf_("arm:   r%d=%08X", i, A.r[i]);
    FILE *f = fopen("build/missing_arm_entries.txt", "a");
    if (f) { fprintf(f, "%08X  # lr=%08X\n", pc, A.r[14]); fclose(f); }
    fatal("sound driver reached unrecompiled code at %08X", pc);
}

void arm_undefined(uint32_t pc, uint32_t op) {
    static int reported;
    if (reported++ < 8) logf_("arm: undefined instruction %08X at %08X", op, pc);
}
