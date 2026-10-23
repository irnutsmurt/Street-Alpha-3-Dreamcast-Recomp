/* Controller setup: devices, button names and bindings for players 1 and 2
 * (see controls.h). Ports 3 and 4 (VS Dramatic's third player, testing) use
 * their own gamepad with the default bindings. */
#include "controls.h"
#include "cpu.h"
#include "sprite_swap.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void settings_save(void);

/* what each control sends: a Dreamcast button bit, or a full trigger pull */
static const struct { uint16_t bit; int trig; } s_out[ACT_COUNT] = {
    {BTN_UP, 0}, {BTN_DOWN, 0}, {BTN_LEFT, 0}, {BTN_RIGHT, 0},
    {BTN_X, 0}, {BTN_Y, 0}, {0, 1}, {BTN_A, 0}, {BTN_B, 0}, {0, 2}, {BTN_START, 0},
    {0, 0}, {0, 0},  /* 3P / 3K: press_control */
};
static const char *const s_act_names[ACT_COUNT] = {
    "UP", "DOWN", "LEFT", "RIGHT", "LIGHT PUNCH", "MEDIUM PUNCH", "HEAVY PUNCH",
    "LIGHT KICK", "MEDIUM KICK", "HEAVY KICK", "START", "3 PUNCHES", "3 KICKS",
};
static const char *const s_act_keys[ACT_COUNT] = {"up", "down", "left", "right", "lp", "mp", "hp",
                                                  "lk", "mk", "hk", "start", "3p", "3k"};
static const char *const s_type_names[CTL_COUNT] = {"AUTO", "XBOX", "PLAYSTATION", "SWITCH", "KEYBOARD"};

#define NONE (-1)
static int s_type[2], s_pad[2];
static int s_key[2][ACT_COUNT][2];  /* SDL scancodes: main key, second key (0 = none) */
static int s_btn[2][ACT_COUNT];     /* gamepad: SDL button, PADB_LT / PADB_RT, or NONE */

static const int s_default_btn[ACT_COUNT] = {
    NONE, NONE, NONE, NONE,
    SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, PADB_RT, SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PADB_LT,
};
static const int s_default_key[2][ACT_COUNT][2] = {
    {   /* player 1: arrows / WASD, punches U I O, kicks J K L */
        {SDL_SCANCODE_UP, SDL_SCANCODE_W}, {SDL_SCANCODE_DOWN, SDL_SCANCODE_S},
        {SDL_SCANCODE_LEFT, SDL_SCANCODE_A}, {SDL_SCANCODE_RIGHT, SDL_SCANCODE_D},
        {SDL_SCANCODE_U}, {SDL_SCANCODE_I}, {SDL_SCANCODE_O},
        {SDL_SCANCODE_J}, {SDL_SCANCODE_K}, {SDL_SCANCODE_L}, {SDL_SCANCODE_RETURN},
        {SDL_SCANCODE_P}, {SDL_SCANCODE_SEMICOLON},
    },
    {   /* player 2 (sharing the keyboard): numeric keypad, Insert/Home/PgUp, Delete/End/PgDn */
        {SDL_SCANCODE_KP_8}, {SDL_SCANCODE_KP_5}, {SDL_SCANCODE_KP_4}, {SDL_SCANCODE_KP_6},
        {SDL_SCANCODE_INSERT}, {SDL_SCANCODE_HOME}, {SDL_SCANCODE_PAGEUP},
        {SDL_SCANCODE_DELETE}, {SDL_SCANCODE_END}, {SDL_SCANCODE_PAGEDOWN}, {SDL_SCANCODE_KP_ENTER},
        {SDL_SCANCODE_KP_7}, {SDL_SCANCODE_KP_9},
    },
};

static int s_init;
static void init(void) {
    if (s_init) return;
    s_init = 1;
    controls_reset(0);
    controls_reset(1);
    s_init = 1;
}

void controls_reset(int p) {
    s_init = 1;
    s_type[p] = CTL_AUTO;
    s_pad[p] = 0;
    memcpy(s_key[p], s_default_key[p], sizeof s_key[p]);
    memcpy(s_btn[p], s_default_btn, sizeof s_btn[p]);
}

