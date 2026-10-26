/* Netplay: LAN discovery, hosting and joining from the NETWORK lobby, and
 * delay-based lockstep sessions.
 *
 * Offline, the lobby hosts (listening on NETPLAY_PORT and answering LAN
 * discovery broadcasts) or joins a game. When a player joins, the two sides
 * measure their round trip, agree on an input delay and copy the host's
 * memory card to the guest; then both games reset in place (main.c) to a
 * fresh deterministic boot (irq.c). The boot is fast-forwarded without the
 * network: the lobby's autopilot drives it to the NETWORK screen, which both
 * games reach on the same frame. From there the session runs in lockstep:
 * each side's controller input for frame F is captured at frame F - delay and
 * sent to the other side (with the previous few frames repeated, so a lost
 * packet costs nothing), and frame F only runs once both inputs are known.
 * The host is player 1 (port A), the guest player 2 (port B).
 *
 * Everything shown in the session lobby is part of the synced game, so values
 * that differ per side (the measured ping) travel inside the host's input.
 * When the session ends (a player leaves, the host removes the guest, or the
 * connection is lost), both games reset again and fast-forward back to the
 * NETWORK lobby with each player's own memory card.
 *
 * Every SYNC_EVERY frames both sides hash the game's RAM and compare, so a
 * desync is caught at once. Latency statistics are logged every second. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chartables.h"
#include "cpu.h"
#include "input.h"
#include "lobby.h"
#include "menu.h"
#include "net.h"
#include "netplay.h"
#include "pvr.h"
#include "runtime.h"
#include "settings.h"

#define PROTOCOL 2
#define RING 256
#define MAX_SEND 16       /* inputs per packet (the newest MAX_SEND frames) */
#define SYNC_EVERY 60
#define SESSION_TIMEOUT_NS 5000000000ull
#define SETUP_TIMEOUT_NS 10000000000ull
#define VMU_SIZE (128 * 1024)

enum {
    PKT_DISCOVER = 1, PKT_HOST_INFO, PKT_HELLO, PKT_REFUSE, PKT_PING, PKT_PONG,
    PKT_VMU, PKT_VMU_ACK, PKT_START, PKT_START_ACK, PKT_INPUT, PKT_BYE,
};
enum { REFUSE_VERSION = 1, REFUSE_BUSY };
enum { PH_OFFLINE, PH_SYNC, PH_LOCKSTEP, PH_RETURN };

typedef struct {
    uint8_t type, arg, pad[2];
    uint32_t build, session;
    uint64_t t;
    char name[16];
} Msg;

typedef struct {
    uint16_t buttons;
    uint8_t lt, rt, ax, ay;
    uint8_t aux;  /* host: round trip in ms, up to 255 (shown in the lobby) */
    uint8_t flags;  /* FLAG_* requests, acted on by both sides at this frame */
    uint16_t buttons2;  /* a second controller on this machine (FLAG_PAD2): a third player */
    uint8_t lt2, rt2, ax2, ay2;
} Input7;
#define FLAG_LOBBY 1
#define FLAG_PAD2 2

extern uint64_t g_vblank_count;
extern uint8_t *g_main_image;
extern uint32_t g_main_image_size;
extern int g_audio_mute;
uint8_t *maple_vmu_image(void);
void maple_vmu_use_session_image(const uint8_t *img);
void maple_vmu_restore_own(void);
void irq_det_init(void);
void irq_leave_det(void);

static int s_phase;
static uint32_t s_build;
static uint64_t s_game_id;         /* this running game, so a host found twice is listed once */
static uint64_t s_reset_t0;        /* wall clock of the last netplay reset (logging) */
static int s_cli_delay;

/* offline */
static int s_listen;               /* hosting: accepting a player */
static int s_browse;
static NetGame s_games[8];
static int s_ngames;
static uint64_t s_last_discover;
static int s_join;
static NetAddr s_join_addr;
static uint64_t s_join_t0, s_join_last;
static char s_error[64];
static int s_landing;
static char s_landing_msg[64];

/* the session */
static struct {
    int host, delay;
    NetAddr peer;
    uint32_t id;
    char names[2][16];
    double setup_rtt;
} s_sess;
static uint8_t s_sess_vmu[VMU_SIZE];

/* lockstep */
static int64_t s_start = -1;       /* first lockstep frame */
static Input7 s_local[RING], s_remote[RING];
static int64_t s_local_max, s_remote_max;  /* newest frame known */
static uint64_t s_last_send_ns, s_last_recv_ns;
static int s_synced_ping;          /* host's round trip, from the synced input */
static int s_flags_pending;        /* FLAG_* to send with this side's next input */
static int s_third;                /* synced: whose second controller is the third player (1 host, 2 guest) */

