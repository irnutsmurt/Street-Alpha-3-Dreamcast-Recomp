/* Native replacement for the game's cooperative task switch (0x8C0100C8).
 *
 * Each guest task runs on its own guest stack; switching tasks swaps r15
 * and "returns" into the other task. Recompiled code keeps its call chain
 * on the host C stack, so every guest task gets a host fiber as well. The
 * guest-visible effects (registers pushed/popped on the guest stacks, the
 * saved stack pointers) are reproduced exactly as the original routine.
 *
 *   switch(task)        scheduler -> task: save sp at SCHED_SP, sp = task->sp
 *   switch(0xFF000000)  task -> scheduler: task->sp = sp (unless dead), sp = SCHED_SP
 *
 * Task slots (24 bytes, 8 of them at 0x8C14BE28): +0 state, +4 sleep counter,
 * +8 TCB (the slot itself), +12 entry, +16 saved sp, +20 stack top.
 */
#include "cpu.h"
#include "fiber.h"
#include "runtime.h"

#define SCHED_SP 0x8C14BEECu     /* scheduler's saved stack pointer */
#define CUR_TASK 0x8C14BEE8u     /* pointer to the running task */
#define TASK_DEAD 0x00040000u
#define TASK_SP 16
#define YIELD 0xFF000000u
#define TAG_TASK 0x80000000u     /* fiber tags: task struct | TAG_TASK */

static Fiber *s_sched;           /* fiber that runs the scheduler (NULL = host thread) */

/* Task slots whose frame was just (re)built by the create routine; their
 * next activation must start a new fiber. A saved stack pointer alone can't
 * tell a fresh frame from a suspended one: they can coincide exactly. */
#define MAX_FRESH 16
static uint32_t s_fresh[MAX_FRESH];

void f_8C0100BC_orig(void);

void hle_task_create(void) {
    uint32_t slot = g.r[6];
    f_8C0100BC_orig();
    for (int i = 0; i < MAX_FRESH; i++)
        if (s_fresh[i] == slot || !s_fresh[i]) { s_fresh[i] = slot; return; }
    fatal("too many fresh tasks");
}

static int take_fresh(uint32_t slot) {
    for (int i = 0; i < MAX_FRESH; i++)
        if (s_fresh[i] == slot) { s_fresh[i] = 0; return 1; }
    return 0;
}

static void guest_save(void) {
    uint32_t sp = g.r[15];
    for (int f = 14; f >= 0; f -= 2) { sp -= FSZ(); FMOV_ST(sp, f); }
    for (int r = 14; r >= 0; r--) { sp -= 4; W32(sp, g.r[r]); }
    sp -= 4;
    W32(sp, g.pr);
    g.r[15] = sp;
}

static void guest_restore(void) {
    uint32_t sp = g.r[15];
    g.pr = R32(sp);
    sp += 4;
    for (int r = 0; r <= 14; r++) { g.r[r] = R32(sp); sp += 4; }
    for (int f = 0; f <= 14; f += 2) { FMOV_LD(f, sp); sp += FSZ(); }
    g.r[15] = sp;
}

static void task_entry(void) {
    /* First activation: pop the initial frame. Its PR slot holds the entry
     * itself, so when the task function returns (rts -> PC = PR) the CPU
     * re-enters it; follow PR the same way. */
    guest_restore();
    static int logged;
    if (logged++ < 40) logf_("task fiber start: pc=%08X sp=%08X", g.pr, g.r[15]);
    for (;;) call_indirect(g.pr, 0x8C0100C8u);
}

void task_reset(void) {
    s_sched = NULL;
    for (int i = 0; i < MAX_FRESH; i++) s_fresh[i] = 0;
}

void hle_task_switch(void) {
    guest_save();
    if (g.r[4] == YIELD) {
        /* task -> scheduler */
        uint32_t task = R32(CUR_TASK);
        Fiber *me = fiber_current();
        static int logged;
        if (R32(task) == 1 && logged++ < 40)
            logf_("task %08X restart -> entry %08X requested from pr=%08X", task, R32(task + 12), g.pr);
        if ((R32(task) & TASK_DEAD) != TASK_DEAD) {
            W32(task + TASK_SP, g.r[15]);
            fiber_set_tag(me, task | TAG_TASK, g.r[15]);
        } else {
            fiber_kill(me);
        }
        g.r[15] = R32(SCHED_SP);
        fiber_switch(s_sched);
    } else {
        /* scheduler -> task */
        uint32_t task = g.r[4];
        s_sched = fiber_current();
        W32(SCHED_SP, g.r[15]);
        uint32_t sp = R32(task + TASK_SP);
        Fiber *f = fiber_find_tag(task | TAG_TASK);
        if (f && (take_fresh(task) || fiber_sp(f) != sp)) { fiber_kill(f); f = NULL; }
        static int logged;
        if (R32(task) != 3 && logged++ < 60) logf_("switch to task %08X sp=%08X state=%08X fiber=%s", task, sp, R32(task), f ? "resume" : "new");
        if (!f) f = fiber_spawn(task_entry);
        fiber_set_tag(f, task | TAG_TASK, sp);
        g.r[15] = sp;
        fiber_switch(f);
    }
    guest_restore();
}