const char *controls_action_name(int act) { return act >= 0 && act < ACT_COUNT ? s_act_names[act] : ""; }
const char *controls_type_name(int t) { return t >= 0 && t < CTL_COUNT ? s_type_names[t] : ""; }
int controls_type(int p) { init(); return s_type[p & 1]; }
void controls_set_type(int p, int t) { init(); s_type[p & 1] = (t + CTL_COUNT) % CTL_COUNT; settings_save(); }
int controls_pad_choice(int p) { init(); return s_pad[p & 1]; }
void controls_set_pad_choice(int p, int n) { init(); s_pad[p & 1] = (n + 5) % 5; settings_save(); }

/* ---------------- devices ---------------- */

static SDL_Gamepad *const *s_pads;  /* the open gamepads (input_sdl.c), as last seen */

int controls_pad_count(void) {
    int n = 0;
    for (int i = 0; s_pads && i < 4; i++) n += s_pads[i] != NULL;
    return n;
}

/* the gamepad a port reads, or NULL */
static SDL_Gamepad *port_pad(int port, SDL_Gamepad *const *pads) {
    if (!pads) return NULL;
    if (port < 2) {
        if (s_type[port] == CTL_KEYBOARD) return NULL;
        if (s_pad[port]) return pads[s_pad[port] - 1];
    }
    return pads[port & 3];
}

/* the keyboard drives player 1 on AUTO (alongside a gamepad) and any player set to KEYBOARD */
static int uses_keyboard(int port) {
    return port < 2 && (s_type[port] == CTL_KEYBOARD || (s_type[port] == CTL_AUTO && port == 0));
}

int controls_port_has_device(int port, SDL_Gamepad *const *pads) {
    init();
    s_pads = pads;
    return port_pad(port, pads) != NULL || (port == 1 && s_type[1] == CTL_KEYBOARD);
}

int controls_edits_keyboard(int p) {
    init();
    return s_type[p] == CTL_KEYBOARD || (s_type[p] == CTL_AUTO && p == 0 && !port_pad(0, s_pads));
}

/* ---------------- names ---------------- */

/* button names follow the controller family */
enum { FAM_XBOX, FAM_PS, FAM_SWITCH };
static int family(int p) {
    int t = s_type[p];
    if (t == CTL_PLAYSTATION) return FAM_PS;
    if (t == CTL_SWITCH) return FAM_SWITCH;
    if (t == CTL_AUTO) {
        SDL_Gamepad *gp = port_pad(p, s_pads);
        switch (gp ? SDL_GetGamepadType(gp) : SDL_GAMEPAD_TYPE_UNKNOWN) {
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return FAM_PS;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return FAM_SWITCH;
        default: break;
        }
    }
    return FAM_XBOX;
}

static const char *pad_button_name(int fam, int b, char *tmp, size_t n) {
    static const SDL_GamepadType types[3] = {SDL_GAMEPAD_TYPE_XBOXONE, SDL_GAMEPAD_TYPE_PS5,
                                             SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO};
    static const char *const face[] = {"?", "A", "B", "X", "Y", "CROSS", "CIRCLE", "SQUARE", "TRIANGLE"};
    static const char *const other[3][6] = {
        {"LB", "RB", "LT", "RT", "LS", "RS"}, {"L1", "R1", "L2", "R2", "L3", "R3"}, {"L", "R", "ZL", "ZR", "LS", "RS"},
    };
    static const char *const start[3] = {"MENU", "OPTIONS", "PLUS"};
    switch (b) {
    case NONE: return "-";
    case PADB_LT: return other[fam][2];
    case PADB_RT: return other[fam][3];
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return other[fam][0];
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return other[fam][1];
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return other[fam][4];
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return other[fam][5];
    case SDL_GAMEPAD_BUTTON_START: return start[fam];
    case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH: {
        int l = (int)SDL_GetGamepadButtonLabelForType(types[fam], (SDL_GamepadButton)b);
        return face[l >= 0 && l < 9 ? l : 0];
    }
    default:
        snprintf(tmp, n, "BUTTON %d", b);
        return tmp;
    }
}

