/* Hardware register space. Main RAM is handled inline in cpu.h; everything
 * else lands here: SH-4 on-chip modules, Holly (system bus, PVR, Maple,
 * GD-ROM), AICA, VRAM and the TA FIFO. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "pvr.h"
#include "runtime.h"

uint8_t *g_ram;
uint8_t *g_vram;
uint8_t *g_ext_ram;
uint8_t *g_aica_ram;

extern uint32_t g_intevt;
extern uint32_t g_holly_mask[3][3];

/* Backing store for registers without side effects. */
static uint32_t s_p4[0x10000];      /* indexed by P4I(): module (addr bits 16-23) and register */
#define P4I(a) (((((uint32_t)(a)) >> 16) & 0xFF) << 8 | ((((uint32_t)(a)) >> 2) & 0xFF))
static uint32_t s_holly[0x4000];    /* 0x005F0000-0x005FFFFF (word index) */
static uint32_t s_qacr[2];
static uint32_t s_sq[2][8];

static uint8_t s_seen[0x10000];
static void unknown(const char *kind, uint32_t a, uint32_t v) {
    unsigned h = ((a >> 2) ^ (a >> 18)) & 0xFFFF;
    if (s_seen[h] < 2) {
        s_seen[h]++;
        logf_("mmio %s %08X = %08X (pr=%08X)", kind, a, v, g.pr);
    }
}

/* ---------------- VRAM: 64-bit (interleaved) vs 32-bit views ---------------- */
/* Storage is kept in 64-bit-bus order (as textures see it). An address in the
 * 32-bit area maps to storage by interleaving the two 4 MB banks per word. */
static inline uint32_t vram32_to_64(uint32_t off) {
    off &= 0x7FFFFF;
    uint32_t bank = (off >> 22) & 1;
    return (off & 0x3) | ((off & 0x3FFFFC) << 1) | (bank << 2);
}

uint32_t vram_read32_linear(uint32_t off) {
    uint32_t v;
    memcpy(&v, g_vram + vram32_to_64(off), 4);
    return v;
}

/* ---------------- SH-4 timers ---------------- */
static uint64_t s_tmu_base_ns[3];
static uint32_t s_tmu_base_cnt[3];
static uint32_t s_tstr;

#define now_ns host_time_ns

static uint32_t tmu_div(int ch) {
    static const uint32_t d[8] = {4, 16, 64, 256, 1024, 1024, 1024, 1024};
    return d[s_p4[P4I(0xFFD80010u + ch * 0xC)] & 7];
}

static uint32_t tmu_count(int ch) {
    if (!(s_tstr & (1u << ch))) return s_tmu_base_cnt[ch];
    /* Peripheral clock 50 MHz */
    uint64_t ticks = (now_ns() - s_tmu_base_ns[ch]) * 50 / 1000 / tmu_div(ch);
    uint32_t cor = s_p4[P4I(0xFFD80008u + ch * 0xC)];
    uint64_t c = s_tmu_base_cnt[ch];
    if (ticks <= c) return (uint32_t)(c - ticks);
    ticks -= c + 1;
    uint64_t period = (uint64_t)cor + 1;
    return (uint32_t)(cor - (ticks % period));
}

static void tmu_set_count(int ch, uint32_t v) {
    s_tmu_base_cnt[ch] = v;
    s_tmu_base_ns[ch] = now_ns();
}

static void tmu_set_tstr(uint32_t v) {
    for (int ch = 0; ch < 3; ch++) {
        uint32_t bit = 1u << ch;
        if ((s_tstr & bit) && !(v & bit)) s_tmu_base_cnt[ch] = tmu_count(ch);
        if (!(s_tstr & bit) && (v & bit)) s_tmu_base_ns[ch] = now_ns();
    }
    s_tstr = v & 7;
}

/* ---------------- SH-4 P4 area ---------------- */
static uint32_t p4_read(uint32_t a) {
    a |= 0xE0000000u;  /* area-7 mirror 0x1Fxxxxxx */
    switch (a) {
    case 0xFF000028: return g_intevt;
    case 0xFFD80004: return s_tstr;
    case 0xFFD8000C: return tmu_count(0);
    case 0xFFD80018: return tmu_count(1);
    case 0xFFD80024: return tmu_count(2);
    case 0xFF800030: /* PDTRA: cable type in bits 8-9 (3 = VGA? 0 = VGA, 2 = RGB, 3 = composite) */
        return (s_p4[P4I(a)] & ~0x300u) | (0u << 8) | 0x3;
    case 0xFF000038: return s_qacr[0];
    case 0xFF00003C: return s_qacr[1];
    }
    if (a >= 0xFFA00000 && a < 0xFFA00050) return s_p4[P4I(a)];
    uint32_t v = s_p4[P4I(a)];
    unknown("p4 rd", a, v);
    return v;
}

