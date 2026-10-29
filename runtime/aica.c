/* AICA sound chip: register file shared by the SH-4 (0x00700000) and the
 * ARM7 (0x00800000), timers and interrupts in both directions, and a 64-voice
 * PCM/ADPCM synthesizer streamed to SDL3. The recompiled sound driver
 * (MANATEE.DRV) runs here, interleaved with sample generation. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "arm.h"
#include "cpu.h"
#include "pvr.h"
#include "runtime.h"

#define RATE 44100
#define ARM_CYCLES_PER_SAMPLE 256
#define CHUNK 1  /* run the sound CPU every sample so timer interrupts are serviced on time */

static uint8_t s_regs[0x8000];
#define REG16(off) (*(uint16_t *)&s_regs[(off)])

enum { EG_ATTACK, EG_DECAY1, EG_DECAY2, EG_RELEASE, EG_OFF };

typedef struct {
    int keyed;          /* the slot's key state: on from a key-on until a key-off, whether it still sounds or not */
    int playing;
    int eg_state;
    float env;          /* attenuation 0 (loud) .. 1023 (silent) */
    uint64_t pos;       /* sample position, 32.32 fixed point */
    uint32_t last_idx;  /* last integer sample index decoded (ADPCM) */
    int16_t cur, prev;  /* ADPCM decoder output: current and previous sample */
    int adpcm_step;
    int16_t loop_sample;
    int loop_step;
    int loop_saved;
    int lp_flag;        /* loop end reached since last monitor read */
} Voice;

static Voice s_voice[64];
static int s_arm_running;
static uint32_t s_sci_pending, s_mci_pending;
static uint32_t s_timer_sub[3];
static SDL_AudioStream *s_stream;
static uint64_t s_samples_done;
static uint64_t s_start_ns;
static uint64_t s_sci_count[11];  /* interrupts raised per source (debug) */
static int s_snd_log;             /* SFA3_KEYON_LOG: log key-ons, key-offs, timer setup */
static FILE *s_pcm_dump;          /* SFA3_PCM_DUMP=<file>: record the mixed output */

/* SFA3_STREAM_LOG: per-frame trace of the audio stream ring (debug aid) */
int g_stream_trace = -1;
static uint32_t st_bytes, st_lo = 0xFFFFFFFF, st_hi, st_ca_reads, st_ca_last, st_dma;
static uint32_t st_obytes, st_olo = 0xFFFFFFFF, st_ohi;
void stream_trace_write(uint32_t off, int size) {
    if (off < 0x1FBFC0) {  /* outside the stream ring: sample banks, driver data */
        st_obytes += (uint32_t)size;
        if (off < st_olo) st_olo = off;
        if (off > st_ohi) st_ohi = off;
        return;
    }
    st_bytes += (uint32_t)size;
    if (off < st_lo) st_lo = off;
    if (off > st_hi) st_hi = off;
}
void stream_trace_frame(void) {
    extern uint64_t g_vblank_count;
    if (st_bytes || st_ca_reads || st_dma)
        logf_("stream vbl=%llu wr=%u [%06X-%06X] ca_reads=%u ca=%04X play48=%04X dma=%u", (unsigned long long)g_vblank_count,
              st_bytes, st_lo, st_hi, st_ca_reads, st_ca_last, (uint32_t)(s_voice[48].pos >> 32) & 0xFFFF, st_dma);
    if (st_obytes)
        logf_("sndram vbl=%llu wr=%u [%06X-%06X]", (unsigned long long)g_vblank_count, st_obytes, st_olo, st_ohi);
    if (st_obytes && st_olo >= 0x13000 && st_ohi < 0x14000) {  /* driver command area */
        char b[600];
        int k = 0;
        for (uint32_t a = st_olo & ~15u; a <= st_ohi && k < 560; a += 4)
            k += snprintf(b + k, sizeof b - k, "%s%08X", (a & 15) ? " " : "\n    ", *(uint32_t *)(g_aica_ram + a));
        logf_("  cmd area %06X:%s", st_olo & ~15u, b);
    }
    st_obytes = st_ohi = 0;
    st_olo = 0xFFFFFFFF;
    st_bytes = st_ca_reads = st_dma = 0;
    st_lo = 0xFFFFFFFF;
    st_hi = 0;
}
static int s_started;
int g_audio_mute;  /* fast-forward (netplay): the sound runs but isn't heard */
static float s_db_table[1024];
uint64_t g_aica_keyons, g_aica_sh4_irqs, g_aica_arm_irqs;
extern uint64_t g_aica_fiqs;

