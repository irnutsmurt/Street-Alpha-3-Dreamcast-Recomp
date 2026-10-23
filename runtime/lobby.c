/* Mode Menu > NETWORK, rebuilt as the netplay lobby.
 *
 * On the Dreamcast this screen offered the online "Saikyo Dojo" and the web
 * page. Its init (0x8C02D3E0) and per-frame logic (0x8C02D4C0) are replaced:
 * the background, title bar, description box and sounds stay the game's own,
 * the old buttons are left out, and the lobby's text is drawn with the game's
 * print routine.
 *
 * Offline it hosts or joins a game (netplay.c). In a session it is part of
 * the synced game: both players see the same screen, driven only by the two
 * players' synced controllers, and the host (player 1) picks the mode. A mode
 * starts straight from here (through the Mode Menu's own start routine, the
 * menu itself never shown), and leaving a mode by any route that would show
 * the Mode Menu comes back here instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "gameui.h"
#include "input.h"
#include "lobby.h"
#include "net.h"
#include "netplay.h"
#include "runtime.h"
#include "settings.h"

#define STATE 0x8C17FBC0u        /* game state: +0/+2 routine numbers, +4/+6 screen sub-state, +0x72 game mode */
#define NET_MENU 0x8C1D0CF0u     /* the old screen's button state */

/* game modes (state +0x72) */
enum { MODE_VS = 1, MODE_TEAM = 6, MODE_DRAMATIC = 8, MODE_NETWORK = 12, MODE_VS_DRAMATIC = 13 };

extern uint64_t g_vblank_count;

/* ---------------- Mode Menu ----------------
 * The Mode Menu's input routine (0x8C06273C, r4 = game state) is wrapped so a
 * mode can be started without showing the menu: exactly what pressing A on
 * that mode does. */
static int s_pending_mode = -1;

void lobby_queue_mode(int mode) { s_pending_mode = mode; }

void f_8C06273C_orig(void);
void f_8C062BAC(void);
void hle_mode_menu_input(void) {
    /* in a session the Mode Menu is never shown: leaving a mode returns here */
    if (s_pending_mode < 0 && netplay_active()) s_pending_mode = MODE_NETWORK;
    if (s_pending_mode < 0) { f_8C06273C_orig(); return; }
    uint32_t st = g.r[4], arg = g.r[5];
    int mode = s_pending_mode;
    s_pending_mode = -1;
    logf_("mode menu: starting mode %d at frame %llu", mode, (unsigned long long)g_vblank_count);
    W8(st + 0x72, (uint8_t)mode);
    W8(st + 0x35, 1);
    g.r[4] = st;
    g.r[5] = arg;
    g.r[6] = 0;
    f_8C062BAC();  /* leaves the menu task; doesn't come back here */
}

/* ---------------- VS Dramatic Battle: the third fighter ----------------
 * The mode needs three controllers. In a session, controller 3 is reported
 * plugged in; a second controller on either machine plays it (the host's
 * first), otherwise the game's own AI does:
 *   - side select: the CPU confirms the side it's on; the humans arrange
 *     themselves (both on one side leaves the CPU alone on the other);
 *   - character select: once the CPU's teammate has chosen their own fighter,
 *     their controller chooses the CPU's fighter and options, then returns
 *     to them (with no teammate, whichever human chose first does);
 *   - fights: the CPU's fighter is flagged CPU-controlled (+0x155), which
 *     hands it to the game's AI, at the level the host set in the lobby.
 * All of it depends only on the synced game and inputs, so both sides do the
 * same. SFA3_CPU3=1 does the same offline (testing). */
#define FIGHTERS 0x8C17FD38u
#define FIGHTER_STRIDE 0x44Cu      /* 4 fighters: 0/1 are one side, 2/3 the other */
#define FIGHTER_ACTIVE 0x000u
#define FIGHTER_STEP 0x004u        /* select step: 1 = choosing a fighter */
#define FIGHTER_CHOOSING 0x005u    /* 1 while on the character select */
#define FIGHTER_CPU 0x155u         /* 1 = played by the game's AI */
#define FIGHTER_PAD 0x2E7u         /* controller 0-2, 255 = slot unused */
#define DIFFICULTY 0x8C17FBEDu     /* Game Option difficulty, 0-7 (the AI's level) */

static uint32_t fighter(int i) { return FIGHTERS + (uint32_t)i * FIGHTER_STRIDE; }

static int fighter_for_pad(int pad) {
    for (int i = 0; i < 4; i++)
        if ((int)R8(fighter(i) + FIGHTER_PAD) == pad) return i;
    return -1;
}

