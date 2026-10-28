#pragma once
#include <stdint.h>

typedef struct {
    uint32_t tcw;
    uint32_t w, h;
    uint32_t addr;      /* byte offset in 64-bit VRAM */
    uint32_t size;      /* bytes the texture spans */
    uint32_t pal;       /* palette base index */
    int paletted;
} TexKey;

void pvr_tex_info(uint32_t tcw, uint32_t tsp, TexKey *key);
void pvr_tex_decode(const TexKey *key, uint32_t *out_rgba);
uint64_t pvr_tex_hash(const TexKey *key);
/* a palette entry as decoded colour (pvr_tex_decode's); a paletted texture's entries' use: hist[entry] += texels */
uint32_t pvr_pal_color(uint32_t i);
void pvr_tex_pal_hist(const TexKey *key, float *hist);
