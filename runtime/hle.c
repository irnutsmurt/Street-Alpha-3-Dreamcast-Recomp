/* Native implementations of the Dreamcast boot ROM services the game calls:
 * system info, ROM font, flash ROM, GD-ROM drive and misc vectors. */
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "gdi.h"
#include "pvr.h"
#include "runtime.h"

/* Magic entry points written into the BIOS vector table. They lie in the
 * boot ROM's private RAM area, where the game has no code. */
#define HLE_SYSINFO 0x8C001000u
#define HLE_ROMFONT 0x8C001010u
#define HLE_FLASH   0x8C001020u
#define HLE_GDROM   0x8C001030u
#define HLE_MENU    0x8C001040u
#define HLE_UNUSED  0x8C001050u

static uint8_t s_flash[0x20000];

/* ---------------- GD-ROM drive ---------------- */

enum { GD_IDLE = 0, GD_BUSY = 1, GD_DONE = 2, GD_ERROR = -1 };

typedef struct {
    int id;
    int cmd;
    uint32_t params[4];
    int state;
    uint32_t transferred;
} GdReq;

static GdReq s_req;
static int s_next_id = 1;
static uint32_t s_sector_mode[4] = {0, 0x2000, 0x400, 2048};
static uint32_t s_drive_status = 2;  /* standby */

/* stream state for CMD_DMAREAD_STREAM / PIOREAD_STREAM */
static uint32_t s_stream_fad, s_stream_left;

static void gd_read(uint32_t fad, uint32_t count, uint32_t dst) {
    uint8_t buf[2048];
    for (uint32_t i = 0; i < count; i++) {
        gdi_read_sector(fad + i, buf);
        g_stats.gd_sectors++;
        uint32_t a = dst + i * 2048;
        if (is_ram(a) && is_ram(a + 2047)) {
            memcpy(ram_ptr(a), buf, 2048);
        } else {
            for (int k = 0; k < 2048; k += 4) {
                uint32_t w;
                memcpy(&w, buf + k, 4);
                W32(a + k, w);
            }
        }
    }
}

static void gd_exec(GdReq *r) {
    uint32_t *p = r->params;
    switch (r->cmd) {
    case 16: /* PIOREAD */
    case 17: /* DMAREAD: fad, count, buffer, unknown */
        { static int n; if (n++ < 300) logf_("gd read cmd=%d fad=%u n=%u -> %08X", r->cmd, p[0], p[1], p[2]); }
        gd_read(p[0], p[1], p[2]);
        r->transferred = p[1] * 2048;
        s_drive_status = 2;
        break;
    case 18: /* GETTOC */
    case 19: { /* GETTOC2: area, buffer */
        uint32_t toc[102];
        gdi_toc((int)p[0], toc);
        for (int i = 0; i < 102; i++) W32(p[1] + i * 4, toc[i]);
        break;
    }
    case 24: /* INIT */
        s_drive_status = 2;
        break;
    case 22: /* PAUSE */
    case 20: case 21: /* PLAY / PLAY2 (CD-DA): no audio tracks besides the warning */
    case 33: /* STOP */
    case 27: /* SEEK */
        break;
    case 28: /* DMAREAD_STREAM: fad, byte count */
    case 37: /* PIOREAD_STREAM */
    case 38: case 39:
        { static int n; if (n++ < 300) logf_("gd stream cmd=%d fad=%u n=%X", r->cmd, p[0], p[1]); }
        s_stream_fad = p[0];
        s_stream_left = p[1] * (r->cmd == 28 || r->cmd == 37 ? 1 : 2048);
        r->state = GD_BUSY;
        return;
    case 35: { /* GETSES: session, buffer */
        W8(p[1] + 0, 1);
        break;
    }
    case 40: { /* GET_VERS */
        const char *v = "GDC Version 1.10 1999-03-31";
        for (int i = 0; v[i]; i++) W8(p[0] + i, (uint32_t)v[i]);
        break;
    }
    default:
        logf_("gdrom: unhandled command %d (%08X %08X %08X %08X)", r->cmd, p[0], p[1], p[2], p[3]);
        break;
    }
    r->state = GD_DONE;
}