static int vs_dramatic(void) { return R8(STATE + 0x72) == MODE_VS_DRAMATIC; }

static int cpu_third(void) {
    static int offline = -1;
    if (offline < 0) offline = getenv("SFA3_CPU3") != NULL;
    if (!vs_dramatic()) return 0;
    if (netplay_active()) return !netplay_third_player();
    return offline;
}

int lobby_third_pad(void) { return (netplay_active() && vs_dramatic()) || cpu_third(); }

/* the human who chooses for a CPU with no teammate, latched until the select ends (-1 = none yet) */
static int s_alone_picker = -1;

/* this controller's player has chosen their own fighter */
static int chosen(int pad) {
    int f = fighter_for_pad(pad);
    return f >= 0 && !(R8(fighter(f) + FIGHTER_CHOOSING) && R8(fighter(f) + FIGHTER_STEP) == 1);
}

void lobby_cpu_player(void) {
    if (!cpu_third() || R16(STATE + 2) != 1) s_alone_picker = -1;
    if (!cpu_third()) return;
    PadState neutral = {0, 0, 0, 0x80, 0x80}, *p3 = input_state(2);
    *p3 = neutral;
    int cpu = fighter_for_pad(2);
    if (cpu < 0) return;
    uint32_t c = fighter(cpu);
    if (R16(STATE + 2) != 1) {  /* not on the select screens: a fight */
        if (R8(c + FIGHTER_ACTIVE) == 1) W8(c + FIGHTER_CPU, 1);
        return;
    }
    if (R8(c + FIGHTER_CHOOSING)) {
        /* the teammate chooses for the CPU once done with their own fighter;
         * alone, whichever human is done first (the host on a tie) */
        int mate = R8(fighter(cpu ^ 1) + FIGHTER_PAD);
        int picker = mate <= 1 ? mate : s_alone_picker;
        if (picker < 0) {
            for (int pad = 0; pad < 2 && picker < 0; pad++)
                if (chosen(pad)) picker = pad;
            if (picker < 0) return;
            s_alone_picker = picker;
        }
        if (!chosen(picker)) return;
        *p3 = *input_state(picker);
        *input_state(picker) = neutral;
        return;
    }
    /* side select: confirm the side the CPU is on (a press every 12 frames) */
    if (!R8(c + FIGHTER_ACTIVE) || R8(c + FIGHTER_STEP) <= 1)
        if (g_vblank_count % 12 < 4) p3->buttons = BTN_A;
}

/* "3P" tag (16x7, in the select screens' cursor and portrait frame) and its
 * replacement: '.' clear, 'o' outline, 'w' white */
static const char *const TAG_3P[7] = {
    ".ooooooo.oooooo.", "owwwwwwoowwwwwwo", ".oooowwwowwoooww", ".owwwwwoowwwwwwo",
    ".oooowwwowwoooo.", "owwwwwwoowwo....", ".ooooooo.oo.....",
};
static const char *const TAG_CPU[7] = {
    ".oooooooooooooo.", "oowwwowwwoowwowo", "owwooowwowowwowo", "owwooowwwoowwowo",
    "owwooowwooowwowo", "oowwwowwoooowwwo", ".oooooooooooooo.",
};

int lobby_cpu_label(void) { return cpu_third(); }

void lobby_patch_texture(uint8_t *idx, uint32_t w, uint32_t h) {
    if (w < 16 || h < 7 || !lobby_cpu_label()) return;
    for (uint32_t y = 0; y + 7 <= h; y++)
        for (uint32_t x = 0; x + 16 <= w; x++) {
            uint8_t *p = idx + y * w + x;
            int o = -1, wh = -1, ok = 1;
            for (int r = 0; r < 7 && ok; r++)
                for (int c = 0; c < 16 && ok; c++) {
                    int v = p[r * w + c], *slot = TAG_3P[r][c] == 'o' ? &o : TAG_3P[r][c] == 'w' ? &wh : NULL;
                    if (!slot) ok = v == 0;
                    else if (*slot < 0) *slot = v;
                    else ok = *slot == v;
                }
            if (!ok || o == wh || o <= 0 || wh <= 0) continue;
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 16; c++)
                    p[r * w + c] = TAG_CPU[r][c] == 'o' ? o : TAG_CPU[r][c] == 'w' ? wh : 0;
        }
}

/* ---------------- Start skips the Capcom logos ----------------
 * At power-on the boot task shows the 3D CAPCOM logo (state 2, sub-state 1:
 * a 10-step animation that sets sub-state 2 itself, whose cleanup moves on
 * to loading); Start there ends it the same way. After the save loads, the
 * menu task's mode 1 shows the small CAPCOM logo, then QSound; the game skips
 * them on Start (0x8C06228C, into +53) but only while no fade runs (+93), so
 * a press during the logo's fade-in was lost: it is kept and handed over
 * when the fade ends. Uses only the synced game and inputs (netplay safe). */
