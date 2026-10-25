/* SFA3 native runtime entry point. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  /* Windows: the WinMain that calls main() */
#include <signal.h>
#include <stdarg.h>
#ifdef _WIN32
__declspec(dllimport) void *__stdcall GetCurrentProcess(void);
__declspec(dllimport) int __stdcall TerminateProcess(void *process, unsigned code);
/* no backtrace(): the guest call stack in crash logs is Linux-only for now */
static int backtrace(void **frames, int n) { (void)frames; (void)n; return 0; }
static char **backtrace_symbols(void *const *frames, int n) { (void)frames; (void)n; return NULL; }
static int setenv(const char *name, const char *value, int overwrite) { (void)overwrite; return _putenv_s(name, value); }
#else
#include <execinfo.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cheats.h"
#include "chartables.h"
#include "cpu.h"
#include "input.h"
#include "pvr.h"
#include "runtime.h"
#include "menu.h"
#include "options.h"
#include "settings.h"
#include "netplay.h"
#include "fiber.h"
#include "lobby.h"

int g_trace;
extern int g_quit_requested;
static FILE *s_log;

void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_log ? s_log : stderr, fmt, ap);
    va_end(ap);
    fputc('\n', s_log ? s_log : stderr);
    if (s_log) fflush(s_log);
}

_Noreturn void fatal(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    logf_("FATAL: %s", msg);
    fprintf(stderr, "FATAL: %s\n", msg);
    fprintf(stderr, "  r0-r7: %08X %08X %08X %08X %08X %08X %08X %08X\n", g.r[0], g.r[1], g.r[2],
            g.r[3], g.r[4], g.r[5], g.r[6], g.r[7]);
    fprintf(stderr, "  r8-15: %08X %08X %08X %08X %08X %08X %08X %08X\n", g.r[8], g.r[9], g.r[10],
            g.r[11], g.r[12], g.r[13], g.r[14], g.r[15]);
    fprintf(stderr, "  pr=%08X sr=%08X vbr=%08X\n", g.pr, sh4_get_sr(), g.vbr);
    maple_flush_saves();
    exit(1);
}

uint8_t *g_main_image;  /* pristine copy of 1ST_READ.BIN */
uint32_t g_main_image_size;
static uint8_t s_ip_bin[0x8000];

static size_t load_file(const char *path, uint32_t addr) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open %s", path);
    /* ask for exactly the file's size: Windows fails the whole read (0 bytes) if the
     * buffer range asked for runs past the end of the 16 MB RAM block */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t room = (16u << 20) - (addr & 0x00FFFFFFu);
    if (size <= 0 || (size_t)size > room) fatal("%s: bad size %ld", path, size);
    size_t n = fread(ram_ptr(addr), 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) fatal("%s: read %zu of %ld bytes", path, n, size);
    logf_("loaded %s: %zu bytes at %08X", path, n, addr);
    return n;
}

SDL_Window *g_window;
#define s_window g_window

/* sfa3.cfg: "key = value" lines; settings are exported as SFA3_<KEY>
 * environment variables (already-set environment variables win). */
static uint64_t s_frames;

/* Sync testing (deterministic mode): SFA3_HASH_LOG=<file> writes a hash of the
 * whole guest state at every vblank, so two runs can be compared frame by frame. */
extern uint64_t g_vblank_count;
static uint64_t hash_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, b + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static void state_hash_frame(void) {
    static FILE *f;
    static int init;
    if (!init) {
        init = 1;
        const char *p = getenv("SFA3_HASH_LOG");
        if (p) f = fopen(p, "w");
    }
    if (!f) return;
    extern uint8_t *g_vram, *g_aica_ram;
    uint64_t h_ram = hash_bytes(0, g_ram, 16 << 20);
    uint64_t h_vram = hash_bytes(0, g_vram, 8 << 20);
    uint64_t h_snd = hash_bytes(0, g_aica_ram, 2 << 20);
    uint64_t h_cpu = hash_bytes(0, &g, sizeof g);
    fprintf(f, "%llu %016llx %016llx %016llx %016llx\n", (unsigned long long)g_vblank_count,
            (unsigned long long)h_ram, (unsigned long long)h_vram, (unsigned long long)h_snd,
            (unsigned long long)h_cpu);
    if (g_vblank_count % 60 == 0) fflush(f);
}

