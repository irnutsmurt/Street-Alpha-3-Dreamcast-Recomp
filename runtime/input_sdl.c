/* Host input via SDL3: gamepads map to Dreamcast controllers; keyboard drives
 * port A when no pad is attached. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "controls.h"
#include "cpu.h"
#include "input.h"
#include "lobby.h"
#include "netplay.h"
#include "menu.h"
#include "options.h"
#include "runtime.h"
#include "settings.h"

int title_quit_available(void);
void title_quit_toggle(void);

static SDL_Gamepad *s_pads[4];
extern SDL_Window *g_window;
extern uint64_t g_vblank_count;
static PadState s_state[4];
int g_quit_requested;

void input_init(void) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---------------- text entry ---------------- */
static int s_text_on;
static char s_text_q[64];
static int s_text_head, s_text_tail;

static void text_push(int c) {
    int next = (s_text_tail + 1) % (int)sizeof s_text_q;
    if (next == s_text_head) return;
    s_text_q[s_text_tail] = (char)c;
    s_text_tail = next;
}

void input_text_begin(void) {
    if (s_text_on) return;
    s_text_on = 1;
    s_text_head = s_text_tail = 0;
    SDL_StartTextInput(g_window);
}

void input_text_end(void) {
    if (!s_text_on) return;
    s_text_on = 0;
    SDL_StopTextInput(g_window);
}

int input_text_get(void) {
    if (s_text_head == s_text_tail) return -1;
    int c = (unsigned char)s_text_q[s_text_head];
    s_text_head = (s_text_head + 1) % (int)sizeof s_text_q;
    return c;
}

/* SFA3_PADS=<n>: controllers reported as plugged in (testing; default 2) */
int input_connected(int port) {
    static int n = -1;
    if (n < 0) n = getenv("SFA3_PADS") ? atoi(getenv("SFA3_PADS")) : 2;
    if (netplay_active()) return port < 2 || (port == 2 && lobby_third_pad());  /* VS Dramatic's third player */
    return port < n || input_pad_present(port) || (port == 2 && lobby_third_pad());
}

/* a real device drives this port (a gamepad, or the keyboard set for player 2) */
int input_pad_present(int port) { return port >= 0 && port < 4 && controls_port_has_device(port, s_pads); }

PadState *input_state(int port) { return &s_state[port & 3]; }

void input_poll(int port, PadState *out) {
    if (port < 0 || port > 3) { *out = (PadState){0, 0, 0, 0x80, 0x80}; return; }
    *out = s_state[port];
}

static int pad_index(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++)
        if (s_pads[i] && SDL_GetGamepadID(s_pads[i]) == id) return i;
    return -1;
}

static void open_pads(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; i < n && i < 4; i++)
        if (!s_pads[i]) s_pads[i] = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

/* SFA3_INPUT="vbl:button,..." presses a port-A button for 8 frames at each
 * listed vblank (testing aid for headless runs). */
static void scripted_input(PadState *st) {
    static const struct { const char *name; uint16_t bit; } names[] = {
        {"start", BTN_START}, {"a", BTN_A}, {"b", BTN_B}, {"x", BTN_X}, {"y", BTN_Y},
        {"up", BTN_UP}, {"down", BTN_DOWN}, {"left", BTN_LEFT}, {"right", BTN_RIGHT},
    };
    const char *e = getenv("SFA3_INPUT");
    if (!e) return;
    while (*e) {
        unsigned long long at = strtoull(e, (char **)&e, 10);
        if (*e != ':') return;
        e++;
        size_t len = strcspn(e, ",");
        if (g_vblank_count >= at && g_vblank_count < at + 8)
            for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
                if (strlen(names[i].name) == len && !strncmp(e, names[i].name, len)) st->buttons |= names[i].bit;
        e += len;
        if (*e == ',') e++;
    }
}

/* SFA3_CONTROL=<file>: a testing aid. The file is polled every frame; when it
 * appears its lines are queued and it is deleted. Commands:
 *   press <button> [frames]   hold a port-A button (default 6 frames, then 10 released)
 *   press2 <button> [frames]  the same on port B (player 2)
 *   wait <frames>
 *   shot                      write a screenshot (build/shot_<frame>.png)
 *   shotp                     screenshot of the presented window image
 *   menu <toggle|up|down|left|right|accept|back>  drive the settings menu */
int g_shot_request, g_shot_present_request;
static char s_ctl_queue[64][256];
static int s_ctl_head, s_ctl_tail, s_ctl_timer;
static uint16_t s_ctl_hold;
static int s_ctl_port;  /* 0 = player 1 ("press"), 1 = player 2 ("press2") */

