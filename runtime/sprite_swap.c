/* Sprite swaps: some of the game's sprites drawn with the port's own images.
 *
 * The memory card screen (and its save / load prompts) shows each controller
 * port's two expansion slots, a VMU where a memory card is and an empty slot
 * otherwise, with "1" / "2" under them. The port emulates one memory card
 * per controller, so each port gets one image instead: assets/vmu_card.png
 * where the game shows a VMU, assets/vmu_empty.png where it shows an empty
 * slot, centred over the port's label, and the slot numbers go.
 *
 * The game builds its sprites from 16x16 tiles of runtime-assembled 4/8-bit
 * textures, so the sprites are recognised tile by tile: a hash of the tile's
 * colour indices (not its palette or place in video memory). A sprite counts
 * only as a full block of recognised tiles, so a plain tile that happens to
 * match elsewhere is left alone. Display only, so netplay can't see it. */
#include "sprite_swap.h"

#include <math.h>
#include <string.h>

#include "pvr_tex.h"
#include "runtime.h"

#include "ps_circle.inc"
#include "ps_cross.inc"
#include "ps_l1.inc"
#include "ps_l2.inc"
#include "ps_r1.inc"
#include "ps_r2.inc"
#include "ps_square.inc"
#include "ps_triangle.inc"
#include "vmu_card.inc"
#include "xb_a.inc"
#include "xb_b.inc"
#include "xb_lb.inc"
#include "xb_lt.inc"
#include "xb_rb.inc"
#include "xb_rt.inc"
#include "xb_x.inc"
#include "xb_y.inc"
#include "vmu_empty.inc"

/* plain colours (ABGR in memory): the pause menu's panel and cursor bar */
#define SOLID(c) {c, c, c, c, c, c, c, c, c, c, c, c, c, c, c, c}
static const uint32_t s_solid_panel[16] = SOLID(0xE0802080u), s_solid_bar[16] = SOLID(0xA0909000u);
#define IMG(name) {name##_rgba, name##_w, name##_h}
static const struct { const uint32_t *px; uint32_t w, h; } s_images[IMG_COUNT] = {
    IMG(vmu_card), IMG(vmu_empty), IMG(ps_cross), IMG(ps_circle), IMG(ps_square), IMG(ps_triangle),
    IMG(ps_l1), IMG(ps_r1), IMG(ps_l2), IMG(ps_r2), IMG(xb_a), IMG(xb_b), IMG(xb_x), IMG(xb_y),
    IMG(xb_lb), IMG(xb_rb), IMG(xb_lt), IMG(xb_rt), {s_solid_panel, 4, 4}, {s_solid_bar, 4, 4},
};
#define IMG_CARD IMG_VMU_CARD
#define IMG_EMPTY IMG_VMU_EMPTY

const uint32_t *sprite_swap_image(uint32_t id, uint32_t *w, uint32_t *h) {
    if (id >= IMG_COUNT) return NULL;
    *w = s_images[id].w;
    *h = s_images[id].h;
    return s_images[id].px;
}

/* tiles (FNV-1a of the 16x16 colour indices, row by row) */
static const uint32_t s_vmu_tiles[] = {
    0x1BE24AF9, 0x2DBAB9EF, 0x4DBFA6ED, 0x602D8D13, 0x62E56110, 0x6B55EE34,
    0x8783771B, 0x8A6369B4, 0xA018AEDE, 0xC953CCC9, 0xD04E019C, 0xEB2E4495,
};
static const uint32_t s_empty_tiles[] = {
    0x0376E1D2, 0x07E9BB3D, 0x2AC1E83C, 0x2F7815C5, 0x2FFC6533,
    0x36A1AFE0, 0x53F9462A, 0x9A5FC06A, 0xC7140C45, 0xDA9ABA85,
};
static const uint32_t s_digit_tiles[] = {0x3D4A4BB0, 0xBEA627A1};  /* the slots' "1" and "2" */

static int in(const uint32_t *set, int n, uint32_t h) {
    for (int i = 0; i < n; i++)
        if (set[i] == h) return 1;
    return 0;
}

static uint32_t twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t mn = w < h ? w : h, idx = 0, bit = 0;
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

/* the 16x16 tile a polygon shows, as a hash of its colour indices; 0 if it
 * doesn't show exactly one tile of a 4/8-bit texture */