static void dmac_ch2_kick(void);

static void p4_write(uint32_t a, uint32_t v) {
    a |= 0xE0000000u;
    switch (a) {
    case 0xFF000038: s_qacr[0] = v; return;
    case 0xFF00003C: s_qacr[1] = v; return;
    case 0xFFD80004: tmu_set_tstr(v); return;
    case 0xFFD8000C: tmu_set_count(0, v); return;
    case 0xFFD80018: tmu_set_count(1, v); return;
    case 0xFFD80024: tmu_set_count(2, v); return;
    }
    if (a >= 0xF0000000 && a < 0xF8000000) return;          /* cache address arrays */
    if (a >= 0xFFA00000 && a < 0xFFA00050 && getenv("SFA3_DMA_LOG")) {
        extern uint64_t g_vblank_count;
        if (g_vblank_count > 2300 && g_vblank_count < 2320)
            logf_("dmac wr %08X = %08X pr=%08X", a, v, g.pr);
    }
    s_p4[P4I(a)] = v;
    if (a >= 0xFFA00000 && a < 0xFFA00050) return;          /* DMAC */
    if (a >= 0xFFD80000 && a < 0xFFD80030) return;          /* TMU config */
    if (a == 0xFF00001C || a == 0xFF000010 || a == 0xFF000000 || a == 0xFF000004) return;
    if (a >= 0xF0000000 && a < 0xF8000000) return;          /* cache address arrays */
    unknown("p4 wr", a, v);
}

/* ---------------- store queues ---------------- */
static void sq_write(uint32_t a, uint32_t v) { s_sq[(a >> 5) & 1][(a >> 2) & 7] = v; }

static void dest_write_block(uint32_t dst, const uint32_t *src, unsigned words);
void renderer_vram_block_written(uint32_t off64, uint32_t bytes);

void sq_flush(uint32_t a) {
    int q = (a >> 5) & 1;
    uint32_t dst = ((s_qacr[q] & 0x1C) << 24) | (a & 0x03FFFFE0);
    dest_write_block(dst, s_sq[q], 8);
}

/* Route a block write (SQ flush or DMA) by destination area. */
static void dest_write_block(uint32_t dst, const uint32_t *src, unsigned words) {
    uint32_t p = dst & 0x1FFFFFFF;
    if (p >= 0x10000000 && p < 0x14000000) {
        if (p < 0x10800000 || (p >= 0x12000000 && p < 0x12800000)) {
            ta_write(src, words);
        } else if ((p >= 0x11000000 && p < 0x12000000) || p >= 0x13000000) {
            /* direct texture path: 64-bit VRAM area */
            uint32_t off = p & 0x7FFFFF;
            for (unsigned i = 0; i < words; i++, off += 4)
                memcpy(g_vram + (off & 0x7FFFFF), &src[i], 4);
            renderer_vram_block_written(p & 0x7FFFFF, words * 4);
        } else {
            unknown("yuv fifo", p, src[0]);
        }
        return;
    }
    for (unsigned i = 0; i < words; i++) W32(dst + i * 4, src[i]);
}

/* SH-4 DMAC channel 2: RAM -> TA / texture memory, started via SB_C2DST. */
static int s_ch2_busy;
void mmio_tick(void) {
    if (s_ch2_busy) { s_ch2_busy = 0; holly_raise(0, 1u << 19); }
}

static void dmac_ch2_kick(void) {
    {
        static int dbg = -1;
        extern uint64_t g_vblank_count;
        if (dbg < 0) dbg = getenv("SFA3_DMA_LOG") != NULL;
        if (dbg && g_vblank_count > 2300)
        {
            uint32_t sum = 0, sa = s_p4[P4I(0xFFA00020)], n = s_holly[0x6804 >> 2];
            for (uint32_t i = 0; i < n && i < 0x10000; i += 4) sum = sum * 31 + R32(sa + i);
            logf_("ch2: vbl=%llu sar=%08X dst=%08X len=%X pr=%08X sum=%08X", (unsigned long long)g_vblank_count,
                  sa, s_holly[0x6800 >> 2], n, g.pr, sum);
        }
    }
    if (g_trace)
        logf_("ch2 dma: sar=%08X dst=%08X len=%08X chcr=%08X", s_p4[P4I(0xFFA00020)],
              s_holly[0x6800 >> 2], s_holly[0x6804 >> 2], s_p4[P4I(0xFFA0002C)]);
    uint32_t sar = s_p4[P4I(0xFFA00020)];
    uint32_t len = s_holly[(0x6804 >> 2)];
    uint32_t dst = s_holly[(0x6800 >> 2)];
    if (!len) len = s_p4[P4I(0xFFA00028)] * 32;
    uint32_t buf[8];
    for (uint32_t done = 0; done < len; done += 32) {
        for (int i = 0; i < 8; i++) buf[i] = R32(sar + done + i * 4);
        dest_write_block(dst + ((dst & 0x1FFFFFFF) >= 0x11000000 ? done : 0), buf, 8);
    }
    s_p4[P4I(0xFFA00020)] = sar + len;
    s_p4[P4I(0xFFA00028)] = 0;
    s_p4[P4I(0xFFA0002C)] |= 2;  /* CHCR2.TE */
    s_holly[0x6804 >> 2] = 0;
    s_holly[0x6800 >> 2] = dst + len;
    if (getenv("SFA3_DMA_DEFER")) s_ch2_busy = 1;  /* completion signalled on the next tick */
    else holly_raise(0, 1u << 19);  /* ch2 DMA end */
}