static uint16_t button_bit(const char *name) {
    static const struct { const char *name; uint16_t bit; } names[] = {
        {"start", BTN_START}, {"a", BTN_A}, {"b", BTN_B}, {"x", BTN_X}, {"y", BTN_Y},
        {"up", BTN_UP}, {"down", BTN_DOWN}, {"left", BTN_LEFT}, {"right", BTN_RIGHT},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i].name)) return names[i].bit;
    return 0;
}

static void control_file(PadState *st) {
    static const char *path;
    static int init;
    if (!init) { init = 1; path = getenv("SFA3_CONTROL"); }
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if ((s_ctl_tail + 1) % 64 != s_ctl_head) {
                line[strcspn(line, "\r\n")] = 0;
                snprintf(s_ctl_queue[s_ctl_tail], sizeof s_ctl_queue[0], "%s", line);
                s_ctl_tail = (s_ctl_tail + 1) % 64;
            }
        fclose(f);
        remove(path);
    }
    if (s_ctl_timer > 0) {
        s_ctl_timer--;
        if (s_ctl_timer > 10) st[s_ctl_port].buttons |= s_ctl_hold;
        return;
    }
    s_ctl_hold = 0;
    if (s_ctl_head == s_ctl_tail) return;
    char cmd[32], arg[200] = "";
    int n = 0;
    const char *line = s_ctl_queue[s_ctl_head];
    sscanf(line, "%31s %199s %d", cmd, arg, &n);
    s_ctl_head = (s_ctl_head + 1) % 64;
    if (!strcmp(cmd, "press") || !strcmp(cmd, "press2") || !strcmp(cmd, "press3")) {
        s_ctl_hold = button_bit(arg);
        s_ctl_port = cmd[5] ? cmd[5] - '1' : 0;
        s_ctl_timer = (n > 0 ? n : 6) + 10;
    } else if (!strcmp(cmd, "wait")) {
        s_ctl_timer = atoi(arg);
    } else if (!strcmp(cmd, "shot")) {
        g_shot_request = 1;
    } else if (!strcmp(cmd, "shotp")) {  /* presented image: includes scanlines and the menu */
        g_shot_present_request = 1;
    } else if (!strcmp(cmd, "peek")) {  /* peek <hex address>: log a guest byte (debug) */
        uint32_t a = (uint32_t)strtoul(arg, NULL, 16);
        extern uint64_t g_vblank_count;
        logf_("peek %08X = %u (vbl %llu)", a, R8(a), (unsigned long long)g_vblank_count);
    } else if (!strcmp(cmd, "banner")) {  /* banner [text]: the netplay notice (testing its look) */
        const char *t = strchr(line, ' ');
        menu_banner(t ? t + 1 : "");
    } else if (!strcmp(cmd, "mode")) {  /* mode <id>: the Mode Menu starts this game mode next time it runs */
        void lobby_queue_mode(int mode);
        lobby_queue_mode(atoi(arg));
    } else if (!strcmp(cmd, "key")) {  /* key <SDL key name>: a key press event (testing CONTROLS) */
        const char *t = strchr(line, ' ');
        SDL_Event ev = {0};
        ev.type = SDL_EVENT_KEY_DOWN;
        ev.key.scancode = SDL_GetScancodeFromName(t ? t + 1 : "");
        ev.key.down = true;
        SDL_PushEvent(&ev);
    } else if (!strcmp(cmd, "key")) {  /* key <SDL key name>: a key press event (testing CONTROLS) */
        const char *t = strchr(line, ' ');
        SDL_Event ev = {0};
        ev.type = SDL_EVENT_KEY_DOWN;
        ev.key.scancode = SDL_GetScancodeFromName(t ? t + 1 : "");
        ev.key.down = true;
        SDL_PushEvent(&ev);
    } else if (!strcmp(cmd, "dumprender")) {  /* log the next render's polygons, textures to build/tex_*.png */
        void renderer_dump_next(void);
        renderer_dump_next();
    } else if (!strcmp(cmd, "quit")) {  /* as if the window were closed */
        g_quit_requested = 1;
    } else if (!strcmp(cmd, "autopilot")) {  /* drive player 1 to Mode Menu > NETWORK */
        void lobby_autopilot_start(void);
        lobby_autopilot_start();
    } else if (!strcmp(cmd, "reset")) {  /* reset <0|1>: in-place machine reset (1 = deterministic) */
        machine_reset_request(atoi(arg));
    } else if (!strcmp(cmd, "poke")) {  /* poke <hex address> <value>: write a guest byte (debug) */
        W8((uint32_t)strtoul(arg, NULL, 16), (uint32_t)n);
    } else if (!strcmp(cmd, "ramdump")) {  /* ramdump <file>: write the 16 MB of guest RAM (debug) */
        FILE *f = fopen(arg, "wb");
        if (f) { fwrite(g_ram, 1, 16 << 20, f); fclose(f); logf_("ramdump %s", arg); }
    } else if (!strcmp(cmd, "vramdump")) {  /* vramdump <file>: write the 8 MB of video RAM (debug) */
        FILE *f = fopen(arg, "wb");
        if (f) { fwrite(g_vram, 1, 8 << 20, f); fclose(f); logf_("vramdump %s", arg); }
    } else if (!strcmp(cmd, "hist")) {  /* hist <start|stop>: log indirect call targets */
        if (!strcmp(arg, "start")) g_call_hist_on = 1;
        else { g_call_hist_on = 0; call_hist_dump(); }
    } else if (!strcmp(cmd, "menu")) {  /* menu <toggle|up|down|left|right|accept|back> */
        if (!strcmp(arg, "toggle")) menu_toggle();
        else if (!strcmp(arg, "up")) menu_action(MENU_UP);
        else if (!strcmp(arg, "down")) menu_action(MENU_DOWN);
        else if (!strcmp(arg, "left")) menu_action(MENU_LEFT);
        else if (!strcmp(arg, "right")) menu_action(MENU_RIGHT);
        else if (!strcmp(arg, "accept")) menu_action(MENU_ACCEPT);
        else if (!strcmp(arg, "back")) menu_action(MENU_BACK);
    }
}


