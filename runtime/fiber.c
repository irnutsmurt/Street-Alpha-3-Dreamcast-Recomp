#include "fiber.h"

#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  /* fibers: CreateFiber / SwitchToFiber */
#else
#include <ucontext.h>
#endif

#include "runtime.h"

#define FIBER_STACK (1u << 20)
#define MAX_FIBERS 512

struct Fiber {
    int used, dead;
    uint32_t tag, sp;
#ifdef _WIN32
    void *handle;
#else
    ucontext_t ctx;
    void *stack;
#endif
    void (*entry)(void);
    void **pending_jump;
};

static Fiber s_fibers[MAX_FIBERS];
static Fiber s_main = {.used = 1};
static Fiber *s_cur = &s_main;

Fiber *fiber_current(void) { return s_cur == &s_main ? NULL : s_cur; }

static void reap(void) {
    for (int i = 0; i < MAX_FIBERS; i++) {
        Fiber *f = &s_fibers[i];
        if (f->used && f->dead && f != s_cur) {
#ifdef _WIN32
            DeleteFiber(f->handle);
#else
            free(f->stack);
#endif
            f->used = 0;
        }
    }
}

#ifdef _WIN32
static void WINAPI trampoline(void *arg) {
    (void)arg;
#else
static void trampoline(void) {
#endif
    s_cur->entry();
    fatal("fiber entry returned (tag %08X)", s_cur->tag);
}

Fiber *fiber_spawn(void (*entry)(void)) {
    reap();
    for (int i = 0; i < MAX_FIBERS; i++) {
        Fiber *f = &s_fibers[i];
        if (f->used) continue;
        *f = (Fiber){0};
        f->used = 1;
        f->entry = entry;
#ifdef _WIN32
        if (!s_main.handle) s_main.handle = ConvertThreadToFiber(NULL);
        f->handle = CreateFiber(FIBER_STACK, trampoline, NULL);
        if (!f->handle) fatal("CreateFiber failed");
#else
        f->stack = malloc(FIBER_STACK);
        getcontext(&f->ctx);
        f->ctx.uc_stack.ss_sp = f->stack;
        f->ctx.uc_stack.ss_size = FIBER_STACK;
        makecontext(&f->ctx, trampoline, 0);
#endif
        return f;
    }
    fatal("out of fibers");
}

void fiber_switch(Fiber *to) {
    if (!to) to = &s_main;
    if (to == s_cur) return;
    Fiber *from = s_cur;
    s_cur = to;
#ifdef _WIN32
    (void)from;
    SwitchToFiber(to->handle);
#else
    swapcontext(&from->ctx, &to->ctx);
#endif
    /* resumed */
    reap();
    if (s_cur->pending_jump) {
        void **jb = s_cur->pending_jump;
        s_cur->pending_jump = NULL;
        __builtin_longjmp(jb, 1);
    }
}

void fiber_kill(Fiber *f) { if (f) f->dead = 1; }

void fiber_kill_all(void) {
    for (int i = 0; i < MAX_FIBERS; i++)
        if (s_fibers[i].used && &s_fibers[i] != s_cur) s_fibers[i].dead = 1;
    reap();
}
void fiber_set_tag(Fiber *f, uint32_t tag, uint32_t sp) { if (f) { f->tag = tag; f->sp = sp; } }
uint32_t fiber_tag(Fiber *f) { return f ? f->tag : 0; }
uint32_t fiber_sp(Fiber *f) { return f ? f->sp : 0; }

Fiber *fiber_find_tag(uint32_t tag) {
    for (int i = 0; i < MAX_FIBERS; i++)
        if (s_fibers[i].used && !s_fibers[i].dead && s_fibers[i].tag == tag) return &s_fibers[i];
    return NULL;
}

void fiber_set_pending_jump(Fiber *f, void **hostbuf) { (f ? f : &s_main)->pending_jump = hostbuf; }
