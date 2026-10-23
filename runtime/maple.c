/* Maple bus: controllers (from host gamepads/keyboard) and a VMU memory card
 * backed by a file, serviced when the game kicks Maple DMA. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "controls.h"
#include "input.h"
#include "runtime.h"

#define FUNC_CONTROLLER 0x00000001u
#define FUNC_MEMCARD 0x00000002u
#define FUNC_LCD 0x00000004u
#define FUNC_CLOCK 0x00000008u
#define FUNC_VIBRATION 0x00000100u  /* the Jump Pack (Puru Puru Pack) */

enum {
    MDCMD_DEVICE_INFO = 1, MDCMD_EXT_INFO = 2, MDCMD_RESET = 3, MDCMD_SHUTDOWN = 4,
    MDCMD_GET_COND = 9, MDCMD_GET_MEMINFO = 10, MDCMD_BLOCK_READ = 11, MDCMD_BLOCK_WRITE = 12,
    MDCMD_GET_LAST_ERR = 13, MDCMD_SET_COND = 14,
    MDRS_DEVICE_INFO = 5, MDRS_EXT_INFO = 6, MDRS_ACK = 7, MDRS_DATA = 8,
    MDRE_NO_RESPONSE = 0xFF, MDRE_UNKNOWN_FUNC = 0xFE, MDRE_UNKNOWN_CMD = 0xFD,
};

/* ---------------- VMU ---------------- */
static uint8_t s_vmu[128 * 1024];
static char s_vmu_path[1024];
static int s_vmu_dirty;

static void vmu_format(void) {
    memset(s_vmu, 0, sizeof s_vmu);
    uint8_t *root = s_vmu + 255 * 512;
    memset(root, 0x55, 16);
    root[0x10] = 1;  /* custom colour */
    uint16_t *r16 = (uint16_t *)root;
    r16[0x46 / 2] = 254;  /* FAT block */
    r16[0x48 / 2] = 1;
    r16[0x4A / 2] = 253;  /* directory */
    r16[0x4C / 2] = 13;
    r16[0x50 / 2] = 200;  /* user blocks */
    r16[0x52 / 2] = 31;
    r16[0x54 / 2] = 128;
    uint16_t *fat = (uint16_t *)(s_vmu + 254 * 512);
    for (int i = 0; i < 256; i++) fat[i] = 0xFFFC;
    fat[255] = 0xFFFA;
    fat[254] = 0xFFFA;
    for (int b = 253; b > 241; b--) fat[b] = (uint16_t)(b - 1);
    fat[241] = 0xFFFA;
}

static void vmu_load(const char *dir) {
    snprintf(s_vmu_path, sizeof s_vmu_path, "%s/vmu_a1.bin", dir);
    FILE *f = fopen(s_vmu_path, "rb");
    if (f && fread(s_vmu, 1, sizeof s_vmu, f) == sizeof s_vmu) {
        fclose(f);
        return;
    }
    if (f) fclose(f);
    vmu_format();
    s_vmu_dirty = 1;
}

/* netplay: the client plays with the host's memory card, so neither side
 * writes the card file during a session */
static int s_vmu_readonly;
uint8_t *maple_vmu_image(void) { return s_vmu; }

/* Netplay: both players use the host's memory card for a session, and nothing
 * is written to either player's file; the player's own card returns afterwards. */
void maple_vmu_use_session_image(const uint8_t *img) {
    memcpy(s_vmu, img, sizeof s_vmu);
    s_vmu_readonly = 1;
    s_vmu_dirty = 0;
}

void maple_vmu_restore_own(void) {
    s_vmu_readonly = 0;
    s_vmu_dirty = 0;
    FILE *f = fopen(s_vmu_path, "rb");
    if (f && fread(s_vmu, 1, sizeof s_vmu, f) == sizeof s_vmu) {
        fclose(f);
        return;
    }
    if (f) fclose(f);
    vmu_format();
    s_vmu_dirty = 1;
}

void maple_flush_saves(void) {
    if (!s_vmu_dirty || s_vmu_readonly) return;
    FILE *f = fopen(s_vmu_path, "wb");
    if (f) {
        fwrite(s_vmu, 1, sizeof s_vmu, f);
        fclose(f);
        s_vmu_dirty = 0;
    }
}