/* sync check */
static uint64_t s_my_hash[RING / SYNC_EVERY + 4], s_their_hash[RING / SYNC_EVERY + 4];
static int64_t s_my_hash_frame, s_their_hash_frame, s_checked_frame;
static int s_desyncs;

/* statistics */
static uint64_t s_ping_sent_ns;
static double s_rtt_ms, s_rtt_sum;
static int s_rtt_n;
static uint64_t s_stall_ns, s_stall_max_ns;
static int s_stalls, s_margin_min, s_margin_sum, s_margin_n;
static uint64_t s_last_ping_seen, s_last_ping_seen_at;
static int s_my_margin16, s_their_margin16;  /* smoothed input margins, frames x16 */
static char s_status[160];

int netplay_active(void) { return s_phase == PH_SYNC || s_phase == PH_LOCKSTEP; }
int netplay_is_host(void) { return s_sess.host; }
const char *netplay_player_name(int player) { return s_sess.names[player & 1]; }
int netplay_ping_ms(void) { return s_synced_ping; }
int netplay_third_player(void) { return netplay_active() ? s_third : 0; }
int netplay_fast_forward(void) { return s_phase == PH_SYNC || s_phase == PH_RETURN; }
const char *netplay_status(void) { return s_phase == PH_LOCKSTEP ? s_status : NULL; }
int netplay_hosting(void) { return s_listen; }
int netplay_joining(void) { return s_join; }

const char *netplay_take_error(void) {
    static char e[64];
    if (!s_error[0]) return NULL;
    snprintf(e, sizeof e, "%s", s_error);
    s_error[0] = 0;
    return e;
}

int netplay_take_landing(char *msg, int cap) {
    int l = s_landing;
    snprintf(msg, (size_t)cap, "%s", s_landing_msg);
    s_landing = LAND_NONE;
    s_landing_msg[0] = 0;
    return l;
}

static void set_landing(int where, const char *msg) {
    s_landing = where;
    snprintf(s_landing_msg, sizeof s_landing_msg, "%s", msg ? msg : "");
}

static uint32_t build_id(void) {
    if (s_build) return s_build;
    uint32_t h = 2166136261u ^ PROTOCOL;
    for (uint32_t i = 0; i < g_main_image_size; i += 64) h = (h ^ g_main_image[i]) * 16777619u;
    /* the PSP characters add-on changes the game (its select screens, tables): a game with it only
     * plays one with the same add-on */
    for (uint32_t i = 0; i < g_psp_char_data_size; i++) h = (h ^ g_psp_char_data[i]) * 16777619u;
    return s_build = h ? h : 1;
}

static uint64_t ram_hash(void) {
    uint64_t h = 0;
    const uint8_t *b = g_ram;
    for (size_t i = 0; i < (16u << 20); i += 8) {
        uint64_t v;
        memcpy(&v, b + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}

static uint64_t now(void) { return SDL_GetTicksNS(); }
static void wait_ms(int ms) { SDL_DelayNS((uint64_t)ms * 1000000ull); }
static int same_addr(const NetAddr *a, const NetAddr *b) { return a->ip == b->ip && a->port == b->port; }

static int quit_requested(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) return 1;
    return 0;
}

/* a notice over the current picture; it stays up through a fast-forward,
 * which presents nothing */
static void banner(const char *text) {
    menu_banner(text);
    renderer_present_fb();
}

static void send_msg(const NetAddr *to, int type, int arg, uint32_t session, uint64_t t) {
    Msg m = {.type = (uint8_t)type, .arg = (uint8_t)arg, .build = build_id(), .session = session, .t = t};
    snprintf(m.name, sizeof m.name, "%s", g_settings.name);
    net_send(to, &m, sizeof m);
}

static int recv_msg(uint8_t *buf, int cap, NetAddr *from) {
    int n = net_recv(buf, (size_t)cap, from);
    if (n >= (int)sizeof(Msg) && buf[0] != PKT_VMU && buf[0] != PKT_INPUT) ((Msg *)buf)->name[15] = 0;
    return n;
}

/* ---------------- hosting / browsing / joining ---------------- */

int netplay_host_start(void) {
    if (s_listen) return 1;
    netplay_join_cancel();
    s_browse = 0;
    if (!net_open(NETPLAY_PORT)) {
        snprintf(s_error, sizeof s_error, "PORT %d IS IN USE", NETPLAY_PORT);
        return 0;
    }
    s_listen = 1;
    logf_("netplay: hosting on UDP port %d as %s", NETPLAY_PORT, g_settings.name);
    return 1;
}

void netplay_host_stop(void) {
    if (!s_listen) return;
    s_listen = 0;
    net_close();
    logf_("netplay: stopped hosting");
}

static int open_client_socket(void) {
    if (s_listen) netplay_host_stop();
    return net_is_open() || net_open(0);
}

void netplay_browse(int on) {
    if (on && !open_client_socket()) return;
    if (on && !s_browse) {
        s_ngames = 0;
        s_last_discover = 0;
    }
    s_browse = on;
}

int netplay_games(const NetGame **list) {
    *list = s_games;
    return s_ngames;
}

int netplay_join(const char *addr) {
    char host[256];
    snprintf(host, sizeof host, "%s", addr);
    uint16_t port = NETPLAY_PORT;
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = 0;
        port = (uint16_t)atoi(colon + 1);
    }
    if (!open_client_socket()) {
        snprintf(s_error, sizeof s_error, "NETWORK UNAVAILABLE");
        return 0;
    }
    if (!net_resolve(host, port ? port : NETPLAY_PORT, &s_join_addr)) {
        snprintf(s_error, sizeof s_error, "ADDRESS NOT FOUND");
        return 0;
    }
    s_join = 1;
    s_join_t0 = now();
    s_join_last = 0;
    logf_("netplay: joining %s", net_addr_str(&s_join_addr));
    return 1;
}

