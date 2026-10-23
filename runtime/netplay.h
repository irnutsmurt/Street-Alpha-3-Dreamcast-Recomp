/* Netplay: LAN discovery, hosting and joining from the NETWORK lobby, and
 * delay-based lockstep sessions (see netplay.c). */
#pragma once
#include <stdint.h>

#include "net.h"

#define NETPLAY_PORT 7777

/* ---- offline: the lobby's HOST GAME / JOIN GAME ---- */
int netplay_host_start(void);      /* listen for players; 0 if the port is in use */
void netplay_host_stop(void);
int netplay_hosting(void);
void netplay_browse(int on);       /* look for games on the local network */
typedef struct {
    char name[16];
    NetAddr addr;
    uint64_t id, seen_ns;
} NetGame;
int netplay_games(const NetGame **list);  /* games found while browsing */
int netplay_join(const char *addr);        /* "host[:port]"; 0 if it can't be resolved */
void netplay_join_cancel(void);
int netplay_joining(void);
/* a failed join or hosting problem to show, or NULL (taken once) */
const char *netplay_take_error(void);

/* ---- sessions ---- */
int netplay_active(void);          /* in a session (both players' games in lockstep) */
int netplay_is_host(void);
const char *netplay_player_name(int player);  /* 0 = P1 (host), 1 = P2 */
int netplay_ping_ms(void);         /* host's measured round trip, synced (same on both sides) */
int netplay_third_player(void);    /* synced: a second controller plays 3P (1 = host's, 2 = guest's), or 0 */
enum { END_GUEST_LEFT = 1, END_HOST_LEFT, END_KICKED, END_LOST };
/* Called on both sides at the same frame (a synced lobby event), or on a
 * lost connection: both games return to their own NETWORK lobby. */
void netplay_session_end(int reason);
/* From the F1 menu, either player: both games restart in the session lobby
 * (sent with this player's input, so both act on the same frame). */
void netplay_request_lobby(void);
void netplay_leave(void);          /* end the session from this side */

/* Where the lobby opens after a session or a command-line start. */
enum { LAND_NONE, LAND_MENU, LAND_HOSTING, LAND_JOIN, LAND_SESSION };
int netplay_take_landing(char *msg, int cap);

/* ---- per frame (main.c) ---- */
void netplay_tick(void);           /* each vblank: network service, phase changes */
void netplay_frame(void);          /* each vblank, after input is read: lockstep */
int netplay_fast_forward(void);    /* skip pacing, presenting and sound */
void netplay_on_reset(void);       /* the machine was just reset */
void netplay_shutdown(void);       /* the program is closing */
void netplay_cli(int host, const char *join, int delay);  /* --host / --join / --delay */
const char *netplay_status(void);