/* ---------------- in-place machine reset ----------------
 * Netplay sessions start (and end) from a fresh boot on both sides without
 * restarting the program: every guest-visible part of the machine returns to
 * its power-on state and the game boots again in the same window. Requested
 * from anywhere; performed at the end of the current vblank. */
static void *s_reset_jb[5];
static int s_reset_det = -1;  /* pending reset: deterministic mode afterwards (0/1), or -1 */

void machine_reset_request(int det) { s_reset_det = det; }

void irq_reset(int det);
void mmio_reset(void);
void pvr_reset(void);
void ta_power_reset(void);
void aica_reset(void);
void hle_reset(void);
void task_reset(void);
void sjlj_reset(void);
void renderer_vram_block_written(uint32_t off64, uint32_t bytes);

static void cpu_power_on(void) {
    /* CPU state as left by the boot ROM when it jumps to 1ST_READ.BIN */
    memset(&g, 0, sizeof g);
    g.sr_rest = 0x600000F0u;  /* MD=1, RB=1, IMASK=15 */
    g.r[15] = 0x8C00F400u;
    g.vbr = 0x8C00F400u;
    g.fpscr = 0x00040001u;
    g.pr = 0x8C00E000u;
}

static _Noreturn void machine_reset_now(void) {
    maple_flush_saves();
    if (fiber_current()) {
        /* leave the task's fiber; the host thread lands in main() */
        fiber_set_pending_jump(NULL, s_reset_jb);
        fiber_switch(NULL);
    }
    __builtin_longjmp(s_reset_jb, 1);
}

static void machine_reset_state(int det) {
    irq_reset(det);
    mmio_reset();
    pvr_reset();
    ta_power_reset();
    aica_reset();
    task_reset();
    sjlj_reset();
    fiber_kill_all();
    memcpy(ram_ptr(0x8C008000), s_ip_bin, sizeof s_ip_bin);
    memcpy(ram_ptr(0x8C010000), g_main_image, g_main_image_size);
    char_tables_init();
    hle_reset();
    renderer_vram_block_written(0, 8u << 20);  /* every cached texture is stale */
    cpu_power_on();
    lobby_on_reset();
    netplay_on_reset();
    logf_("machine reset (%s)", det ? "deterministic" : "real time");
}

/* Called on every vblank-in edge. */
static void dump_state(const char *why) {
    logf_("%s: pr=%08X r15=%08X sr=%08X", why, g.pr, g.r[15], sh4_get_sr());
    char buf[256];
    int n = 0;
    for (unsigned i = 0; i < 16; i++)
        n += snprintf(buf + n, sizeof buf - (size_t)n, " %08X",
                      g_stats.last_indirect[(g_stats.last_idx + i) & 15]);
    logf_("  recent indirect calls:%s", buf);
    /* optional memory watch list from SFA3_WATCH="addr,addr,..." */
    const char *w = getenv("SFA3_WATCH");
    while (w && *w) {
        uint32_t a = (uint32_t)strtoul(w, (char **)&w, 16);
        logf_("  [%08X] = %08X %08X %08X %08X", a, R32(a), R32(a + 4), R32(a + 8), R32(a + 12));
        if (*w == ',') w++;
    }
}

