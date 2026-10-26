/* sfa3conv: the PSP characters add-on's slow conversions, in C.
 *
 * The add-on (tools/psp) converts the PSP characters' sprite tiles and voices;
 * in Python those loops take minutes. tools/psp/native.py loads this library
 * through ctypes (libsfa3conv.so / sfa3conv.dll, built by `sfa3port.py
 * dev-libs`, shipped in the app's bin/) and falls back to the Python versions
 * without it. Each function gives the same bytes as its Python version
 * (pac.py: unlz, twiddle, lz_opt, convert_tiles; sound.py: vag_decode,
 * resample's filter, aica_encode). Only the algorithms: no game data. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

/* bumped when any function's output changes (native.py checks it; the add-on's cache keys on it) */
EXPORT int sfa3conv_version(void) { return 1; }  /* 1: 0.2.5 */

/* ---------------- sprite tiles (pac.py) ---------------- */

/* unlz: size bytes from src at pos into out (room for size + 16: a run or copy may overshoot).
 * Returns 0, or -1 if the data runs out or copies from before the start. */
static int unlz(const uint8_t *src, size_t len, size_t pos, uint8_t *out, size_t size) {
    size_t n = 0;
    unsigned flags = 0, nb = 0;
    while (n < size) {
        if (nb == 0) {
            if (pos >= len) return -1;
            flags = src[pos++];
            nb = 8;
        }
        if (flags & 1) {
            if (pos >= len) return -1;
            unsigned b = src[pos++], ln = b & 15, d = b >> 4;
            if (d == 0) {
                if (pos >= len) return -1;
                uint8_t v = src[pos++];
                for (unsigned k = 0; k <= ln; k++) out[n++] = v;
            } else {
                if (d > n) return -1;
                for (unsigned k = 0; k < ln; k++, n++) out[n] = out[n - d];
            }
        } else {
            if (pos >= len) return -1;
            out[n++] = src[pos++];
        }
        flags >>= 1;
        nb--;
    }
    return 0;
}

/* PowerVR twiddle of a 16x16 4-bit tile: y in the even bits, x in the odd */
static unsigned twid(unsigned x, unsigned y) {
    unsigned r = 0;
    for (unsigned b = 0; b < 4; b++) r |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
    return r;
}

static void twiddle(const uint8_t *lin, uint8_t *out);
EXPORT void sfa3conv_twiddle(const uint8_t *lin, uint8_t *out) { twiddle(lin, out); }

/* untwiddle: the other way (Dreamcast tile -> rows) */
EXPORT void sfa3conv_untwiddle(const uint8_t *tw, uint8_t *out) {
    memset(out, 0, 128);
    for (unsigned y = 0; y < 16; y++)
        for (unsigned x = 0; x < 16; x++) {
            unsigned s = twid(x, y), v = (tw[s >> 1] >> (4 * (s & 1))) & 15, i = y * 16 + x;
            out[i >> 1] |= (uint8_t)(v << (4 * (i & 1)));
        }
}

static void twiddle(const uint8_t *lin, uint8_t *out) {
    memset(out, 0, 128);
    for (unsigned y = 0; y < 16; y++)
        for (unsigned x = 0; x < 16; x++) {
            unsigned s = y * 16 + x, v = (lin[s >> 1] >> (4 * (s & 1))) & 15, i = twid(x, y);
            out[i >> 1] |= (uint8_t)(v << (4 * (i & 1)));
        }
}

/* lz_opt: the smallest encoding for unlz (pac.py's, choice for choice). out: room for n + n / 8 + 1.
 * Returns its length. */
