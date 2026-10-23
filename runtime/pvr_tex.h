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