static int s_menu_task;  /* the menu task has started: STATE's modes are its own */
static uint16_t s_prev_start;
static int s_logo_skip;

static int s_autopilot;  /* below: the autopilot is driving the boot */

void lobby_boot_skip(void) {
    uint16_t held = (uint16_t)((input_state(0)->buttons | input_state(1)->buttons) & BTN_START);
    int pressed = held && !s_prev_start;
    s_prev_start = held;
    /* SKIP INTRO: as if Start were pressed on each (not in netplay: a session's
     * boot must be the same on both machines; its autopilot skips them anyway) */
    if (g_settings.skip_intro && !netplay_active() && !netplay_fast_forward() && !s_autopilot) pressed = 1;
    int mode = R16(STATE), sub = R16(STATE + 2);
    if (mode == 8) s_menu_task = 1;
    if (!s_menu_task) {
        if (mode == 2 && sub == 1 && pressed) W16(STATE + 2, 2);
        return;
    }
    if (mode != 1 || sub > 3) { s_logo_skip = 0; return; }
    if (pressed) s_logo_skip = 1;
    if (s_logo_skip && !R8(STATE + 93) && !R16(STATE + 8)) {
        W8(STATE + 53, R8(STATE + 53) | 1);
        s_logo_skip = 0;
    }
}

/* the CPU's level for VS Dramatic Battle, 1-8, set by the host in the lobby */
static int s_cpu_level;

/* ---------------- autopilot ----------------
 * Drives player 1's controller from power-on to this screen: Start through
 * the intro and title, then NETWORK from the Mode Menu. It depends only on the
 * game's state, so in netplay both sides reach the lobby on the same frame. */
static int s_autopilot;
static uint64_t s_net_frames;  /* lobby frames run since the screen opened */

void lobby_autopilot_start(void) {
    s_autopilot = 1;
    s_net_frames = 0;
    s_pending_mode = MODE_NETWORK;
}
int lobby_autopilot_active(void) { return s_autopilot; }

void lobby_autopilot(PadState *p1) {
    if (!s_autopilot) return;
    if (s_net_frames) {
        s_autopilot = 0;
        logf_("autopilot: lobby reached at frame %llu", (unsigned long long)g_vblank_count);
        return;
    }
    *p1 = (PadState){0, 0, 0, 0x80, 0x80};
    /* Start, every other frame, until the Mode Menu takes over (its input
     * routine ignores the pad while a mode is queued) */
    if (s_pending_mode == MODE_NETWORK && !(g_vblank_count & 1)) p1->buttons = BTN_START;
}

/* ---------------- screen init ---------------- */

static void call0(uint32_t fn, uint32_t from) { call_indirect(fn, from); }
static void call2(uint32_t fn, uint32_t a, uint32_t b, uint32_t from) {
    g.r[4] = a;
    g.r[5] = b;
    call_indirect(fn, from);
}

static void lobby_open(void);

/* The original init, without the three buttons, their connecting lines and
 * the description text (the description box stays). */
void hle_network_init(void) {
    const uint32_t from = 0x8C02D3E0u;
    uint32_t st = g.r[4];
    int32_t idx = (int32_t)R32(0x8C09B180u);
    if (idx >= 0) {
        g.r[0] = 852;
        g.r[1] = 0x8C18DB14u;
        g.r[2] = 0x8C18AFD0u + 852u * (uint32_t)idx;
        call0(0x8C092298u, from);  /* copy */
    }
    W16(st + 4, R16(st + 4) + 1);
    call0(0x8C01C170u, from);
    call0(0x8C01CE74u, from);
    static const uint8_t parts[] = {15, 16, 17, 18, 19, 24};  /* background */
    for (int i = 0; i < 6; i++) call2(0x8C025930u, 6, parts[i], from);
    call2(0x8C025930u, 16, 4, from);  /* description box */
    W8(NET_MENU + 0, 255);
    W8(NET_MENU + 1, 0);
    W8(NET_MENU + 2, 3);
    W8(NET_MENU + 3, 0);
    W16(NET_MENU + 8, 0);
    W16(st + 14, 0);
    g.r[4] = 1;
    g.r[5] = R32(0x8C1A2110u);
    g.r[6] = 0;
    g.r[7] = 0;
    g.r[15] -= 4;
    W32(g.r[15], 6);
    call0(0x8C015AF6u, from);
    g.r[15] += 4;
    call0(0x8C01E5BAu, from);
    W8(st + 145, (uint8_t)g.r[0]);
    s_net_frames = 0;
    lobby_open();
}