/* ---------------- Holly ---------------- */
void maple_dma(uint32_t desc);
void gdrom_dma_start(void);
void aica_dma_start(void);

static uint32_t holly_read(uint32_t a) {
    uint32_t off = a & 0xFFFF;
    switch (off) {
    case 0x6900: return holly_status(0);
    case 0x6904: return holly_status(1);
    case 0x6908: return holly_status(2);
    case 0x688C: return 0;           /* SB_FFST: FIFOs empty */
    case 0x689C: return 0x0B;        /* SB_SBREV: retail Holly */
    case 0x6C18: return 0;           /* SB_MDST: maple DMA idle */
    case 0x7418: return 0;           /* SB_GDST */
    case 0x6808: return s_ch2_busy;  /* SB_C2DST */
    case 0x8000: return 0x17FD11DB;  /* PVR ID */
    case 0x8004: return 0x00000011;  /* PVR revision */
    case 0x810C: return pvr_spg_status();
    }
    if (off >= 0x7000 && off < 0x7100) return gdrom_reg_read(off);
    if (off >= 0x8000) return pvr_reg_read(off);
    uint32_t v = s_holly[(off >> 2) & 0x3FFF];
    if (!(off >= 0x6910 && off < 0x6940)) unknown("holly rd", a, v);
    return v;
}

static void holly_write(uint32_t a, uint32_t v) {
    uint32_t off = a & 0xFFFF;
    switch (off) {
    case 0x6900: holly_clear(0, v); return;
    case 0x6904: return;
    case 0x6908: holly_clear(2, v); return;
    case 0x6910: g_holly_mask[2][0] = v; break;
    case 0x6914: g_holly_mask[2][1] = v; break;
    case 0x6918: g_holly_mask[2][2] = v; break;
    case 0x6920: g_holly_mask[1][0] = v; break;
    case 0x6924: g_holly_mask[1][1] = v; break;
    case 0x6928: g_holly_mask[1][2] = v; break;
    case 0x6930: g_holly_mask[0][0] = v; break;
    case 0x6934: g_holly_mask[0][1] = v; break;
    case 0x6938: g_holly_mask[0][2] = v; break;
    case 0x6808: s_holly[off >> 2] = v; if (v & 1) dmac_ch2_kick(); return;
    case 0x6C18: if (v & 1) maple_dma(s_holly[0x6C04 >> 2]); return;
    case 0x7418: if (v & 1) gdrom_dma_start(); s_holly[off >> 2] = 0; return;
    case 0x7818: /* SB_ADST: G2 AICA DMA; completes immediately and self-clears */
        if (v & 1) aica_dma_start();
        s_holly[off >> 2] = 0;
        return;
    }
    if (off >= 0x7000 && off < 0x7100) { gdrom_reg_write(off, v); return; }
    if (off >= 0x8000) { pvr_reg_write(off, v); return; }
    s_holly[(off >> 2) & 0x3FFF] = v;
    if (off >= 0x6910 && off < 0x6940) {
        if (irq_any_pending()) g_irq_pending = 1;
        return;
    }
    unknown("holly wr", a, v);
}

uint32_t holly_reg(uint32_t off) { return s_holly[(off & 0xFFFF) >> 2]; }