void input_update(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) g_quit_requested = 1;
        if (controls_capturing()) {  /* CONTROLS page: the next key or button is the new binding */
            int took = 0;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) took = controls_capture_key(e.key.scancode);
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) took = controls_capture_pad(pad_index(e.gbutton.which), e.gbutton.button);
            if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 20000 &&
                (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                took = controls_capture_pad(pad_index(e.gaxis.which),
                                            e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LT : PADB_RT);
            if (took) { menu_refresh(); continue; }
            if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) continue;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
            (e.key.scancode == SDL_SCANCODE_F11 ||
             (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
            /* toggle between a window and the last fullscreen kind */
            static int last_fs = WINDOW_BORDERLESS;
            if (g_settings.window_mode == WINDOW_WINDOWED) {
                g_settings.window_mode = last_fs;
            } else {
                last_fs = g_settings.window_mode;
                g_settings.window_mode = WINDOW_WINDOWED;
            }
            options_apply_window();
            settings_save();
            continue;
        }
        if (s_text_on && !menu_active()) {
            if (e.type == SDL_EVENT_TEXT_INPUT) {
                for (const char *t = e.text.text; *t; t++)
                    if ((unsigned char)*t < 128) text_push(*t);
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN) {
                SDL_Scancode sc = e.key.scancode;
                if (sc == SDL_SCANCODE_BACKSPACE) { text_push('\b'); continue; }
                if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) { text_push('\n'); continue; }
                if (sc == SDL_SCANCODE_ESCAPE) { text_push(27); continue; }
            }
        }
        /* the settings overlay: F1 only (its options are in the game's own
         * pause menu now). On the title screen Back / Esc ask QUIT GAME?
         * (title_quit.c); in the overlay they go back. */
        int back = (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE && !e.key.repeat) ||
                   (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK);
        if (back && !menu_active() && title_quit_available()) {
            title_quit_toggle();
            continue;
        }
        if (back && menu_active()) {
            menu_action(MENU_BACK);
            continue;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F1 && !e.key.repeat) {
            if (menu_active()) menu_action(MENU_BACK); else menu_toggle();
            continue;
        }
        if (menu_active() && e.type == SDL_EVENT_KEY_DOWN) {
            switch (e.key.scancode) {
            case SDL_SCANCODE_UP: case SDL_SCANCODE_W: menu_action(MENU_UP); break;
            case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: menu_action(MENU_DOWN); break;
            case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: menu_action(MENU_LEFT); break;
            case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: menu_action(MENU_RIGHT); break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_SPACE: case SDL_SCANCODE_J: menu_action(MENU_ACCEPT); break;
            case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_K: menu_action(MENU_BACK); break;
            default: break;
            }
        }
        if (menu_active() && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: menu_action(MENU_UP); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: menu_action(MENU_DOWN); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: menu_action(MENU_LEFT); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: menu_action(MENU_RIGHT); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_START: menu_action(MENU_ACCEPT); break;
            case SDL_GAMEPAD_BUTTON_EAST: menu_action(MENU_BACK); break;
            default: break;
            }
        }
        if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
            for (int i = 0; i < 4; i++) if (s_pads[i]) { SDL_CloseGamepad(s_pads[i]); s_pads[i] = NULL; }
            open_pads();
        }
    }
    for (int p = 0; p < 4; p++) {
        PadState st = {0, 0, 0, 0x80, 0x80};
        controls_read(p, s_pads, s_text_on, &st);
        s_state[p] = st;
    }
    scripted_input(&s_state[0]);
    control_file(s_state);
    if (menu_active())  /* the game sees no input while the menu is open */
        for (int p = 0; p < 4; p++) s_state[p] = (PadState){0, 0, 0, 0x80, 0x80};
}