/* ---------------- the lobby ---------------- */

/* Text arena: the old screen's logic and its online sub-screens (0x8C02D4C0
 * up to the exit routine at 0x8C02D830), never run in the recompiled game. */
#define TEXT_ARENA 0x8C02D4C0u
#define TEXT_ARENA_END 0x8C02D82Cu
#define LOGIC_FN 0x8C02D4C0u

/* layout, in the print routine's 384x240 grid */
#define TITLE_X 136
#define TITLE_Y 5
#define ROW_Y0 40
#define ROW_STEP 13
#define LIST_X 60
#define VALUE_RIGHT 324
#define BOX_Y 172       /* the description box: two lines of up to BOX_CHARS */
#define BOX_STEP 12
#define BOX_CHARS 16

enum { SCR_MENU, SCR_HOSTING, SCR_JOIN, SCR_JOINING, SCR_ADDRESS, SCR_NAME, SCR_SESSION };
enum { MENU_HOST, MENU_JOIN, MENU_NAME, MENU_EXIT, MENU_COUNT };
enum { SES_VS, SES_TEAM, SES_DRAMATIC, SES_VS_DRAMATIC, SES_REMOVE, SES_CLOSE, SES_COUNT };

static int s_screen, s_cursor;
static int s_session_cursor;     /* back on the mode just played (same on both sides) */
static char s_note[64];          /* a message for the description box (errors, why a session ended) */
static MenuInput s_in[2];
static char s_join_name[16];

/* text entry (name, address) */
#define GRID_COLS 13
static const char *const s_grid[3] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "0123456789.-:"};
enum { KEY_SPACE, KEY_DELETE, KEY_OK };
static char s_edit[24];
static int s_edit_max, s_gx, s_gy;

static void set_note(const char *s) { snprintf(s_note, sizeof s_note, "%s", s ? s : ""); }

static void go(int screen, int cursor) {
    s_screen = screen;
    s_cursor = cursor;
}

static void begin_edit(int screen, const char *initial, int max) {
    snprintf(s_edit, sizeof s_edit, "%s", initial);
    s_edit_max = max;
    s_gx = s_gy = 0;
    go(screen, 0);
    input_text_begin();
}

static void end_edit(int screen, int cursor) {
    input_text_end();
    go(screen, cursor);
}

static void lobby_open(void) {
    s_in[0] = s_in[1] = (MenuInput){0};
    s_note[0] = 0;
    if (netplay_active()) {
        go(SCR_SESSION, s_session_cursor);
        return;
    }
    char msg[64];
    switch (netplay_take_landing(msg, sizeof msg)) {
    case LAND_HOSTING:
        go(SCR_HOSTING, 0);
        set_note(msg);
        if (!netplay_hosting() && !netplay_host_start()) go(SCR_MENU, MENU_HOST);
        break;
    case LAND_JOIN:
        netplay_browse(1);
        go(netplay_joining() ? SCR_JOINING : SCR_JOIN, 0);
        snprintf(s_join_name, sizeof s_join_name, "GAME");
        set_note(msg);
        break;
    default:
        go(SCR_MENU, MENU_HOST);
        break;
    }
}

/* leave the screen through the game's own exit (to the Mode Menu) */
static void leave_screen(void) {
    W16(STATE + 4, 5);
    W16(STATE + 6, 0);
}

/* ---- drawing ---- */

static void print_row(const char *label, int row, int selected, int enabled) {
    gameui_print(label, LIST_X, ROW_Y0 + row * ROW_STEP, !enabled ? COL_GREY : selected ? COL_GOLD : COL_WHITE);
}

static void print_right(const char *s, int y, int colour) { gameui_print(s, VALUE_RIGHT - gameui_text_w(s), y, colour); }

/* the description box: two centred lines; a long text wraps at a space */
static void box2(const char *a, const char *b) {
    gameui_center(a, BOX_Y, COL_WHITE);
    gameui_center(b, BOX_Y + BOX_STEP, COL_WHITE);
}

static void box(const char *text) {
    char a[BOX_CHARS + 1] = "", b[64] = "";
    size_t n = strlen(text);
    if (n <= BOX_CHARS) {
        snprintf(a, sizeof a, "%s", text);
    } else {
        int cut = BOX_CHARS;
        while (cut > 0 && text[cut] != ' ') cut--;
        if (!cut) cut = BOX_CHARS;
        snprintf(a, sizeof a, "%.*s", cut, text);
        snprintf(b, sizeof b, "%.*s", BOX_CHARS, text + cut + (text[cut] == ' '));
    }
    gameui_center(a, BOX_Y, COL_WHITE);
    if (b[0]) gameui_center(b, BOX_Y + BOX_STEP, COL_WHITE);
}

