/* Runtime-internal interfaces. */
#pragma once
#include <stdint.h>

#include "dispatch.h"

#define OVERLAY_START 0x800000u  /* RAM offset of code overlays (characters, endings) */
#define OVERLAY_END   0x83C000u  /* three 0x14000-byte player slots */

_Noreturn void fatal(const char *fmt, ...);
void logf_(const char *fmt, ...);
extern int g_trace;

/* sh4.c */
void dispatch_init(void);

/* irq.c */
int irq_any_pending(void);
void holly_raise(int reg, uint32_t bits);  /* reg 0=normal 1=ext 2=err */
void holly_clear(int reg, uint32_t bits);
uint32_t holly_status(int reg);
void host_wait_for_irq(void);
void irq_timer_tick(void);

/* mmio.c */
void mmio_init(void);
void sq_flush(uint32_t a);
extern uint8_t *g_vram;      /* 8 MB */
extern uint8_t *g_aica_ram;  /* 2 MB */

/* hle.c: BIOS syscalls; returns 1 if addr was an HLE entry point */
int hle_call(uint32_t addr);
void hle_init(const char *disc_dir);

/* overlay.c */
GuestFn overlay_lookup(uint32_t addr);

/* ta.c */
void ta_write(const uint32_t *data, unsigned words);
void pvr_start_render(void);

/* bring-up statistics, logged once per second */
typedef struct {
    uint64_t vblanks, irqs, renders, ta_words, gd_sectors, maple_dmas, indirect_calls;
    uint32_t last_indirect[16];
    unsigned last_idx;
} Stats;
extern Stats g_stats;

/* host time per second, logged with the stats ("perf:"): where a slow
 * second went. ns; tex_decodes is a count. */
typedef struct {
    uint64_t draw_ns, present_ns, gpu_wait_ns, tex_ns, idle_ns, tex_decodes;
} Perf;
extern Perf g_perf;

/* the data folder: disc_path.txt, saves, log, screenshots (main.c) */
extern const char *g_data_dir;

/* host clock excluding time paused in the settings menu (irq.c) */
uint64_t host_time_ns(void);
void host_pause_add(uint64_t ns);

/* indirect-call histogram (debug) */
extern int g_call_hist_on;
void call_hist_add(uint32_t a);
void call_hist_dump(void);

/* deterministic mode (irq.c) */
extern int g_det;
void irq_det_init(void);
void det_pace(void);
void det_pace_shift(uint64_t ns);

/* In-place machine reset at the end of the current vblank: the game boots
 * again from power-on, deterministic (netplay) or real time. */
void machine_reset_request(int det);

/* Extension memory: host memory the game can reach at 0x98000000 (the SH-4's
 * area 6, unused on a Dreamcast), for data a PSP character has more of than
 * the Dreamcast's 16 MB holds room for (psp_load.c). Not saved in states. */
#define EXT_RAM_PHYS 0x18000000u
#define EXT_RAM_BASE 0x98000000u
#define EXT_RAM_SIZE (16u << 20)
extern uint8_t *g_ext_ram;

void read_trace_dump(const char *path);  /* read_trace.c (debug) */