static uint32_t tile_hash(const PvrFrame *pf, const PvrPoly *p, float box[4], float uv[4]) {
    if (!(p->isp & (1u << 25)) || p->vertex_count != 4 || SPRITE_SWAP_IS_TCW(p->tcw)) return 0;
    int fmt = (p->tcw >> 27) & 7;
    if (fmt != 5 && fmt != 6) return 0;
    TexKey key;
    pvr_tex_info(p->tcw, p->tsp, &key);
    box[0] = box[1] = uv[0] = uv[1] = 1e9f;
    box[2] = box[3] = uv[2] = uv[3] = -1e9f;
    for (uint32_t k = 0; k < 4; k++) {
        const PvrVertex *v = &pf->verts[p->first_vertex + k];
        box[0] = fminf(box[0], v->x); box[2] = fmaxf(box[2], v->x);
        box[1] = fminf(box[1], v->y); box[3] = fmaxf(box[3], v->y);
        uv[0] = fminf(uv[0], v->u); uv[2] = fmaxf(uv[2], v->u);
        uv[1] = fminf(uv[1], v->v); uv[3] = fmaxf(uv[3], v->v);
    }
    float tx = uv[0] * key.w, ty = uv[1] * key.h;
    if (fabsf((uv[2] - uv[0]) * key.w - 16) > 0.5f || fabsf((uv[3] - uv[1]) * key.h - 16) > 0.5f) return 0;
    uint32_t cx = (uint32_t)lroundf(tx), cy = (uint32_t)lroundf(ty);
    if (cx % 16 || cy % 16 || cx + 16 > key.w || cy + 16 > key.h) return 0;
    uint32_t hsh = 2166136261u;
    for (uint32_t y = cy; y < cy + 16; y++)
        for (uint32_t x = cx; x < cx + 16; x++) {
            uint32_t t = twiddle(x, y, key.w, key.h), i;
            if (fmt == 5) {
                uint8_t b = g_vram[(key.addr + (t >> 1)) & 0x7FFFFF];
                i = (t & 1) ? b >> 4 : b & 15;
            } else {
                i = g_vram[(key.addr + t) & 0x7FFFFF];
            }
            hsh = (hsh ^ i) * 16777619u;
        }
    return hsh;
}

#define MAX_TILES 256
#define MAX_GROUPS 16
typedef struct {
    float box[4];
    int card, tiles, poly;  /* poly: a tile of it, the template for the image */
} Group;

/* the image over a box: a polygon like the tile `tpl`, its corners moved */
static void add_image(PvrFrame *pf, uint32_t tpl, int img, float x0, float y0, float x1, float y1) {
    if (pf->npolys + 1 > pf->cap_polys || pf->nverts + 4 > pf->cap_verts) return;
    const PvrPoly *t = &pf->polys[tpl];
    float bx0 = 1e9f, by0 = 1e9f, z = 0;
    for (uint32_t k = 0; k < 4; k++) {
        const PvrVertex *v = &pf->verts[t->first_vertex + k];
        bx0 = fminf(bx0, v->x);
        by0 = fminf(by0, v->y);
        z = fmaxf(z, v->z);
    }
    PvrPoly *p = &pf->polys[pf->npolys++];
    *p = *t;
    p->tcw = SPRITE_SWAP_TCW(img);
    p->tsp = (t->tsp & ~0x3Fu) | (3u << 3) | 3u;  /* 64x64 */
    p->first_vertex = pf->nverts;
    p->vertex_count = 4;  /* the template is hidden by now */
    for (uint32_t k = 0; k < 4; k++) {
        PvrVertex v = pf->verts[t->first_vertex + k];
        int left = v.x - bx0 < 0.5f, top = v.y - by0 < 0.5f;
        v.x = left ? x0 : x1;
        v.y = top ? y0 : y1;
        v.u = left ? 0.0f : 1.0f;
        v.v = top ? 0.0f : 1.0f;
        v.z = z;
        pf->verts[pf->nverts++] = v;
    }
}

/* ---------------- BUTTON CONFIG: the cursor bar and the 3-punch / 3-kick icons ---------------- */

