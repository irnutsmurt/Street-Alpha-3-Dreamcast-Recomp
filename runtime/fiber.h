/* Host fibers backing the game's non-local control flow (task switches and
 * setjmp/longjmp coroutines), which can't be expressed as plain C calls. */
#pragma once
#include <stdint.h>

typedef struct Fiber Fiber;

Fiber *fiber_current(void);         /* NULL = the original host thread */
Fiber *fiber_spawn(void (*entry)(void));
void fiber_switch(Fiber *to);       /* to == NULL switches to the host thread */
void fiber_kill(Fiber *f);          /* freed once it is not running */
void fiber_kill_all(void);          /* every fiber but the running one (machine reset) */
void fiber_set_tag(Fiber *f, uint32_t tag, uint32_t sp);
uint32_t fiber_tag(Fiber *f);
uint32_t fiber_sp(Fiber *f);
Fiber *fiber_find_tag(uint32_t tag);

/* A host jump target to take when a fiber is next resumed (setjmp landing). */
void fiber_set_pending_jump(Fiber *f, void **hostbuf);