void aica_log_sources(void) {
    logf_("aica irq sources: ext=%llu midi_in=%llu dma=%llu cpu=%llu tA=%llu tB=%llu tC=%llu midi_out=%llu sample=%llu scieb=%04X",
          (unsigned long long)s_sci_count[0], (unsigned long long)s_sci_count[3], (unsigned long long)s_sci_count[4],
          (unsigned long long)s_sci_count[5], (unsigned long long)s_sci_count[6], (unsigned long long)s_sci_count[7],
          (unsigned long long)s_sci_count[8], (unsigned long long)s_sci_count[9], (unsigned long long)s_sci_count[10],
          REG16(0x289C));
}

int aica_active_voices(void) {
    int n = 0;
    for (int i = 0; i < 64; i++) n += s_voice[i].playing;
    return n;
}


#define now_ns host_time_ns

/* ---------------- channel register fields ---------------- */
static inline uint16_t creg(int ch, int r) { return REG16(ch * 0x80 + r); }
static inline uint32_t ch_sa(int ch) { return ((uint32_t)(creg(ch, 0x00) & 0x7F) << 16) | creg(ch, 0x04); }
static inline int ch_pcms(int ch) { return (creg(ch, 0x00) >> 7) & 3; }
static inline int ch_lpctl(int ch) { return (creg(ch, 0x00) >> 9) & 1; }
static inline uint32_t ch_lsa(int ch) { return creg(ch, 0x08); }
static inline uint32_t ch_lea(int ch) { return creg(ch, 0x0C); }

/* ---------------- interrupts ---------------- */
static void update_irqs(void) {
    uint32_t scieb = REG16(0x289C) & 0x7FF;
    A.fiq_pending = (s_sci_pending & scieb) != 0;
    uint32_t mcieb = REG16(0x28B4) & 0x7FF;
    if (s_mci_pending & mcieb) holly_raise(1, 1u << 1);
    else holly_clear(1, 1u << 1);
}

/* Interrupt level for the ARM's FIQ handler: SCILV0-2 give each source a
 * 3-bit level (sources 7-10 share bit 7). */
static uint32_t irq_level(void) {
    uint32_t active = s_sci_pending & REG16(0x289C) & 0x7FF;
    for (int bit = 0; bit < 11; bit++) {
        if (!(active & (1u << bit))) continue;
        int b = bit > 7 ? 7 : bit;
        uint32_t lv = ((REG16(0x28A8) >> b) & 1) | ((REG16(0x28AC) >> b) & 1) << 1 |
                      ((REG16(0x28B0) >> b) & 1) << 2;
        return lv;
    }
    return 0;
}

static void raise_sci(uint32_t bits) {
    for (int b = 0; b < 11; b++) if ((bits >> b) & 1) s_sci_count[b]++;
    s_sci_pending |= bits;
    s_mci_pending |= bits;
    update_irqs();
}

/* ---------------- voices ---------------- */
static void key_on(int ch) {
    Voice *v = &s_voice[ch];
    g_aica_keyons++;
    if (s_snd_log) {
        extern uint64_t g_vblank_count;
        logf_("keyon vbl=%llu ch=%d sa=%06X lsa=%04X lea=%04X pcms=%d loop=%d oct/fns=%04X eg=%04X/%04X vol=%04X/%04X",
              (unsigned long long)g_vblank_count, ch, ch_sa(ch), ch_lsa(ch), ch_lea(ch), ch_pcms(ch), ch_lpctl(ch),
              creg(ch, 0x18), creg(ch, 0x10), creg(ch, 0x14), creg(ch, 0x24), creg(ch, 0x28));
    }
    v->keyed = 1;
    v->playing = 1;
    v->eg_state = EG_ATTACK;
    v->env = 1023;
    v->pos = 0;
    v->last_idx = 0xFFFFFFFFu;
    v->cur = v->prev = 0;
    v->adpcm_step = 127;
    v->loop_saved = 0;
    v->lp_flag = 0;
}

