/* Tile Accelerator: decodes the parameter stream the game writes to the TA
 * FIFO into a PvrFrame (vertex + strip lists) for the renderer. */
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "pvr.h"
#include "runtime.h"
#include "widescreen.h"

/* PCW fields */
#define PT_END 0
#define PT_USERCLIP 1
#define PT_OBJLIST 2
#define PT_POLY 4
#define PT_SPRITE 5
#define PT_VERTEX 7

/* Frames are keyed by the ISP/param base they were built at, so a frame
 * built while the previous one renders is matched on STARTRENDER. */
#define MAX_FRAMES 4
static PvrFrame s_frames[MAX_FRAMES];
static uint32_t s_frame_key[MAX_FRAMES];
static int s_cur = -1;

static uint32_t s_buf[16];
static unsigned s_nbuf;

static int s_list = -1;        /* current open list or -1 */
static int s_vtype;            /* vertex parameter type for current global */
static int s_sprite;
static uint32_t s_pcw, s_isp, s_tsp, s_tcw;
static float s_face[4], s_face_off[4];  /* ARGB intensity colours */
static uint32_t s_sprite_base, s_sprite_off;
static int16_t s_uclip[4];
static PvrPoly *s_strip;       /* strip being built */

static PvrFrame *cur(void) { return &s_frames[s_cur]; }

void ta_power_reset(void) {
    memset(s_frame_key, 0, sizeof s_frame_key);
    s_cur = -1;
    s_nbuf = 0;
    s_list = -1;
    s_strip = NULL;
}

void ta_reset(void) {
    s_nbuf = 0;
    s_list = -1;
    s_strip = NULL;
}

void ta_list_init(void) {
    uint32_t key = pvr_reg(PVR_TA_ISP_BASE);
    int slot = -1;
    for (int i = 0; i < MAX_FRAMES; i++)
        if (s_frame_key[i] == key && s_frames[i].verts) slot = i;
    if (slot < 0) {
        static int rr;
        slot = rr++ % MAX_FRAMES;
    }
    s_cur = slot;
    s_frame_key[slot] = key;
    PvrFrame *f = cur();
    f->nverts = f->npolys = 0;
    f->wide_list = widescreen_list_begin();  /* the game sends the frame it laid out last (widescreen.c) */
    ta_reset();
}

PvrFrame *ta_frame_for(uint32_t param_base) {
    for (int i = 0; i < MAX_FRAMES; i++)
        if (s_frame_key[i] == param_base && s_frames[i].verts) return &s_frames[i];
    return s_cur >= 0 ? cur() : NULL;
}

static PvrVertex *new_vertex(void) {
    PvrFrame *f = cur();
    if (f->nverts == f->cap_verts) {
        f->cap_verts = f->cap_verts ? f->cap_verts * 2 : 16384;
        f->verts = realloc(f->verts, f->cap_verts * sizeof(PvrVertex));
    }
    return &f->verts[f->nverts++];
}

static PvrPoly *new_poly(void) {
    PvrFrame *f = cur();
    if (f->npolys == f->cap_polys) {
        f->cap_polys = f->cap_polys ? f->cap_polys * 2 : 4096;
        f->polys = realloc(f->polys, f->cap_polys * sizeof(PvrPoly));
    }
    PvrPoly *p = &f->polys[f->npolys++];
    memset(p, 0, sizeof *p);
    p->pcw = s_pcw;
    /* ISP bits 25..20 come from the PCW obj-control bits */
    p->isp = (s_isp & ~0x03C00000u) | ((s_pcw & 8) ? 1u << 25 : 0) | ((s_pcw & 4) ? 1u << 24 : 0) |
             ((s_pcw & 2) ? 1u << 23 : 0) | ((s_pcw & 1) ? 1u << 22 : 0);
    p->tsp = s_tsp;
    p->tcw = s_tcw;
    p->list = (uint32_t)s_list;
    p->first_vertex = f->nverts;
    memcpy(p->clip, s_uclip, sizeof s_uclip);
    p->tile_clip = (s_pcw >> 16) & 3;
    return p;
}

static inline float fw(uint32_t w) { fpreg_t r; r.u = w; return r.f; }

static uint32_t pack_argb(float a, float r, float g_, float b) {
    #define C8(x) ((uint32_t)((x) <= 0 ? 0 : (x) >= 1 ? 255 : (int)((x) * 255.0f + 0.5f)))
    return C8(a) << 24 | C8(r) << 16 | C8(g_) << 8 | C8(b);
    #undef C8
}

static uint32_t intensity(const float *face, float i) {
    return pack_argb(face[0], face[1] * i, face[2] * i, face[3] * i);
}

