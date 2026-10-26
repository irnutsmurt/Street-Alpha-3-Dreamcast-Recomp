#include <stdlib.h>
#include <SDL3/SDL.h>
/* Interrupt sources (Holly ASIC + SH-4 TMU) and synchronous delivery. */
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "runtime.h"

/* Holly interrupt status (SB_ISTNRM/EXT/ERR) and per-level masks
 * (SB_IML2*, SB_IML4*, SB_IML6*): mask[level_index][reg]. */
static uint32_t s_status[3];
uint32_t g_holly_mask[3][3];

uint32_t g_intevt;  /* SH-4 INTEVT register (0xFF000028) */

uint32_t holly_status(int reg) {
    uint32_t v = s_status[reg];
    if (reg == 0) {
        if (s_status[1]) v |= 1u << 30;
        if (s_status[2]) v |= 1u << 31;
    }
    return v;
}

void holly_raise(int reg, uint32_t bits) {
    s_status[reg] |= bits;
    if (irq_any_pending()) g_irq_pending = 1;
}

void holly_clear(int reg, uint32_t bits) {
    /* SB_ISTNRM is write-1-to-clear; EXT is cleared at its source */
    s_status[reg] &= ~bits;
}

/* Highest Holly level asserted: 6, 4, 2, or 0. */
static int holly_level(void) {
    static const int levels[3] = {6, 4, 2};
    for (int l = 0; l < 3; l++)
        for (int r = 0; r < 3; r++)
            if (s_status[r] & g_holly_mask[l][r]) return levels[l];
    return 0;
}

static int imask(void) { return (g.sr_rest >> 4) & 0xF; }
static int blocked(void) { return (g.sr_rest >> 28) & 1; }

int irq_any_pending(void) {
    if (blocked()) return 0;
    int lvl = holly_level();
    return lvl > imask();
}

static int s_depth;

void irq_service(void) {
    g_irq_pending = 0;
    irq_timer_tick();
    while (irq_any_pending()) {
        int lvl = holly_level();
        g_intevt = 0x200 + 0x20 * (15 - lvl);
        if (g_trace)
            logf_("irq level %d intevt %03X st=%08X/%08X/%08X m6=%08X m4=%08X m2=%08X", lvl, g_intevt,
                  s_status[0], s_status[1], s_status[2], g_holly_mask[0][0], g_holly_mask[1][0],
                  g_holly_mask[2][0]);
        /* hardware exception entry */
        g.ssr = sh4_get_sr();
        g.spc = 0xFFFFFFFFu;  /* resume point is the C call site */
        g.sgr = g.r[15];
        sh4_set_sr(g.ssr | (1u << 30) | (1u << 29) | (1u << 28));  /* MD RB BL */
        s_depth++;
        g_stats.irqs++;
        if (s_depth > 4) fatal("interrupt nesting overflow");
        call_indirect(g.vbr + 0x600, 0);
        s_depth--;
    }
}

/* ---------------- timing ---------------- */

/* Guest clock, in ns, for vblanks, SH-4 timers, SPG status and the sound chip.
 *
 * Normal mode: the host's clock, minus time spent paused in the settings menu.
 *
 * Deterministic mode (netplay and sync tests): the clock advances only as the
 * game executes. Every IRQ_CHECK (loop back-edge) counts as DET_NS_PER_CHECK
 * nanoseconds, timer work runs every DET_TICK_CHECKS checks instead of from a
 * host timer thread, and `sleep` skips ahead to the next event. Interrupts thus
 * land on the same instruction in every run, and identical inputs give an
 * identical game on any machine. Real-time pacing happens only at vblank
 * (det_pace), outside the game's view. */
int g_det;
int32_t g_tick_countdown = DET_TICK_CHECKS;
static uint64_t s_paused_ns, s_emu_ns, s_det_ns_per_check = DET_NS_PER_CHECK;
uint64_t host_time_ns(void) { return g_det ? s_emu_ns : SDL_GetTicksNS() - s_paused_ns; }
void host_pause_add(uint64_t ns) { s_paused_ns += ns; }
#define now_ns host_time_ns

void irq_det_init(void) {
    g_det = 1;
    const char *e = getenv("SFA3_DET_NS");  /* guest ns per loop iteration (tuning) */
    if (e && atoi(e) > 0) s_det_ns_per_check = (uint64_t)atoi(e);
    logf_("deterministic mode: %llu ns per check", (unsigned long long)s_det_ns_per_check);
}

/* IRQ_CHECK slow path: a pending interrupt, or (deterministic mode) the tick countdown ran out. */
void irq_slowpath(void) {
    if (g_tick_countdown < 0) {
        g_tick_countdown = DET_TICK_CHECKS;
        if (g_det) {
            s_emu_ns += DET_TICK_CHECKS * s_det_ns_per_check;
            irq_service();
            return;
        }
    }
    if (g_irq_pending) irq_service();
}

#define FRAME_NS 16683350ull /* 59.94 Hz NTSC */

uint64_t renderer_frame_ns(void);

/* Real-time pacing in deterministic mode, called once per vblank: wait until
 * this frame's wall-clock slot. Never affects guest state. */
static uint64_t s_pace_base, s_pace_frames;

/* Netplay time sync: a wait for the other player shifts this side's frame
 * schedule instead of being caught up afterwards, so both sides settle into step. */