static void key_off(int ch) {
    Voice *v = &s_voice[ch];
    if (s_snd_log && v->playing && ch < 8) {
        extern uint64_t g_vblank_count;
        logf_("keyoff vbl=%llu ch=%d pos=%u env=%.0f state=%d", (unsigned long long)g_vblank_count, ch,
              (uint32_t)(v->pos >> 32), v->env, v->eg_state);
    }
    v->keyed = 0;
    if (v->playing && v->eg_state != EG_OFF) v->eg_state = EG_RELEASE;
}

/* KYONEX: every slot whose KB is set and isn't keyed on is keyed on, every one keyed on whose KB is clear is keyed
 * off. A one-shot sample played to its end stays keyed on, silent, until the driver keys it off: the next KYONEX,
 * for another slot, doesn't start it again (it did: the KO call and the round's call now and then twice) */
static void key_exec(void) {
    for (int ch = 0; ch < 64; ch++) {
        int kb = (creg(ch, 0x00) >> 14) & 1;
        Voice *v = &s_voice[ch];
        if (kb && !v->keyed) key_on(ch);
        else if (!kb && v->keyed) key_off(ch);
    }
}

static const int s_adpcm_scale[8] = {230, 230, 230, 230, 307, 409, 512, 614};

static void adpcm_decode(Voice *v, uint8_t nib) {
    int step = v->adpcm_step;
    int delta = (((nib & 7) * 2 + 1) * step) >> 3;
    int s = v->cur + ((nib & 8) ? -delta : delta);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    v->prev = v->cur;
    v->cur = (int16_t)s;
    step = (step * s_adpcm_scale[nib & 7]) >> 8;
    if (step < 127) step = 127;
    if (step > 24576) step = 24576;
    v->adpcm_step = step;
}

/* Envelope rates. Times in ms for a full 0..1023 sweep at effective rate
 * 0..63, from the AICA/SCSP rate tables (as used by MAME's reference
 * implementation); rates 0 and 1 never move. */
static const float s_ar_ms[64] = {
    0, 0, 8100.0, 6900.0, 6000.0, 4800.0, 4000.0, 3400.0, 3000.0, 2400.0, 2000.0, 1700.0, 1500.0, 1200.0, 1000.0,
    860.0, 760.0, 600.0, 500.0, 430.0, 380.0, 300.0, 250.0, 220.0, 190.0, 150.0, 130.0, 110.0, 95.0, 76.0, 63.0,
    55.0, 47.0, 38.0, 31.0, 27.0, 24.0, 19.0, 15.0, 13.0, 12.0, 9.4, 7.9, 6.8, 6.0, 4.7, 3.8, 3.4, 3.0, 2.4, 2.0,
    1.8, 1.6, 1.3, 1.1, 0.93, 0.85, 0.65, 0.53, 0.44, 0.40, 0.35, 0.0, 0.0};
static const float s_dr_ms[64] = {
    0, 0, 118200.0, 101300.0, 88600.0, 70900.0, 59100.0, 50700.0, 44300.0, 35500.0, 29600.0, 25300.0, 22200.0,
    17700.0, 14800.0, 12700.0, 11100.0, 8900.0, 7400.0, 6300.0, 5500.0, 4400.0, 3700.0, 3200.0, 2800.0, 2200.0,
    1800.0, 1600.0, 1400.0, 1100.0, 920.0, 790.0, 690.0, 550.0, 460.0, 390.0, 340.0, 270.0, 230.0, 200.0, 170.0,
    140.0, 110.0, 98.0, 85.0, 68.0, 57.0, 49.0, 43.0, 34.0, 28.0, 25.0, 22.0, 18.0, 14.0, 12.0, 11.0, 8.5, 7.1,
    6.1, 5.4, 4.3, 3.6, 3.1};

/* Effective rate: register value R (0..31) doubled plus the key-rate-scaling
 * base from the voice's pitch; R = 0 always means "hold". */