static const char *const s_menu_help[MENU_COUNT] = {
    "HOST A GAME FOR A FRIEND", "JOIN A FRIEND'S GAME", "YOUR NAME IN THE LOBBY", "BACK TO THE MODE MENU",
};
static const char *const s_ses_labels[SES_COUNT] = {
    "VS MODE", "TEAM BATTLE", "DRAMATIC BATTLE", "VS DRAMATIC BATTLE", "REMOVE PLAYER", "CLOSE LOBBY",
};
static const char *const s_ses_help[SES_COUNT] = {
    "ONE ON ONE", "TEAM VS TEAM", "TEAM UP VS CPU", "", "SEND P2 AWAY", "END THE SESSION",
};
static int session_row_enabled(int r) { (void)r; return 1; }

static void draw_menu(void) {
    static const char *const labels[MENU_COUNT] = {"HOST GAME", "JOIN GAME", "NAME", "EXIT"};
    for (int r = 0; r < MENU_COUNT; r++) {
        print_row(labels[r], r + 1, r == s_cursor, 1);
        if (r == MENU_NAME) print_right(g_settings.name, ROW_Y0 + (r + 1) * ROW_STEP, COL_BLUE);
    }
    box(s_note[0] ? s_note : s_menu_help[s_cursor]);
}

static void draw_hosting(void) {
    gameui_center("WAITING FOR A PLAYER", ROW_Y0, COL_GOLD);
    print_row("NAME", 2, 0, 1);
    print_right(g_settings.name, ROW_Y0 + 2 * ROW_STEP, COL_BLUE);
    char addrs[3][16];
    int n = net_local_addrs(addrs, 3);
    print_row("ADDRESS", 3, 0, 1);
    for (int i = 0; i < n; i++) print_right(addrs[i], ROW_Y0 + (3 + i) * ROW_STEP, COL_BLUE);
    if (!n) print_right("-", ROW_Y0 + 3 * ROW_STEP, COL_BLUE);
    int port_row = 3 + (n ? n : 1);
    char port[8];
    snprintf(port, sizeof port, "%d", NETPLAY_PORT);
    print_row("PORT", port_row, 0, 1);
    print_right(port, ROW_Y0 + port_row * ROW_STEP, COL_BLUE);
    gameui_center("CANCEL", ROW_Y0 + (port_row + 2) * ROW_STEP, COL_GOLD);
    box(s_note[0] ? s_note : "FRIENDS ON YOUR NETWORK SEE YOU");
}

/* JOIN GAME: games found on the local network, then ENTER ADDRESS and BACK */
static int shown_games(const NetGame **games) {
    int n = netplay_games(games);
    return n > 6 ? 6 : n;
}

static void draw_join(void) {
    const NetGame *games;
    int n = shown_games(&games);
    gameui_center(n ? "GAMES ON YOUR NETWORK" : "SEARCHING YOUR NETWORK...", ROW_Y0, COL_GOLD);
    for (int i = 0; i < n; i++) {
        print_row(games[i].name, i + 1, s_cursor == i, 1);
        char a[24];
        snprintf(a, sizeof a, "%s", net_addr_str(&games[i].addr));
        char *colon = strrchr(a, ':');
        if (colon && atoi(colon + 1) == NETPLAY_PORT) *colon = 0;
        print_right(a, ROW_Y0 + (i + 1) * ROW_STEP, s_cursor == i ? COL_GOLD : COL_BLUE);
    }
    int r = (n ? n : 1) + 2;
    print_row("ENTER ADDRESS", r, s_cursor == n, 1);
    print_row("BACK", r + 1, s_cursor == n + 1, 1);
    box(s_note[0] ? s_note : s_cursor < n ? "JOIN THIS GAME" : s_cursor == n ? "JOIN BY IP ADDRESS" : "BACK");
}

static void draw_joining(void) {
    char b[48];
    snprintf(b, sizeof b, "JOINING %.10s...", s_join_name);
    gameui_center(b, ROW_Y0 + 2 * ROW_STEP, COL_GOLD);
    gameui_center("CANCEL", ROW_Y0 + 5 * ROW_STEP, COL_GOLD);
    box("WAITING FOR AN ANSWER");
}