#define LZ_MAX 4096
static size_t lz_opt(const uint8_t *data, size_t n, uint8_t *out) {
    int best[LZ_MAX + 1];  /* on the stack: the add-on runs conversions on several threads */
    uint8_t kind[LZ_MAX], hd[LZ_MAX], hl[LZ_MAX];  /* 'L' literal, 'R' run (hl), 'C' copy (hd, hl) */
    if (n > LZ_MAX) return 0;
    best[n] = 0;
    for (size_t ii = n; ii-- > 0;) {
        size_t i = ii;
        int c = 9 + best[i + 1];
        uint8_t k = 'L', d0 = 0, l0 = 0;
        size_t run = 1;
        while (i + run < n && run < 16 && data[i + run] == data[i]) run++;
        for (size_t L = 1; L <= run; L++) {
            int cc = 17 + best[i + L];
            if (cc < c) c = cc, k = 'R', l0 = (uint8_t)L;
        }
        size_t dmax = i < 15 ? i : 15;
        for (size_t d = 1; d <= dmax; d++) {
            size_t L = 0;
            while (L < 15 && i + L < n && data[i + L] == data[i + L - d]) {
                L++;
                int cc = 9 + best[i + L];
                if (cc < c) c = cc, k = 'C', d0 = (uint8_t)d, l0 = (uint8_t)L;
            }
        }
        best[i] = c;
        kind[i] = k, hd[i] = d0, hl[i] = l0;
    }
    size_t o = 0, i = 0;
    while (i < n) {
        size_t flag_pos = o++;
        unsigned flags = 0;
        for (unsigned bit = 0; bit < 8 && i < n; bit++) {
            if (kind[i] == 'L') {
                out[o++] = data[i++];
            } else if (kind[i] == 'R') {
                flags |= 1u << bit;
                out[o++] = (uint8_t)(hl[i] - 1);
                out[o++] = data[i];
                i += hl[i];
            } else {
                flags |= 1u << bit;
                out[o++] = (uint8_t)(hd[i] << 4 | hl[i]);
                i += hl[i];
            }
        }
        out[flag_pos] = (uint8_t)flags;
    }
    return o;
}

EXPORT size_t sfa3conv_lz_opt(const uint8_t *data, size_t n, uint8_t *out) { return lz_opt(data, n, out); }

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* convert_tiles: a PSP sprite tile section (u32 offsets, then each 16x16 tile LZ-packed, row by row)
 * as the Dreamcast's (the same, twiddled). out: room for sfa3conv_tiles_room(). Returns the
 * length, or 0 if the section is damaged. */
EXPORT size_t sfa3conv_tiles_room(const uint8_t *sec, size_t len) {
    return len < 4 ? 0 : (u32(sec) / 4) * (4 + 128 + 17);
}

EXPORT size_t sfa3conv_convert_tiles(const uint8_t *sec, size_t len, uint8_t *out, size_t room) {
    if (len < 4) return 0;
    size_t count = u32(sec) / 4, o = 4 * count;
    if (4 * count > len || room < sfa3conv_tiles_room(sec, len)) return 0;
    uint8_t lin[128 + 16], tw[128];
    for (size_t k = 0; k < count; k++) {
        uint32_t at = u32(sec + 4 * k);
        uint32_t off = (uint32_t)o;
        memcpy(out + 4 * k, &off, 4);  /* little-endian hosts only (x86_64) */
        if (unlz(sec, len, at, lin, 128)) return 0;
        twiddle(lin, tw);
        o += lz_opt(tw, 128, out + o);
    }
    return o;
}

/* ---------------- voices (sound.py) ---------------- */