static uint32_t gdrom_syscall(uint32_t func) {
    if (g_trace && func != 2 && func != 1)
        logf_("gd syscall %u r4=%08X r5=%08X [%08X %08X %08X %08X] pr=%08X", func, g.r[4], g.r[5],
              g.r[5] && is_ram(g.r[5]) ? R32(g.r[5]) : 0, g.r[5] && is_ram(g.r[5]) ? R32(g.r[5] + 4) : 0,
              g.r[5] && is_ram(g.r[5]) ? R32(g.r[5] + 8) : 0, g.r[5] && is_ram(g.r[5]) ? R32(g.r[5] + 12) : 0,
              g.pr);
    switch (func) {
    case 0: { /* send command: r4 cmd, r5 params */
        s_req.cmd = (int)g.r[4];
        for (int i = 0; i < 4; i++) s_req.params[i] = g.r[5] ? R32(g.r[5] + i * 4) : 0;
        s_req.id = s_next_id++;
        s_req.state = GD_BUSY;
        s_req.transferred = 0;
        gd_exec(&s_req);
        return (uint32_t)s_req.id;
    }
    case 1: { /* check command: r4 id, r5 status[4] */
        uint32_t st;
        if ((int)g.r[4] != s_req.id) st = 0;  /* no such request */
        else st = s_req.state == GD_DONE ? 2 : s_req.state == GD_BUSY ? 1 : 0xFFFFFFFFu;
        if (g.r[5]) {
            W32(g.r[5] + 0, 0);
            W32(g.r[5] + 4, 0);
            W32(g.r[5] + 8, s_req.transferred);
            W32(g.r[5] + 12, 0);
        }
        return st;
    }
    case 2: /* main loop */
        return 0;
    case 3: /* init */
        s_req.state = GD_IDLE;
        return 0;
    case 4: /* check drive: r4 -> {status, disc type} */
        W32(g.r[4] + 0, s_drive_status);
        W32(g.r[4] + 4, 0x80);  /* GD-ROM */
        return 0;
    case 5: /* G1 DMA end */
        return 0;
    case 6: { /* request DMA transfer of streamed data: r4 id, r5 {addr, size} */
        uint32_t dst = R32(g.r[5]), size = R32(g.r[5] + 4);
        { static int n; if (n++ < 300) logf_("gd stream xfer dst=%08X size=%X left=%X fad=%u", dst, size, s_stream_left, s_stream_fad); }
        uint8_t buf[2048];
        uint32_t done = 0;
        while (done < size && s_stream_left) {
            gdi_read_sector(s_stream_fad++, buf);
            uint32_t n = size - done < 2048 ? size - done : 2048;
            for (uint32_t k = 0; k < n; k++) W8(dst + done + k, buf[k]);
            done += n;
            s_stream_left = s_stream_left > n ? s_stream_left - n : 0;
        }
        s_req.transferred += done;
        if (!s_stream_left) s_req.state = GD_DONE;
        holly_raise(0, 1u << 14);  /* GD-ROM DMA end */
        return 0;
    }
    case 7: /* check DMA transfer: r4 id, r5 -> remaining */
        W32(g.r[5], 0);
        return 0;
    case 8: /* abort */
        s_req.state = GD_IDLE;
        s_stream_left = 0;
        return 0;
    case 9: /* reset */
        return 0;
    case 10: /* sector mode: r4 -> {set/get, ?, mode, size} */
        if (R32(g.r[4]) == 0) {
            for (int i = 0; i < 4; i++) s_sector_mode[i] = R32(g.r[4] + i * 4);
        } else {
            for (int i = 1; i < 4; i++) W32(g.r[4] + i * 4, s_sector_mode[i]);
        }
        return 0;
    }
    logf_("gdrom syscall %u unhandled", func);
    return 0;
}

/* ---------------- flash ROM ---------------- */

static void flash_init(void) {
    memset(s_flash, 0xFF, sizeof s_flash);
    /* partition 0 (factory): region/broadcast settings at 0x1A000 */
    memcpy(s_flash + 0x1A000, "00000", 5);
    s_flash[0x1A002] = '1';  /* USA */
    s_flash[0x1A003] = '0';  /* NTSC */
    s_flash[0x1A004] = '1';  /* English */
    memcpy(s_flash + 0x1A0A0, "00000", 5);
    s_flash[0x1A0A2] = '1';
}

static const uint32_t s_part_off[5] = {0x1A000, 0x18000, 0x1C000, 0x10000, 0x00000};
static const uint32_t s_part_size[5] = {0x2000, 0x2000, 0x4000, 0x8000, 0x10000};

