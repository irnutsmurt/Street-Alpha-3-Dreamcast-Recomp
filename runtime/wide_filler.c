/* 16:9 fights: finding a stage's filler (widescreen.c, vk_renderer.c).
 *
 * The stages' background layers hold, besides their art, parts the 4:3 game never shows: past where its camera
 * reaches, and behind nearer layers. Some of that is painted on like the rest (more sky, more of a building: kept,
 * the wider picture shows it); some is flat filler in colours the art doesn't use (0C's sky is navy, black and
 * magenta behind the houses: the artists' "nobody sees this"). A 16:9 picture can show both; the filler is
 * treated as missing art (the background extension continues the painted art over it).
 *
 * Input: each layer drawn alone over the part of its map the 4:3 and 16:9 games can show (the "atlas", map units
 * by rows of 2 guest pixels, point-sampled: no blends at boundaries), how it moves with the camera (rate, middle
 * position) and how near it is. For every 4:3 camera from wall to wall and screen column, a layer's pixel is shown
 * if no nearer layer is opaque in front of it; the rest of its opaque pixels are hidden. Filler: hidden regions of
 * exactly one colour, at least FILLER_MIN pixels (painted art, gradients and dithering, doesn't make those), in a
 * colour at least 98% hidden, that don't continue accepted art (a quarter of their border with it within
 * FILLER_CONTRAST of their colour), accepted art growing from the shown art outward; at the very end of the
 * layer's art, such a region that is a solid rectangle though it continues the art (08's sky ends in flat
 * dark-teal bands); and, next to those, regions of a colour the shown art uses whose own patch (the colour's
 * connected pixels) is at least 98% hidden and that don't continue the art either (0C's black corner: black is the
 * 4:3 game's too, in its own corner). Offline prototype: build/test/junk.py (JUNK_COMPONENTS=1). */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wide_filler.h"

#define FILLER_MIN 64
#define FILLER_CONTRAST 40