/* Number of 32-bit words for a parameter starting with this PCW. */
static unsigned param_words(uint32_t pcw) {
    switch (pcw >> 29) {
    case PT_POLY: {
        if (s_list == LIST_OPAQUE_MOD || s_list == LIST_TRANS_MOD) return 8;
        int list = s_list >= 0 ? s_list : (int)((pcw >> 24) & 7);
        if (list == LIST_OPAQUE_MOD || list == LIST_TRANS_MOD) return 8;
        int col = (pcw >> 4) & 3, vol = (pcw >> 6) & 1, off = (pcw >> 2) & 1;
        if (col == 2 && (vol || off)) return 16;
        return 8;
    }
    case PT_VERTEX:
        if (s_list == LIST_OPAQUE_MOD || s_list == LIST_TRANS_MOD) return 16;
        if (s_sprite) return 16;
        switch (s_vtype) {
        case 5: case 6: case 11: case 12: case 13: case 14: return 16;
        }
        return 8;
    default:
        return 8;
    }
}

static int vertex_type(uint32_t pcw) {
    int tex = (pcw >> 3) & 1, col = (pcw >> 4) & 3, vol = (pcw >> 6) & 1, uv16 = pcw & 1;
    if (!vol) {
        if (!tex) return col == 0 ? 0 : col == 1 ? 1 : 2;
        if (col == 0) return uv16 ? 4 : 3;
        if (col == 1) return uv16 ? 6 : 5;
        return uv16 ? 8 : 7;
    }
    if (!tex) return col == 0 ? 9 : 10;
    if (col == 0) return uv16 ? 12 : 11;
    return uv16 ? 14 : 13;
}

static void global_param(const uint32_t *w) {
    uint32_t pcw = w[0];
    if (s_list < 0) s_list = (pcw >> 24) & 7;
    s_strip = NULL;
    s_pcw = pcw;
    s_isp = w[1];
    s_tsp = w[2];
    s_tcw = w[3];
    s_sprite = (pcw >> 29) == PT_SPRITE;
    if (s_sprite) {
        s_sprite_base = w[4];
        s_sprite_off = w[5];
        return;
    }
    if (s_list == LIST_OPAQUE_MOD || s_list == LIST_TRANS_MOD) return;
    s_vtype = vertex_type(pcw);
    int col = (pcw >> 4) & 3, vol = (pcw >> 6) & 1, off = (pcw >> 2) & 1;
    if (col == 2) {
        if (vol || off) {
            for (int i = 0; i < 4; i++) s_face[i] = fw(w[8 + i]), s_face_off[i] = fw(w[12 + i]);
        } else {
            for (int i = 0; i < 4; i++) s_face[i] = fw(w[4 + i]);
            s_face_off[0] = s_face_off[1] = s_face_off[2] = s_face_off[3] = 0;
        }
    }
}

static void sprite_vertex(const uint32_t *w) {
    PvrPoly *p = new_poly();
    float ax = fw(w[1]), ay = fw(w[2]), az = fw(w[3]);
    float bx = fw(w[4]), by = fw(w[5]), bz = fw(w[6]);
    float cx = fw(w[7]), cy = fw(w[8]), cz = fw(w[9]);
    float dx = fw(w[10]), dy = fw(w[11]);
    /* D's depth from the plane through A, B, C */
    float ux = bx - ax, uy = by - ay, uz = bz - az, vx = cx - ax, vy = cy - ay, vz = cz - az;
    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    float dz = nz != 0 ? az - (nx * (dx - ax) + ny * (dy - ay)) / nz : az;
    float au = 0, av = 0, bu = 0, bv = 0, cu = 0, cv = 0;
    if (s_pcw & 8) {
        au = fw(w[13] & 0xFFFF0000u); av = fw(w[13] << 16);
        bu = fw(w[14] & 0xFFFF0000u); bv = fw(w[14] << 16);
        cu = fw(w[15] & 0xFFFF0000u); cv = fw(w[15] << 16);
    }
    /* D's UV completes the parallelogram */
    float du = au + cu - bu, dv = av + cv - bv;
    /* strip order A, B, D, C */
    const float xs[4] = {ax, bx, dx, cx}, ys[4] = {ay, by, dy, cy}, zs[4] = {az, bz, dz, cz};
    const float us[4] = {au, bu, du, cu}, vs[4] = {av, bv, dv, cv};
    for (int i = 0; i < 4; i++) {
        PvrVertex *v = new_vertex();
        v->x = xs[i]; v->y = ys[i]; v->z = zs[i];
        v->u = us[i]; v->v = vs[i];
        v->base = s_sprite_base;
        v->offset = s_sprite_off;
    }
    p->vertex_count = 4;
}