extern uint64_t g_vblank_count;
static struct { float left, width; uint64_t frame; } s_bar[2];
static uint64_t s_icons_frame;
static int recent(uint64_t f) { return f && g_vblank_count - f <= 2; }

void sprite_swap_bar(int p, float left_px, float width_px) {
    s_bar[p & 1].left = left_px;
    s_bar[p & 1].width = width_px;
    s_bar[p & 1].frame = g_vblank_count;
}
void sprite_swap_attack_icons(int on) { s_icons_frame = on ? g_vblank_count : 0; }

static void poly_box(const PvrFrame *pf, const PvrPoly *p, float b[4]) {
    b[0] = b[1] = 1e9f;
    b[2] = b[3] = -1e9f;
    for (uint32_t k = 0; k < p->vertex_count; k++) {
        const PvrVertex *v = &pf->verts[p->first_vertex + k];
        b[0] = fminf(b[0], v->x); b[2] = fmaxf(b[2], v->x);
        b[1] = fminf(b[1], v->y); b[3] = fmaxf(b[3], v->y);
    }
}

static void button_config(PvrFrame *pf) {
    uint32_t npolys = pf->npolys;
    for (uint32_t i = 0; i < npolys; i++) {
        PvrPoly *p = &pf->polys[i];
        if (p->vertex_count != 4) continue;
        float b[4];
        poly_box(pf, p, b);
        if (!(p->isp & (1u << 25))) {
            /* a cursor bar: plain, about 32 pixels tall, at most 260 wide,
             * on its player's half. The game animates a new bar in (the
             * pause screen remakes both on every move), so any width or
             * place counts: it is put on the cell. */
            float w = b[2] - b[0], h = b[3] - b[1];
            if (w < 8 || w > 268 || h < 24 || h > 40) continue;
            int k = (b[0] + b[2]) / 2 >= 320;
            if (!recent(s_bar[k].frame) || s_bar[k].left < 0) continue;
            for (uint32_t v = 0; v < 4; v++) {
                PvrVertex *pv = &pf->verts[p->first_vertex + v];
                pv->x = pv->x > (b[0] + b[2]) / 2 ? s_bar[k].left + s_bar[k].width : s_bar[k].left;
            }
            continue;
        }
        if (!recent(s_icons_frame)) continue;
        /* an attack icon: a 32x32 cell of the 256x256 sheet; heavy punch at
         * (128, 32), heavy kick at (224, 32); 3 punches at (96, 64), 3 kicks
         * at (160, 64) */
        TexKey key;
        pvr_tex_info(p->tcw, p->tsp, &key);
        if (key.w != 256 || key.h != 256) continue;
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (uint32_t k = 0; k < 4; k++) {
            const PvrVertex *v = &pf->verts[p->first_vertex + k];
            u0 = fminf(u0, v->u); u1 = fmaxf(u1, v->u);
            v0 = fminf(v0, v->v); v1 = fmaxf(v1, v->v);
        }
        if (fabsf(u1 - u0 - 0.125f) > 0.01f || fabsf(v0 - 0.125f) > 0.01f || fabsf(v1 - v0 - 0.125f) > 0.01f) continue;
        float nu;
        if (fabsf(u0 - 0.5f) < 0.01f) nu = 0.375f;
        else if (fabsf(u0 - 0.875f) < 0.01f) nu = 0.625f;
        else continue;
        if (pf->npolys + 1 > pf->cap_polys || pf->nverts + 4 > pf->cap_verts) return;
        PvrPoly *c = &pf->polys[pf->npolys++];
        *c = *p;
        c->first_vertex = pf->nverts;
        for (uint32_t k = 0; k < 4; k++) {
            PvrVertex v = pf->verts[p->first_vertex + k];
            v.y += 32;  /* the next row: 16 units */
            v.u += nu - u0;
            v.v += 0.125f;
            pf->verts[pf->nverts++] = v;
        }
    }
}

/* ---------------- images over the picture ---------------- */

#define MAX_IMAGES 32
static struct { int img, behind; float box[4]; } s_req[MAX_IMAGES];
static int s_nreq;
static uint64_t s_req_frame;