static int eg_rate(int ch, int r) {
    if (!r) return 0;
    int krs = (creg(ch, 0x14) >> 10) & 15;
    int base = 0;
    if (krs != 15) {
        uint16_t pitch = creg(ch, 0x18);
        int oct = (pitch >> 11) & 15;
        if (oct & 8) oct -= 16;
        base = oct + 2 * krs + ((pitch >> 9) & 1);
    }
    int rate = base + 2 * r;
    return rate < 0 ? 0 : rate > 63 ? 63 : rate;
}

/* attenuation change per output sample for a table time in ms (0 = instant) */
static float eg_step(const float *table, int rate) {
    if (rate <= 1) return 0.0f;
    float ms = table[rate];
    if (ms <= 0.0f) return 1024.0f;
    return 1024.0f / (ms * (RATE / 1000.0f));
}

static float voice_sample(int ch) {
    Voice *v = &s_voice[ch];
    uint32_t sa = ch_sa(ch);
    int pcms = ch_pcms(ch);
    uint32_t idx = (uint32_t)(v->pos >> 32);
    uint32_t lea = ch_lea(ch), lsa = ch_lsa(ch);
    if (idx >= lea && lea) {
        v->lp_flag = 1;
        if (ch_lpctl(ch) && lea > lsa) {
            uint64_t len = (uint64_t)(lea - lsa) << 32;
            v->pos = ((uint64_t)lsa << 32) + (v->pos - ((uint64_t)lea << 32)) % len;
            idx = (uint32_t)(v->pos >> 32);
            if (pcms >= 2 && v->loop_saved) {
                v->cur = v->loop_sample;
                v->adpcm_step = v->loop_step;
                v->last_idx = lsa;
            }
        } else {
            if (s_snd_log && ch < 8) logf_("sample end ch=%d idx=%u", ch, idx);
            v->playing = 0;
            v->eg_state = EG_OFF;
            return 0;
        }
    }
    float s0, s1;
    uint32_t frac = (uint32_t)v->pos;
    switch (pcms) {
    case 0: { /* 16-bit PCM */
        uint32_t a = (sa + idx * 2) & 0x1FFFFE;
        int16_t x, y;
        memcpy(&x, g_aica_ram + a, 2);
        memcpy(&y, g_aica_ram + ((a + 2) & 0x1FFFFE), 2);
        s0 = x; s1 = y;
        break;
    }
    case 1: { /* 8-bit PCM */
        s0 = (float)((int8_t)g_aica_ram[(sa + idx) & 0x1FFFFF] * 256);
        s1 = (float)((int8_t)g_aica_ram[(sa + idx + 1) & 0x1FFFFF] * 256);
        break;
    }
    default: { /* 4-bit ADPCM: decode forward to idx */
        if (v->last_idx == 0xFFFFFFFFu || idx < v->last_idx) {
            v->cur = v->prev = 0;
            v->adpcm_step = 127;
            v->last_idx = 0xFFFFFFFFu;
        }
        while (v->last_idx == 0xFFFFFFFFu || v->last_idx < idx) {
            uint32_t n = v->last_idx + 1;
            uint8_t byte = g_aica_ram[(sa + (n >> 1)) & 0x1FFFFF];
            adpcm_decode(v, (n & 1) ? byte >> 4 : byte & 15);
            v->last_idx = n;
            if (n == lsa && !v->loop_saved) {
                v->loop_sample = v->prev;
                v->loop_step = v->adpcm_step;
                v->loop_saved = 1;
            }
        }
        s0 = v->prev;
        s1 = v->cur;
        frac = 0xFFFFFFFFu;  /* decoder output lags by one sample; no interpolation */
        break;
    }
    }
    float t = (float)(frac >> 8) / 16777216.0f;
    return s0 + (s1 - s0) * t;
}

