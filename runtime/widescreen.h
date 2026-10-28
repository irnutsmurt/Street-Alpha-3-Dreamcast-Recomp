/* 16:9 fights (widescreen.c): what the renderer needs. */
#pragma once
#include <stdint.h>

/* a background layer to extend this frame (vk_renderer.c, ext.frag) */
typedef struct {
    float z;             /* its depth, 1/w */
    float q0, q1;        /* the picture's columns to draw over (guest pixels from its left edge) */
    float r0, r1;        /* and rows (guest pixels from its top) */
    int layer;           /* its fill's rows in the renderer's images: layer x WIDE_MASK_ROWS */
    int backdrop;        /* the farthest layer: rows past its fill's are a gap too (nothing is behind it) */
    float u0;            /* its map column (from the fill's first) at the picture's left edge */
    float v0;            /* its map row at the picture's top */
} WideExt;

#define WIDE_MASK_W 1024
#define WIDE_MASK_ROWS 240

/* The game sends a frame's polygons to the TA as the next frame starts, and they're drawn a frame after that (the
 * game has laid out its next frame by then): each stage layout's state is kept by its id, which the TA list that
 * holds its polygons takes when it starts (ta.c; 0: no layout since the last list, none) */
uint32_t widescreen_list_begin(void);
int widescreen_stage_frame(uint32_t list, float *bar);
int widescreen_extensions(uint32_t list, WideExt *ext, int max, int *mode);
/* the list holds layer captures (laid out at ATLAS_X: vk_renderer.c draws them into their own images) */
int widescreen_list_captures(uint32_t list);
/* the layer captures done (bits), their pixels (w x h each) and palettes (each texel's, the renderer's numbering: 0
 * none; WIDE_MASK_W x WIDE_MASK_ROWS each) */
void widescreen_atlas_ready(int layers, const uint8_t *rgba, const uint8_t *pal, int w, int h);
/* a stage's extension fill to upload (3 x WIDE_MASK_ROWS rows of WIDE_MASK_W): modes (0 none, 64 the art next to a
 * gap and 128 a gap: drawn behind the layer, 255 drawn over it) each with the palette of the art the colour comes
 * from (pairs of bytes), and colours (RGBA bytes, premultiplied); the renderer copies them */
void renderer_filler_masks(const uint8_t *modes, const uint32_t *fill);
