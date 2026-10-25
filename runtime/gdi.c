/* Reads 2048-byte data sectors from the user's GDI dump. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gdi.h"
#include "runtime.h"

/* track files pass 2 GB, and Windows' long is 32-bit */
#ifdef _WIN32
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

/* A track line: "track lba type sector_size file offset". The compact images
 * the setup app writes add a 7th column, the number of sectors, and may give a
 * track several lines (stretches of it; what's between them reads as zeros). */
typedef struct {
    FILE *f;
    uint32_t num, lba, sectors, sector_size, ctrl;
    int64_t base;  /* byte offset of the stretch's first sector in its file (the GDI's 6th column) */
} Track;

static Track s_tracks[99];
static int s_ntracks;

int gdi_open(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/'), *bslash = strrchr(dir, '\\');
    if (bslash > slash) slash = bslash;  /* Windows paths */
    if (slash) slash[1] = 0; else dir[0] = 0;
    char line[1200];
    int n = 0;
    if (!fgets(line, sizeof line, f) || sscanf(line, "%d", &n) != 1 || n <= 0 || n > 99) { fclose(f); return 0; }
    while (s_ntracks < 99 && fgets(line, sizeof line, f)) {
        unsigned num, lba, ctrl, ssize;
        int used = 0;
        if (sscanf(line, " %u %u %u %u %n", &num, &lba, &ctrl, &ssize, &used) != 4 || !ssize) continue;
        /* the file name may be quoted and contain spaces */
        char name[512];
        const char *p = line + used;
        if (*p == '"') {
            const char *q = strchr(p + 1, '"');
            if (!q) continue;
            snprintf(name, sizeof name, "%.*s", (int)(q - p - 1), p + 1);
            p = q + 1;
        } else {
            size_t len = strcspn(p, " \t\r\n");
            snprintf(name, sizeof name, "%.*s", (int)len, p);
            p += len;
        }
        long long off = 0, count = -1;
        if (sscanf(p, " %lld %lld", &off, &count) < 1) continue;
        char full[1600];
        int absolute = name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':');
        snprintf(full, sizeof full, "%s%s", absolute ? "" : dir, name);  /* a written GDI may point at the user's files */
        Track *t = &s_tracks[s_ntracks++];
        t->f = fopen(full, "rb");
        if (!t->f) fatal("cannot open track %s", full);
        fseek64(t->f, 0, SEEK_END);
        t->num = num;
        t->sector_size = ssize;
        t->base = off > 0 ? off : 0;  /* e.g. a Redump BIN's pregap (sfa3port.py writes these) */
        t->sectors = count >= 0 ? (uint32_t)count : (uint32_t)((ftell64(t->f) - t->base) / ssize);
        t->lba = lba;
        t->ctrl = ctrl;
    }
    fclose(f);
    return s_ntracks;
}

/* Replacement and added files. A replacement is a file of the disc's root
 * directory read from a file of ours instead (an add-on's character, say); an
 * added file is one the disc doesn't have. The root directory is served from
 * memory, moved past the end of the disc (ROOT_LBA): the original records,
 * replaced ones pointing at their new data (sectors from VIRTUAL_LBA on, so a
 * file may be any size) and the added files' records, all sorted by name.
 * Everything that says where the root is gets changed as the game reads it:
 * the volume descriptor, both path tables (the game's file system finds
 * directories through them) and the ".." records of the subdirectories. */
#define VIRTUAL_LBA 0x00800000u
#define ROOT_LBA 0x007F0000u
#define OVERRIDE_MAX 64
typedef struct {
    char name[32];  /* as in the directory, without ";1" */
    FILE *f;
    uint32_t size, lba, sectors;
    int added, found;
} Override;
static Override s_over[OVERRIDE_MAX];
static int s_nover, s_over_ready;
static uint32_t s_next_virtual = VIRTUAL_LBA;
static uint8_t *s_root;  /* the root directory, as served */
static uint32_t s_root_sectors, s_pvd_lba, s_root_orig;
static uint32_t s_lpath, s_mpath;  /* the path tables (little- and big-endian) */
#define SUBDIR_MAX 32
static uint32_t s_subdir[SUBDIR_MAX];  /* the root's subdirectories (their ".." is the root) */
static int s_nsubdir;

static int read_real(uint32_t lba, uint8_t *out);

