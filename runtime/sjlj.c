/* setjmp/longjmp of the game's C library, used by middleware as a coroutine
 * mechanism (threads on separate stacks resumed via longjmp).
 *
 * Every guest setjmp call site captures a host jump buffer in the calling
 * recompiled function's frame (see SJLJ_SITE in cpu.h). A guest longjmp first
 * restores guest registers exactly like the original routine, then:
 *   - lands on the matching host capture in the same fiber, or
 *   - resumes the fiber that owns the capture and lands there, or
 *   - for a hand-built jmp_buf (new thread), starts a fresh fiber at its pc.
 */
#include <string.h>

#include "cpu.h"
#include "fiber.h"
#include "runtime.h"

#define MAX_BUFS 256

typedef struct {
    uint32_t guest;        /* guest jmp_buf address */
    uint32_t sp, pr;       /* guest state captured by that setjmp */
    Fiber *owner;
    void *host[5];
} JmpTarget;

static JmpTarget s_targets[MAX_BUFS];
static JmpTarget *s_pending;

static JmpTarget *lookup(uint32_t buf, int create) {
    JmpTarget *free_slot = NULL;
    for (int i = 0; i < MAX_BUFS; i++) {
        if (s_targets[i].guest == buf) return &s_targets[i];
        if (!s_targets[i].guest && !free_slot) free_slot = &s_targets[i];
    }
    if (!create) return NULL;
    if (!free_slot) fatal("too many jmp_bufs");
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->guest = buf;
    return free_slot;
}

void sjlj_reset(void) {
    memset(s_targets, 0, sizeof s_targets);
    s_pending = NULL;
}

void **sjlj_site(uint32_t buf) {
    JmpTarget *t = lookup(buf, 1);
    t->owner = fiber_current();
    s_pending = t;
    return t->host;
}

void sjlj_record(void) {
    /* after the guest setjmp body ran: buf+0 = r15, buf+8 = pr */
    JmpTarget *t = s_pending;
    t->sp = R32(t->guest);
    t->pr = R32(t->guest + 8);
}

static void fresh_entry(void) {
    /* rts from the coroutine's top function continues at PR, as on hardware */
    for (;;) call_indirect(g.pr, 0x8C094510u);
}

void f_8C094510_orig(void);

void hle_longjmp(void) {
    uint32_t buf = g.r[4];
    uint32_t sp = R32(buf), pr = R32(buf + 8);
    f_8C094510_orig();  /* restores the guest context; r0 = return value */
    JmpTarget *t = lookup(buf, 0);
    if (t && (t->sp != sp || t->pr != pr)) t = NULL;  /* buffer rewritten by hand */
    if (!t) {
        /* new coroutine: continue at pr on a fresh fiber */
        Fiber *f = fiber_spawn(fresh_entry);
        fiber_switch(f);
        fatal("fiber resumed after starting a coroutine, without a jump target");
    }
    if (t->owner == fiber_current()) __builtin_longjmp(t->host, 1);
    fiber_set_pending_jump(t->owner, t->host);
    fiber_switch(t->owner);
    fatal("longjmp: target fiber returned control unexpectedly");
}