void netplay_join_cancel(void) { s_join = 0; }

static void join_failed(const char *why) {
    s_join = 0;
    snprintf(s_error, sizeof s_error, "%s", why);
    logf_("netplay: join failed: %s", why);
}

static void session_prepare(int host, int delay, const NetAddr *peer, uint32_t id, const char *p1, const char *p2,
                            double rtt);

/* Host: a player said hello. Measure the round trip, send the memory card
 * and start the session (blocking: well under a second on a LAN). */
static void setup_host(const NetAddr *peer, const char *guest_name) {
    uint32_t id = (uint32_t)(now() ^ (peer->ip * 2654435761u) ^ peer->port) | 1;
    logf_("netplay: %s (%s) is joining", guest_name, net_addr_str(peer));
    char joining[32];
    snprintf(joining, sizeof joining, "%.10s IS JOINING", guest_name);
    banner(joining);
    maple_flush_saves();
    memcpy(s_sess_vmu, maple_vmu_image(), VMU_SIZE);
    uint8_t buf[1500];
    NetAddr from;
    double best = 1e9;
    for (int k = 0; k < 6; k++) {
        uint64_t t = now();
        send_msg(peer, PKT_PING, 0, id, t);
        while (now() - t < 500000000ull) {
            int n = recv_msg(buf, sizeof buf, &from);
            if (n >= (int)sizeof(Msg) && buf[0] == PKT_PONG && same_addr(&from, peer) && ((Msg *)buf)->t == t) {
                double ms = (now() - t) / 1e6;
                if (ms < best) best = ms;
                break;
            }
            if (!n) wait_ms(1);
        }
    }
    if (best >= 1e9) {
        snprintf(s_error, sizeof s_error, "%.10s DID NOT ANSWER", guest_name);
        menu_banner("");
        return;
    }
    /* enough frames to cover a one-way trip, plus headroom */
    int delay = s_cli_delay > 0 ? s_cli_delay : (int)(best / 2 / 16.68) + 2;
    if (delay > 8) delay = 8;
    logf_("netplay: round trip %.1f ms, input delay %d frames", best, delay);

    /* the memory card, in 1 KB chunks until the guest has all of them */
    uint8_t have[128] = {0};
    uint64_t t0 = now();
    for (;;) {
        int missing = 0;
        for (int c = 0; c < 128; c++) {
            if (have[c]) continue;
            missing++;
            uint8_t pkt[8 + 1024] = {PKT_VMU, (uint8_t)c};
            memcpy(pkt + 4, &id, 4);
            memcpy(pkt + 8, s_sess_vmu + c * 1024, 1024);
            net_send(peer, pkt, sizeof pkt);
        }
        if (!missing) break;
        uint64_t until = now() + 100000000ull;
        while (now() < until) {
            int n = net_recv(buf, sizeof buf, &from);
            if (n >= 2 + 128 && buf[0] == PKT_VMU_ACK && same_addr(&from, peer)) memcpy(have, buf + 2, 128);
            else if (!n) wait_ms(1);
        }
        if (now() - t0 > SETUP_TIMEOUT_NS || quit_requested()) {
            snprintf(s_error, sizeof s_error, "CONNECTION FAILED");
            menu_banner("");
            return;
        }
    }
    /* go: the guest answers START so neither side starts alone */
    t0 = now();
    for (int acked = 0; !acked && now() - t0 < 2000000000ull;) {
        send_msg(peer, PKT_START, delay, id, 0);
        uint64_t until = now() + 20000000ull;
        while (!acked && now() < until) {
            int n = recv_msg(buf, sizeof buf, &from);
            if (n >= (int)sizeof(Msg) && buf[0] == PKT_START_ACK && same_addr(&from, peer)) acked = 1;
            else if (!n) wait_ms(1);
        }
    }
    session_prepare(1, delay, peer, id, g_settings.name, guest_name, best);
}