static void poly_vertex(const uint32_t *w) {
    if (!s_strip) s_strip = new_poly();
    PvrVertex *v = new_vertex();
    v->x = fw(w[1]);
    v->y = fw(w[2]);
    v->z = fw(w[3]);
    v->u = v->v = 0;
    v->offset = 0;
    switch (s_vtype) {
    case 0: v->base = w[6]; break;
    case 1: v->base = pack_argb(fw(w[4]), fw(w[5]), fw(w[6]), fw(w[7])); break;
    case 2: v->base = intensity(s_face, fw(w[6])); break;
    case 3: v->u = fw(w[4]); v->v = fw(w[5]); v->base = w[6]; v->offset = w[7]; break;
    case 4:
        v->u = fw(w[4] & 0xFFFF0000u); v->v = fw(w[4] << 16);
        v->base = w[6]; v->offset = w[7];
        break;
    case 5:
        v->u = fw(w[4]); v->v = fw(w[5]);
        v->base = pack_argb(fw(w[8]), fw(w[9]), fw(w[10]), fw(w[11]));
        v->offset = pack_argb(fw(w[12]), fw(w[13]), fw(w[14]), fw(w[15]));
        break;
    case 6:
        v->u = fw(w[4] & 0xFFFF0000u); v->v = fw(w[4] << 16);
        v->base = pack_argb(fw(w[8]), fw(w[9]), fw(w[10]), fw(w[11]));
        v->offset = pack_argb(fw(w[12]), fw(w[13]), fw(w[14]), fw(w[15]));
        break;
    case 7:
        v->u = fw(w[4]); v->v = fw(w[5]);
        v->base = intensity(s_face, fw(w[6]));
        v->offset = intensity(s_face_off, fw(w[7]));
        break;
    case 8:
        v->u = fw(w[4] & 0xFFFF0000u); v->v = fw(w[4] << 16);
        v->base = intensity(s_face, fw(w[6]));
        v->offset = intensity(s_face_off, fw(w[7]));
        break;
    /* two-volume types: use volume 0 */
    case 9: v->base = w[4]; break;
    case 10: v->base = intensity(s_face, fw(w[4])); break;
    case 11: v->u = fw(w[4]); v->v = fw(w[5]); v->base = w[6]; v->offset = w[7]; break;
    case 12:
        v->u = fw(w[4] & 0xFFFF0000u); v->v = fw(w[4] << 16);
        v->base = w[6]; v->offset = w[7];
        break;
    case 13:
        v->u = fw(w[4]); v->v = fw(w[5]);
        v->base = intensity(s_face, fw(w[6])); v->offset = intensity(s_face_off, fw(w[7]));
        break;
    case 14:
        v->u = fw(w[4] & 0xFFFF0000u); v->v = fw(w[4] << 16);
        v->base = intensity(s_face, fw(w[6])); v->offset = intensity(s_face_off, fw(w[7]));
        break;
    }
    s_strip->vertex_count++;
    if (w[0] & (1u << 28)) s_strip = NULL;  /* end of strip */
}

static void end_of_list(void) {
    static const uint32_t bits[LIST_COUNT] = {1u << 7, 1u << 8, 1u << 9, 1u << 10, 1u << 21};
    if (getenv("SFA3_RENDER_LOG")) {  /* debug: which lists end */
        static int n;
        extern uint64_t g_vblank_count;
        if ((long long)g_vblank_count >= atoll(getenv("SFA3_RENDER_LOG")) && n++ < 400)
            logf_("ta: end of list %d at frame %llu", s_list, (unsigned long long)g_vblank_count);
    }
    if (s_list >= 0 && s_list < LIST_COUNT) holly_raise(0, bits[s_list]);
    s_list = -1;
    s_strip = NULL;
}

static void process(const uint32_t *w) {
    uint32_t pcw = w[0];
    static int logged;
    if (logged < 200) {
        logged++;
        logf_("ta: %08X %08X %08X %08X %08X %08X %08X %08X list=%d", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], s_list);
    }
    switch (pcw >> 29) {
    case PT_END: end_of_list(); break;
    case PT_USERCLIP:
        s_uclip[0] = (int16_t)w[4]; s_uclip[1] = (int16_t)w[5];
        s_uclip[2] = (int16_t)w[6]; s_uclip[3] = (int16_t)w[7];
        break;
    case PT_OBJLIST: break;
    case PT_POLY:
    case PT_SPRITE: global_param(w); break;
    case PT_VERTEX:
        if (s_list == LIST_OPAQUE_MOD || s_list == LIST_TRANS_MOD) break;  /* modifier volumes: TODO */
        if (s_sprite) sprite_vertex(w); else poly_vertex(w);
        break;
    default: break;
    }
}

void ta_write(const uint32_t *data, unsigned words) {
    if (s_cur < 0) ta_list_init();
    g_stats.ta_words += words;
    for (unsigned i = 0; i < words; i++) {
        s_buf[s_nbuf++] = data[i];
        if (s_nbuf == 8 || s_nbuf == 16) {
            unsigned need = param_words(s_buf[0]);
            if (s_nbuf >= need) {
                process(s_buf);
                s_nbuf = 0;
            }
        }
    }
}

void ta_write_word(uint32_t addr, uint32_t v) {
    (void)addr;
    ta_write(&v, 1);
}