static void voice_advance(int ch) {
    Voice *v = &s_voice[ch];
    uint16_t pitch = creg(ch, 0x18);
    int oct = (pitch >> 11) & 15;
    if (oct & 8) oct -= 16;
    uint32_t fns = pitch & 0x3FF;
    uint64_t step = ((uint64_t)(1024 + fns) << 22);  /* (1 + fns/1024) in 32.32 */
    step = oct >= 0 ? step << oct : step >> -oct;
    v->pos += step;

    /* envelope */
    uint16_t r10 = creg(ch, 0x10), r14 = creg(ch, 0x14);
    int ar = r10 & 31, d1r = (r10 >> 6) & 31, d2r = (r10 >> 11) & 31;
    int rr = r14 & 31, dl = (r14 >> 5) & 31;
    switch (v->eg_state) {
    case EG_ATTACK:
        v->env -= eg_step(s_ar_ms, eg_rate(ch, ar));
        if (ar == 31 || v->env <= 0) { v->env = 0; v->eg_state = EG_DECAY1; }
        break;
    case EG_DECAY1:
        v->env += eg_step(s_dr_ms, eg_rate(ch, d1r));
        if (v->env >= dl * 32.0f) { v->env = dl * 32.0f; v->eg_state = EG_DECAY2; }
        break;
    case EG_DECAY2:
        v->env += eg_step(s_dr_ms, eg_rate(ch, d2r));
        if (v->env >= 1023) v->env = 1023;
        break;
    case EG_RELEASE:
        v->env += eg_step(s_dr_ms, eg_rate(ch, rr));
        if (v->env >= 1023) {
            if (s_snd_log && ch < 8) logf_("release done ch=%d pos=%u", ch, (uint32_t)(v->pos >> 32));
            v->env = 1023; v->eg_state = EG_OFF; v->playing = 0;
        }
        break;
    }
}

static inline float db_to_amp(float db) { return powf(10.0f, -db / 20.0f); }