static const int VAG_FILTERS[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

/* vag_decode: Sony ADPCM to 16-bit samples, stopping after a frame flagged as the end.
 * out: room for len / 16 * 28. Returns the count. */
EXPORT size_t sfa3conv_vag_decode(const uint8_t *data, size_t len, int32_t *out) {
    size_t n = 0;
    int s1 = 0, s2 = 0;
    for (size_t o = 0; o + 15 < len; o += 16) {
        unsigned head = data[o], flags = data[o + 1], shift = head & 15, f = head >> 4;
        int f0 = f < 5 ? VAG_FILTERS[f][0] : 0, f1 = f < 5 ? VAG_FILTERS[f][1] : 0;
        for (unsigned k = 0; k < 28; k++) {
            unsigned b = data[o + 2 + k / 2], nib = (k & 1) ? b >> 4 : b & 15;
            int v = (int)(nib & 8 ? (int)nib - 16 : (int)nib) * 4096;  /* << 12 */
            v >>= shift;  /* arithmetic, as Python's */
            v += (s1 * f0 + s2 * f1 + 32) >> 6;
            if (v < -32768) v = -32768;
            if (v > 32767) v = 32767;
            s2 = s1, s1 = v;
            out[n++] = v;
        }
        if (flags & 1) break;
    }
    return n;
}

/* resample's filter: out[j] for j < nout, x padded with half zeros each side, the windowed-sinc
 * table (phases rows of 2 x half taps, made by sound.py) */
EXPORT void sfa3conv_resample(const int32_t *x, size_t n, double ratio, const double *table, int phases, int half,
                              int32_t *out, size_t nout) {
    int64_t taps = 2 * (int64_t)half;
    for (size_t j = 0; j < nout; j++) {
        double t = (double)j / ratio;
        int64_t i = (int64_t)t;
        const double *row = table + (int64_t)((t - (double)i) * phases) * taps;
        /* Python: sum(a * b for a, b in zip(xp[i + 1:i + 1 + 2 * half], row)), xp = half zeros + x + half
         * zeros; the same sum in the same order, without the zeros' terms (adding 0.0 changes nothing) */
        double v = 0.0;
        for (int64_t k = 0; k < taps; k++) {
            int64_t p = i + 1 + k - half;  /* the index into x */
            if (p >= 0 && p < (int64_t)n) v += (double)x[p] * row[k];
        }
        int64_t r = (int64_t)v;  /* int(): towards zero */
        out[j] = r < -32768 ? -32768 : r > 32767 ? 32767 : (int32_t)r;
    }
}

/* round(): to the nearest, halves to even (Python's) */
static int64_t round_even(double y) {
    int64_t t = (int64_t)y;    /* towards zero */
    double f = y - (double)t;  /* exact */
    if (f > 0.5 || (f == 0.5 && (t & 1))) t++;
    else if (f < -0.5 || (f == -0.5 && (t & 1))) t--;
    return t;
}

/* the voices' level: int(round(v * gain)) for each sample */
EXPORT void sfa3conv_scale(const int32_t *x, size_t n, double gain, int32_t *out) {
    for (size_t i = 0; i < n; i++) out[i] = (int32_t)round_even((double)x[i] * gain);
}

static const int AICA_SCALE[8] = {230, 230, 230, 230, 307, 409, 512, 614};

/* aica_encode: 16-bit samples to AICA ADPCM, each nibble the one whose decoded value comes nearest.
 * out: (n + 1) / 2 bytes */
EXPORT void sfa3conv_aica_encode(const int32_t *x, size_t n, uint8_t *out) {
    memset(out, 0, (n + 1) / 2);
    int cur = 0, step = 127;
    for (size_t i = 0; i < n; i++) {
        int d = x[i] - cur, ad = d < 0 ? -d : d;
        int mag = step ? (ad * 8 / step) / 2 : 0;
        if (mag > 7) mag = 7;
        int lo = mag - 1 < 0 ? 0 : mag - 1, hi = mag + 1 > 7 ? 7 : mag + 1;
        int bm = -1, bv = 0;
        for (int m = lo; m <= hi; m++) {  /* the set {lo, mag, hi}, ascending, as Python iterates it */
            int delta = ((m * 2 + 1) * step) >> 3;
            int v = d < 0 ? cur - delta : cur + delta;
            if (v < -32768) v = -32768;
            if (v > 32767) v = 32767;
            int e = x[i] - v, be = x[i] - bv;
            if (bm < 0 || (e < 0 ? -e : e) < (be < 0 ? -be : be)) bm = m, bv = v;
        }
        cur = bv;
        unsigned nib = (unsigned)bm | (d < 0 ? 8u : 0u);
        step = (step * AICA_SCALE[bm]) >> 8;
        if (step < 127) step = 127;
        if (step > 24576) step = 24576;
        out[i >> 1] |= (uint8_t)((i & 1) ? nib << 4 : nib);
    }
}
