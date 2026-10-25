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

/* Replacement files: a file of the disc's root directory read from a file of
 * ours instead (an add-on's character, say). Its directory record is changed
 * as the game reads it, to point at sectors past the end of the disc (from
 * VIRTUAL_LBA) and to give the new size, so the replacement may be any size. */
#define VIRTUAL_LBA 0x00800000u
#define OVERRIDE_MAX 64
typedef struct {
    char name[32];  /* as in the directory, without ";1" */
    FILE *f;
    uint32_t size, lba, sectors;
    uint32_t dir_lba, dir_off;  /* where its record is */
} Override;
static Override s_over[OVERRIDE_MAX];
static int s_nover, s_over_ready;
static uint32_t s_next_virtual = VIRTUAL_LBA;

static int read_real(uint32_t lba, uint8_t *out);

void gdi_override(const char *name, const char *path) {
    if (s_nover >= OVERRIDE_MAX) return;
    FILE *f = fopen(path, "rb");
    if (!f) {
        logf_("disc: replacement for %s missing: %s", name, path);
        return;
    }
    Override *o = &s_over[s_nover++];
    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", name);
    o->f = f;
    fseek64(f, 0, SEEK_END);
    o->size = (uint32_t)ftell64(f);
    o->sectors = (o->size + 2047) / 2048;
    o->lba = s_next_virtual;
    s_next_virtual += (o->sectors + 15) & ~15u;
    s_over_ready = 0;
}

/* find each replaced file's record in the root directory (ISO9660 in the
 * high-density area: its volume descriptor 16 sectors in) */
static void find_records(void) {
    s_over_ready = 1;
    uint32_t hd = 0;
    for (int i = 0; i < s_ntracks; i++)
        if (s_tracks[i].lba >= 45000 && (!hd || s_tracks[i].lba < hd)) hd = s_tracks[i].lba;
    uint8_t sec[2048];
    if (!hd || !read_real(hd + 16, sec) || memcmp(sec + 1, "CD001", 5)) return;
    uint32_t root, len;
    memcpy(&root, sec + 156 + 2, 4);
    memcpy(&len, sec + 156 + 10, 4);
    for (uint32_t s = 0; s < (len + 2047) / 2048; s++) {
        if (!read_real(root + s, sec)) return;
        for (uint32_t off = 0; off < 2048 && sec[off];) {
            uint8_t rlen = sec[off], nlen = sec[off + 32];
            const char *nm = (const char *)sec + off + 33;
            for (int k = 0; k < s_nover; k++) {
                size_t l = strlen(s_over[k].name);
                if (nlen >= l && !memcmp(nm, s_over[k].name, l) && (nlen == l || nm[l] == ';')) {
                    s_over[k].dir_lba = root + s;
                    s_over[k].dir_off = off;
                    logf_("disc: %s replaced (%u bytes)", s_over[k].name, s_over[k].size);
                }
            }
            off += rlen;
        }
    }
}

static void put32_both(uint8_t *p, uint32_t v) {  /* ISO9660: little- then big-endian */
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
    p[4] = v >> 24; p[5] = v >> 16; p[6] = v >> 8; p[7] = v;
}

/* GD-ROM LBAs are offset by 150 relative to the GDI's (FAD = LBA + 150). */
int gdi_read_sector(uint32_t fad, uint8_t *out) {
    uint32_t lba = fad - 150;
    if (s_nover) {
        if (!s_over_ready) find_records();
        for (int k = 0; k < s_nover; k++) {
            Override *o = &s_over[k];
            if (o->dir_lba && lba >= o->lba && lba < o->lba + o->sectors) {
                memset(out, 0, 2048);
                fseek64(o->f, (int64_t)(lba - o->lba) * 2048, SEEK_SET);
                fread(out, 1, 2048, o->f);
                return 1;
            }
        }
        int r = read_real(lba, out);
        for (int k = 0; k < s_nover; k++)
            if (s_over[k].dir_lba == lba) {
                put32_both(out + s_over[k].dir_off + 2, s_over[k].lba);
                put32_both(out + s_over[k].dir_off + 10, s_over[k].size);
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
