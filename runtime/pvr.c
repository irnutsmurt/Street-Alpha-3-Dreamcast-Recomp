/* PVR register file, sync pulse generator status, and render start. */
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "pvr.h"
#include "runtime.h"
#include "sprite_swap.h"

static uint32_t s_regs[0x2000 / 4 * 4];  /* 0x8000..0x9FFF, word indexed */

#define REG(off) s_regs[((off) - 0x8000) >> 2]

static uint32_t s_last_scan;  /* pvr_spg_status() */
static uint64_t s_last_frame;

void pvr_reset(void) {
    memset(s_regs, 0, sizeof s_regs);
    s_last_scan = 0;
    s_last_frame = 0;
}

uint32_t pvr_reg(uint32_t off) { return REG(off); }

uint32_t pvr_reg_read(uint32_t off) {
    if (off >= 0xA000) return 0;
    return REG(off);
}

uint64_t irq_frame_pos_ns(void);

/* SPG_STATUS: scanline (9:0), fieldnum (10), blank (11), hsync (12), vsync (13).
 * The beam position follows the vblank timeline. A read that has crossed the
 * end of a frame since the previous read reports line 0, so polling loops
 * waiting for a specific line can't miss it the way a preempted thread could. */
uint32_t pvr_spg_status(void) {
    extern uint64_t g_vblank_count;
    uint32_t total_lines = ((REG(PVR_SPG_LOAD) >> 16) & 0x3FF) + 1;
    if (total_lines < 200) total_lines = 263;
    uint32_t vb = REG(PVR_SPG_VBLANK);
    uint32_t vbstart = vb & 0x3FF, vbend = (vb >> 16) & 0x3FF;
    if (!vbstart || vbstart >= total_lines) { vbstart = total_lines - 23; vbend = 21; }
    uint64_t pos = irq_frame_pos_ns();
    uint32_t line = (uint32_t)(pos * total_lines / 16683350ull);
    if (line >= total_lines) line = total_lines - 1;
    /* timeline starts at vblank-in (scanline vbstart) */
    uint32_t scan = (line + vbstart) % total_lines;
    uint64_t frame = g_vblank_count * total_lines + line + vbstart;  /* absolute line count */
    uint64_t frame_no = frame / total_lines;
    if (frame_no != s_last_frame && scan != 0 && s_last_scan != 0) {
        s_last_frame = frame_no;
        s_last_scan = 0;
        scan = 0;
    } else {
        s_last_frame = frame_no;
        s_last_scan = scan;
    }
    int in_vblank = scan >= vbstart || scan < vbend;
    uint32_t v = scan & 0x3FF;
    if (in_vblank) v |= 1u << 13 | 1u << 11;
    return v;
}

static void start_render(void) {
    g_stats.renders++;
    PvrFrame *f = ta_frame_for(REG(PVR_PARAM_BASE));
    static int logged;
    static long long from = -2;  /* SFA3_RENDER_LOG=<frame>: log every render from then on (debug) */
    if (from == -2) from = getenv("SFA3_RENDER_LOG") ? atoll(getenv("SFA3_RENDER_LOG")) : -1;
    extern uint64_t g_vblank_count;
    if (f && (g_trace || logged++ < 30 || (from >= 0 && (long long)g_vblank_count >= from && logged < 400))) {
        int counts[LIST_COUNT] = {0};
        for (uint32_t i = 0; i < f->npolys; i++) if (f->polys[i].list < LIST_COUNT) counts[f->polys[i].list]++;
        logf_("render: param=%06X fb_w=%06X polys=%u verts=%u (op %d pt %d tr %d)", REG(PVR_PARAM_BASE),
              REG(PVR_FB_W_SOF1), f->npolys, f->nverts, counts[LIST_OPAQUE], counts[LIST_PUNCH], counts[LIST_TRANS]);
    }
    if (f) {
        f->fb_w_sof1 = REG(PVR_FB_W_SOF1);
        f->fb_w_ctrl = REG(PVR_FB_W_CTRL);
        f->fb_w_linestride = REG(PVR_FB_W_LINESTRIDE);
        f->fb_x_clip = REG(PVR_FB_X_CLIP);
        f->fb_y_clip = REG(PVR_FB_Y_CLIP);
        f->fog_col_vert = REG(PVR_FOG_COL_VERT);
        f->fog_col_ram = REG(PVR_FOG_COL_RAM);
        f->fog_density = REG(PVR_FOG_DENSITY);
        f->pt_alpha_ref = REG(PVR_PT_ALPHA_REF);
        f->pal_ctrl = REG(PVR_PAL_RAM_CTRL);
        f->isp_feed_cfg = REG(0x8098);
        f->regs_bg_tag = REG(PVR_ISP_BACKGND_T);
        f->regs_bg_depth = REG(PVR_ISP_BACKGND_D);
        sprite_swap_frame(f);
        uint64_t t0 = SDL_GetTicksNS();
        renderer_draw(f);
        g_perf.draw_ns += SDL_GetTicksNS() - t0;
    }
    /* render done: ISP, TSP and video */
    holly_raise(0, 1u << 0 | 1u << 1 | 1u << 2);
}

void pvr_reg_write(uint32_t off, uint32_t v) {
    if (off >= 0xA000) return;
    REG(off) = v;
    switch (off) {
    case PVR_STARTRENDER: start_render(); break;
    case PVR_TA_LIST_INIT: if (v & 0x80000000u) ta_list_init(); break;
    case PVR_TA_LIST_CONT: if (v & 0x80000000u) ta_reset(); break;
    case PVR_SOFTRESET: if (v & 1) ta_reset(); break;
    }
}

const uint32_t *pvr_palette(void) { return &REG(PVR_PALETTE); }

/* Texture cache invalidation hook. */
void renderer_vram_written(uint32_t off, int size);
void pvr_vram_written(uint32_t p, int size) { renderer_vram_written(p, size); }