void sprite_swap_images_begin(void) { s_nreq = 0; s_req_frame = g_vblank_count; }
static void request(int img, int behind, float x0, float y0, float x1, float y1) {
    if (s_nreq == MAX_IMAGES) return;
    s_req[s_nreq].img = img;
    s_req[s_nreq].behind = behind;
    s_req[s_nreq].box[0] = x0; s_req[s_nreq].box[1] = y0;
    s_req[s_nreq].box[2] = x1; s_req[s_nreq].box[3] = y1;
    s_nreq++;
    s_req_frame = g_vblank_count;
}
void sprite_swap_image_at(int img, float x0, float y0, float x1, float y1) { request(img, 0, x0, y0, x1, y1); }
void sprite_swap_image_behind(int img, float x0, float y0, float x1, float y1) { request(img, 1, x0, y0, x1, y1); }

/* the requested images, in front of everything, drawn like the frame's
 * frontmost textured translucent polygon. A panel (sprite_swap_image_behind)
 * goes in front of the whole picture, and the text letters on it (the print
 * routine's 20x24 sprites inside it) in front of the panel: a prompt over the
 * title screen's logo. */
#define MAX_LIFT 64
static void images(PvrFrame *pf) {
    uint32_t lift[MAX_LIFT];
    int nlift = 0;
    int tpl = -1, panels = 0;
    float z = 0, zall = 0;
    for (uint32_t i = 0; i < pf->nverts; i++)
        if (pf->verts[i].z < 1e30f && pf->verts[i].z > zall) zall = pf->verts[i].z;
    for (int r = 0; r < s_nreq; r++) panels |= s_req[r].behind;
    if (panels) {
        for (uint32_t i = 0; i < pf->npolys; i++) {
            const PvrPoly *p = &pf->polys[i];
            if (p->list != LIST_TRANS || !(p->isp & (1u << 25)) || !p->vertex_count) continue;
            float b[4];
            poly_box(pf, p, b);
            if (b[2] - b[0] > 22 || b[3] - b[1] > 26) continue;  /* the print routine's letters: 20x24 */
            for (int r = 0; r < s_nreq; r++)
                if (s_req[r].behind && b[0] >= s_req[r].box[0] - 1 && b[2] <= s_req[r].box[2] + 1 &&
                    b[1] >= s_req[r].box[1] - 1 && b[3] <= s_req[r].box[3] + 1) {
                    for (uint32_t k = 0; k < p->vertex_count; k++) pf->verts[p->first_vertex + k].z = zall * 1.002f;
                    if (nlift < MAX_LIFT) lift[nlift++] = i;
                    break;
                }
        }
    }
    for (uint32_t i = 0; i < pf->npolys; i++) {
        const PvrPoly *p = &pf->polys[i];
        if (p->list != LIST_TRANS || !(p->isp & (1u << 25)) || p->vertex_count < 3 || SPRITE_SWAP_IS_TCW(p->tcw)) continue;
        float pz = pf->verts[p->first_vertex].z;
        if (tpl < 0 || pz >= z) { tpl = (int)i; z = pz; }
    }
    if (tpl < 0) return;
    for (int r = 0; r < s_nreq; r++) {
        if (pf->npolys + 1 > pf->cap_polys || pf->nverts + 4 > pf->cap_verts) return;
        const PvrPoly *t = &pf->polys[tpl];
        PvrPoly *p = &pf->polys[pf->npolys++];
        *p = *t;
        p->tcw = SPRITE_SWAP_TCW(s_req[r].img);
        p->tsp = (t->tsp & ~0x3Fu) | (3u << 3) | 3u;  /* 64x64 */
        p->tsp &= ~(1u << 19);                        /* the image's alpha */
        if (s_req[r].img == IMG_PANEL || s_req[r].img == IMG_BAR) p->tsp &= ~(3u << 13);  /* plain edges */
        p->first_vertex = pf->nverts;
        p->vertex_count = 4;
        static const float corner[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};  /* a strip */
        for (int k = 0; k < 4; k++) {
            PvrVertex v = pf->verts[t->first_vertex];
            v.x = corner[k][0] ? s_req[r].box[2] : s_req[r].box[0];
            v.y = corner[k][1] ? s_req[r].box[3] : s_req[r].box[1];
            v.u = corner[k][0];
            v.v = corner[k][1];
            v.z = s_req[r].behind ? zall * (1.0005f + 0.0005f * (float)r / MAX_IMAGES) : z;
            v.base = 0xFFFFFFFFu;
            v.offset = 0;
            pf->verts[pf->nverts++] = v;
        }
    }
    /* the letters after the panel too (a frame whose translucent sprites are
     * drawn in submission order) */
    for (int k = 0; k < nlift && pf->npolys < pf->cap_polys; k++) {
        pf->polys[pf->npolys] = pf->polys[lift[k]];
        pf->polys[lift[k]].vertex_count = 0;
        pf->npolys++;
    }
}
void sprite_swap_frame(PvrFrame *pf) {
    if (s_nreq && recent(s_req_frame)) images(pf);
    s_nreq = 0;  /* requests are per frame */
    if (recent(s_icons_frame) || recent(s_bar[0].frame) || recent(s_bar[1].frame)) button_config(pf);
    static uint32_t tiles[MAX_TILES], digits[MAX_TILES], hidden[MAX_TILES];
    static float boxes[MAX_TILES][4];
    static int card[MAX_TILES];
    int nt = 0, nd = 0, nh = 0;
    uint32_t npolys = pf->npolys;
    for (uint32_t i = 0; i < npolys; i++) {
        float box[4], uv[4];
        uint32_t h = tile_hash(pf, &pf->polys[i], box, uv);
        if (!h) continue;
        int is_card = in(s_vmu_tiles, 12, h), is_empty = in(s_empty_tiles, 10, h);
        if ((is_card || is_empty) && nt < MAX_TILES) {
            tiles[nt] = i;
            card[nt] = is_card;
            memcpy(boxes[nt++], box, sizeof box);
        } else if (in(s_digit_tiles, 2, h) && nd < MAX_TILES) {
            digits[nd++] = i;
        }
    }
    if (!nt) return;

    /* tiles into slots: a slot is three tiles wide; slots stand side by side */
    Group g[MAX_GROUPS];
    int ng = 0;
    for (int done = 0; done < nt;) {
        int first = -1;  /* the leftmost tile not yet in a slot */
        for (int k = 0; k < nt; k++)
            if (tiles[k] != ~0u && (first < 0 || boxes[k][0] < boxes[first][0])) first = k;
        if (first < 0 || ng == MAX_GROUPS) break;
        float x0 = boxes[first][0], tw = boxes[first][2] - boxes[first][0];
        Group *gr = &g[ng++];
        gr->box[0] = gr->box[1] = 1e9f;
        gr->box[2] = gr->box[3] = -1e9f;
        gr->card = gr->tiles = 0;
        gr->poly = (int)tiles[first];
        for (int k = 0; k < nt; k++) {
            if (tiles[k] == ~0u || boxes[k][0] >= x0 + 2.5f * tw) continue;
            for (int c = 0; c < 2; c++) gr->box[c] = fminf(gr->box[c], boxes[k][c]);
            for (int c = 2; c < 4; c++) gr->box[c] = fmaxf(gr->box[c], boxes[k][c]);
            gr->card |= card[k];
            gr->tiles++;
            pf->polys[tiles[k]].vertex_count = 0;  /* hidden */
            hidden[nh++] = tiles[k];
            tiles[k] = ~0u;
            done++;
        }
    }

    /* the image: 64x64 at the tiles' scale (a tile is 16 texels) */
    int full = 1;
    for (int k = 0; k < ng; k++) full &= g[k].tiles >= 8;
    if (!full) {  /* not slots after all: put the tiles back */
        for (int k = 0; k < nh; k++) pf->polys[hidden[k]].vertex_count = 4;
        return;
    }
    int paired = ng == 8;  /* the four ports' slot pairs: one image per port */
    for (int k = 0; k < ng; k += paired ? 2 : 1) {
        const Group *a = &g[k], *b = paired ? &g[k + 1] : a;
        float scale = (a->box[2] - a->box[0]) / 48.0f;  /* a slot is 48 texels wide */
        float cx = (a->box[0] + b->box[2]) / 2, cy = (a->box[1] + a->box[3]) / 2, half = 32 * scale;
        add_image(pf, (uint32_t)a->poly, a->card || b->card ? IMG_CARD : IMG_EMPTY, cx - half, cy - half, cx + half,
                  cy + half);
    }
    if (paired)
        for (int k = 0; k < nd; k++) pf->polys[digits[k]].vertex_count = 0;
}