static void draw_edit(void) {
    gameui_center(s_screen == SCR_NAME ? "ENTER YOUR NAME" : "ENTER THE ADDRESS", ROW_Y0, COL_GOLD);
    char b[32];
    snprintf(b, sizeof b, "%s%c", s_edit, (g_vblank_count / 20) & 1 ? GAMEUI_BLOCK : ' ');
    gameui_center(b, ROW_Y0 + 2 * ROW_STEP, COL_BLUE);
    /* the letter grid: one string per row, the selected letter printed apart in gold */
    const int gx0 = SCREEN_W / 2 - (GRID_COLS * 2 - 1) * CHAR_W / 2;
    for (int y = 0; y < 3; y++) {
        char row[GRID_COLS * 2];
        for (int x = 0; x < GRID_COLS; x++) {
            row[x * 2] = (y == s_gy && x == s_gx) ? ' ' : s_grid[y][x];
            row[x * 2 + 1] = ' ';
        }
        row[GRID_COLS * 2 - 1] = 0;
        int ry = ROW_Y0 + (4 + y) * ROW_STEP;
        gameui_print(row, gx0, ry, COL_WHITE);
        if (y == s_gy) {
            char c[2] = {s_grid[y][s_gx], 0};
            gameui_print(c, gx0 + s_gx * 2 * CHAR_W, ry, COL_GOLD);
        }
    }
    static const char *const keys[3] = {"SPACE", "DELETE", "OK"};
    static const int key_x[3] = {84, 168, 276};
    for (int k = 0; k < 3; k++)
        gameui_print(keys[k], key_x[k], ROW_Y0 + 7 * ROW_STEP + 4, s_gy == 3 && s_gx == k ? COL_GOLD : COL_WHITE);
    box("TYPE OR PICK LETTERS");
}

static void draw_session(void) {
    char b[48];
    print_row("P1", 0, 0, 1);
    gameui_print(netplay_player_name(0), LIST_X + 4 * CHAR_W, ROW_Y0, COL_BLUE);
    print_right("HOST", ROW_Y0, COL_WHITE);
    print_row("P2", 1, 0, 1);
    gameui_print(netplay_player_name(1), LIST_X + 4 * CHAR_W, ROW_Y0 + ROW_STEP, COL_BLUE);
    snprintf(b, sizeof b, "PING %d MS", netplay_ping_ms());
    print_right(b, ROW_Y0 + ROW_STEP, COL_WHITE);
    for (int r = 0; r < SES_COUNT; r++)
        print_row(s_ses_labels[r], r + 3, r == s_cursor, session_row_enabled(r));
    if (s_cursor == SES_VS_DRAMATIC) {
        /* the third fighter: a second controller, or the CPU at the host's level */
        int who = netplay_third_player();
        if (who) {
            snprintf(b, sizeof b, "3P: %s PAD 2", who == 1 ? "P1" : "P2");
        } else {
            if (!s_cpu_level) s_cpu_level = R8(DIFFICULTY) % 8 + 1;
            snprintf(b, sizeof b, "CPU LEVEL " GAMEUI_LEFT " %d " GAMEUI_RIGHT, s_cpu_level);
        }
        box2("2 VS 1", b);
    } else {
        box2(s_ses_help[s_cursor], "P2: B TO LEAVE");
    }
}

static void draw(void) {
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, LOGIC_FN);
    gameui_print_layer("NETWORK", 2, TITLE_X, TITLE_Y, COL_TITLE);
    switch (s_screen) {
    case SCR_MENU: draw_menu(); break;
    case SCR_HOSTING: draw_hosting(); break;
    case SCR_JOIN: draw_join(); break;
    case SCR_JOINING: draw_joining(); break;
    case SCR_ADDRESS: case SCR_NAME: draw_edit(); break;
    case SCR_SESSION: draw_session(); break;
    }
}

/* ---- input ---- */

static int move_cursor(uint32_t pressed, int count) {
    int d = (pressed & PAD_UP) ? -1 : (pressed & PAD_DOWN) ? 1 : 0;
    if (!d) return 0;
    s_cursor = (s_cursor + d + count) % count;
    gameui_sound(SND_CURSOR);
    return 1;
}

static void logic_menu(uint32_t pressed) {
    if (move_cursor(pressed, MENU_COUNT)) { s_note[0] = 0; return; }
    if (pressed & PAD_CANCEL) {
        gameui_sound(SND_CANCEL);
        leave_screen();
        return;
    }
    if (!(pressed & PAD_OK)) return;
    s_note[0] = 0;
    switch (s_cursor) {
    case MENU_HOST:
        if (netplay_host_start()) {
            gameui_sound(SND_OK);
            go(SCR_HOSTING, 0);
        } else {
            gameui_sound(SND_CANCEL);
            set_note(netplay_take_error());
        }
        break;
    case MENU_JOIN:
        gameui_sound(SND_OK);
        netplay_browse(1);
        go(SCR_JOIN, 0);
        break;
    case MENU_NAME:
        gameui_sound(SND_OK);
        begin_edit(SCR_NAME, g_settings.name, NAME_MAX_LEN);
        break;
    case MENU_EXIT:
        gameui_sound(SND_CANCEL);
        leave_screen();
        break;
    }
}