/* Guest: the host answered (its first ping). Receive the memory card and
 * the start signal (blocking). */
static void setup_guest(const Msg *first_ping) {
    NetAddr peer = s_join_addr;
    uint32_t id = first_ping->session;
    send_msg(&peer, PKT_PONG, 0, id, first_ping->t);
    banner("JOINING...");
    maple_flush_saves();
    uint8_t have[128] = {0}, buf[1500];
    int got = 0, delay = -1;
    char host_name[16] = "";
    NetAddr from;
    uint64_t t0 = now(), last_ack = 0;
    while (delay < 0 || got < 128) {
        int n = recv_msg(buf, sizeof buf, &from);
        if (n > 0 && same_addr(&from, &peer)) {
            Msg *m = (Msg *)buf;
            if (buf[0] == PKT_PING && n >= (int)sizeof(Msg)) {
                send_msg(&peer, PKT_PONG, 0, id, m->t);
            } else if (buf[0] == PKT_VMU && n == 8 + 1024 && buf[1] < 128 && !memcmp(buf + 4, &id, 4)) {
                if (!have[buf[1]]) got++;
                memcpy(s_sess_vmu + buf[1] * 1024, buf + 8, 1024);
                have[buf[1]] = 1;
            } else if (buf[0] == PKT_START && n >= (int)sizeof(Msg) && m->session == id) {
                delay = m->arg;
                snprintf(host_name, sizeof host_name, "%.15s", m->name);
                send_msg(&peer, PKT_START_ACK, 0, id, 0);
            }
        } else if (!n) {
            wait_ms(1);
        }
        if (now() - last_ack > 20000000ull) {
            uint8_t ack[2 + 128] = {PKT_VMU_ACK};
            memcpy(ack + 2, have, 128);
            net_send(&peer, ack, sizeof ack);
            last_ack = now();
        }
        if (now() - t0 > SETUP_TIMEOUT_NS || quit_requested()) {
            join_failed("CONNECTION FAILED");
            menu_banner("");
            return;
        }
    }
    for (int i = 0; i < 3; i++) send_msg(&peer, PKT_START_ACK, 0, id, 0);
    session_prepare(0, delay, &peer, id, host_name, g_settings.name, -1);
}

static void add_game(const NetAddr *from, const Msg *m) {
    int i = 0;
    while (i < s_ngames && s_games[i].id != m->t) i++;
    if (i == s_ngames) {
        if (s_ngames == (int)(sizeof s_games / sizeof s_games[0])) return;
        s_ngames++;
        s_games[i].addr = *from;
    }
    /* a game on this PC answers on several addresses: keep a LAN one */
    if (s_games[i].addr.ip == 0x0100007Fu) s_games[i].addr = *from;
    s_games[i].id = m->t;
    snprintf(s_games[i].name, sizeof s_games[i].name, "%.15s", m->name);
    s_games[i].seen_ns = now();
}

static void offline_service(void) {
    if (!net_is_open()) return;
    uint64_t t = now();
    if (s_browse && t - s_last_discover > 1000000000ull) {
        Msg m = {.type = PKT_DISCOVER, .build = build_id()};
        net_broadcast(NETPLAY_PORT, &m, sizeof m);
        NetAddr local = {0x0100007Fu, NETPLAY_PORT};  /* 127.0.0.1: a game hosted on this PC */
        net_send(&local, &m, sizeof m);
        s_last_discover = t;
    }
    for (int i = 0; i < s_ngames;)  /* forget games that stopped answering */
        if (t - s_games[i].seen_ns > 3500000000ull) s_games[i] = s_games[--s_ngames]; else i++;
    if (s_join && t - s_join_last > 250000000ull) {
        send_msg(&s_join_addr, PKT_HELLO, 0, 0, 0);
        s_join_last = t;
    }
    if (s_join && t - s_join_t0 > SETUP_TIMEOUT_NS) join_failed("NO ANSWER");

    uint8_t buf[1500];
    NetAddr from;
    int n;
    while ((n = recv_msg(buf, sizeof buf, &from)) > 0) {
        if (n < (int)sizeof(Msg)) continue;
        Msg *m = (Msg *)buf;
        switch (m->type) {
        case PKT_DISCOVER:
            if (!s_game_id) s_game_id = now() ^ ((uint64_t)SDL_rand_bits() << 32);
            if (s_listen) send_msg(&from, PKT_HOST_INFO, 0, 0, s_game_id);
            break;
        case PKT_HOST_INFO:
            if (s_browse && m->build == build_id()) add_game(&from, m);
            break;
        case PKT_HELLO:
            if (!s_listen) break;
            if (m->build != build_id()) {
                send_msg(&from, PKT_REFUSE, REFUSE_VERSION, 0, 0);
                break;
            }
            setup_host(&from, m->name);
            return;  /* a session may have started */
        case PKT_REFUSE:
            if (s_join && same_addr(&from, &s_join_addr))
                join_failed(m->arg == REFUSE_VERSION ? "DIFFERENT VERSION" : "LOBBY IS BUSY");
            break;
        case PKT_PING:
            if (s_join && same_addr(&from, &s_join_addr)) {
                setup_guest(m);
                return;
            }
            break;
        }
    }
}