/* ---------------- dispatch by area ---------------- */
static uint32_t rd(uint32_t a, int size) {
    if (a >= 0xE0000000u) {
        if (a < 0xE4000000u) return s_sq[(a >> 5) & 1][(a >> 2) & 7];
        return p4_read(a);
    }
    uint32_t p = a & 0x1FFFFFFF;
    if (p - EXT_RAM_PHYS < EXT_RAM_SIZE) {  /* the port's extension memory (see runtime.h) */
        uint32_t v = 0;
        memcpy(&v, g_ext_ram + (p - EXT_RAM_PHYS), (size_t)size);
        return v;
    }
    if (p >= 0x1F000000) return p4_read(p);
    if (p >= 0x005F0000 && p < 0x00600000) return holly_read(p);
    if (p >= 0x00700000 && p < 0x00710000) return aica_reg_read(p & 0xFFFF, size);
    if (p >= 0x00710000 && p < 0x00710010) return aica_rtc_read(p);
    if (p >= 0x00800000 && p < 0x00A00000) {
        uint32_t v = 0;
        memcpy(&v, g_aica_ram + (p & 0x1FFFFF), size);
        unknown("aica ram rd", p, v);
        return v;
    }
    if (p >= 0x04000000 && p < 0x08000000) {
        uint32_t v = 0;
        if (p & 0x01000000) {  /* 32-bit area */
            for (int i = 0; i < size; i++)
                ((uint8_t *)&v)[i] = g_vram[vram32_to_64(p + i)];
        } else {
            memcpy(&v, g_vram + (p & 0x7FFFFF), size);
        }
        return v;
    }
    if (p < 0x00200000) return bios_read(p, size);
    if (p >= 0x00200000 && p < 0x00220000) return flash_read(p & 0x1FFFF, size);
    unknown("rd", a, 0);
    return 0;
}

static void wr(uint32_t a, uint32_t v, int size) {
    if (a >= 0xE0000000u) {
        if (a < 0xE4000000u) { sq_write(a, v); return; }
        p4_write(a, v);
        return;
    }
    uint32_t p = a & 0x1FFFFFFF;
    if (p - EXT_RAM_PHYS < EXT_RAM_SIZE) { memcpy(g_ext_ram + (p - EXT_RAM_PHYS), &v, (size_t)size); return; }
    if (p >= 0x1F000000) { p4_write(p, v); return; }
    if (p >= 0x005F0000 && p < 0x00600000) { holly_write(p, v); return; }
    if (p >= 0x00700000 && p < 0x00710000) { aica_reg_write(p & 0xFFFF, v, size); return; }
    if (p >= 0x00710000 && p < 0x00710010) return;
    if (p >= 0x00800000 && p < 0x00A00000) {
        extern int g_stream_trace;
        void stream_trace_write(uint32_t off, int size);
        if (g_stream_trace > 0) stream_trace_write(p & 0x1FFFFF, size);
        memcpy(g_aica_ram + (p & 0x1FFFFF), &v, size);
        return;
    }
    if (p >= 0x04000000 && p < 0x08000000) {
        if (p & 0x01000000) {
            for (int i = 0; i < size; i++)
                g_vram[vram32_to_64(p + i)] = (uint8_t)(v >> (8 * i));
        } else {
            memcpy(g_vram + (p & 0x7FFFFF), &v, size);
        }
        pvr_vram_written(p, size);
        return;
    }
    if (p >= 0x10000000 && p < 0x14000000) {
        /* direct (non-SQ) writes to the TA FIFO */
        ta_write_word(p, v);
        return;
    }
    unknown("wr", a, v);
}

uint32_t mmio_read8(uint32_t a) { return rd(a, 1) & 0xFF; }
uint32_t mmio_read16(uint32_t a) { return rd(a, 2) & 0xFFFF; }
uint32_t mmio_read32(uint32_t a) { return rd(a, 4); }
void mmio_write8(uint32_t a, uint32_t v) { wr(a, v & 0xFF, 1); }
void mmio_write16(uint32_t a, uint32_t v) { wr(a, v & 0xFFFF, 2); }
void mmio_write32(uint32_t a, uint32_t v) { wr(a, v, 4); }

void mmio_reset(void) {
    memset(g_ram, 0, 16 << 20);
    if (g_ext_ram) memset(g_ext_ram, 0, EXT_RAM_SIZE);
    memset(g_vram, 0, 8 << 20);
    memset(g_aica_ram, 0, 2 << 20);
    memset(s_p4, 0, sizeof s_p4);
    memset(s_holly, 0, sizeof s_holly);
    memset(s_qacr, 0, sizeof s_qacr);
    memset(s_sq, 0, sizeof s_sq);
    memset(s_tmu_base_ns, 0, sizeof s_tmu_base_ns);
    s_tstr = 0;
    s_ch2_busy = 0;
    s_p4[P4I(0xFFD80008)] = 0xFFFFFFFF;
    s_p4[P4I(0xFFD80014)] = 0xFFFFFFFF;
    s_p4[P4I(0xFFD80020)] = 0xFFFFFFFF;
    for (int i = 0; i < 3; i++) s_tmu_base_cnt[i] = 0xFFFFFFFF;
}

void mmio_init(void) {
    g_ram = calloc(1, 16 << 20);
    g_vram = calloc(1, 8 << 20);
    if (!g_ext_ram) g_ext_ram = calloc(1, EXT_RAM_SIZE);
    g_aica_ram = calloc(1, 2 << 20);
    mmio_reset();
}