static uint32_t flash_syscall(uint32_t func) {
    switch (func) {
    case 0: /* info: r4 partition, r5 -> {offset, size} */
        if (g.r[4] > 4) return 0xFFFFFFFFu;
        W32(g.r[5], s_part_off[g.r[4]]);
        W32(g.r[5] + 4, s_part_size[g.r[4]]);
        return 0;
    case 1: /* read: r4 offset, r5 dst, r6 size */
        for (uint32_t i = 0; i < g.r[6]; i++) W8(g.r[5] + i, s_flash[(g.r[4] + i) & 0x1FFFF]);
        return 0;
    case 2: /* write */
        for (uint32_t i = 0; i < g.r[6]; i++) s_flash[(g.r[4] + i) & 0x1FFFF] &= (uint8_t)R8(g.r[5] + i);
        return 0;
    case 3: /* delete (erase partition containing r4) */
        for (int p = 0; p < 5; p++)
            if (g.r[4] == s_part_off[p]) memset(s_flash + s_part_off[p], 0xFF, s_part_size[p]);
        return 0;
    }
    return 0xFFFFFFFFu;
}

uint32_t flash_read(uint32_t off, int size) {
    uint32_t v = 0;
    memcpy(&v, s_flash + (off & 0x1FFFF), (size_t)size);
    return v;
}

uint32_t bios_read(uint32_t p, int size) {
    (void)size;
    if (g_trace) logf_("bios read %08X", p);
    return 0;
}

/* ---------------- dispatcher ---------------- */

int hle_call(uint32_t addr) {
    addr = (addr & 0x1FFFFFFF) | 0x80000000u;
    if ((addr & 0xFFFFF000u) != 0x8C001000u) return 0;
    uint32_t func = g.r[7];
    switch (addr) {
    case HLE_SYSINFO:
        if (func == 0) {
            /* init: system id and region data into 0x8C000068.. */
            for (int i = 0; i < 8; i++) W8(0x8C000068 + i, 0x11 * (i + 1));
            for (int i = 0; i < 5; i++) W8(0x8C000070 + i, s_flash[0x1A000 + i]);
            g.r[0] = 0;
        } else if (func == 3) {
            g.r[0] = 0x8C000068;
        } else {
            g.r[0] = 0;
        }
        break;
    case HLE_ROMFONT:
        if (g.r[1] == 0) g.r[0] = 0xA0100020;  /* font address in boot ROM */
        else g.r[0] = 0;                        /* lock / unlock */
        break;
    case HLE_FLASH:
        g.r[0] = flash_syscall(func);
        break;
    case HLE_GDROM:
        if (g.r[6] == 0xFFFFFFFFu) g.r[0] = 0;  /* misc: init / set vector */
        else g.r[0] = gdrom_syscall(func);
        break;
    case HLE_MENU:
        fatal("game requested return to the Dreamcast system menu (r4=%d)", (int)g.r[4]);
    default:
        fatal("unknown HLE vector %08X", addr);
    }
    return 1;
}

/* Boot ROM state at power-on (also used by an in-place machine reset). */
void hle_reset(void) {
    memset(&s_req, 0, sizeof s_req);
    s_next_id = 1;
    s_sector_mode[0] = 0;
    s_sector_mode[1] = 0x2000;
    s_sector_mode[2] = 0x400;
    s_sector_mode[3] = 2048;
    s_drive_status = 2;
    s_stream_fad = s_stream_left = 0;
    flash_init();
    W32(0x8C0000B0, HLE_SYSINFO);
    W32(0x8C0000B4, HLE_ROMFONT);
    W32(0x8C0000B8, HLE_FLASH);
    W32(0x8C0000BC, HLE_GDROM);
    W32(0x8C0000C0, HLE_UNUSED);
    W32(0x8C0000E0, HLE_MENU);
}

void hle_init(const char *gdi_path) {
    if (!gdi_open(gdi_path))
        fatal("cannot open disc image %s (moved or deleted? Open the setup app and convert again from your disc image)",
              gdi_path);
    hle_reset();
}

/* G1 GD-ROM ATA registers: report an idle, ready drive. */
uint32_t gdrom_reg_read(uint32_t off) {
    switch (off) {
    case 0x7018: /* alternate status */
    case 0x709C: /* status */
        return 0x50;  /* DRDY | DSC */
    }
    return 0;
}
void gdrom_reg_write(uint32_t off, uint32_t v) { (void)off; (void)v; }
void gdrom_dma_start(void) { holly_raise(0, 1u << 14); }