static void logic_hosting(uint32_t pressed) {
    const char *err = netplay_take_error();
    if (err) set_note(err);
    if (pressed & (PAD_CANCEL | PAD_OK)) {
        gameui_sound(SND_CANCEL);
        netplay_host_stop();
        s_note[0] = 0;
        go(SCR_MENU, MENU_HOST);
    }
}

static void logic_join(uint32_t pressed) {
    const NetGame *games;
    int n = shown_games(&games);
    int rows = n + 2;
    if (s_cursor >= rows) s_cursor = rows - 1;
    if (move_cursor(pressed, rows)) { s_note[0] = 0; return; }
    if ((pressed & PAD_CANCEL) || ((pressed & PAD_OK) && s_cursor == n + 1)) {
        gameui_sound(SND_CANCEL);
        netplay_browse(0);
        s_note[0] = 0;
        go(SCR_MENU, MENU_JOIN);
        return;
    }
    if (!(pressed & PAD_OK)) return;
    s_note[0] = 0;
    if (s_cursor == n) {
        gameui_sound(SND_OK);
        begin_edit(SCR_ADDRESS, "", 21);
        return;
    }
    char addr[32];
    snprintf(addr, sizeof addr, "%s", net_addr_str(&games[s_cursor].addr));
    snprintf(s_join_name, sizeof s_join_name, "%s", games[s_cursor].name);
    if (netplay_join(addr)) {
        gameui_sound(SND_OK);
        go(SCR_JOINING, 0);
    } else {
        gameui_sound(SND_CANCEL);
        set_note(netplay_take_error());
    }
}

static void logic_joining(uint32_t pressed) {
    if (!netplay_joining()) {  /* failed */
        const char *err = netplay_take_error();
        set_note(err ? err : "NO ANSWER");
        go(SCR_JOIN, 0);
        return;
    }
    if (pressed & (PAD_CANCEL | PAD_OK)) {
        gameui_sound(SND_CANCEL);
        netplay_join_cancel();
        go(SCR_JOIN, 0);
    }
}

static void edit_type(int c) {
    size_t n = strlen(s_edit);
    if (c >= 'a' && c <= 'z') c -= 32;
    if (!gameui_printable(c) || (int)n >= s_edit_max) return;
    if (s_screen == SCR_ADDRESS && c == ' ') return;
    s_edit[n] = (char)c;
    s_edit[n + 1] = 0;
}

static void edit_delete(void) {
    size_t n = strlen(s_edit);
    if (n) s_edit[n - 1] = 0;
}

static void edit_finish(int ok) {
    if (s_screen == SCR_NAME) {
        if (ok && s_edit[0]) settings_set_name(s_edit);
        end_edit(SCR_MENU, MENU_NAME);
        return;
    }
    if (!ok || !s_edit[0]) {
        end_edit(SCR_JOIN, 0);
        return;
    }
    snprintf(s_join_name, sizeof s_join_name, "%.15s", s_edit);
    char addr[32];
    snprintf(addr, sizeof addr, "%s", s_edit);
    end_edit(SCR_JOIN, 0);
    if (netplay_join(addr)) go(SCR_JOINING, 0);
    else set_note(netplay_take_error());
}

static void logic_edit(uint32_t pressed) {
    /* keyboard: typed characters, Backspace, Enter, Esc */
    for (int c; (c = input_text_get()) >= 0;) {
        if (c == '\b') edit_delete();
        else if (c == '\n') { gameui_sound(SND_OK); edit_finish(1); return; }
        else if (c == 27) { gameui_sound(SND_CANCEL); edit_finish(0); return; }
        else edit_type(c);
    }
    /* controller: pick letters from the grid */
    int dx = (pressed & PAD_LEFT) ? -1 : (pressed & PAD_RIGHT) ? 1 : 0;
    int dy = (pressed & PAD_UP) ? -1 : (pressed & PAD_DOWN) ? 1 : 0;
    if (dx || dy) {
        if (dy) {
            int old = s_gy;
            s_gy = (s_gy + dy + 4) % 4;
            if (s_gy == 3) s_gx = s_gx * 3 / GRID_COLS;
            else if (old == 3) s_gx = s_gx * GRID_COLS / 3 + 2;
        }
        int w = s_gy == 3 ? 3 : GRID_COLS;
        if (s_gx >= w) s_gx = w - 1;
        if (dx) s_gx = (s_gx + dx + w) % w;
        gameui_sound(SND_CURSOR);
        return;
    }
    if (pressed & PAD_START) { gameui_sound(SND_OK); edit_finish(1); return; }
    if (pressed & PAD_CANCEL) {
        if (s_edit[0]) { edit_delete(); gameui_sound(SND_CURSOR); }
        else { gameui_sound(SND_CANCEL); edit_finish(0); }
        return;
    }
    if (!(pressed & PAD_OK)) return;
    gameui_sound(SND_CURSOR);
    if (s_gy < 3) edit_type(s_grid[s_gy][s_gx]);
    else if (s_gx == KEY_SPACE) edit_type(' ');
    else if (s_gx == KEY_DELETE) edit_delete();
    else edit_finish(1);
}