/* ---------------- sessions ---------------- */

static void lockstep_init(void) {
    Input7 neutral = {0, 0, 0, 0x80, 0x80, 0, 0, 0, 0, 0, 0x80, 0x80};
    s_flags_pending = 0;
    s_third = 0;
    for (int i = 0; i < RING; i++) s_local[i] = s_remote[i] = neutral;
    s_start = -1;
    s_local_max = s_remote_max = -1;
    s_last_send_ns = 0;
    s_last_recv_ns = now();
    s_synced_ping = 0;
    memset(s_my_hash, 0, sizeof s_my_hash);
    memset(s_their_hash, 0, sizeof s_their_hash);
    s_my_hash_frame = s_their_hash_frame = s_checked_frame = -1;
    s_desyncs = 0;
    s_ping_sent_ns = 0;
    s_rtt_ms = s_sess.setup_rtt;
    s_rtt_sum = 0;
    s_rtt_n = 0;
    s_stall_ns = s_stall_max_ns = 0;
    s_stalls = 0;
    s_margin_min = 1 << 30;
    s_margin_sum = s_margin_n = 0;
    s_last_ping_seen = s_last_ping_seen_at = 0;
    s_my_margin16 = s_their_margin16 = 0;
}

static void session_prepare(int host, int delay, const NetAddr *peer, uint32_t id, const char *p1, const char *p2,
                            double rtt) {
    s_sess.host = host;
    s_sess.delay = delay;
    s_sess.peer = *peer;
    s_sess.id = id;
    s_sess.setup_rtt = rtt;
    snprintf(s_sess.names[0], sizeof s_sess.names[0], "%.15s", p1);
    snprintf(s_sess.names[1], sizeof s_sess.names[1], "%.15s", p2);
    s_listen = s_join = s_browse = 0;
    s_phase = PH_SYNC;
    s_reset_t0 = now();
    logf_("netplay: session %08X: %s (P1, host) vs %s (P2), input delay %d frames, peer %s", id, p1, p2, delay,
          net_addr_str(peer));
    lobby_autopilot_start();
    machine_reset_request(1);
}

void netplay_on_reset(void) {
    if (s_phase == PH_SYNC) {
        maple_vmu_use_session_image(s_sess_vmu);
        lockstep_init();
        g_audio_mute = 1;
    } else if (s_phase == PH_RETURN) {
        maple_vmu_restore_own();
        g_audio_mute = 1;
    }
}

static void send_bye(int reason) {
    for (int i = 0; i < 3; i++) send_msg(&s_sess.peer, PKT_BYE, reason, s_sess.id, 0);
}

void netplay_session_end(int reason) {
    if (!netplay_active()) return;
    static const char *const why[] = {"", "guest left", "host left", "guest removed", "connection lost"};
    logf_("netplay: session ended at frame %llu: %s", (unsigned long long)g_vblank_count,
          why[reason >= 1 && reason <= 4 ? reason : 0]);
    send_bye(reason);
    banner("BACK TO THE LOBBY...");
    char msg[64] = "";
    if (s_sess.host) {
        if (reason == END_HOST_LEFT) {
            set_landing(LAND_MENU, "");
            net_close();
        } else {
            if (reason == END_GUEST_LEFT) snprintf(msg, sizeof msg, "%.10s LEFT", s_sess.names[1]);
            else if (reason == END_KICKED) snprintf(msg, sizeof msg, "%.10s REMOVED", s_sess.names[1]);
            else snprintf(msg, sizeof msg, "CONNECTION LOST");
            set_landing(LAND_HOSTING, msg);
            s_listen = 1;  /* back to waiting, on the same port */
        }
    } else {
        if (reason == END_HOST_LEFT) snprintf(msg, sizeof msg, "HOST CLOSED THE LOBBY");
        else if (reason == END_KICKED) snprintf(msg, sizeof msg, "REMOVED BY HOST");
        else if (reason == END_LOST) snprintf(msg, sizeof msg, "CONNECTION LOST");
        set_landing(LAND_JOIN, msg);
    }
    s_phase = PH_RETURN;
    s_reset_t0 = now();
    lobby_autopilot_start();
    machine_reset_request(1);
}