static void key_name(int sc, char *out, size_t n) {
    /* punctuation the menus' fonts don't have */
    static const struct { int sc; const char *name; } words[] = {
        {SDL_SCANCODE_SEMICOLON, "SEMICOLON"}, {SDL_SCANCODE_COMMA, "COMMA"}, {SDL_SCANCODE_PERIOD, "PERIOD"},
        {SDL_SCANCODE_APOSTROPHE, "QUOTE"}, {SDL_SCANCODE_LEFTBRACKET, "L BRACKET"},
        {SDL_SCANCODE_RIGHTBRACKET, "R BRACKET"}, {SDL_SCANCODE_GRAVE, "GRAVE"}, {SDL_SCANCODE_EQUALS, "EQUALS"},
        {SDL_SCANCODE_BACKSLASH, "BACKSLASH"}, {SDL_SCANCODE_SLASH, "SLASH"}, {SDL_SCANCODE_MINUS, "MINUS"},
    };
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++)
        if (words[i].sc == sc) { snprintf(out, n, "%s", words[i].name); return; }
    const char *s = sc ? SDL_GetScancodeName((SDL_Scancode)sc) : "";
    size_t k = 0;
    for (; *s && k + 1 < n; s++) out[k++] = (char)toupper((unsigned char)*s);
    out[k] = 0;
    if (!k) snprintf(out, n, "-");
}

void controls_binding_name(int p, int act, char *out, size_t n) {
    init();
    if (controls_edits_keyboard(p)) {
        char a[32], b[32];
        key_name(s_key[p][act][0], a, sizeof a);
        key_name(s_key[p][act][1], b, sizeof b);
        if (s_key[p][act][1]) snprintf(out, n, "%s / %s", a, b);
        else snprintf(out, n, "%s", a);
        return;
    }
    if (act < ACT_FIRST_BUTTON) { snprintf(out, n, "D-PAD / STICK"); return; }
    char tmp[16];
    snprintf(out, n, "%s", pad_button_name(family(p), s_btn[p][act], tmp, sizeof tmp));
}

/* Short names for the game's BUTTON CONFIG screen: "LEFT SHIFT" -> "LSHIFT",
 * "KEYPAD 8" -> "KP8"; only characters the game's font has. */
void controls_short_name(int p, int act, int max, char *out, size_t n) {
    init();
    char full[48], b[48];
    if (controls_edits_keyboard(p)) key_name(s_key[p][act][0], full, sizeof full);
    else if (act < ACT_FIRST_BUTTON) snprintf(full, sizeof full, "D-PAD");
    else {
        char tmp[16];
        const char *s = pad_button_name(family(p), s_btn[p][act], tmp, sizeof tmp);
        if (s == tmp) snprintf(full, sizeof full, "BTN %d", s_btn[p][act]);  /* paddles and other extras */
        else snprintf(full, sizeof full, "%s", s);
    }
    static const struct { const char *from, *to; } abbrev[] = {
        {"LEFT ", "L"}, {"RIGHT ", "R"}, {"L BRACKET", "L BRKT"}, {"R BRACKET", "R BRKT"}, {"KEYPAD ", "KP"},
        {"PAGE", "PG"}, {"BACKSPACE", "BKSPACE"}, {"SEMICOLON", "SEMI"}, {"BACKSLASH", "BSLASH"},
    };
    size_t k = 0;
    for (const char *s = full; *s && k + 1 < sizeof b;) {
        int done = 0;
        for (size_t i = 0; i < sizeof abbrev / sizeof abbrev[0] && !done; i++) {
            size_t l = strlen(abbrev[i].from);
            if (!strncmp(s, abbrev[i].from, l)) {
                for (const char *t = abbrev[i].to; *t && k + 1 < sizeof b; t++) b[k++] = *t;
                s += l;
                done = 1;
            }
        }
        if (done) continue;
        char c = *s++;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '/' || c == '+') b[k++] = c;
        else if (c == ' ' && k && b[k - 1] != ' ') b[k++] = ' ';
    }
    b[k] = 0;
    if (!k) snprintf(b, sizeof b, "-");
    if (max <= 3) {  /* the BUTTON CONFIG grid's cells */
        static const struct { const char *name, *s; } three[] = {
            {"CROSS", "X"}, {"CIRCLE", "CIR"}, {"SQUARE", "SQR"}, {"TRIANGLE", "TRI"}, {"OPTIONS", "OPT"},
            {"MENU", "MNU"}, {"PLUS", "+"}, {"LSHIFT", "LSH"}, {"RSHIFT", "RSH"}, {"LCTRL", "LCT"},
            {"RCTRL", "RCT"}, {"LALT", "LAL"}, {"RALT", "RAL"}, {"RETURN", "ENT"}, {"SPACE", "SPC"},
            {"BKSPACE", "BSP"}, {"SEMI", "SEM"}, {"PGUP", "PGU"}, {"PGDOWN", "PGD"}, {"INSERT", "INS"},
            {"DELETE", "DEL"}, {"HOME", "HOM"}, {"ESCAPE", "ESC"}, {"CAPSLOCK", "CAP"}, {"QUOTE", "QUO"},
            {"COMMA", "COM"}, {"PERIOD", "PER"}, {"SLASH", "/"}, {"MINUS", "-"}, {"EQUALS", "EQU"},
            {"KPENTER", "KPE"}, {"L BRKT", "LBR"}, {"R BRKT", "RBR"}, {"BSLASH", "BSL"}, {"GRAVE", "GRV"},
        };
        char flat[48];
        size_t f = 0;
        for (const char *c = b; *c; c++)
            if (*c != ' ' || !strncmp(b, "L BRKT", 6) || !strncmp(b, "R BRKT", 6)) flat[f++] = *c;
        flat[f] = 0;
        for (size_t i = 0; i < sizeof three / sizeof three[0]; i++)
            if (!strcmp(flat, three[i].name) || !strcmp(b, three[i].name)) { snprintf(out, n, "%s", three[i].s); return; }
        if (!strncmp(flat, "BTN", 3)) { snprintf(out, n, "B%.2s", flat + 3); return; }
        snprintf(out, n, "%.3s", flat);
        return;
    }
    snprintf(out, n, "%.*s", max, b);
}

