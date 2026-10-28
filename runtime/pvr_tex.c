/* PowerVR2 texture decoding to RGBA8. Handles twiddled and linear (scan
 * order / stride) layouts, VQ compression, 4/8bpp palettes, YUV422 and
 * mipmapped textures (level 0 only). */
#include "pvr_tex.h"

#include <string.h>

#include "lobby.h"
#include "pvr.h"
#include "runtime.h"

uint32_t pvr_reg(uint32_t off);
const uint32_t *pvr_palette(void);

enum { FMT_1555, FMT_565, FMT_4444, FMT_YUV422, FMT_BUMP, FMT_PAL4, FMT_PAL8, FMT_RESERVED };

/* Twiddled (Morton) index for texel (x, y) of a w x h power-of-two texture:
 * low bits interleave y and x (y in the even bit), the larger dimension's
 * remaining bits sit above. */
static inline uint32_t twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t mn = w < h ? w : h;
    uint32_t idx = 0, bit = 0;
    for (uint32_t i = 1; i < mn; i <<= 1) {
        if (y & i) idx |= 1u << bit;
        bit++;
        if (x & i) idx |= 1u << bit;
        bit++;
    }
    uint32_t shift = (uint32_t)__builtin_ctz(mn);
    if (w > h) idx |= (x >> shift) << bit;
    else if (h > w) idx |= (y >> shift) << bit;
    return idx;
}