void netplay_request_lobby(void) {
    if (s_phase == PH_LOCKSTEP) s_flags_pending |= FLAG_LOBBY;
}

void netplay_leave(void) {
    if (netplay_active()) netplay_session_end(s_sess.host ? END_HOST_LEFT : END_GUEST_LEFT);
}

void netplay_shutdown(void) {
    if (netplay_active()) send_bye(s_sess.host ? END_HOST_LEFT : END_GUEST_LEFT);
}

void netplay_cli(int host, const char *join, int delay) {
    s_cli_delay = delay;
    if (!host && !join) return;
    /* fast-forward the boot to the lobby, then host or join there */
    irq_det_init();
    s_phase = PH_RETURN;
    s_reset_t0 = now();
    g_audio_mute = 1;
    lobby_autopilot_start();
    if (host) {
        set_landing(LAND_HOSTING, "");
        s_listen = net_open(NETPLAY_PORT);
        if (!s_listen) logf_("netplay: cannot listen on port %d", NETPLAY_PORT);
    } else {
        set_landing(LAND_JOIN, "");
        netplay_join(join);
    }
}

void netplay_tick(void) {
    if (s_phase == PH_OFFLINE) offline_service();
}

/* ---------------- lockstep ---------------- */

typedef struct {
    uint8_t type, count, pad[2];
    uint32_t session;
    uint32_t first;        /* frame of inputs[0] */
    uint32_t ack;          /* newest remote frame we have */
    uint64_t ping, pong;   /* sender's clock / echo of the other side's */
    uint32_t pong_age_us;  /* how long the echoed ping waited here before this send */
    int16_t margin_x16;    /* sender's input margin (frames x16), for time sync */
    uint32_t hash_frame;   /* sync check */
    uint64_t hash;
    Input7 inputs[MAX_SEND];
} __attribute__((packed)) InputPkt;

static void send_inputs(void) {
    InputPkt p = {.type = PKT_INPUT, .session = s_sess.id};
    int64_t first = s_local_max - MAX_SEND + 1;
    if (first < s_start + s_sess.delay) first = s_start + s_sess.delay;
    if (first > s_local_max) return;
    p.first = (uint32_t)first;
    p.count = (uint8_t)(s_local_max - first + 1);
    for (int i = 0; i < p.count; i++) p.inputs[i] = s_local[(first + i) % RING];
    p.ack = (uint32_t)(s_remote_max < 0 ? 0 : s_remote_max);
    p.ping = now();
    p.pong = s_last_ping_seen;
    p.pong_age_us = s_last_ping_seen ? (uint32_t)((now() - s_last_ping_seen_at) / 1000) : 0;
    p.margin_x16 = (int16_t)s_my_margin16;
    if (s_my_hash_frame >= 0) {
        p.hash_frame = (uint32_t)s_my_hash_frame;
        p.hash = s_my_hash[(s_my_hash_frame / SYNC_EVERY) % (sizeof s_my_hash / sizeof s_my_hash[0])];
    }
    net_send(&s_sess.peer, &p, sizeof p - (MAX_SEND - p.count) * sizeof(Input7));
    s_last_send_ns = now();
}

static void check_sync(void) {
    if (s_their_hash_frame < 0 || s_their_hash_frame > s_my_hash_frame || s_their_hash_frame == s_checked_frame)
        return;
    size_t k = (size_t)(s_their_hash_frame / SYNC_EVERY) % (sizeof s_my_hash / sizeof s_my_hash[0]);
    s_checked_frame = s_their_hash_frame;
    if (s_my_hash[k] != s_their_hash[k]) {
        s_desyncs++;
        logf_("netplay: DESYNC at frame %lld (ours %016llx, theirs %016llx)", (long long)s_their_hash_frame,
              (unsigned long long)s_my_hash[k], (unsigned long long)s_their_hash[k]);
    }
}