uint32_t g_watch_ram = 0xFFFFFFFFu, g_watch_len = 1;  /* SFA3_WATCH_WRITE=addr[,bytes] */
void watch_hit(uint32_t a, uint32_t v, int size) {
    static int n;
    static long long from = -1;  /* SFA3_WATCH_FROM=<frame>: ignore writes before it */
    if (from < 0) from = getenv("SFA3_WATCH_FROM") ? atoll(getenv("SFA3_WATCH_FROM")) : 0;
    if ((long long)g_vblank_count < from) return;
    if (n++ > 400) return;
    void *frames[16];
    int k = backtrace(frames, 16);
    char **names = backtrace_symbols(frames, k);
    char buf[512] = "";
    int len = 0;
    for (int i = 1; i < k && names && len < 400; i++) {
        const char *f = strstr(names[i], "(f_");
        if (!f) f = strstr(names[i], "(p_");  /* PSP character code */
        if (f) len += snprintf(buf + len, sizeof buf - (size_t)len, " %.12s", f + 1);
    }
    free(names);
    logf_("watch: write%d %08X = %X at frame %llu by%s", size * 8, a, v, (unsigned long long)g_vblank_count, buf);
}

static void on_signal(int sig) {
    dump_state(sig == SIGTERM ? "SIGTERM" : "SIGINT");
    /* recompiled functions are named after guest addresses, so the host
     * backtrace doubles as the guest call stack */
    void *frames[64];
    int n = backtrace(frames, 64);
    char **names = backtrace_symbols(frames, n);
    for (int i = 0; i < n && names; i++) {
        const char *f = strstr(names[i], "(f_");
        if (f) logf_("  guest frame: %.40s", f + 1);
    }
    maple_flush_saves();
    _exit(1);
}

Perf g_perf;