static void add_override(const char *name, const char *path, int added) {
    if (s_nover >= OVERRIDE_MAX) return;
    FILE *f = fopen(path, "rb");
    if (!f) {
        logf_("disc: file for %s missing: %s", name, path);
        return;
    }
    Override *o = &s_over[s_nover++];
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", name);
    o->f = f;
    o->added = added;
    fseek64(f, 0, SEEK_END);
    o->size = (uint32_t)ftell64(f);
    o->sectors = (o->size + 2047) / 2048;
    o->lba = s_next_virtual;
    s_next_virtual += (o->sectors + 15) & ~15u;
    s_over_ready = 0;
}

void gdi_override(const char *name, const char *path) { add_override(name, path, 0); }
void gdi_add_file(const char *name, const char *path) { add_override(name, path, 1); }

static int same_name(const char *a, const char *b, size_t n) {  /* case-insensitive */
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return 0;
    }
    return 1;
}

static void put32_both(uint8_t *p, uint32_t v) {  /* ISO9660: little- then big-endian */
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
    p[4] = v >> 24; p[5] = v >> 16; p[6] = v >> 8; p[7] = v;
}

/* the root directory as served: the disc's, records of replaced files
 * changed, then the added files' records */
static void build_root(void) {
    s_over_ready = 1;
    free(s_root);
    s_root = NULL;
    uint32_t hd = 0;
    for (int i = 0; i < s_ntracks; i++)
        if (s_tracks[i].lba >= 45000 && (!hd || s_tracks[i].lba < hd)) hd = s_tracks[i].lba;
    uint8_t sec[2048];
    if (!hd || !read_real(hd + 16, sec) || memcmp(sec + 1, "CD001", 5)) return;
    s_pvd_lba = hd + 16;
    uint32_t root, len, pt_size;
    memcpy(&root, sec + 156 + 2, 4);
    memcpy(&len, sec + 156 + 10, 4);
    memcpy(&pt_size, sec + 132, 4);
    memcpy(&s_lpath, sec + 140, 4);
    s_mpath = (uint32_t)sec[148] << 24 | (uint32_t)sec[149] << 16 | (uint32_t)sec[150] << 8 | sec[151];
    s_root_orig = root;
    /* the path table: records (name length, attribute length, u32 extent,
     * u16 parent, name, padding to even); directory 1 is the root */
    s_nsubdir = 0;
    uint8_t pt[2048];
    if (pt_size <= 2048 && read_real(s_lpath, pt)) {
        for (uint32_t off = 0, num = 1; off + 8 <= pt_size && pt[off]; num++) {
            uint32_t ext;
            memcpy(&ext, pt + off + 2, 4);
            uint16_t parent = (uint16_t)(pt[off + 6] | pt[off + 7] << 8);
            if (num > 1 && parent == 1 && s_nsubdir < SUBDIR_MAX) s_subdir[s_nsubdir++] = ext;
            off += 8 + pt[off] + (pt[off] & 1);
        }
    }
    uint32_t n = (len + 2047) / 2048, added = 0;
    for (int k = 0; k < s_nover; k++) added += s_over[k].added;
    uint32_t extra = added ? (added * 48 + 2047) / 2048 + 1 : 0;
    s_root_sectors = n + extra;
    s_root = calloc(s_root_sectors, 2048);
    for (uint32_t i = 0; i < n; i++) read_real(root + i, s_root + 2048 * i);
    for (uint32_t s = 0; s < n; s++) {
        uint8_t *d = s_root + 2048 * s;
        for (uint32_t off = 0; off < 2048 && d[off];) {
            uint8_t rlen = d[off], nlen = d[off + 32];
            const char *nm = (const char *)d + off + 33;
            if (nlen == 1 && (nm[0] == 0 || nm[0] == 1)) {  /* "." and ".." of the root: itself */
                put32_both(d + off + 2, ROOT_LBA);
                put32_both(d + off + 10, s_root_sectors * 2048);
            }
            for (int k = 0; k < s_nover; k++) {
                size_t l = strlen(s_over[k].name);
                if (!s_over[k].added && nlen >= l && same_name(nm, s_over[k].name, l) && (nlen == l || nm[l] == ';')) {
                    put32_both(d + off + 2, s_over[k].lba);
                    put32_both(d + off + 10, s_over[k].size);
                    s_over[k].found = 1;
                    logf_("disc: %s replaced (%u bytes)", s_over[k].name, s_over[k].size);
                }
            }
            off += rlen;
        }
    }
    /* the added files: all records re-sorted by name (the game's lookup
     * relies on the directory's order) and packed again */
    if (!added) return;
    typedef struct { char key[40]; uint8_t rec[64]; uint8_t len; } Rec;
    uint32_t cap = added, nrec = 0;
    for (uint32_t s = 0; s < n; s++)
        for (uint32_t off = 0; off < 2048 && s_root[2048 * s + off]; off += s_root[2048 * s + off]) cap++;
    Rec *recs = calloc(cap, sizeof *recs);
    for (uint32_t s = 0; s < n && nrec < cap; s++) {
        uint8_t *d = s_root + 2048 * s;
        for (uint32_t off = 0; off < 2048 && d[off] && nrec < cap;) {
            uint8_t rlen = d[off], nlen = d[off + 32];
            if (rlen <= 64) {
                Rec *r = &recs[nrec++];
                memcpy(r->rec, d + off, rlen);
                r->len = rlen;
                for (int i = 0; i < nlen && i < 39; i++) {
                    char c = (char)d[off + 33 + i];
                    r->key[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
                }
            }
            off += rlen;
        }
    }
    for (int k = 0; k < s_nover && nrec < cap; k++) {
        Override *o = &s_over[k];
        if (!o->added) continue;
        char id[40];
        int nl = snprintf(id, sizeof id, "%s;1", o->name);
        Rec *r = &recs[nrec++];
        uint8_t rl = (uint8_t)((33 + nl + 1) & ~1);
        r->len = rl;
        r->rec[0] = rl;
        put32_both(r->rec + 2, o->lba);
        put32_both(r->rec + 10, o->size);
        r->rec[18] = 100; r->rec[19] = 1; r->rec[20] = 1;
        r->rec[28] = 1; r->rec[31] = 1;
        r->rec[32] = (uint8_t)nl;
        memcpy(r->rec + 33, id, (size_t)nl);
        for (int i = 0; i < nl && i < 39; i++) r->key[i] = id[i] >= 'a' && id[i] <= 'z' ? (char)(id[i] - 32) : id[i];
        o->found = 1;
        logf_("disc: %s added (%u bytes)", o->name, o->size);
    }
    /* "." and ".." (one-byte names 0 and 1) stay first; the rest by name */
    for (uint32_t i = 1; i < nrec; i++)
        for (uint32_t j = i; j > 0; j--) {
            Rec *x = &recs[j - 1], *y = &recs[j];
            int xs = x->rec[32] == 1 && x->rec[33] <= 1, ys = y->rec[32] == 1 && y->rec[33] <= 1;
            if (xs || (!ys && strcmp(x->key, y->key) <= 0)) break;
            Rec t = *x; *x = *y; *y = t;
        }
    /* records don't cross sectors: lay them out, then size the directory */
    uint32_t pos = 0, *at = calloc(nrec ? nrec : 1, sizeof *at);
    for (uint32_t i = 0; i < nrec; i++) {
        if (pos % 2048 + recs[i].len > 2048) pos = (pos / 2048 + 1) * 2048;
        at[i] = pos;
        pos += recs[i].len;
    }
    s_root_sectors = (pos + 2047) / 2048;
    free(s_root);
    s_root = calloc(s_root_sectors, 2048);
    for (uint32_t i = 0; i < nrec; i++) {
        uint8_t *r = recs[i].rec;
        if (r[32] == 1 && r[33] <= 1) {  /* "." and "..": the directory's new place and size */
            put32_both(r + 2, ROOT_LBA);
            put32_both(r + 10, s_root_sectors * 2048);
        }
        memcpy(s_root + at[i], r, recs[i].len);
    }
    logf_("disc: root directory rebuilt, %u records in %u sectors", nrec, s_root_sectors);
    free(at);
    free(recs);
}

/* GD-ROM LBAs are offset by 150 relative to the GDI's (FAD = LBA + 150). */
int gdi_read_sector(uint32_t fad, uint8_t *out) {
    uint32_t lba = fad - 150;
    if (s_nover) {
        if (!s_over_ready) build_root();
        if (s_root && lba - ROOT_LBA < s_root_sectors) {
            memcpy(out, s_root + 2048 * (lba - ROOT_LBA), 2048);
            return 1;
        }
        for (int k = 0; k < s_nover; k++) {
            Override *o = &s_over[k];
            if (o->found && lba >= o->lba && lba < o->lba + o->sectors) {
                if (lba == o->lba) logf_("disc: reading %s", o->name);
                memset(out, 0, 2048);
                fseek64(o->f, (int64_t)(lba - o->lba) * 2048, SEEK_SET);
                fread(out, 1, 2048, o->f);
                return 1;
            }
        }
        int r = read_real(lba, out);
        if (!s_root) return r;
        if (lba == s_pvd_lba) {  /* the root directory: ours */
            put32_both(out + 156 + 2, ROOT_LBA);
            put32_both(out + 156 + 10, s_root_sectors * 2048);
        } else if (lba == s_lpath) {  /* the root is the path table's first record */
            out[2] = ROOT_LBA & 0xFF; out[3] = (ROOT_LBA >> 8) & 0xFF;
            out[4] = (ROOT_LBA >> 16) & 0xFF; out[5] = ROOT_LBA >> 24;
        } else if (lba == s_mpath) {
            out[2] = ROOT_LBA >> 24; out[3] = (ROOT_LBA >> 16) & 0xFF;
            out[4] = (ROOT_LBA >> 8) & 0xFF; out[5] = ROOT_LBA & 0xFF;
        } else {
            for (int k = 0; k < s_nsubdir; k++) {
                if (lba != s_subdir[k]) continue;
                for (uint32_t off = 0; off < 2048 && out[off]; off += out[off]) {
                    uint32_t ext;
                    memcpy(&ext, out + off + 2, 4);
                    if (out[off + 32] == 1 && out[off + 33] == 1 && ext == s_root_orig) {  /* ".." */
                        put32_both(out + off + 2, ROOT_LBA);
                        put32_both(out + off + 10, s_root_sectors * 2048);
                    }
                }
            }
        }
        return r;
    }
    return read_real(lba, out);
}

static int read_real(uint32_t lba, uint8_t *out) {
    memset(out, 0, 2048);
    for (int i = s_ntracks - 1; i >= 0; i--) {
        Track *t = &s_tracks[i];
        if (lba >= t->lba && lba < t->lba + t->sectors) {
            int64_t pos = t->base + (int64_t)(lba - t->lba) * t->sector_size + (t->sector_size == 2352 ? 16 : 0);
            fseek64(t->f, pos, SEEK_SET);
            fread(out, 1, 2048, t->f);  /* a short read past the file's end: zeros */
            return 1;
        }
    }
    static int misses;
    if (misses++ < 10) logf_("disc: read outside the image at LBA %u", lba);
    return 0;
}

/* Build a GD-ROM TOC (area 0 = single density, 1 = high density). */
void gdi_toc(int area, uint32_t *toc) {
    /* each track: its first sector and its end, over all its stretches */
    uint32_t lba[99], end[99], ctrl[99];
    int ntracks = 0;
    for (int i = 0; i < s_ntracks; i++) {
        const Track *t = &s_tracks[i];
        int k = (int)t->num - 1;
        if (k < 0 || k >= 99) continue;
        if (k >= ntracks) {
            for (int j = ntracks; j <= k; j++) lba[j] = end[j] = ctrl[j] = 0;
            ntracks = k + 1;
            lba[k] = t->lba;
        }
        if (t->lba < lba[k] || !end[k]) lba[k] = t->lba;
        if (t->lba + t->sectors > end[k]) end[k] = t->lba + t->sectors;
        ctrl[k] = t->ctrl;
    }
    for (int i = 0; i < 102; i++) toc[i] = 0xFFFFFFFFu;
    if (!ntracks) return;
    int first = area ? 2 : 0, last = area ? ntracks - 1 : (ntracks > 1 ? 1 : 0);
    for (int i = first; i <= last; i++) {
        uint32_t c = ctrl[i] == 4 ? 0x41 : 0x01;
        toc[i] = c << 24 | (lba[i] + 150);  /* ctrl:4 adr:4 in the top byte, FAD in the low 24 bits */
    }
    uint32_t fctrl = ctrl[first] == 4 ? 0x41 : 0x01;
    uint32_t lctrl = ctrl[last] == 4 ? 0x41 : 0x01;
    toc[99] = fctrl << 24 | (uint32_t)(first + 1) << 16;
    toc[100] = lctrl << 24 | (uint32_t)(last + 1) << 16;
    toc[101] = lctrl << 24 | (end[last] + 150);
}
