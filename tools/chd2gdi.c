/* chd2gdi: unpack a Dreamcast GD-ROM CHD into a GDI (disc.gdi + track files).
 *
 *     chd2gdi <image.chd> <out_dir>
 *
 * Built with libchdr (tools/third_party/libchdr, BSD) by the setup pipeline
 * and the converter app. Prints "progress <done>/<total>" lines (frames) and
 * exits non-zero with a message on failure.
 *
 * Each track is written as 2352-byte sectors, audio little-endian (CHD
 * stores it big-endian, except legacy GDROMLE images). In the CHD each track
 * takes FRAMES frames rounded up to a multiple of 4 (CD_TRACK_PADDING).
 *   - GD-ROM metadata (chdman createcd from a GDI): FRAMES includes PAD
 *     frames of filler after the track's data, so the tracks sit at their
 *     disc LBAs: each starts at the previous tracks' FRAMES summed (0, 600,
 *     45000 for SFA3).
 *   - CD metadata: a pregap stored in the CHD (PGTYPE V...) is skipped; the
 *     low-density area from 0, the high-density area from 45000, each track
 *     after the previous one plus its pregap (two seconds at least between
 *     data and audio). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#include "libchdr/cdrom.h"
#include "libchdr/chd.h"

#define HD_AREA_LBA 45000
#define MAX_TRACKS 99

typedef struct {
    int number, frames, pad, pregap, pregap_stored, audio;
    char type[32];
} Track;

static int fail(const char *msg, const char *detail) {
    fprintf(stderr, "chd2gdi: %s%s%s\n", msg, detail ? ": " : "", detail ? detail : "");
    printf("error %s\n", msg);
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) return fail("usage: chd2gdi <image.chd> <out_dir>", NULL);
    chd_file *chd;
    chd_error err = chd_open(argv[1], CHD_OPEN_READ, NULL, &chd);
    if (err != CHDERR_NONE) return fail("cannot open the CHD", chd_error_string(err));
    const chd_header *h = chd_get_header(chd);
    if (h->unitbytes != CD_FRAME_SIZE) return fail("not a CD / GD-ROM CHD", NULL);
    uint32_t frames_per_hunk = h->hunkbytes / CD_FRAME_SIZE;

    /* the track list: GD-ROM metadata (or CD metadata, for a GD-ROM stored as a CD) */
    Track tracks[MAX_TRACKS];
    int n = 0, legacy_le = 0, gdrom = 0;
    char meta[512];
    for (int i = 0; i < MAX_TRACKS; i++) {
        uint32_t len, tag;
        uint8_t flags;
        Track *t = &tracks[n];
        memset(t, 0, sizeof *t);
        char subtype[32], pgtype[32] = "", pgsub[32];
        int postgap = 0;
        if (chd_get_metadata(chd, GDROM_TRACK_METADATA_TAG, (uint32_t)i, meta, sizeof meta - 1, &len, &tag, &flags) ==
            CHDERR_NONE) {
            meta[len] = 0;
            gdrom = 1;
            if (sscanf(meta, GDROM_TRACK_METADATA_FORMAT, &t->number, t->type, subtype, &t->frames, &t->pad,
                       &t->pregap, pgtype, pgsub, &postgap) < 5)
                return fail("unreadable track metadata", meta);
        } else if (chd_get_metadata(chd, GDROM_OLD_METADATA_TAG, (uint32_t)i, meta, sizeof meta - 1, &len, &tag,
                                    &flags) == CHDERR_NONE) {
            meta[len] = 0;
            legacy_le = gdrom = 1;
            if (sscanf(meta, GDROM_TRACK_METADATA_FORMAT, &t->number, t->type, subtype, &t->frames, &t->pad,
                       &t->pregap, pgtype, pgsub, &postgap) < 5)
                return fail("unreadable track metadata", meta);
        } else if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, (uint32_t)i, meta, sizeof meta - 1, &len, &tag,
                                    &flags) == CHDERR_NONE) {
            meta[len] = 0;
            if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &t->number, t->type, subtype, &t->frames, &t->pregap,
                       pgtype, pgsub, &postgap) < 4)
                return fail("unreadable track metadata", meta);
            t->pad = 0;
        } else {
            break;
        }
        t->audio = !strcmp(t->type, "AUDIO");
        t->pregap_stored = pgtype[0] == 'V';
        if (!t->audio && strcmp(t->type, "MODE1_RAW") && strcmp(t->type, "MODE1") && strcmp(t->type, "MODE2_RAW"))
            return fail("unsupported track type", t->type);
        n++;
    }
    if (n < 3) return fail("not a GD-ROM image (fewer than 3 tracks)", NULL);

    uint64_t total = 0;
    for (int i = 0; i < n; i++) total += (uint64_t)tracks[i].frames;
    uint8_t *hunk = malloc(h->hunkbytes);
    if (!hunk) return fail("out of memory", NULL);
    char path[4096];
    make_dir(argv[2]);  /* may exist already */
    snprintf(path, sizeof path, "%s/disc.gdi", argv[2]);
    FILE *gdi = fopen(path, "w");
    if (!gdi) return fail("cannot write", path);
    fprintf(gdi, "%d\n", n);

    uint64_t phys = 0, done = 0, last_report = 0;  /* phys: the CHD's frame index */
    uint32_t cached = 0xFFFFFFFFu;
    int lba = 0, prev_end = 0, logical = 0;
    for (int i = 0; i < n; i++) {
        Track *t = &tracks[i];
        int skip = !gdrom && t->pregap_stored ? t->pregap : 0;
        int data = t->frames - t->pad;  /* frames holding the track (the rest is filler) */
        int sectors = data - skip;
        uint64_t track_phys = phys;
        /* where the track starts */
        if (gdrom) {
            lba = logical;
        } else if (i == 2) {
            lba = HD_AREA_LBA;
        } else if (i > 0) {
            int gap = t->pregap;
            if (t->audio != tracks[i - 1].audio && gap < 150) gap = 150;
            lba = prev_end + gap;
        }
        char name[32];
        snprintf(name, sizeof name, "track%02d.%s", i + 1, t->audio ? "raw" : "bin");
        snprintf(path, sizeof path, "%s/%s", argv[2], name);
        FILE *out = fopen(path, "wb");
        if (!out) return fail("cannot write", path);
        for (int f = 0; f < data; f++, phys++, done++) {
            uint32_t hn = (uint32_t)(phys / frames_per_hunk);
            if (hn != cached) {
                err = chd_read(chd, hn, hunk);
                if (err != CHDERR_NONE) return fail("read error (damaged CHD?)", chd_error_string(err));
                cached = hn;
            }
            if (f < skip) continue;
            uint8_t sector[CD_MAX_SECTOR_DATA];
            memcpy(sector, hunk + (phys % frames_per_hunk) * CD_FRAME_SIZE, CD_MAX_SECTOR_DATA);
            if (t->audio && !legacy_le)
                for (int b = 0; b < CD_MAX_SECTOR_DATA; b += 2) {
                    uint8_t tmp = sector[b];
                    sector[b] = sector[b + 1];
                    sector[b + 1] = tmp;
                }
            if (fwrite(sector, 1, sizeof sector, out) != sizeof sector) return fail("write error (disk full?)", path);
            if (done - last_report >= total / 200) {
                last_report = done;
                printf("progress %llu/%llu\n", (unsigned long long)done, (unsigned long long)total);
                fflush(stdout);
            }
        }
        phys = track_phys + (uint64_t)(t->frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING * CD_TRACK_PADDING;
        logical += t->frames;
        done += (uint64_t)t->pad;
        fclose(out);
        fprintf(gdi, "%d %d %d %d %s 0\n", i + 1, lba, t->audio ? 0 : 4, CD_MAX_SECTOR_DATA, name);
        prev_end = lba + sectors;
    }
    fclose(gdi);
    printf("progress %llu/%llu\ndone\n", (unsigned long long)total, (unsigned long long)total);
    chd_close(chd);
    free(hunk);
    return 0;
}