/* ---------------- device info ---------------- */
static void put_str(uint8_t *dst, const char *s, int n) {
    int l = (int)strlen(s);
    for (int i = 0; i < n; i++) dst[i] = i < l ? (uint8_t)s[i] : ' ';
}

#ifndef MEMCARD_FUNC_DATA
#define MEMCARD_FUNC_DATA 0x00410F00u
#endif

/* Device info is 28 words: func, func_data[3], area, dir, name[30], license[60], power */
static int device_info(int port, int unit, uint32_t *out) {
    uint8_t *b = (uint8_t *)out;
    memset(out, 0, 112);
    if (unit == 0) {
        if (!input_connected(port)) return 0;
        out[0] = __builtin_bswap32(FUNC_CONTROLLER);
        out[1] = __builtin_bswap32(0x000F06FE);  /* standard controller buttons */
        put_str(b + 18, "Dreamcast Controller", 30);
    } else if (unit == 1 && port == 0) {
        out[0] = __builtin_bswap32(FUNC_MEMCARD);
        out[1] = MEMCARD_FUNC_DATA;
        put_str(b + 18, "Visual Memory", 30);
    } else if (unit == 2 && input_connected(port)) {
        /* a Jump Pack in every controller's second slot: its rumble goes to
         * the player's gamepad (controls_rumble) */
        out[0] = __builtin_bswap32(FUNC_VIBRATION);
        out[1] = 0x00000101;  /* one vibration source */
        put_str(b + 18, "Puru Puru Pack", 30);
    } else {
        return 0;
    }
    b[16] = 0xFF;  /* area code: all regions */
    b[17] = 0;     /* connector direction */
    put_str(b + 48, "Produced By or Under License From SEGA ENTERPRISES,LTD.", 60);
    out[27] = 0x01F401AE;  /* standby/max power */
    return 28;
}

/* Returns response length in words (excluding header), sets *code. */
static int handle(int port, int unit, uint32_t cmd, const uint32_t *in, int inlen, uint32_t *out,
                  uint32_t *code) {
    switch (cmd) {
    case MDCMD_DEVICE_INFO:
    case MDCMD_EXT_INFO: {
        int n = device_info(port, unit, out);
        if (!n) { *code = MDRE_NO_RESPONSE; return -1; }
        *code = MDRS_DEVICE_INFO;
        return n;
    }
    case MDCMD_GET_COND: {
        uint32_t func = __builtin_bswap32(in[0]);
        if (unit == 0 && func == FUNC_CONTROLLER) {
            PadState st;
            input_poll(port, &st);
            out[0] = __builtin_bswap32(FUNC_CONTROLLER);
            uint8_t *b = (uint8_t *)&out[1];
            b[0] = (uint8_t)~st.buttons;
            b[1] = (uint8_t)(~st.buttons >> 8);
            b[2] = st.rtrig;
            b[3] = st.ltrig;
            b[4] = st.ax;
            b[5] = st.ay;
            b[6] = 0x80;
            b[7] = 0x80;
            *code = MDRS_DATA;
            return 3;
        }
        if (unit == 2 && func == FUNC_VIBRATION) {
            out[0] = __builtin_bswap32(FUNC_VIBRATION);
            out[1] = 0;
            *code = MDRS_DATA;
            return 2;
        }
        if (unit == 1 && func == FUNC_CLOCK) {
            out[0] = __builtin_bswap32(FUNC_CLOCK);
            memset(&out[1], 0, 8);
            *code = MDRS_DATA;
            return 3;
        }
        *code = MDRE_UNKNOWN_FUNC;
        return 0;
    }
    case MDCMD_GET_MEMINFO: {
        if (unit == 2) {  /* the Jump Pack's capabilities */
            out[0] = __builtin_bswap32(FUNC_VIBRATION);
            out[1] = 0x3B07E010;
            *code = MDRS_DATA;
            return 2;
        }
        out[0] = __builtin_bswap32(FUNC_MEMCARD);
        /* halfwords in transfer order: last block, partition, system (root) block, FAT block,
         * FAT blocks, directory block, directory blocks, icon/reserved bytes, user blocks, reserved */
        static const uint16_t info[12] = {255, 0, 255, 254, 1, 253, 13, 0, 200, 0, 0, 0};
        memcpy(&out[1], info, sizeof info);
        *code = MDRS_DATA;
        return 7;
    }
    case MDCMD_BLOCK_READ: {
        uint32_t loc = __builtin_bswap32(in[1]);
        uint32_t block = loc & 0xFFFF;
        out[0] = __builtin_bswap32(FUNC_MEMCARD);
        out[1] = in[1];
        if (block < 256) memcpy(&out[2], s_vmu + block * 512, 512);
        *code = MDRS_DATA;
        return 2 + 128;
    }
    case MDCMD_BLOCK_WRITE: {
        uint32_t loc = __builtin_bswap32(in[1]);
        uint32_t block = loc & 0xFFFF, phase = (loc >> 16) & 0xFF;
        int bytes = (inlen - 2) * 4;
        if (unit == 1 && __builtin_bswap32(in[0]) == FUNC_MEMCARD && block < 256) {
            uint32_t off = block * 512 + phase * 128;
            if (bytes > 0 && off + (uint32_t)bytes <= sizeof s_vmu) {
                memcpy(s_vmu + off, &in[2], (size_t)bytes);
                s_vmu_dirty = 1;
            }
        }
        *code = MDRS_ACK;
        return 0;
    }
    case MDCMD_SET_COND:
        if (unit == 2 && inlen >= 2 && __builtin_bswap32(in[0]) == FUNC_VIBRATION) {
            /* condition bytes: special (motor), effect 1, effect 2, duration */
            controls_rumble(port, (const uint8_t *)&in[1]);
        }
        *code = MDRS_ACK;
        return 0;
    case MDCMD_GET_LAST_ERR:
    case MDCMD_RESET:
    case MDCMD_SHUTDOWN:
        *code = MDRS_ACK;
        return 0;
    }
    *code = MDRE_UNKNOWN_CMD;
    return 0;
}