int controls_button_icon(int p, int act) {
    init();
    if (controls_edits_keyboard(p) || act < ACT_FIRST_BUTTON) return -1;
    int fam = family(p), k;
    if (fam == FAM_SWITCH) return -1;
    switch (s_btn[p][act]) {  /* same order in both sets: south east west north, LB RB LT RT */
    case SDL_GAMEPAD_BUTTON_SOUTH: k = 0; break;
    case SDL_GAMEPAD_BUTTON_EAST: k = 1; break;
    case SDL_GAMEPAD_BUTTON_WEST: k = 2; break;
    case SDL_GAMEPAD_BUTTON_NORTH: k = 3; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: k = 4; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: k = 5; break;
    case PADB_LT: k = 6; break;
    case PADB_RT: k = 7; break;
    default: return -1;
    }
    return (fam == FAM_PS ? IMG_PS_CROSS : IMG_XB_A) + k;
}

const char *controls_short_type_name(int t) {
    static const char *const names[CTL_COUNT] = {"AUTO", "XBOX", "PS", "SWITCH", "KEYBOARD"};
    return t >= 0 && t < CTL_COUNT ? names[t] : "";
}

/* ---------------- binding capture ---------------- */

static int s_cap_player = -1, s_cap_act, s_cap_keyboard;

void controls_capture_begin(int p, int act) {
    init();
    s_cap_player = p;
    s_cap_act = act;
    s_cap_keyboard = controls_edits_keyboard(p);
}
int controls_capturing(void) { return s_cap_player >= 0; }
int controls_capture_player(void) { return s_cap_player; }
int controls_capture_action(void) { return s_cap_act; }
void controls_capture_cancel(void) { s_cap_player = -1; }

static void bind_key(int p, int act, int sc) {
    int old = s_key[p][act][0];
    for (int a = 0; a < ACT_COUNT; a++) {
        if (a == act) continue;
        if (s_key[p][a][0] == sc) s_key[p][a][0] = old;  /* swap with the control that had it */
        if (s_key[p][a][1] == sc) s_key[p][a][1] = 0;
    }
    s_key[p][act][0] = sc;
    s_key[p][act][1] = 0;
}

static void bind_button(int p, int act, int b) {
    int old = s_btn[p][act];
    for (int a = ACT_FIRST_BUTTON; a < ACT_COUNT; a++)
        if (a != act && s_btn[p][a] == b) s_btn[p][a] = old;
    s_btn[p][act] = b;
}

int controls_capture_key(int sc) {
    if (s_cap_player < 0) return 0;
    if (sc == SDL_SCANCODE_ESCAPE) { controls_capture_cancel(); return 1; }
    if (!s_cap_keyboard || sc == SDL_SCANCODE_F1 || sc == SDL_SCANCODE_F11) return 1;
    bind_key(s_cap_player, s_cap_act, sc);
    controls_capture_cancel();
    settings_save();
    return 1;
}