/* In a session: both players' pads are the synced ones in guest RAM. */
static void logic_session(uint32_t p1, uint32_t p2) {
    if (p2 & PAD_CANCEL) {
        gameui_sound(SND_CANCEL);
        netplay_session_end(END_GUEST_LEFT);
        return;
    }
    if (move_cursor(p1, SES_COUNT)) return;
    if (s_cursor == SES_VS_DRAMATIC && (p1 & (PAD_LEFT | PAD_RIGHT)) && !netplay_third_player()) {
        if (!s_cpu_level) s_cpu_level = R8(DIFFICULTY) % 8 + 1;
        s_cpu_level = (s_cpu_level + ((p1 & PAD_LEFT) ? 6 : 0)) % 8 + 1;
        gameui_sound(SND_CURSOR);
        return;
    }
    if (p1 & PAD_CANCEL) {  /* to CLOSE LOBBY, as the game's menus go to EXIT */
        if (s_cursor != SES_CLOSE) gameui_sound(SND_CURSOR);
        s_cursor = SES_CLOSE;
        return;
    }
    if (!(p1 & PAD_OK)) return;
    static const int modes[] = {MODE_VS, MODE_TEAM, MODE_DRAMATIC, MODE_VS_DRAMATIC};
    if (!session_row_enabled(s_cursor)) {
        gameui_sound(SND_CANCEL);
        return;
    }
    switch (s_cursor) {
    case SES_REMOVE:
        gameui_sound(SND_OK);
        netplay_session_end(END_KICKED);
        break;
    case SES_CLOSE:
        gameui_sound(SND_CANCEL);
        netplay_session_end(END_HOST_LEFT);
        break;
    default:
        gameui_sound(SND_OK);
        s_session_cursor = s_cursor;
        s_pending_mode = modes[s_cursor];
        if (s_pending_mode == MODE_VS_DRAMATIC && s_cpu_level) W8(DIFFICULTY, s_cpu_level - 1);
        leave_screen();  /* the Mode Menu starts it at once */
        break;
    }
}

void hle_network_logic(void) {
    const uint32_t from = LOGIC_FN;
    s_net_frames++;
    gameui_begin(TEXT_ARENA, TEXT_ARENA_END, from);
    if (!s_autopilot) {
        if (s_screen == SCR_SESSION) {
            uint32_t p1 = gameui_presses(&s_in[0], gameui_pad_held(0), 0);
            uint32_t p2 = gameui_presses(&s_in[1], gameui_pad_held(1), 0);
            if (netplay_active()) logic_session(p1, p2);
        } else {
            /* offline: either controller works the menus */
            uint32_t pressed = gameui_presses(&s_in[0], gameui_pad_held(0) | gameui_pad_held(1), 0);
            switch (s_screen) {
            case SCR_MENU: logic_menu(pressed); break;
            case SCR_HOSTING: logic_hosting(pressed); break;
            case SCR_JOIN: logic_join(pressed); break;
            case SCR_JOINING: logic_joining(pressed); break;
            case SCR_ADDRESS: case SCR_NAME: logic_edit(pressed); break;
            }
        }
    }
    draw();
    /* the original routine's per-frame tail */
    call0(0x8C030120u, from);
    call0(0x8C05F9B4u, from);
    call0(0x8C015CD2u, from);
}

void lobby_on_reset(void) {
    if (!s_autopilot) s_pending_mode = -1;  /* an autopilot started for this reset carries on */
    s_net_frames = 0;
    s_screen = SCR_MENU;
    s_cursor = 0;
    s_session_cursor = SES_VS;
    s_cpu_level = 0;
    s_alone_picker = -1;
    s_menu_task = 0;
    s_logo_skip = 0;
    s_prev_start = 0;
    s_in[0] = s_in[1] = (MenuInput){0};
    input_text_end();
}