void maple_dma(uint32_t desc) {
    g_stats.maple_dmas++;
    uint32_t in[256], out[256];
    for (;;) {
        uint32_t ctl = R32(desc), recv = R32(desc + 4);
        uint32_t len = ctl & 0xFF;
        int last = ctl >> 31;
        if (((ctl >> 8) & 7) == 7) {  /* NOP pattern: a single control word */
            desc += 4;
            if (last) break;
            continue;
        }
        uint32_t frame = R32(desc + 8);
        uint32_t cmd = frame & 0xFF, dst = (frame >> 8) & 0xFF;
        for (uint32_t i = 0; i < len && i < 255; i++) in[i] = R32(desc + 12 + i * 4);
        if (g_trace) logf_("maple desc %08X: ctl=%08X recv=%08X frame=%08X", desc, ctl, recv, frame);
        int port = (int)(dst >> 6), unit = 0;
        if (dst & 0x1F) unit = __builtin_ctz(dst & 0x1F) + 1;
        uint32_t code;
        int n = handle(port, unit, cmd, in, (int)len, out, &code);
        if (unit && getenv("SFA3_MAPLE_LOG")) {
            static int logged;
            if (logged++ < 400)
                logf_("maple port%d unit%d cmd=%u len=%u in0=%08X in1=%08X -> code=%u n=%d", port, unit, cmd, len,
                      len > 0 ? in[0] : 0, len > 1 ? in[1] : 0, code, n);
        }
        if (code == MDRE_NO_RESPONSE || n < 0) {
            W32(recv, 0xFFFFFFFFu);
        } else {
            /* reply: cmd, dest = original sender, src = device (with sub-units present) */
            uint32_t src = dst;
            /* sub-units: a VMU in slot 1 of port A, a Jump Pack in every controller's slot 2 */
            if (unit == 0) src |= (port == 0 ? 0x01 : 0) | (input_connected(port) ? 0x02 : 0);
            W32(recv, code | ((frame >> 16) & 0xFF) << 8 | src << 16 | (uint32_t)n << 24);
            for (int i = 0; i < n; i++) W32(recv + 4 + i * 4, out[i]);
        }
        desc += 12 + len * 4;
        if (last) break;
    }
    holly_raise(0, 1u << 12);  /* Maple DMA end */
}

void maple_init(const char *save_dir) { vmu_load(save_dir); }