int controls_capture_pad(int pad_index, int b) {
    if (s_cap_player < 0) return 0;
    if (b == SDL_GAMEPAD_BUTTON_BACK) { controls_capture_cancel(); return 1; }
    if (s_cap_keyboard || s_cap_act < ACT_FIRST_BUTTON) return 1;
    SDL_Gamepad *gp = port_pad(s_cap_player, s_pads);
    if (!s_pads || pad_index < 0 || pad_index > 3 || s_pads[pad_index] != gp) return 1;  /* another player's pad */
    if (b == SDL_GAMEPAD_BUTTON_GUIDE || (b >= SDL_GAMEPAD_BUTTON_DPAD_UP && b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
        return 1;  /* the d-pad always moves; Guide belongs to the system */
    bind_button(s_cap_player, s_cap_act, b);
    controls_capture_cancel();
    settings_save();
    return 1;
}

/* ---------------- input ---------------- */

static uint8_t axis8(int16_t v) { return (uint8_t)((v + 32768) >> 8); }

/* Option Mode > BUTTON CONFIG: the attack (0-5 = L/M/H punch, L/M/H kick) of
 * each Dreamcast button X Y L A B R, 16 bits each, 12 bytes per player */
#define GAME_BUTTON_CONFIG 0x8C09B18Cu

static void press(PadState *st, int a) {
    st->buttons |= s_out[a].bit;
    if (s_out[a].trig == 1) st->ltrig = 255;
    if (s_out[a].trig == 2) st->rtrig = 255;
}

/* an attack (0-5: L/M/H punch, L/M/H kick): the Dreamcast button(s) the
 * game's setting gives it (port 3 / 4 or none set: the default button) */
static void press_attack(PadState *st, int p, int attack) {
    int found = 0;
    for (int j = 0; p >= 0 && j < 6; j++)
        if ((int)R16(GAME_BUTTON_CONFIG + (uint32_t)p * 12 + (uint32_t)j * 2) == attack) {
            press(st, ACT_LP + j);
            found = 1;
        }
    if (!found) press(st, ACT_LP + attack);
}

static void press_control(PadState *st, int p, int a) {
    if (a >= ACT_LP && a <= ACT_HK) press_attack(st, p, a - ACT_LP);
    else if (a == ACT_3P || a == ACT_3K)
        for (int k = 0; k < 3; k++) press_attack(st, p, (a == ACT_3K ? 3 : 0) + k);
    else press(st, a);
}

void controls_read(int port, SDL_Gamepad *const *pads, int text_entry, PadState *st) {
    init();
    s_pads = pads;
    int p = port < 2 ? port : -1;
    SDL_Gamepad *gp = port_pad(port, pads);
    if (gp) {
        static const SDL_GamepadButton dirs[4] = {SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                                  SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
        for (int a = 0; a < 4; a++)
            if (SDL_GetGamepadButton(gp, dirs[a])) st->buttons |= s_out[a].bit;
        st->ax = axis8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
        st->ay = axis8(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
        for (int a = ACT_FIRST_BUTTON; a < ACT_COUNT; a++) {
            int b = p >= 0 ? s_btn[p][a] : s_default_btn[a], on;
            if (b == NONE) continue;
            if (b == PADB_LT || b == PADB_RT)
                on = SDL_GetGamepadAxis(gp, b == PADB_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000;
            else
                on = SDL_GetGamepadButton(gp, (SDL_GamepadButton)b);
            if (on) press_control(st, p, a);
        }
    }
    if (p >= 0 && uses_keyboard(port) && !text_entry) {
        const bool *k = SDL_GetKeyboardState(NULL);
        for (int a = 0; a < ACT_COUNT; a++) {
            if (!(k[s_key[p][a][0]] && s_key[p][a][0]) && !(k[s_key[p][a][1]] && s_key[p][a][1])) continue;
            press_control(st, p, a);
        }
    }
}

/* ---------------- rumble ---------------- */

/* The Jump Pack condition, as sent: special (0x10 motor 1, +0x01
 * continuous), effect 1 (strength in bits 4-6; 0 = stop), effect 2 (the
 * vibration frequency), duration (1/4 seconds; 0 = until the next command,
 * which SFA3 always sends). Capped so a pad can't be left rumbling. */
void controls_rumble(int port, const uint8_t *c) {
    SDL_Gamepad *gp = port_pad(port, s_pads);
    if (!gp) return;
    int strength = (c[1] >> 4) & 7;
    uint32_t ms = c[3] ? (uint32_t)c[3] * 250 : 2000;
    uint16_t v = (uint16_t)(strength * 0xFFFF / 7);
    SDL_RumbleGamepad(gp, v, v, strength ? ms : 0);
}

/* ---------------- sfa3.cfg ---------------- */

static int parse_pad(const char *v) {
    if (!strcmp(v, "none")) return NONE;
    if (!strcmp(v, "lefttrigger")) return PADB_LT;
    if (!strcmp(v, "righttrigger")) return PADB_RT;
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(v);
    return b == SDL_GAMEPAD_BUTTON_INVALID ? NONE : (int)b;
}

static const char *pad_string(int b) {
    if (b == PADB_LT) return "lefttrigger";
    if (b == PADB_RT) return "righttrigger";
    const char *s = b == NONE ? NULL : SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
    return s ? s : "none";
}

static int parse_key(const char *v) {
    if (!*v || !strcmp(v, "none")) return 0;
    return (int)SDL_GetScancodeFromName(v);
}

/* p1_controller, p1_gamepad, p1_<control> (keys), p1_pad_<button> */
int controls_cfg_read(const char *key, const char *val) {
    init();
    if ((key[0] != 'p') || (key[1] != '1' && key[1] != '2') || key[2] != '_') return 0;
    int p = key[1] - '1';
    const char *k = key + 3;
    if (!strcmp(k, "controller")) {
        for (int t = 0; t < CTL_COUNT; t++)
            if (!SDL_strcasecmp(val, s_type_names[t])) s_type[p] = t;
        return 1;
    }
    if (!strcmp(k, "gamepad")) { int n = atoi(val); s_pad[p] = n < 0 || n > 4 ? 0 : n; return 1; }
    int pad = !strncmp(k, "pad_", 4);
    if (pad) k += 4;
    for (int a = 0; a < ACT_COUNT; a++) {
        if (strcmp(k, s_act_keys[a])) continue;
        if (pad) {
            if (a >= ACT_FIRST_BUTTON) s_btn[p][a] = parse_pad(val);
        } else {
            char first[64] = "", second[64] = "";
            const char *comma = strchr(val, ',');
            snprintf(first, sizeof first, "%.*s", (int)(comma ? comma - val : (long)strlen(val)), val);
            if (comma) snprintf(second, sizeof second, "%s", comma + 1 + (comma[1] == ' '));
            s_key[p][a][0] = parse_key(first);
            s_key[p][a][1] = parse_key(second);
        }
        return 1;
    }
    return 0;
}

void controls_cfg_write(FILE *f) {
    init();
    fputs("# controls (also on the F1 / Esc menu > CONTROLS). controller: auto, xbox, playstation,\n"
          "# switch or keyboard; gamepad: 0 = automatic, 1-4 = that gamepad. Keys use SDL key names\n"
          "# (\"Left Shift\", \"Keypad 8\"), two allowed with a comma; gamepad buttons use SDL names\n"
          "# (a b x y leftshoulder rightshoulder lefttrigger righttrigger start leftstick ...).\n"
          "# 3p / 3k press all three punches / kicks at once.\n", f);
    for (int p = 0; p < 2; p++) {
        char lower[16];
        snprintf(lower, sizeof lower, "%s", s_type_names[s_type[p]]);
        for (char *c = lower; *c; c++) *c = (char)tolower((unsigned char)*c);
        fprintf(f, "p%d_controller = %s\np%d_gamepad = %d\n", p + 1, lower, p + 1, s_pad[p]);
        for (int a = 0; a < ACT_COUNT; a++) {
            const char *k0 = s_key[p][a][0] ? SDL_GetScancodeName((SDL_Scancode)s_key[p][a][0]) : "none";
            if (s_key[p][a][1])
                fprintf(f, "p%d_%s = %s, %s\n", p + 1, s_act_keys[a], k0,
                        SDL_GetScancodeName((SDL_Scancode)s_key[p][a][1]));
            else
                fprintf(f, "p%d_%s = %s\n", p + 1, s_act_keys[a], k0);
        }
        for (int a = ACT_FIRST_BUTTON; a < ACT_COUNT; a++)
            fprintf(f, "p%d_pad_%s = %s\n", p + 1, s_act_keys[a], pad_string(s_btn[p][a]));
    }
}