void video_vblank(void) {
    s_frames++;
    g_stats.vblanks++;
    cheats_frame();
    {  /* SFA3_FORCE_CHAR=<player>:<hex id>: keep a fighter's character number (testing, debug) */
        static int player = -2, id;
        if (player == -2) {
            const char *e = getenv("SFA3_FORCE_CHAR");
            player = e ? atoi(e) : -1;
            id = e && strchr(e, ':') ? (int)strtol(strchr(e, ':') + 1, NULL, 16) : 0;
        }
        if (player >= 0 && player < 4) W8(0x8C17FD38u + 0x44Cu * (uint32_t)player + 0x132u, (uint32_t)id);
    }
    menu_tag(cheats_active() ? "CHEATS ON" : "");
    if (s_frames % 60 == 0) {
        static uint64_t last;
        uint64_t t = SDL_GetTicksNS(), wall = last ? t - last : 0;
        last = t;
        uint64_t busy = g_perf.draw_ns + g_perf.present_ns + g_perf.idle_ns;
        logf_("perf: wall=%.0fms draw=%.1f present=%.1f (gpu wait %.1f) tex=%llu/%.1fms idle=%.1f game=%.1f",
              wall / 1e6, g_perf.draw_ns / 1e6, g_perf.present_ns / 1e6, g_perf.gpu_wait_ns / 1e6,
              (unsigned long long)g_perf.tex_decodes, g_perf.tex_ns / 1e6, g_perf.idle_ns / 1e6,
              wall > busy ? (wall - busy) / 1e6 : 0.0);
        memset(&g_perf, 0, sizeof g_perf);
        logf_("stats: vbl=%llu irq=%llu render=%llu ta=%llu gd=%llu maple=%llu icall=%llu",
              (unsigned long long)g_stats.vblanks, (unsigned long long)g_stats.irqs,
              (unsigned long long)g_stats.renders, (unsigned long long)g_stats.ta_words,
              (unsigned long long)g_stats.gd_sectors, (unsigned long long)g_stats.maple_dmas,
              (unsigned long long)g_stats.indirect_calls);
        extern uint64_t g_aica_keyons, g_aica_fiqs, g_aica_sh4_irqs, g_aica_arm_irqs;
        int aica_active_voices(void);
        void aica_log_sources(void);
        if (getenv("SFA3_KEYON_LOG")) aica_log_sources();
        logf_("audio: keyons=%llu fiqs=%llu sh4->arm=%llu arm->sh4=%llu voices=%d",
              (unsigned long long)g_aica_keyons, (unsigned long long)g_aica_fiqs,
              (unsigned long long)g_aica_arm_irqs, (unsigned long long)g_aica_sh4_irqs, aica_active_voices());
    }
    if (g_det) {
        state_hash_frame();
        if (!netplay_fast_forward()) det_pace();
        /* SFA3_EXIT_AT=<frame>: stop cleanly at a given vblank (sync tests) */
        static long exit_at = -2;
        if (exit_at == -2) exit_at = getenv("SFA3_EXIT_AT") ? atol(getenv("SFA3_EXIT_AT")) : -1;
        if (exit_at > 0 && (long)g_vblank_count >= exit_at) g_quit_requested = 1;
    }
    {
        extern int g_stream_trace;
        void stream_trace_frame(void);
        if (g_stream_trace < 0) g_stream_trace = getenv("SFA3_STREAM_LOG") != NULL;
        if (g_stream_trace) stream_trace_frame();
    }
    /* a fast-forward (netplay reset) keeps the last picture on screen */
    if (!netplay_fast_forward()) {
        uint64_t t0 = SDL_GetTicksNS();
        renderer_present_fb();
        g_perf.present_ns += SDL_GetTicksNS() - t0;
    }
    input_update();
    netplay_tick();
    lobby_autopilot(input_state(0));
    if (netplay_fast_forward())  /* identical on both sides: only the autopilot's controller */
        for (int p = 1; p < 4; p++) *input_state(p) = (PadState){0, 0, 0, 0x80, 0x80};
    netplay_frame();
    lobby_cpu_player();
    lobby_boot_skip();
    if (menu_active() && !netplay_active()) {  /* a session can't pause: the other player would wait */
        /* paused: keep presenting the last picture with the menu over it; the
         * guest clocks skip the paused time so the game doesn't see a jump */
        uint64_t t0 = SDL_GetTicksNS();
        while (menu_active() && !g_quit_requested) {
            renderer_present_fb();
            input_update();
            SDL_Delay(16);
        }
        host_pause_add(SDL_GetTicksNS() - t0);
    }
    {
        /* SFA3_RESET_AT=<frame>: test the in-place reset (deterministic boot
         * afterwards; compare SFA3_HASH_LOG with a fresh deterministic run) */
        static long reset_at = -2;
        if (reset_at == -2) reset_at = getenv("SFA3_RESET_AT") ? atol(getenv("SFA3_RESET_AT")) : -1;
        if (reset_at > 0 && (long)g_vblank_count >= reset_at) {
            reset_at = -1;
            machine_reset_request(1);
        }
    }
    if (s_reset_det >= 0) machine_reset_now();
    if ((s_frames % 600) == 0) maple_flush_saves();
    if (g_quit_requested) {
        netplay_shutdown();
        maple_flush_saves();
#ifdef _WIN32
        /* exit() from inside the running game (on a fiber, with the graphics
         * driver's and SDL's threads live) can hang in DLL shutdown on
         * Windows; saves and the log are written, so just end the process */
        TerminateProcess(GetCurrentProcess(), 0);
#endif
        exit(0);
    }
}

/* Requests a periodic IRQ_CHECK so timer edges are noticed in busy loops. */
static Uint32 poll_timer(void *ud, SDL_TimerID id, Uint32 interval) {
    (void)ud; (void)id;
    if (!g_det) g_irq_pending = 1;  /* deterministic mode must not see host timing */
    return interval;
}

void f_8C010000(void);
void aica_init(void);

const char *g_data_dir = "build";
const char *g_instance;  /* --instance: tag for log and screenshot names (two copies on one PC) */

/* The data folder (disc_path.txt, the extracted disc, saves, log; sfa3.cfg
 * one level up) when --data isn't given: build/ in the current folder (a
 * source checkout), else data/ next to the executable (an installed copy),
 * else the folder above it (build/game/sfa3, build/cmake/sfa3). */
