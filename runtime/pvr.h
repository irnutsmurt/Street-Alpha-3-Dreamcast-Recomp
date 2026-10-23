/* PowerVR2 (CLX2) register file, Tile Accelerator and display-list types. */
#pragma once
#include <stdint.h>

uint32_t pvr_reg_read(uint32_t off);
void pvr_reg_write(uint32_t off, uint32_t v);
uint32_t pvr_reg(uint32_t off);
uint32_t pvr_spg_status(void);
void pvr_vram_written(uint32_t p, int size);
uint32_t vram_read32_linear(uint32_t off);
void ta_write_word(uint32_t addr, uint32_t v);

uint32_t gdrom_reg_read(uint32_t off);
void gdrom_reg_write(uint32_t off, uint32_t v);
uint32_t aica_reg_read(uint32_t off, int size);
void aica_reg_write(uint32_t off, uint32_t v, int size);
uint32_t aica_rtc_read(uint32_t p);
uint32_t bios_read(uint32_t p, int size);
uint32_t flash_read(uint32_t off, int size);
uint32_t holly_reg(uint32_t off);

/* PVR register offsets (from 0x005F0000) */
enum {
    PVR_SOFTRESET = 0x8008,
    PVR_STARTRENDER = 0x8014,
    PVR_PARAM_BASE = 0x8020,
    PVR_REGION_BASE = 0x802C,
    PVR_VO_BORDER_COL = 0x8040,
    PVR_FB_R_CTRL = 0x8044,
    PVR_FB_W_CTRL = 0x8048,
    PVR_FB_W_LINESTRIDE = 0x804C,
    PVR_FB_R_SOF1 = 0x8050,
    PVR_FB_R_SOF2 = 0x8054,
    PVR_FB_R_SIZE = 0x805C,
    PVR_FB_W_SOF1 = 0x8060,
    PVR_FB_W_SOF2 = 0x8064,
    PVR_FB_X_CLIP = 0x8068,
    PVR_FB_Y_CLIP = 0x806C,
    PVR_FPU_SHAD_SCALE = 0x8074,
    PVR_FPU_PARAM_CFG = 0x807C,
    PVR_ISP_BACKGND_D = 0x8088,
    PVR_ISP_BACKGND_T = 0x808C,
    PVR_FOG_COL_RAM = 0x80B0,
    PVR_FOG_COL_VERT = 0x80B4,
    PVR_FOG_DENSITY = 0x80B8,
    PVR_SPG_HBLANK_INT = 0x80C8,
    PVR_SPG_VBLANK_INT = 0x80CC,
    PVR_SPG_CONTROL = 0x80D0,
    PVR_SPG_LOAD = 0x80D8,
    PVR_SPG_VBLANK = 0x80DC,
    PVR_TEXT_CONTROL = 0x80E4,
    PVR_VO_CONTROL = 0x80E8,
    PVR_SCALER_CTL = 0x80F4,
    PVR_PAL_RAM_CTRL = 0x8108,
    PVR_SPG_STATUS = 0x810C,
    PVR_PT_ALPHA_REF = 0x811C,
    PVR_TA_OL_BASE = 0x8124,
    PVR_TA_ISP_BASE = 0x8128,
    PVR_TA_ISP_LIMIT = 0x8130,
    PVR_TA_NEXT_OPB = 0x8134,
    PVR_TA_ITP_CURRENT = 0x8138,
    PVR_TA_GLOB_TILE_CLIP = 0x813C,
    PVR_TA_ALLOC_CTRL = 0x8140,
    PVR_TA_LIST_INIT = 0x8144,
    PVR_TA_YUV_TEX_BASE = 0x8148,
    PVR_TA_YUV_TEX_CTRL = 0x814C,
    PVR_TA_LIST_CONT = 0x8160,
    PVR_FOG_TABLE = 0x8200,
    PVR_PALETTE = 0x9000,
};

/* ---- decoded display list handed to the renderer ---- */

enum { LIST_OPAQUE, LIST_OPAQUE_MOD, LIST_TRANS, LIST_TRANS_MOD, LIST_PUNCH, LIST_COUNT };

typedef struct {
    float x, y, z;       /* screen x/y, z = 1/w */
    float u, v;
    uint32_t base;       /* ARGB8888 */
    uint32_t offset;     /* ARGB8888 */
} PvrVertex;

/* Render state captured from the TA global parameter (ISP/TSP/TCW words). */
typedef struct {
    uint32_t pcw, isp, tsp, tcw;
    uint32_t list;
    uint32_t first_vertex;
    uint32_t vertex_count;   /* as triangle-strip vertices */
    uint32_t tile_clip;      /* TA_GLOB_TILE_CLIP / user clip in tiles */
    int16_t clip[4];         /* user tile clip x0,y0,x1,y1 (tiles) */
} PvrPoly;

typedef struct {
    PvrVertex *verts;
    uint32_t nverts, cap_verts;
    PvrPoly *polys;
    uint32_t npolys, cap_polys;
    /* frame-level state snapshotted at STARTRENDER */
    uint32_t regs_bg_tag, regs_bg_depth;
    uint32_t fb_w_sof1, fb_w_ctrl, fb_w_linestride, fb_x_clip, fb_y_clip;
    uint32_t fog_col_vert, fog_col_ram, fog_density, pt_alpha_ref;
    uint32_t pal_ctrl;
    uint32_t isp_feed_cfg;   /* bit 0: translucent presort (1) vs autosort (0) */
    float fog_table[128];
    PvrVertex bg[3];
    uint32_t bg_isp, bg_tsp, bg_tcw;
} PvrFrame;

void ta_list_init(void);
void ta_reset(void);
PvrFrame *ta_frame_for(uint32_t param_base);

/* renderer backend (vk_renderer.c) */
int renderer_init(void *window);
void renderer_draw(const PvrFrame *frame);
void renderer_present_fb(void);  /* scan out the current framebuffer (FB_R_SOF1) */
void renderer_shutdown(void);