static void mix_sample(int16_t *out) {
    float l = 0, r = 0;
    for (int ch = 0; ch < 64; ch++) {
        Voice *v = &s_voice[ch];
        if (!v->playing) continue;
        float s = voice_sample(ch);
        if (!v->playing) continue;
        uint16_t r24 = creg(ch, 0x24), r28 = creg(ch, 0x28);
        int disdl = (r24 >> 8) & 15, dipan = r24 & 31, tl = (r28 >> 8) & 255;
        float att = s_db_table[(int)v->env] + tl * 0.375f;
        float amp = db_to_amp(att);
        if (disdl) {
            float dsend = db_to_amp((15 - disdl) * 3.0f);
            float pl = 1, pr = 1;
            float pan_att = db_to_amp((dipan & 15) * 3.0f);
            if ((dipan & 15) == 15) pan_att = 0;
            if (dipan & 16) pl = pan_att; else pr = pan_att;
            l += s * amp * dsend * pl;
            r += s * amp * dsend * pr;
        }
        voice_advance(ch);
    }
    int mvol = REG16(0x2800) & 15;
    float master = mvol ? db_to_amp((15 - mvol) * 3.0f) : 0.0f;
    l *= master * 0.5f;
    r *= master * 0.5f;
    out[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
    out[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}

static void tick_timers(void) {
    static const uint32_t regs[3] = {0x2890, 0x2894, 0x2898};
    for (int t = 0; t < 3; t++) {
        uint16_t v = REG16(regs[t]);
        uint32_t prescale = 1u << ((v >> 8) & 7);
        if (++s_timer_sub[t] < prescale) continue;
        s_timer_sub[t] = 0;
        uint32_t cnt = (v & 0xFF) + 1;
        if (cnt > 0xFF) {
            cnt = 0;
            raise_sci(1u << (6 + t));
        }
        REG16(regs[t]) = (uint16_t)((v & 0xFF00) | (cnt & 0xFF));
    }
    if (REG16(0x289C) & (1u << 10)) raise_sci(1u << 10);  /* one-sample interval */
}

/* Run the driver and synthesizer up to the current time. */
void aica_update(void) {
    if (!s_started) return;
    uint64_t target = (now_ns() - s_start_ns) * RATE / 1000000000ull;
    if (target > s_samples_done + RATE / 4) {  /* fell far behind */
        extern uint64_t g_vblank_count;
        logf_("audio: fell behind by %llu ms at vblank %llu; skipping",
              (unsigned long long)((target - s_samples_done) * 1000 / RATE), (unsigned long long)g_vblank_count);
        s_samples_done = target - RATE / 4;
    }
    static int16_t buf[2 * 2048];
    int n = 0;
    while (s_samples_done + CHUNK <= target) {
        if (s_arm_running) arm_execute(CHUNK * ARM_CYCLES_PER_SAMPLE);
        for (int i = 0; i < CHUNK; i++) {
            tick_timers();
            mix_sample(&buf[n * 2]);
            n++;
        }
        s_samples_done += CHUNK;
        if (n + CHUNK > 2048) break;
    }
    if (n && s_pcm_dump) fwrite(buf, 4, (size_t)n, s_pcm_dump);  /* debug: raw s16le stereo 44.1 kHz */
    if (n && s_stream && !g_audio_mute) {
        /* keep latency bounded: skip output if the device queue is too full */
        if (SDL_GetAudioStreamQueued(s_stream) < RATE * 4 / 10)
            SDL_PutAudioStreamData(s_stream, buf, n * 4);
    }
}

/* ---------------- register access ---------------- */

static uint32_t reg_read(uint32_t off, int size, int from_arm) {
    off &= 0x7FFF;
    uint32_t w = off & ~3u;
    switch (w) {
    case 0x2810: { /* monitor slot: LP, SGC, EG */
        int ch = (REG16(0x280C) >> 8) & 63;
        Voice *v = &s_voice[ch];
        uint32_t val = (v->lp_flag ? 0x8000u : 0) | (uint32_t)(v->eg_state > 3 ? 3 : v->eg_state) << 13 |
                       ((uint32_t)v->env & 0x1FFF);
        if (!v->playing) val |= 0x1FFF;
        v->lp_flag = 0;
        return val;
    }
    case 0x2814: { /* monitor slot: current address */
        int ch = (REG16(0x280C) >> 8) & 63;
        if (g_stream_trace > 0) { st_ca_reads++; st_ca_last = ((uint32_t)ch << 16 | ((uint32_t)(s_voice[ch].pos >> 32) & 0xFFFF)) & 0xFFFF; }
        return (uint32_t)(s_voice[ch].pos >> 32) & 0xFFFF;
    }
    case 0x28A0: return s_sci_pending;
    case 0x28B8: return s_mci_pending;
    case 0x2D00: return irq_level();
    case 0x2C00: return REG16(0x2C00);
    }
    (void)from_arm;
    uint32_t v = 0;
    memcpy(&v, s_regs + off, (size_t)size);
    return v;
}

static void reg_write(uint32_t off, uint32_t v, int size, int from_arm) {
    off &= 0x7FFF;
    uint32_t w = off & ~3u;
    if (s_snd_log && (w == 0x2890 || w == 0x2894 || w == 0x2898)) {
        static int n;
        if (n++ < 40) logf_("timer write %04X = %04X size %d (%s)", off, v, size, from_arm ? "arm" : "sh4");
    }
    if (w < 0x2000 && (w & 0x7F) == 0 && size >= 2 && (v & 0x8000)) {
        memcpy(s_regs + off, &v, (size_t)size);
        REG16(w) &= 0x7FFF;
        key_exec();
        return;
    }
    switch (w) {
    case 0x28A0: /* SCIPD: only the CPU interrupt bit can be set by writing */
        if (v & (1u << 5)) { g_aica_arm_irqs++; raise_sci(1u << 5); }
        return;
    case 0x28A4: /* SCIRE: clear ARM-side pending */
        s_sci_pending &= ~v;
        update_irqs();
        return;
    case 0x28B8: /* MCIPD: ARM -> SH-4 interrupt */
        if (v & (1u << 5)) { g_aica_sh4_irqs++; s_mci_pending |= 1u << 5; update_irqs(); }
        return;
    case 0x28BC: /* MCIRE */
        s_mci_pending &= ~v;
        update_irqs();
        return;
    case 0x2D04: /* INTClear */
        update_irqs();
        return;
    case 0x2C00: {
        uint32_t old = REG16(0x2C00);
        memcpy(s_regs + off, &v, (size_t)size);
        if ((old & 1) && !(REG16(0x2C00) & 1)) {
            arm_reset();
            s_arm_running = 1;
            uint32_t sum = 0;
            for (uint32_t i = 0; i + 4 <= g_arm_code_size; i += 4) {
                uint32_t w;
                memcpy(&w, g_aica_ram + i, 4);
                sum += w;
            }
            logf_("aica: ARM released from reset; driver in sound RAM %s the recompiled one",
                  sum == g_arm_code_sum ? "matches" : "DOES NOT match");
        } else if (REG16(0x2C00) & 1) {
            s_arm_running = 0;
        }
        return;
    }
    }
    memcpy(s_regs + off, &v, (size_t)size);
    if (w == 0x289C || w == 0x28B4) update_irqs();
    (void)from_arm;
}

uint32_t aica_reg_read(uint32_t off, int size) { return reg_read(off, size, 0); }
void aica_reg_write(uint32_t off, uint32_t v, int size) { reg_write(off, v, size, 0); }
uint32_t aica_arm_reg_read(uint32_t off, int size) { return reg_read(off, size, 1); }
void aica_arm_reg_write(uint32_t off, uint32_t v, int size) { reg_write(off, v, size, 1); }

/* RTC: seconds since 1950-01-01, split in two 16-bit registers */
uint32_t aica_rtc_read(uint32_t p) {
    /* deterministic mode: a fixed start date (2000-01-01) plus guest time, so
     * anything the game derives from the clock matches on every machine */
    uint32_t secs = g_det ? (uint32_t)(946684800u + 631152000u + host_time_ns() / 1000000000ull)
                          : (uint32_t)(time(NULL) + 631152000u);
    if ((p & 0xF) == 0) return secs >> 16;
    if ((p & 0xF) == 4) return secs & 0xFFFF;
    return 0;
}

void aica_dma_start(void) {
    /* G2 DMA between main RAM and sound RAM */
    uint32_t aica = holly_reg(0x7800), sh4 = holly_reg(0x7804), len = holly_reg(0x7808) & 0x7FFFFFFF;
    uint32_t dir = holly_reg(0x780C);
    if (g_stream_trace > 0) { st_dma += len; if (!dir) for (uint32_t i = 0; i < len; i += 4) stream_trace_write((aica + i) & 0x1FFFFF, 4); }
    for (uint32_t i = 0; i < len; i += 4) {
        if (dir) W32(sh4 + i, R32(aica + i));
        else W32(aica + i, R32(sh4 + i));
    }
    holly_raise(0, 1u << 15);  /* AICA DMA end */
}

/* Sound chip and sound CPU at power-on (in-place machine reset). */
void aica_reset(void) {
    memset(s_regs, 0, sizeof s_regs);
    memset(s_voice, 0, sizeof s_voice);
    REG16(0x2C00) = 1;
    for (int ch = 0; ch < 64; ch++) {
        s_voice[ch].eg_state = EG_OFF;
        s_voice[ch].env = 1023;
    }
    s_arm_running = 0;
    s_sci_pending = s_mci_pending = 0;
    memset(s_timer_sub, 0, sizeof s_timer_sub);
    s_samples_done = 0;
    s_start_ns = now_ns();
    memset(&A, 0, sizeof A);
    if (s_stream) SDL_ClearAudioStream(s_stream);
}

void aica_init(void) {
    for (int i = 0; i < 1024; i++) s_db_table[i] = i * (96.0f / 1024.0f);
    s_db_table[1023] = 200.0f;
    REG16(0x2C00) = 1;  /* ARM held in reset at boot */
    for (int ch = 0; ch < 64; ch++) {  /* idle voices sit in release at silence, as on hardware */
        s_voice[ch].eg_state = EG_OFF;
        s_voice[ch].env = 1023;
    }
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, RATE};
    s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (s_stream) SDL_ResumeAudioStreamDevice(s_stream);
    else logf_("audio: %s", SDL_GetError());
    s_snd_log = getenv("SFA3_KEYON_LOG") != NULL;
    if (getenv("SFA3_PCM_DUMP")) s_pcm_dump = fopen(getenv("SFA3_PCM_DUMP"), "wb");
    s_start_ns = now_ns();  /* 0 in deterministic mode, hence the separate flag */
    s_started = 1;
}