static const char *default_data_dir(void) {
    static char dir[1024];
    FILE *f = fopen("build/disc_path.txt", "r");
    if (f) { fclose(f); return "build"; }
    const char *base = SDL_GetBasePath();  /* the executable's folder, with a trailing separator */
    if (base) {
        static const char *const tries[] = {"data", ".."};
        for (int i = 0; i < 2; i++) {
            char probe[1100];
            snprintf(dir, sizeof dir, "%s%s", base, tries[i]);
            snprintf(probe, sizeof probe, "%s/disc_path.txt", dir);
            if ((f = fopen(probe, "r"))) { fclose(f); return dir; }
        }
    }
    return "build";
}

int main(int argc, char **argv) {
    const char *data_dir = NULL;
    const char *gdi = NULL;
    const char *join = NULL, *instance = NULL;
    int host = 0, net_delay = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) g_trace = 1;
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data_dir = argv[++i];
        else if (!strcmp(argv[i], "--host")) host = 1;
        else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc) net_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) setenv("SFA3_NAME", argv[++i], 1);
        else if (!strcmp(argv[i], "--instance") && i + 1 < argc) g_instance = instance = argv[++i];  /* separate log (testing) */
        else gdi = argv[i];
    }
    if (!data_dir) data_dir = default_data_dir();
    g_data_dir = data_dir;
    char path[1024];
    if (!gdi) {
        /* setup records the user's disc image location */
        snprintf(path, sizeof path, "%s/disc_path.txt", data_dir);
        FILE *f = fopen(path, "r");
        static char gdi_buf[1024];
        if (!f || !fgets(gdi_buf, sizeof gdi_buf, f)) {
            fprintf(stderr, "usage: sfa3 [path/to/disc.gdi]  (or run ./sfa3port.py setup first)\n");
            return 1;
        }
        fclose(f);
        gdi_buf[strcspn(gdi_buf, "\r\n")] = 0;
        gdi = gdi_buf;
    }
    if (instance) snprintf(path, sizeof path, "%s/sfa3_%s.log", data_dir, instance);
    else snprintf(path, sizeof path, "%s/sfa3.log", data_dir);
    s_log = fopen(path, "w");
    snprintf(path, sizeof path, "%s/../sfa3.cfg", data_dir);
    settings_load(path);
    if (getenv("SFA3_WATCH_WRITE")) {
        char *end;
        g_watch_ram = (uint32_t)strtoul(getenv("SFA3_WATCH_WRITE"), &end, 16) & 0xFFFFFFu;
        if (*end == ',') g_watch_len = (uint32_t)strtoul(end + 1, NULL, 16);
    }
    if (getenv("SFA3_DETERMINISTIC") && atoi(getenv("SFA3_DETERMINISTIC"))) irq_det_init();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) fatal("SDL_Init: %s", SDL_GetError());
    s_window = SDL_CreateWindow("Street Fighter Alpha 3", 640, 480,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!s_window) fatal("SDL_CreateWindow: %s", SDL_GetError());
    options_apply_window();
    if (!renderer_init(s_window)) fatal("renderer init failed");
    input_init();

    mmio_init();
    dispatch_init();
    snprintf(path, sizeof path, "%s/disc/IP.BIN", data_dir);
    load_file(path, 0x8C008000);
    memcpy(s_ip_bin, ram_ptr(0x8C008000), sizeof s_ip_bin);
    snprintf(path, sizeof path, "%s/disc/1ST_READ.BIN", data_dir);
    g_main_image_size = (uint32_t)load_file(path, 0x8C010000);
    g_main_image = malloc(g_main_image_size);
    memcpy(g_main_image, ram_ptr(0x8C010000), g_main_image_size);
    char_tables_init();
    hle_init(gdi);
    maple_init(data_dir);
    aica_init();
    /* --host / --join: boot straight to the NETWORK lobby and host or join there */
    netplay_cli(host, join, net_delay);

    cpu_power_on();

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    /* real-time mode: a host timer requests the periodic IRQ_CHECK slow path
     * (deterministic mode ticks from IRQ_CHECK itself, and ignores this) */
    SDL_AddTimer(1, poll_timer, NULL);
    if (__builtin_setjmp(s_reset_jb)) machine_reset_state(s_reset_det);
    s_reset_det = -1;
    f_8C010000();
    fatal("main program returned");
}