static int cdiff(uint32_t a, uint32_t b) {
    int d = 0;
    for (int s = 0; s < 24; s += 8) {
        int x = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        x = x < 0 ? -x : x;
        d = x > d ? x : d;
    }
    return d;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* whether region r continues the accepted art: a quarter of its border with it (opaque neighbours outside it, not
 * in a region not accepted) within FILLER_CONTRAST of its colour (d: room for 4 differences a pixel); near: set if
 * it borders a region not accepted (filler) */
static int continues_art(const WideAtlas *a, const int *label, const uint8_t *ok, const int *order, const int *start,
                         int r, int *d, int *near) {
    int w = a->w, h = a->h, nd = 0;
    uint32_t c0 = a->rgb[order[start[r]]];
    for (int j = start[r]; j < start[r + 1]; j++) {
        int p = order[j], y = p / w, x = p % w;
        int nb[4] = {y > 0 ? p - w : -1, y < h - 1 ? p + w : -1, y * w + (x + w - 1) % w, y * w + (x + 1) % w};
        for (int k = 0; k < 4; k++) {
            int q = nb[k];
            if (q < 0 || !a->opaque[q] || label[q] == r) continue;
            if (label[q] >= 0 && !ok[label[q]]) {  /* filler so far: not accepted art */
                if (near) *near = 1;
                continue;
            }
            d[nd++] = cdiff(a->rgb[q], c0);
        }
    }
    if (!nd) return 0;
    int lo = 0, hi = nd - 1, kq = nd / 4;
    while (lo < hi) {  /* the kq-th smallest difference */
        int pv = d[(lo + hi) / 2], i = lo, j = hi;
        while (i <= j) {
            while (d[i] < pv) i++;
            while (d[j] > pv) j--;
            if (i <= j) { int t = d[i]; d[i] = d[j]; d[j] = t; i++; j--; }
        }
        if (kq <= j) hi = j;
        else if (kq >= i) lo = i;
        else break;
    }
    return d[kq] < FILLER_CONTRAST;
}

/* one layer's filler from its pixels (rgb 0xRRGGBB, opaque) and which are shown; out: 1 = filler */
static void layer_filler(const WideAtlas *a, const uint8_t *shown, uint8_t *out) {
    int w = a->w, h = a->h, n = w * h;
    const uint32_t *px = a->rgb;
    const uint8_t *opq = a->opaque;
    memset(out, 0, (size_t)n);
    /* colours: hidden and shown counts (sorted unique keys) */
    uint32_t *keys = malloc((size_t)n * sizeof *keys);
    int nk = 0;
    for (int i = 0; i < n; i++)
        if (opq[i]) keys[nk++] = px[i];
    qsort(keys, (size_t)nk, sizeof *keys, cmp_u32);
    int nu = 0;
    for (int i = 0; i < nk; i++)
        if (!nu || keys[nu - 1] != keys[i]) keys[nu++] = keys[i];
    int *nh = calloc((size_t)nu + 1, sizeof *nh), *ns = calloc((size_t)nu + 1, sizeof *ns);
    int *kidx = malloc((size_t)n * sizeof *kidx);
    for (int i = 0; i < n; i++) {
        kidx[i] = -1;
        if (!opq[i]) continue;
        uint32_t *f = bsearch(&px[i], keys, (size_t)nu, sizeof *keys, cmp_u32);
        kidx[i] = (int)(f - keys);
        if (shown[i]) ns[kidx[i]]++;
        else nh[kidx[i]]++;
    }
    /* patches: each colour's connected pixels, how many hidden and shown */
    int *label = malloc((size_t)n * sizeof *label);
    int *stack = malloc((size_t)n * sizeof *stack);
    int *patch = malloc((size_t)n * sizeof *patch), npatch = 0;
    for (int i = 0; i < n; i++) patch[i] = -1;
    for (int i = 0; i < n; i++) {
        if (patch[i] >= 0 || !opq[i]) continue;
        int sp = 0;
        stack[sp++] = i;
        patch[i] = npatch;
        while (sp) {
            int p = stack[--sp], y = p / w, x = p % w;
            int nb[4] = {y > 0 ? p - w : -1, y < h - 1 ? p + w : -1, y * w + (x + w - 1) % w, y * w + (x + 1) % w};
            for (int k = 0; k < 4; k++) {
                int q = nb[k];
                if (q < 0 || patch[q] >= 0 || !opq[q] || px[q] != px[i]) continue;
                patch[q] = npatch;
                stack[sp++] = q;
            }
        }
        npatch++;
    }
    int *ph = calloc((size_t)npatch + 1, sizeof *ph), *ps = calloc((size_t)npatch + 1, sizeof *ps);
    for (int i = 0; i < n; i++)
        if (opq[i]) (shown[i] ? ps : ph)[patch[i]]++;
    /* candidates: hidden pixels of rare colours, or of patches all but hidden; regions of one colour among them */
    int *start = malloc(((size_t)n + 1) * sizeof *start), *order = malloc((size_t)n * sizeof *order);
    int nreg = 0, norder = 0;
    for (int i = 0; i < n; i++) label[i] = -1;
    for (int i = 0; i < n; i++) {
        if (label[i] >= 0 || !opq[i] || shown[i]) continue;
        if (!(nh[kidx[i]] > 49 * ns[kidx[i]]) && !(ph[patch[i]] > 49 * ps[patch[i]])) continue;
        int sp = 0;
        stack[sp++] = i;
        label[i] = nreg;
        start[nreg] = norder;
        while (sp) {
            int p = stack[--sp], y = p / w, x = p % w;
            order[norder++] = p;
            int nb[4] = {y > 0 ? p - w : -1, y < h - 1 ? p + w : -1, y * w + (x + w - 1) % w, y * w + (x + 1) % w};
            for (int k = 0; k < 4; k++) {
                int q = nb[k];
                if (q < 0 || label[q] >= 0 || !opq[q] || shown[q] || px[q] != px[i]) continue;
                label[q] = nreg;
                stack[sp++] = q;
            }
        }
        nreg++;
    }
    start[nreg] = norder;
    /* accepted art grows from the shown art over the regions that continue it; a region of a colour the shown
     * art uses (a patch all but hidden) is art for now (see below) */
    uint8_t *ok = malloc((size_t)nreg + 1), *by_patch = malloc((size_t)nreg + 1);
    for (int r = 0; r < nreg; r++) {
        int i = order[start[r]];
        by_patch[r] = !(nh[kidx[i]] > 49 * ns[kidx[i]]);
        ok[r] = start[r + 1] - start[r] < FILLER_MIN || by_patch[r];
    }
    int *d = malloc(((size_t)n + 1) * 4 * sizeof *d);
    for (int changed = 1; changed;) {
        changed = 0;
        for (int r = 0; r < nreg; r++) {
            if (ok[r]) continue;
            if (continues_art(a, label, ok, order, start, r, d, NULL)) ok[r] = 1, changed = 1;
        }
    }
    /* a patch region: filler if it doesn't continue the accepted art and it's next to filler found by colour (the
     * same unpainted edge: 0C's black corner by its navy and magenta) or at the layer's art's very end (08's sky
     * ends in flat bands each side); elsewhere a patch all hidden is painted, as likely as not (a lit window pane,
     * a flat roof line) */
    int left = w, right = -1;  /* the layer's art's outermost columns (in the capture) */
    for (int i = 0; i < n; i++)
        if (opq[i]) {
            int x = i % w;
            if (x < left) left = x;
            if (x > right) right = x;
        }
    /* a region of a colour the shown art doesn't use, at the layer's art's very end, that is a solid rectangle: a
     * flat band the artists closed the layer with (08's sky ends in dark-teal bands each side, never shown), filler
     * though its colour is near the art's beside it */
    for (int r = 0; r < nreg; r++) {
        if (by_patch[r] || !ok[r] || start[r + 1] - start[r] < FILLER_MIN) continue;
        int x0 = w, x1 = -1, y0 = h, y1 = -1;
        for (int j = start[r]; j < start[r + 1]; j++) {
            int x = order[j] % w, y = order[j] / w;
            x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
        }
        if ((x0 <= left + 1 || x1 >= right - 1) && (x1 - x0 + 1) * (y1 - y0 + 1) * 49 <= (start[r + 1] - start[r]) * 50)
            for (int j = start[r]; j < start[r + 1]; j++) out[order[j]] = 1;
    }
    for (int r = 0; r < nreg; r++) {
        if (!by_patch[r] || start[r + 1] - start[r] < FILLER_MIN) continue;
        int near = 0, end = 0;
        for (int j = start[r]; j < start[r + 1] && !end; j++) {
            int x = order[j] % w;
            end = x <= left + 1 || x >= right - 1;
        }
        if (!continues_art(a, label, ok, order, start, r, d, &near) && (near || end))
            for (int j = start[r]; j < start[r + 1]; j++) out[order[j]] = 1;
    }
    for (int r = 0; r < nreg; r++)
        if (!ok[r])
            for (int j = start[r]; j < start[r + 1]; j++) out[order[j]] = 1;
    free(d);
    free(by_patch);
    free(ok);
    free(ps);
    free(ph);
    free(patch);
    free(order);
    free(start);
    free(stack);
    free(label);
    free(kidx);
    free(ns);
    free(nh);
    free(keys);
}

#define WORDS 4  /* rows as bits: up to 256 */

int wide_filler(WideAtlas *layers, int count, int lo, int hi, int view) {
    /* shown: for every camera c from lo to hi and screen column s, near to far, what isn't covered yet; a column's
     * rows as bits */
    int found = 0, h = layers[0].h;
    if (h > WORDS * 64) h = WORDS * 64;
    uint64_t *opq[WIDE_LAYERS] = {0}, *shw[WIDE_LAYERS] = {0};
    for (int k = 0; k < count; k++) {
        WideAtlas *a = &layers[k];
        opq[k] = calloc((size_t)a->w * WORDS, sizeof(uint64_t));
        shw[k] = calloc((size_t)a->w * WORDS, sizeof(uint64_t));
        for (int y = 0; y < h; y++)
            for (int u = 0; u < a->w; u++)
                if (a->opaque[y * a->w + u]) opq[k][u * WORDS + y / 64] |= 1ull << (y % 64);
    }
    int order[WIDE_LAYERS];
    for (int k = 0; k < count; k++) order[k] = k;
    for (int i = 0; i < count; i++)  /* near to far: depth (1/w) descending */
        for (int j = i + 1; j < count; j++)
            if (layers[order[j]].z > layers[order[i]].z) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    for (int c = lo; c <= hi; c++) {
        int base[WIDE_LAYERS];
        for (int i = 0; i < count; i++) {
            WideAtlas *a = &layers[order[i]];
            double mx = a->home + a->rate * (c - a->mid) - a->x0;  /* the atlas column at screen column 0 */
            base[i] = (int)(mx < 0 ? mx - 1 : mx);
        }
        for (int s = 0; s < view; s++) {
            uint64_t cov[WORDS] = {0};
            for (int i = 0; i < count; i++) {
                WideAtlas *a = &layers[order[i]];
                int u = ((base[i] + s) % 1024 + 1024) % 1024;
                if (u >= a->w) continue;
                uint64_t *o = &opq[order[i]][u * WORDS], *sh = &shw[order[i]][u * WORDS];
                for (int wd = 0; wd < WORDS; wd++) {
                    sh[wd] |= o[wd] & ~cov[wd];
                    cov[wd] |= o[wd];
                }
            }
        }
    }
    for (int k = 0; k < count; k++) {
        WideAtlas *a = &layers[k];
        uint8_t *shown = calloc((size_t)a->w * a->h, 1);
        for (int y = 0; y < h; y++)
            for (int u = 0; u < a->w; u++) shown[y * a->w + u] = (shw[k][u * WORDS + y / 64] >> (y % 64)) & 1;
        if (!a->filler) a->filler = malloc((size_t)a->w * a->h);
        layer_filler(a, shown, a->filler);
        for (int i = 0; i < a->w * a->h; i++) found |= a->filler[i];
        free(shown);
        free(opq[k]);
        free(shw[k]);
    }
    return found;
}

/* ---------------- the extension's fill ----------------
 * What a 16:9 picture shows of a layer that it has no art for, and the colour to show there, worked out once a
 * stage in the layer's own map (a unit by two guest pixels: the size of the layer's texels); the renderer draws it
 * each frame, a texture (ext.frag). Modes:
 *  1 a gap, drawn behind the layer (whatever the layer draws covers it): past the part of it the 4:3 game shows
 *    (v0 to v1), where it's transparent and its art runs out: the opaque pixel nearest towards that part is no
 *    further in than its edge (RUNOUT_TOL); the art's ends the 4:3 game shows are its own (farther layers show past
 *    them) and stay. And the backdrop (the farthest layer) wherever it's transparent (nothing is behind it);
 *  2 its filler, grown, drawn over the layer (and a gap within the grown filler: the layer is drawn a pixel or two
 *    off this map's grid);
 *  3 behind the layer only: a unit or two of its art next to a gap (that pixel or two again), and all of the
 *    backdrop's art (its layout leaves rows out at the top when the camera rises; nothing is behind it).
 * The colour: the layer's own art nearest on the row (towards the 4:3 part, past it), a band of it averaged over
 * rows, more of them the further from the art (a blur growing with the distance); with no art on the row, the
 * nearest above or below; with none in the column either, the nearest fill in it. */
#define RUNOUT_TOL 2

static void extend_layer(WideAtlas *a, double v0, double v1, int backdrop, int grow, double band_u, double blur_g) {
    int w = a->w, h = a->h, n = w * h;
    uint8_t *fil = calloc((size_t)n, 1), *art = malloc((size_t)n);
    if (a->filler)  /* the filler grown (the layer is drawn up to a unit or two off the analysis's columns) */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                if (!a->filler[y * w + x]) continue;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -grow; dx <= grow; dx++) {
                        int yy = y + dy, xx = x + dx;
                        if (yy >= 0 && yy < h && xx >= 0 && xx < w && (dy == 0 || dx == 0)) fil[yy * w + xx] = 1;
                    }
            }
    for (int i = 0; i < n; i++) art[i] = a->opaque[i] && !fil[i];
    if (!a->mode) a->mode = malloc((size_t)n);
    if (!a->fill) a->fill = malloc((size_t)n * sizeof *a->fill);
    if (!a->fill_pal) a->fill_pal = malloc((size_t)n);
    memset(a->mode, 0, (size_t)n);
    memset(a->fill, 0, (size_t)n * sizeof *a->fill);
    memset(a->fill_pal, 0, (size_t)n);
    const uint8_t *pal = a->pal;
    int *la = malloc((size_t)w * sizeof *la), *ra = malloc((size_t)w * sizeof *ra);
    int *lo = malloc((size_t)w * sizeof *lo), *ro = malloc((size_t)w * sizeof *ro);
    int *va = malloc((size_t)n * sizeof *va);  /* each pixel's nearest art pixel in its column (index), -1 none */
    int xo[8];  /* the band's samples, from the art's edge into it */
    for (int s = 0; s < 8; s++) xo[s] = (int)lround((s + 0.5) * band_u / 8);
    for (int u = 0; u < w; u++) {
        int last = -1;
        for (int y = 0; y < h; y++) {  /* above (or at) */
            if (art[y * w + u]) last = y;
            va[y * w + u] = last;
        }
        last = -1;
        for (int y = h - 1; y >= 0; y--) {  /* below, if nearer */
            if (art[y * w + u]) last = y;
            int up = va[y * w + u];
            if (last >= 0 && (up < 0 || last - y < y - up)) up = last;
            va[y * w + u] = up < 0 ? -1 : up * w + u;
        }
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *ar = art + (size_t)y * w, *op = a->opaque + (size_t)y * w;
        for (int u = 0, l = -1, o = -1; u < w; u++) {  /* the nearest art / opaque pixel at or left of each */
            if (ar[u]) l = u;
            if (op[u]) o = u;
            la[u] = l, lo[u] = o;
        }
        for (int u = w - 1, r = -1, o = -1; u >= 0; u--) {  /* and at or right of it */
            if (ar[u]) r = u;
            if (op[u]) o = u;
            ra[u] = r, ro[u] = o;
        }
        for (int u = 0; u < w; u++) {
            int i = y * w + u, mode = 0, l = -1, r = -1;
            if (op[u]) {
                if (fil[i]) mode = 2, l = la[u], r = ra[u];
            } else if (backdrop) {
                mode = 1, l = la[u], r = ra[u];
            } else if (u < v0 + 1) {
                int o = ro[u];
                if (o >= 0 && o <= v0 + RUNOUT_TOL + 1) mode = 1, r = ra[o];
            } else if (u > v1 - 1) {
                int o = lo[u];
                if (o >= 0 && o >= v1 - RUNOUT_TOL - 1) mode = 1, l = la[o];
            }
            if (mode == 1 && fil[i]) mode = 2;  /* next to filler: over, as the filler (the layer may be drawn a pixel off) */
            if (!mode) continue;
            uint32_t c;
            if (l < 0 && r < 0) {  /* no art on the row: the nearest above or below */
                if (va[i] < 0) {  /* nor in the column: the nearest fill in it, once the rest is filled */
                    a->mode[i] = (uint8_t)(mode | 0x80);
                    continue;
                }
                c = a->rgb[va[i]];
                if (pal) a->fill_pal[i] = pal[va[i]];
            } else {
                int left = r < 0 || (l >= 0 && u - l <= r - u);
                int e = left ? l : r, dir = left ? -1 : 1;  /* the art's edge, and into the art from it */
                if (pal) a->fill_pal[i] = pal[y * w + e];
                double d = abs(u - e) / 0.6;  /* guest pixels */
                double rad = blur_g < 1 + d * 0.5 ? blur_g : 1 + d * 0.5;  /* guest pixels: rows of 2 */
                int sr = 0, sg = 0, sb = 0, ns = 0, yo[7];
                for (int j = -3; j <= 3; j++) yo[j + 3] = y + (int)lround(j * rad / 2 / 3);
                for (int s = 0; s < 8; s++) {
                    int x = e + dir * xo[s];
                    if (x < 0 || x >= w) continue;
                    for (int j = 0; j < 7; j++) {
                        int yy = yo[j];
                        if (yy < 0 || yy >= h || !art[yy * w + x]) continue;
                        uint32_t p = a->rgb[yy * w + x];
                        sr += (int)(p >> 16 & 255), sg += (int)(p >> 8 & 255), sb += (int)(p & 255), ns++;
                    }
                }
                c = ns ? (uint32_t)(sr / ns) << 16 | (uint32_t)(sg / ns) << 8 | (uint32_t)(sb / ns) : a->rgb[y * w + e];
            }
            a->mode[i] = (uint8_t)mode;
            a->fill[i] = 0xFF000000u | (c & 255) << 16 | (c & 0xFF00) | (c >> 16 & 255);  /* 0xRRGGBB -> RGBA bytes */
        }
    }
    for (int u = 0; u < w; u++)  /* those left: the nearest fill in their column */
        for (int y = 0; y < h; y++) {
            int i = y * w + u;
            if (!(a->mode[i] & 0x80)) continue;
            int best = -1;
            for (int d = 1; d < h && best < 0; d++)
                for (int s = -1; s <= 1 && best < 0; s += 2) {
                    int yy = y + s * d;
                    if (yy >= 0 && yy < h && a->mode[yy * w + u] && !(a->mode[yy * w + u] & 0x80)) best = yy * w + u;
                }
            a->mode[i] &= 0x7F;
            if (best < 0) a->mode[i] = 0;
            else a->fill[i] = a->fill[best], a->fill_pal[i] = a->fill_pal[best];
        }
    if (backdrop)  /* its art itself, behind it (mode 3): nothing is behind the backdrop, and where the game leaves
                    * some of it out (its rows past the top when the camera rises: the layout starts lower) this shows */
        for (int i = 0; i < n; i++) {
            if (a->mode[i] || !a->opaque[i]) continue;
            uint32_t c = a->rgb[i];
            a->mode[i] = 3, a->fill[i] = 0xFF000000u | (c & 255) << 16 | (c & 0xFF00) | (c >> 16 & 255);
            if (pal) a->fill_pal[i] = pal[i];
        }
    /* a ring of the layer's art next to its gaps, a unit or two wide (mode 3: drawn behind it only): the layer is drawn
     * up to a pixel off this map's grid, and the fill there, under its own art, closes a seam */
    for (int y = 0; y < h; y++)
        for (int u = 0; u < w; u++) {
            int i = y * w + u;
            if (a->mode[i] || !art[i]) continue;
            for (int d = 1; d <= grow && !a->mode[i]; d++)
                for (int s = -1; s <= 1; s += 2) {
                    int x = u + s * d;
                    if (x >= 0 && x < w && a->mode[y * w + x] == 1) {
                        a->mode[i] = 3, a->fill[i] = a->fill[y * w + x], a->fill_pal[i] = a->fill_pal[y * w + x];
                        break;
                    }
                }
            for (int s = -1; s <= 1 && !a->mode[i]; s += 2) {
                int yy = y + s;
                if (yy >= 0 && yy < h && a->mode[yy * w + u] == 1)
                    a->mode[i] = 3, a->fill[i] = a->fill[yy * w + u], a->fill_pal[i] = a->fill_pal[yy * w + u];
            }
        }
    free(va);
    free(ro);
    free(lo);
    free(ra);
    free(la);
    free(art);
    free(fil);
}

void wide_extend_layer(WideAtlas *layers, int count, int k, int lo, int hi, int view, int grow, float band, float blur) {
    int far = 0;
    for (int j = 1; j < count; j++)
        if (layers[j].z < layers[far].z) far = j;
    WideAtlas *a = &layers[k];
    double v0 = a->home + a->rate * (lo - a->mid) - a->x0, v1 = v0 + a->rate * (hi - lo) + view;
    extend_layer(a, v0, v1, k == far && !layers[far].behind, grow, band * 0.6, blur);
}

void wide_extend(WideAtlas *layers, int count, int lo, int hi, int view, int grow, float band, float blur) {
    for (int k = 0; k < count; k++) wide_extend_layer(layers, count, k, lo, hi, view, grow, band, blur);
}