static inline uint32_t c1555(uint16_t p) {
    uint32_t a = (p & 0x8000) ? 255 : 0;
    uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
    return a << 24 | ((b << 3) | (b >> 2)) << 16 | ((g << 3) | (g >> 2)) << 8 | ((r << 3) | (r >> 2));
}
static inline uint32_t c565(uint16_t p) {
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return 0xFFu << 24 | ((b << 3) | (b >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((r << 3) | (r >> 2));
}
static inline uint32_t c4444(uint16_t p) {
    uint32_t a = (p >> 12) & 15, r = (p >> 8) & 15, g = (p >> 4) & 15, b = p & 15;
    return (a * 17) << 24 | (b * 17) << 16 | (g * 17) << 8 | (r * 17);
}
static inline uint32_t c8888(uint32_t p) {  /* ARGB8888 -> RGBA8 (ABGR in memory) */
    return (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
}
static inline uint32_t conv16(int fmt, uint16_t p) {
    switch (fmt) {
    case FMT_1555: return c1555(p);
    case FMT_565: return c565(p);
    case FMT_4444: return c4444(p);
    default: return c565(p);
    }
}

static uint32_t pal_entry(uint32_t i) {
    uint32_t v = pvr_palette()[i & 1023];
    switch (pvr_reg(PVR_PAL_RAM_CTRL) & 3) {
    case 0: return c1555((uint16_t)v);
    case 1: return c565((uint16_t)v);
    case 2: return c4444((uint16_t)v);
    default: return c8888(v);
    }
}

static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
static void yuv_pair(uint16_t w0, uint16_t w1, uint32_t *out) {
    int u = (w0 & 0xFF) - 128, y0 = w0 >> 8, v = (w1 & 0xFF) - 128, y1 = w1 >> 8;
    int y[2] = {y0, y1};
    for (int i = 0; i < 2; i++) {
        int r = y[i] + (11 * v) / 8, gg = y[i] - (11 * u) / 32 - (23 * v) / 32, b = y[i] + (113 * u) / 64;
        out[i] = 0xFFu << 24 | (uint32_t)clamp8(b) << 16 | (uint32_t)clamp8(gg) << 8 | clamp8(r);
    }
}

/* Byte offset of mip level 0 within a mipmapped texture of size 2^k. */
static uint32_t mip_offset(int fmt, int vq, uint32_t size) {
    static const uint32_t mip16[11] = {0x6, 0x8, 0x10, 0x30, 0xB0, 0x2B0, 0xAB0, 0x2AB0, 0xAAB0,
                                       0x2AAB0, 0xAAAB0};
    static const uint32_t mipvq[11] = {0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556,
                                       0x5556, 0x15556};
    static const uint32_t mippal[11] = {0x3, 0x4, 0x8, 0x18, 0x58, 0x158, 0x558, 0x1558, 0x5558,
                                        0x15558, 0x55558};
    int k = __builtin_ctz(size);
    if (k > 10) k = 10;
    if (vq) return mipvq[k];
    if (fmt == FMT_PAL4) return mippal[k] / 2;
    if (fmt == FMT_PAL8) return mippal[k];
    return mip16[k];
}

void pvr_tex_info(uint32_t tcw, uint32_t tsp, TexKey *key) {
    memset(key, 0, sizeof *key);
    key->tcw = tcw;
    key->w = 8u << ((tsp >> 3) & 7);
    key->h = 8u << (tsp & 7);
    int fmt = (tcw >> 27) & 7;
    int stride = (tcw >> 25) & 1, scan = (tcw >> 26) & 1;
    if (fmt != FMT_PAL4 && fmt != FMT_PAL8 && scan && stride)
        key->w = (pvr_reg(PVR_TEXT_CONTROL) & 31) * 32;
    key->addr = (tcw & 0x1FFFFF) << 3;
    /* bytes covered, for cache invalidation */
    uint32_t texels = key->w * key->h;
    int vq = (tcw >> 30) & 1, mip = (tcw >> 31) & 1;
    uint32_t bytes;
    if (vq) bytes = 2048 + texels / 4;
    else if (fmt == FMT_PAL4) bytes = texels / 2;
    else if (fmt == FMT_PAL8) bytes = texels;
    else bytes = texels * 2;
    if (mip && !(scan && fmt < FMT_PAL4)) bytes = bytes * 4 / 3 + 0x100;
    key->size = bytes;
    if (fmt == FMT_PAL4) key->pal = ((tcw >> 21) & 63) << 4;
    if (fmt == FMT_PAL8) key->pal = ((tcw >> 25) & 3) << 8;
    key->paletted = fmt == FMT_PAL4 || fmt == FMT_PAL8;
}

uint32_t pvr_pal_color(uint32_t i) { return pal_entry(i); }

void pvr_tex_pal_hist(const TexKey *key, float *hist) {
    int fmt = (key->tcw >> 27) & 7, vq = (key->tcw >> 30) & 1, mip = (key->tcw >> 31) & 1;
    if ((fmt != FMT_PAL4 && fmt != FMT_PAL8) || vq) return;
    uint32_t w = key->w, h = key->h, base = key->addr + (mip ? mip_offset(fmt, 0, w) : 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t t = twiddle(x, y, w, h);
            uint8_t b = g_vram[(base + (fmt == FMT_PAL4 ? t >> 1 : t)) & 0x7FFFFF];
            hist[fmt == FMT_PAL4 ? ((t & 1) ? b >> 4 : b & 15) : b] += 1.0f;
        }
}

/* twiddle() split by axis: the x and y bits never overlap, so
 * twiddle(x, y) == tw_x[x] | tw_y[y] (tables built once per decode). */
static void twiddle_tables(uint32_t w, uint32_t h, uint32_t *tx, uint32_t *ty) {
    for (uint32_t x = 0; x < w; x++) tx[x] = twiddle(x, 0, w, h);
    for (uint32_t y = 0; y < h; y++) ty[y] = twiddle(0, y, w, h);
}

void pvr_tex_decode(const TexKey *key, uint32_t *out) {
    const uint8_t *vram = g_vram;
    uint32_t tcw = key->tcw, w = key->w, h = key->h;
    int fmt = (tcw >> 27) & 7;
    int vq = (tcw >> 30) & 1, mip = (tcw >> 31) & 1, scan = (tcw >> 26) & 1;
    uint32_t base = key->addr;
    if (fmt == FMT_PAL4 || fmt == FMT_PAL8) scan = 0;  /* palette textures are always twiddled */
    if (mip && !scan) base += mip_offset(fmt, vq, w);
    #define V8(o) vram[(base + (o)) & 0x7FFFFF]
    #define V16(o) ((uint16_t)(V8(o) | V8((o) + 1) << 8))

    if (vq) {
        uint32_t cb = base - (mip ? mip_offset(fmt, vq, w) : 0);  /* codebook precedes data */
        const uint32_t idx_base = 2048;
        for (uint32_t y = 0; y < h; y += 2)
            for (uint32_t x = 0; x < w; x += 2) {
                uint32_t t = twiddle(x / 2, y / 2, w / 2, h / 2);
                uint8_t code = V8(idx_base + t);
                for (int i = 0; i < 4; i++) {
                    /* the 4 codebook texels are themselves stored twiddled (2x2) */
                    uint32_t px = x + (i >> 1), py = y + (i & 1);
                    uint32_t o = cb + code * 8u + (uint32_t)i * 2;
                    uint16_t p = (uint16_t)(vram[o & 0x7FFFFF] | vram[(o + 1) & 0x7FFFFF] << 8);
                    out[py * w + px] = fmt == FMT_YUV422 ? c565(p) : conv16(fmt, p);
                }
            }
        return;
    }
    switch (fmt) {
    case FMT_PAL4: case FMT_PAL8: {
        /* the palette converted once, not per texel */
        uint32_t lut[256], n = fmt == FMT_PAL4 ? 16 : 256;
        for (uint32_t i = 0; i < n; i++) lut[i] = pal_entry(key->pal + i);
        static uint32_t tx[1024], ty[1024];
        twiddle_tables(w, h, tx, ty);
        uint8_t *idx = (uint8_t *)out;  /* indices first (in place), then colours from the end back */
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = idx + y * w;
            uint32_t yb = ty[y];
            if (fmt == FMT_PAL4)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t t = tx[x] | yb;
                    uint8_t b = V8(t >> 1);
                    row[x] = (t & 1) ? b >> 4 : b & 15;
                }
            else
                for (uint32_t x = 0; x < w; x++) row[x] = V8(tx[x] | yb);
        }
        if (fmt == FMT_PAL4) lobby_patch_texture(idx, w, h);
        for (uint32_t i = w * h; i-- > 0;) out[i] = lut[idx[i]];
        return;
    }
    case FMT_YUV422:
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x += 2) {
                uint16_t a, b;
                if (scan) {
                    a = V16((y * w + x) * 2);
                    b = V16((y * w + x + 1) * 2);
                } else {
                    a = V16(twiddle(x, y, w, h) * 2);
                    b = V16(twiddle(x + 1, y, w, h) * 2);
                }
                yuv_pair(a, b, &out[y * w + x]);
            }
        return;
    default:
        if (scan) {
            for (uint32_t i = 0; i < w * h; i++) out[i] = conv16(fmt, V16(i * 2));
            return;
        }
        {
            static uint32_t tx[1024], ty[1024];
            twiddle_tables(w, h, tx, ty);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) out[y * w + x] = conv16(fmt, V16((tx[x] | ty[y]) * 2));
        }
        return;
    }
    #undef V8
    #undef V16
}

/* FNV-1a over the texture bytes (and palette for paletted textures). */
uint64_t pvr_tex_hash(const TexKey *key) {
    uint64_t h = 1469598103934665603ull;
    uint32_t end = key->addr + key->size;
    for (uint32_t a = key->addr; a < end; a += 8) {
        uint64_t v;
        memcpy(&v, g_vram + (a & 0x7FFFF8), 8);
        h = (h ^ v) * 1099511628211ull;
    }
    if (key->paletted) {
        const uint32_t *pal = pvr_palette();
        uint32_t n = ((key->tcw >> 27) & 7) == FMT_PAL4 ? 16 : 256;
        for (uint32_t i = 0; i < n; i++) h = (h ^ pal[(key->pal + i) & 1023]) * 1099511628211ull;
        h ^= pvr_reg(PVR_PAL_RAM_CTRL) & 3;
        if (lobby_cpu_label()) h = ~h;  /* the patched look is a different texture */
    }
    return h;
}