void det_pace_shift(uint64_t ns) {
    if (s_pace_base) s_pace_base += ns;
}

void det_pace(void) {
    uint64_t base = s_pace_base, frames = s_pace_frames;
    uint64_t period = renderer_frame_ns() ? renderer_frame_ns() : FRAME_NS;
    uint64_t now = SDL_GetTicksNS();
    frames++;
    if (!base || now > base + frames * period + 8 * period || now + 8 * period < base + frames * period) {
        s_pace_base = now;  /* first frame, fell far behind, or resumed from a pause */
        s_pace_frames = 0;
        return;
    }
    uint64_t target = base + frames * period;
    s_pace_base = base;
    s_pace_frames = frames;
    /* wait for this frame's slot, servicing the network meanwhile (netplay) */
    void netplay_poll(void);
    while (now + 1500000 < target) {
        netplay_poll();
        SDL_DelayNS(1000000);
        now = SDL_GetTicksNS();
    }
    if (now < target) SDL_DelayPrecise(target - now);
}

static uint64_t s_next_vblank, s_last_vblank;
uint64_t g_vblank_count;
static int s_vblank_out_due;
void video_vblank(void);
uint64_t renderer_frame_ns(void);

/* Called from IRQ_CHECK paths via host_poll(); raises VBlank-in/out. */
void aica_update(void);

void mmio_tick(void);
void irq_timer_tick(void) {
    /* A host-side stall (loading a stage's textures, Windows holding the window
     * while it's dragged) would otherwise become game time all at once: the
     * sound CPU races ahead of the main CPU that feeds its streams, and the
     * last stretch of a stream plays twice. The music stream's ring is 0x1000
     * samples a channel at 44.1 kHz (93 ms, refilled a 46 ms half at a time),
     * so a stall of more than 30 ms counts as a pause instead (it was 50,
     * which still let some through). Not in deterministic (netplay) mode. */
    static uint64_t s_last_tick;
    if (!g_det) {
        uint64_t t0 = now_ns();
        if (s_last_tick && t0 > s_last_tick + 30000000ull) host_pause_add(t0 - s_last_tick - 1000000ull);
        s_last_tick = now_ns();
    }
    aica_update();
    mmio_tick();
    uint64_t t = now_ns();
    if (!s_next_vblank) s_next_vblank = t + FRAME_NS;
    if (t >= s_next_vblank) {
        g_vblank_count++;
        holly_raise(0, 1u << 3);  /* VBlank in */
        s_vblank_out_due = 1;
        s_last_vblank = t;
        video_vblank();
        /* 59.94 Hz, or the display's own refresh rate with vsync on */
        uint64_t frame = renderer_frame_ns() && !g_det ? renderer_frame_ns() : FRAME_NS;
        s_next_vblank += frame;
        if (now_ns() > s_next_vblank + frame * 4) s_next_vblank = now_ns() + frame;  /* fell behind */
    } else if (s_vblank_out_due && t >= s_last_vblank + 1400000) {
        s_vblank_out_due = 0;
        holly_raise(0, 1u << 4);  /* VBlank out (~1.4 ms after vblank in) */
    }
}

/* time since the last vblank-in edge, for SPG_STATUS */
uint64_t irq_frame_pos_ns(void) {
    uint64_t t = now_ns();
    if (!s_last_vblank || t < s_last_vblank) return 0;
    uint64_t d = t - s_last_vblank;
    return d < FRAME_NS ? d : FRAME_NS - 1;
}

/* Continue in real time from the current deterministic clock (after a
 * fast-forwarded boot back to the lobby): the guest clock carries on without a jump. */
void irq_leave_det(void) {
    if (!g_det) return;
    g_det = 0;
    s_paused_ns = SDL_GetTicksNS() - s_emu_ns;
    logf_("real-time mode from %llu ms", (unsigned long long)(s_emu_ns / 1000000));
}

/* Power-on state for an in-place machine reset (netplay sessions). */
void irq_reset(int det) {
    memset(s_status, 0, sizeof s_status);
    memset(g_holly_mask, 0, sizeof g_holly_mask);
    g_intevt = 0;
    s_depth = 0;
    g_irq_pending = 0;
    g_det = det;
    s_emu_ns = 0;
    g_tick_countdown = DET_TICK_CHECKS;
    s_pace_base = s_pace_frames = 0;
    s_next_vblank = s_last_vblank = 0;
    s_vblank_out_due = 0;
    g_vblank_count = 0;
}

void host_wait_for_irq(void) {
    if (g_det) {
        /* `sleep`: skip guest time ahead in small steps until something is pending */
        for (int i = 0; i < 1000000; i++) {
            s_emu_ns += 10000;
            irq_timer_tick();
            if (irq_any_pending()) return;
        }
        return;
    }
    /* sleep until the next vblank edge */
    uint64_t t0 = SDL_GetTicksNS();
    for (;; g_perf.idle_ns += SDL_GetTicksNS() - t0, t0 = SDL_GetTicksNS()) {
        irq_timer_tick();
        if (irq_any_pending()) return;
        uint64_t t = now_ns();
        if (s_next_vblank > t) {
            uint64_t d = s_next_vblank - t;
            if (d > 1000000) d = 1000000;
            SDL_DelayNS(d);
        }
    }
}