/* returns an END_* reason if the other side ended the session */
static int receive(void) {
    uint8_t buf[1500];
    NetAddr from;
    int n;
    while ((n = net_recv(buf, sizeof buf, &from)) > 0) {
        if (!same_addr(&from, &s_sess.peer)) {
            if (buf[0] == PKT_HELLO) send_msg(&from, PKT_REFUSE, REFUSE_BUSY, 0, 0);  /* someone else joining */
            continue;
        }
        if (buf[0] == PKT_BYE && n >= (int)sizeof(Msg) && ((Msg *)buf)->session == s_sess.id) return buf[1];
        if (buf[0] != PKT_INPUT || n < (int)(sizeof(InputPkt) - sizeof(((InputPkt *)0)->inputs))) continue;
        InputPkt *p = (InputPkt *)buf;
        if (p->session != s_sess.id) continue;
        s_last_recv_ns = now();
        for (int i = 0; i < p->count; i++) {
            int64_t f = (int64_t)p->first + i;
            if (f == s_remote_max + 1) {  /* contiguous only; repeats fill gaps from lost packets */
                s_remote[f % RING] = p->inputs[i];
                s_remote_max = f;
            }
        }
        s_their_margin16 = p->margin_x16;
        if (p->ping > s_last_ping_seen) {
            s_last_ping_seen = p->ping;
            s_last_ping_seen_at = now();
        }
        if (p->pong && p->pong >= s_ping_sent_ns) {
            double ms = (now() - p->pong) / 1e6 - p->pong_age_us / 1000.0;
            s_rtt_sum += ms;
            s_rtt_n++;
            s_ping_sent_ns = p->pong + 1;
        }
        if (p->hash_frame) {
            size_t k = (size_t)(p->hash_frame / SYNC_EVERY) % (sizeof s_their_hash / sizeof s_their_hash[0]);
            s_their_hash[k] = p->hash;
            s_their_hash_frame = p->hash_frame;
        }
    }
    check_sync();
    return 0;
}

/* called while waiting between frames: pick up packets promptly */
void netplay_poll(void) {
    if (s_phase == PH_LOCKSTEP) {
        int bye = receive();
        if (bye) netplay_session_end(bye);
    }
}

static void log_stats(int64_t f) {
    double rtt = s_rtt_n ? s_rtt_sum / s_rtt_n : s_rtt_ms;
    if (s_rtt_n) s_rtt_ms = rtt;
    snprintf(s_status, sizeof s_status, "PING %.0f MS  DELAY %d  STALLS %d", rtt, s_sess.delay, s_stalls);
    logf_("netplay stats: frame %lld rtt %.1f ms delay %d frames (%.1f ms) stalls %d (%.1f ms total, max %.1f ms) "
          "input margin min %d avg %.1f frames desyncs %d",
          (long long)f, rtt, s_sess.delay, s_sess.delay * 16.68, s_stalls, s_stall_ns / 1e6, s_stall_max_ns / 1e6,
          s_margin_min == (1 << 30) ? -1 : s_margin_min, s_margin_n ? (double)s_margin_sum / s_margin_n : -1.0,
          s_desyncs);
    s_rtt_sum = 0;
    s_rtt_n = 0;
    s_stalls = 0;
    s_stall_ns = s_stall_max_ns = 0;
    s_margin_min = 1 << 30;
    s_margin_sum = s_margin_n = 0;
}

static Input7 pack(const PadState *p, const PadState *p2, int aux, int flags) {
    Input7 i = {p->buttons, p->ltrig, p->rtrig, p->ax, p->ay, (uint8_t)aux, (uint8_t)flags, 0, 0, 0, 0x80, 0x80};
    if (p2) {
        i.flags |= FLAG_PAD2;
        i.buttons2 = p2->buttons;
        i.lt2 = p2->ltrig;
        i.rt2 = p2->rtrig;
        i.ax2 = p2->ax;
        i.ay2 = p2->ay;
    }
    return i;
}
static PadState unpack2(Input7 i) { return (PadState){i.buttons2, i.lt2, i.rt2, i.ax2, i.ay2}; }
static PadState unpack(Input7 i) { return (PadState){i.buttons, i.lt, i.rt, i.ax, i.ay}; }

