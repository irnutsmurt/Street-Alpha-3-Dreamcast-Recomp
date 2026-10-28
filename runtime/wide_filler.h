/* 16:9 fights: a stage's filler and the background extension's fill (wide_filler.c). */
#pragma once
#include <stdint.h>

#define WIDE_LAYERS 3

typedef struct {
    int w, h;           /* atlas size: map units by rows of 2 guest pixels */
    uint32_t *rgb;      /* 0xRRGGBB */
    uint8_t *opaque;
    double x0;          /* the atlas's first column on the layer's map (units) */
    double home, rate;  /* the layer's map position at the 4:3 view's left edge: home + rate x (camera - mid) */
    double mid;
    float z;            /* how near: 1/w, larger is nearer */
    int behind;         /* a layer the game draws all of (one that scrolls by itself) is behind this one */
    uint8_t *filler;    /* out: 1 = filler (allocated by wide_filler) */
    uint8_t *mode;      /* out (wide_extend): 0 nothing to fill, 1 a gap (drawn behind the layer), 2 drawn over it,
                         * 3 behind the layer only: its art next to a gap (a seam's), the backdrop's art */
    uint32_t *fill;     /* out (wide_extend): the colour there, 0xAABBGGRR, premultiplied (alpha 255 where filled) */
    uint8_t *pal;       /* in (or NULL): each texel's palette (the renderer's numbering, 0 none) */
    uint8_t *fill_pal;  /* out (wide_extend, with pal): the palette of the art each fill's colour comes from */
} WideAtlas;

/* every layer's filler, for a 4:3 camera from lo to hi and a screen view units wide; 1 if any */
int wide_filler(WideAtlas *layers, int count, int lo, int hi, int view);

/* the background extension's fill for every layer (after wide_filler): the part of each the 4:3 game shows is v0
 * to v1 (its columns; lo, hi, view as above), the farthest layer the backdrop; grow: the filler made this many
 * units wider (and a row taller) first; band, blur: guest pixels (ext.frag) */
void wide_extend(WideAtlas *layers, int count, int lo, int hi, int view, int grow, float band, float blur);
/* the same for layer k alone (the layers don't depend on each other: one a thread) */
void wide_extend_layer(WideAtlas *layers, int count, int k, int lo, int hi, int view, int grow, float band, float blur);
