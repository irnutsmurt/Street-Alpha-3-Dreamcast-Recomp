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

typedef struct {
    FILE *f;
    uint32_t lba, sectors, sector_size, ctrl;
    int64_t base;  /* byte offset of the track's first sector in its file (the GDI's last column) */
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
    int n = 0;
    if (fscanf(f, "%d", &n) != 1 || n <= 0 || n > 99) { fclose(f); return 0; }
    for (int i = 0; i < n; i++) {
        int num, ctrl, ssize;
        unsigned lba;
        char name[512];
        long long off;
        if (fscanf(f, "%d %u %d %d", &num, &lba, &ctrl, &ssize) != 4) break;
        /* filename may be quoted and contain spaces */
        int c;
        while ((c = fgetc(f)) == ' ' || c == '\t') {}
        if (c == '"') {
            if (fscanf(f, "%511[^\"]\"", name) != 1) break;
        } else {
            ungetc(c, f);
            if (fscanf(f, "%511s", name) != 1) break;
        }
        if (fscanf(f, "%lld", &off) != 1) break;
        char full[1600];
        int absolute = name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':');
        snprintf(full, sizeof full, "%s%s", absolute ? "" : dir, name);  /* a written GDI may point at the user's files */
        Track *t = &s_tracks[s_ntracks++];
        t->f = fopen(full, "rb");
        if (!t->f) fatal("cannot open track %s", full);
        fseek64(t->f, 0, SEEK_END);
        t->sector_size = (uint32_t)ssize;
        t->base = off > 0 ? off : 0;  /* e.g. a Redump BIN's pregap (sfa3port.py writes these) */
        t->sectors = (uint32_t)((ftell64(t->f) - t->base) / ssize);
        t->lba = lba;
        t->ctrl = (uint32_t)ctrl;
    }
    fclose(f);
    return s_ntracks;
}

/* GD-ROM LBAs are offset by 150 relative to the GDI's (FAD = LBA + 150). */
int gdi_read_sector(uint32_t fad, uint8_t *out) {
    uint32_t lba = fad - 150;
    for (int i = s_ntracks - 1; i >= 0; i--) {
        Track *t = &s_tracks[i];
        if (lba >= t->lba && lba < t->lba + t->sectors) {
            int64_t pos = t->base + (int64_t)(lba - t->lba) * t->sector_size + (t->sector_size == 2352 ? 16 : 0);
            fseek64(t->f, pos, SEEK_SET);
            return fread(out, 1, 2048, t->f) == 2048;
        }
    }
    memset(out, 0, 2048);
    return 0;
}

/* Build a GD-ROM TOC (area 0 = single density, 1 = high density). */
void gdi_toc(int area, uint32_t *toc) {
    for (int i = 0; i < 102; i++) toc[i] = 0xFFFFFFFFu;
    int first = area ? 2 : 0, last = area ? s_ntracks - 1 : (s_ntracks > 1 ? 1 : 0);
    for (int i = first; i <= last; i++) {
        Track *t = &s_tracks[i];
        uint32_t fad = t->lba + 150;
        uint32_t ctrl = t->ctrl == 4 ? 0x41 : 0x01;
        toc[i] = ctrl << 24 | fad;  /* ctrl:4 adr:4 in the top byte, FAD in the low 24 bits */
    }
    Track *lt = &s_tracks[last];
    uint32_t fctrl = s_tracks[first].ctrl == 4 ? 0x41 : 0x01;
    uint32_t lctrl = lt->ctrl == 4 ? 0x41 : 0x01;
    toc[99] = fctrl << 24 | (uint32_t)(first + 1) << 16;
    toc[100] = lctrl << 24 | (uint32_t)(last + 1) << 16;
    toc[101] = lctrl << 24 | (lt->lba + lt->sectors + 150);
}