void netplay_frame(void) {
    int64_t f = (int64_t)g_vblank_count;
    if (s_phase == PH_RETURN && !lobby_autopilot_active()) {
        /* back in the lobby: real time from here, with sound */
        irq_leave_det();
        g_audio_mute = 0;
        menu_banner("");
        s_phase = PH_OFFLINE;
        logf_("netplay: back in the lobby at frame %lld (%.1f s)", (long long)f, (now() - s_reset_t0) / 1e9);
        return;
    }
    if (s_phase == PH_SYNC && !lobby_autopilot_active()) {
        /* both games reach the lobby on this same frame: lockstep from here,
         * the first `delay` frames with neutral input */
        s_phase = PH_LOCKSTEP;
        s_start = f;
        s_local_max = s_remote_max = f + s_sess.delay - 1;
        s_last_recv_ns = now();
        g_audio_mute = 0;
        menu_banner("");
        logf_("netplay: lockstep from frame %lld (%.1f s after the reset)", (long long)f, (now() - s_reset_t0) / 1e9);
    }
    if (s_phase != PH_LOCKSTEP) return;

    PadState *p1 = input_state(0), *p2 = input_state(1);
    /* this side's input, to be used `delay` frames from now */
    int64_t lf = f + s_sess.delay;
    int rtt = (int)(s_rtt_ms > 0 ? s_rtt_ms + 0.5 : 0);
    s_local[lf % RING] = pack(p1, input_pad_present(1) ? input_state(1) : NULL,
                              s_sess.host ? (rtt > 255 ? 255 : rtt < 1 ? 1 : rtt) : 0, s_flags_pending);
    s_flags_pending = 0;
    s_local_max = lf;
    send_inputs();

    /* both inputs for frame f */
    int bye = receive();
    if (bye) { netplay_session_end(bye); return; }
    if (s_remote_max < f) {
        uint64_t t0 = now();
        s_stalls++;
        uint64_t last_present = t0;
        while (s_remote_max < f) {
            if (now() - s_last_send_ns > 4000000ull) send_inputs();  /* repeat while waiting */
            if (now() - t0 > 300000000ull && now() - last_present > 100000000ull) {
                char b[48];
                snprintf(b, sizeof b, "WAITING FOR %.10s", s_sess.names[s_sess.host ? 1 : 0]);
                menu_banner(b);
                renderer_present_fb();
                last_present = now();
            }
            wait_ms(1);
            bye = receive();
            if (bye) { netplay_session_end(bye); return; }
            if (now() - s_last_recv_ns > SESSION_TIMEOUT_NS) {
                logf_("netplay: nothing from the other player for 5 s");
                netplay_session_end(END_LOST);
                return;
            }
            if (quit_requested()) {
                netplay_shutdown();
                maple_flush_saves();
                exit(0);
            }
        }
        menu_banner("");
        uint64_t d = now() - t0;
        det_pace_shift(d);  /* time sync: follow the slower side rather than catching up */
        s_stall_ns += d;
        if (d > s_stall_max_ns) s_stall_max_ns = d;
    } else {
        int margin = (int)(s_remote_max - f);
        if (margin < s_margin_min) s_margin_min = margin;
        s_margin_sum += margin;
        s_margin_n++;
    }
    /* Time sync: the side whose inputs arrive with less headroom is running
     * ahead of the other; it eases back 1 ms per frame until both have about
     * the same margin, so jitter in either direction is absorbed. */
    int m16 = (int)(s_remote_max - f) * 16;
    s_my_margin16 += (m16 - s_my_margin16) / 8;
    if (s_their_margin16 - s_my_margin16 >= 16) det_pace_shift(1000000);

    Input7 mine = s_local[f % RING], theirs = s_remote[f % RING];
    if (getenv("SFA3_NET_INPUT_LOG") && (mine.buttons || theirs.buttons || p1->buttons))
        logf_("netplay input f=%lld local_now=%04X mine=%04X theirs=%04X", (long long)f, p1->buttons, mine.buttons,
              theirs.buttons);
    *(s_sess.host ? p1 : p2) = unpack(mine);
    *(s_sess.host ? p2 : p1) = unpack(theirs);
    /* a third player: the host's second controller, else the guest's */
    Input7 hi = s_sess.host ? mine : theirs, gi = s_sess.host ? theirs : mine;
    s_third = (hi.flags & FLAG_PAD2) ? 1 : (gi.flags & FLAG_PAD2) ? 2 : 0;
    *input_state(2) = s_third == 1 ? unpack2(hi) : s_third == 2 ? unpack2(gi) : (PadState){0, 0, 0, 0x80, 0x80};
    *input_state(3) = (PadState){0, 0, 0, 0x80, 0x80};
    s_synced_ping = s_sess.host ? mine.aux : theirs.aux;
    if ((mine.flags | theirs.flags) & FLAG_LOBBY) {
        /* both sides, this frame: restart into the session lobby */
        logf_("netplay: back to the lobby at frame %lld", (long long)f);
        banner("BACK TO THE LOBBY...");
        s_sess.id += 0x9E3779B9u;  /* inputs still in flight belong to the old run */
        s_phase = PH_SYNC;
        s_reset_t0 = now();
        lobby_autopilot_start();
        machine_reset_request(1);
        return;
    }

    if (f % SYNC_EVERY == 0) {
        size_t k = (size_t)(f / SYNC_EVERY) % (sizeof s_my_hash / sizeof s_my_hash[0]);
        s_my_hash[k] = ram_hash();
        s_my_hash_frame = f;
        check_sync();
    }
    if (f % 60 == 0) log_stats(f);
}
