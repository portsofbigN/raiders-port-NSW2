// Controller input: nn::hid overrides fed by the Windows keyboard and XInput gamepads. Adapted from TotK Recomp's
// ov_input.c (MIT, MacNativeGames contributors / valentderah): one connected full-key controller as player 1
// (NpadIdType 0, style bit 0), the "docked" controller this host reports; mouse and keyboard devices of the game
// report nothing. State layouts follow libnx HID (NpadFullKeyState 40 bytes).
//
// Keyboard (only while the game window has focus):
//   left stick W A S D, right stick I J K L, D-pad arrow keys,
//   A = Enter / Space, B = Backspace / Esc, X = C, Y = V, L = Q, R = E, ZL = 1, ZR = 3,
//   Plus = P, Minus = M, stick clicks = F (left) / H (right).
// XInput: Nintendo positions (Xbox B -> A, A -> B, Y -> X, X -> Y), LB/RB = L/R, LT/RT = ZL/ZR, Start = Plus,
//   Back = Minus.
// NX2_INPUT_SCRIPT="from-to:Button+Button;..." presses buttons on present numbers from..to (tests), e.g.
//   "2500-2505:Right;2600-2605:A". Button names: A B X Y L R ZL ZR Plus Minus Up Down Left Right, and stick
//   directions LsUp LsDown LsLeft LsRight RsUp RsDown RsLeft RsRight, with Slow for a quarter deflection (up to 256
//   steps).
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "host.h"
#include "input.h"
#include "platform/platform.h"

typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
typedef struct { int64_t sampling; int32_t x, y, dx, dy, wx, wy; uint32_t buttons, attributes; } MouseState;
typedef struct { int64_t sampling; uint32_t modifiers, attributes; uint64_t keys[4]; } KeyboardState;
_Static_assert(sizeof(NpadState) == 40 && sizeof(MouseState) == 40 && sizeof(KeyboardState) == 48, "hid state ABI");

enum {
    B_A = 1 << 0, B_B = 1 << 1, B_X = 1 << 2, B_Y = 1 << 3, B_LS = 1 << 4, B_RS = 1 << 5, B_L = 1 << 6, B_R = 1 << 7,
    B_ZL = 1 << 8, B_ZR = 1 << 9, B_PLUS = 1 << 10, B_MINUS = 1 << 11, B_LEFT = 1 << 12, B_UP = 1 << 13,
    B_RIGHT = 1 << 14, B_DOWN = 1 << 15,
};
enum { STYLE_FULL_KEY = 1, STYLE_HANDHELD = 2, ATTR_CONNECTED = 1 };

static PlatMutex lock;
static uint32_t styles = STYLE_FULL_KEY;
static int enabled, allowed = 1;
static int64_t sample;

// ---- XInput, loaded on first use
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;
typedef struct { DWORD packet; XPad pad; } XState;
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XState *);
static XInputGetStateFn xget;
static int xtried;

__attribute__((unused)) static int xinput(XPad *p) {   // replaced by nx2_pad_read (nx2_pad.c)
    if (!xtried) {
        xtried = 1;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) xget = (XInputGetStateFn)(void *)GetProcAddress(m, "XInputGetState");
        rt_log("hid: XInput %s", xget ? "available" : "not found (keyboard only)");
    }
    if (!xget) return 0;
    for (DWORD i = 0; i < 4; i++) {
        XState s;
        if (xget(i, &s) == ERROR_SUCCESS) {
            *p = s.pad;
            return 1;
        }
    }
    return 0;
}

static int32_t deadzone(int32_t v, int32_t dz) { return v > dz || v < -dz ? v : 0; }

// ---- scripted input
typedef struct { uint64_t from, to; uint64_t buttons; } Script;
#define SCRIPT_MAX 8192
static Script script[SCRIPT_MAX];
// stick directions in scripts (LsUp ... RsRight): high bits, turned into full stick deflections in poll()
#define S_LS_UP (1ull << 56)
#define S_LS_DOWN (1ull << 57)
#define S_LS_LEFT (1ull << 58)
#define S_LS_RIGHT (1ull << 59)
#define S_RS_UP (1ull << 60)
#define S_RS_DOWN (1ull << 61)
#define S_RS_LEFT (1ull << 62)
#define S_RS_RIGHT (1ull << 63)
#define S_SLOW (1ull << 55)   // "Slow": stick directions of that step at a quarter deflection (fine aiming)
static int nscript = -1;

static uint64_t button_named(const char *s, size_t n) {
    static const struct { const char *name; uint64_t bit; } t[] = {
        {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"L", B_L}, {"R", B_R}, {"ZL", B_ZL}, {"ZR", B_ZR},
        {"Plus", B_PLUS}, {"Minus", B_MINUS}, {"Up", B_UP}, {"Down", B_DOWN}, {"Left", B_LEFT}, {"Right", B_RIGHT},
        {"LsUp", S_LS_UP}, {"LsDown", S_LS_DOWN}, {"LsLeft", S_LS_LEFT}, {"LsRight", S_LS_RIGHT},
        {"LStick", B_LS}, {"RStick", B_RS}, {"Slow", S_SLOW}, {"RsUp", S_RS_UP}, {"RsDown", S_RS_DOWN}, {"RsLeft", S_RS_LEFT}, {"RsRight", S_RS_RIGHT},
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (strlen(t[i].name) == n && !strncmp(t[i].name, s, n)) return t[i].bit;
    return 0;
}

static void script_load(void) {
    nscript = 0;
    const char *e = getenv("NX2_INPUT_SCRIPT");
    static char *file_text;   // NX2_INPUT_SCRIPT_FILE=<file>: the same steps from a file (long scripts, recordings)
    const char *fp = getenv("NX2_INPUT_SCRIPT_FILE");
    if (fp && *fp) {
        FILE *f = fopen(fp, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            file_text = calloc(1, (size_t)n + 1);
            if (file_text && fread(file_text, 1, (size_t)n, f) == (size_t)n) {
                char *w = file_text;   // whitespace and newlines out
                for (char *r = file_text; *r; r++)
                    if (*r == 10 || *r == ';') {   // a line ends a step as ';' does
                        if (w > file_text && w[-1] != ';') *w++ = ';';
                    } else if (*r != 13 && *r != ' ' && *r != 9) {
                        *w++ = *r;
                    }
                *w = 0;
                e = file_text;
            }
            fclose(f);
        } else {
            rt_log("hid: cannot open NX2_INPUT_SCRIPT_FILE %s", fp);
        }
    }
    while (e && *e && nscript < SCRIPT_MAX) {
        char *end;
        Script s = {0};
        s.from = strtoull(e, &end, 10);
        s.to = *end == '-' ? strtoull(end + 1, &end, 10) : s.from;
        if (*end != ':') break;
        const char *p = end + 1;
        while (*p && *p != ';') {
            const char *q = p;
            while (*q && *q != '+' && *q != ';') q++;
            s.buttons |= button_named(p, (size_t)(q - p));
            p = *q == '+' ? q + 1 : q;
        }
        script[nscript++] = s;
        e = *p == ';' ? p + 1 : p;
    }
    if (nscript) rt_log("hid: input script with %d steps", nscript);
}

extern uint64_t nx2_vk_frame;
HWND nx2_vk_hwnd(void);

// NX2_INPUT_RECORD=<file>: the real input (keyboard, controller) written as script steps "from-to:Btn+Btn;", one per
// change of state, so a played route replays with NX2_INPUT_SCRIPT_FILE. Stick directions past half deflection are
// recorded full, smaller ones (past the dead zone) with Slow.
static void record(uint64_t b, int32_t ax, int32_t ay, int32_t bx, int32_t by) {
    static int on = -1;
    static FILE *f;
    static uint64_t state, from;
    if (on < 0) {
        const char *p = getenv("NX2_INPUT_RECORD");
        on = p && *p && (f = fopen(p, "w")) != NULL;
        if (on) rt_log("hid: recording input to %s", p);
    }
    if (!on) return;
    uint64_t st = b & 0xffff;
    const int32_t full = 16384, dead = 4000;
    int slow = 0;
    int32_t v[4] = {ax, ay, bx, by};
    uint64_t neg[4] = {S_LS_LEFT, S_LS_DOWN, S_RS_LEFT, S_RS_DOWN}, pos[4] = {S_LS_RIGHT, S_LS_UP, S_RS_RIGHT, S_RS_UP};
    for (int i = 0; i < 4; i++) {
        int32_t a = v[i] < 0 ? -v[i] : v[i];
        if (a <= dead) continue;
        st |= v[i] < 0 ? neg[i] : pos[i];
        slow |= a <= full;
    }
    if (slow) st |= S_SLOW;
    uint64_t now = nx2_vk_frame;
    if (st == state) return;
    if (state) {
        static const struct { const char *name; uint64_t bit; } t[] = {
            {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"L", B_L}, {"R", B_R}, {"ZL", B_ZL}, {"ZR", B_ZR},
            {"Plus", B_PLUS}, {"Minus", B_MINUS}, {"Up", B_UP}, {"Down", B_DOWN}, {"Left", B_LEFT}, {"Right", B_RIGHT},
            {"LStick", B_LS}, {"RStick", B_RS}, {"Slow", S_SLOW}, {"LsUp", S_LS_UP}, {"LsDown", S_LS_DOWN},
            {"LsLeft", S_LS_LEFT}, {"LsRight", S_LS_RIGHT}, {"RsUp", S_RS_UP}, {"RsDown", S_RS_DOWN},
            {"RsLeft", S_RS_LEFT}, {"RsRight", S_RS_RIGHT},
        };
        fprintf(f, "%llu-%llu:", (unsigned long long)from, (unsigned long long)(now > from ? now - 1 : from));
        int first = 1;
        for (size_t i = 0; i < sizeof t / sizeof *t; i++)
            if (state & t[i].bit) fprintf(f, "%s%s", first ? "" : "+", t[i].name), first = 0;
        fprintf(f, ";\n");
        fflush(f);
    }
    state = st, from = now;
}

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    uint64_t b = 0;
    int32_t ax = 0, ay = 0, bx = 0, by = 0;
    HWND w = nx2_vk_hwnd();
    if (w && GetForegroundWindow() == w) {
        if (key(VK_RETURN) || key(VK_SPACE)) b |= B_A;
        if (key(VK_BACK) || key(VK_ESCAPE)) b |= B_B;
        if (key('C')) b |= B_X;
        if (key('V')) b |= B_Y;
        if (key('Q')) b |= B_L;
        if (key('E')) b |= B_R;
        if (key('1')) b |= B_ZL;
        if (key('3')) b |= B_ZR;
        if (key('P')) b |= B_PLUS;
        if (key('M')) b |= B_MINUS;
        if (key('F')) b |= B_LS;
        if (key('H')) b |= B_RS;
        if (key(VK_UP)) b |= B_UP;
        if (key(VK_DOWN)) b |= B_DOWN;
        if (key(VK_LEFT)) b |= B_LEFT;
        if (key(VK_RIGHT)) b |= B_RIGHT;
        ax = (key('D') - key('A')) * 32767, ay = (key('W') - key('S')) * 32767;
        bx = (key('L') - key('J')) * 32767, by = (key('I') - key('K')) * 32767;
    }
    XPad x;
    int nx2_pad_read(XPad *p);   // nx2_pad.c: XInput, DualShock 4, DualSense
    static int by_label = -1;   // NX2_PAD_LAYOUT=xbox: Switch A on the bottom button (A on Xbox, cross on PlayStation)
    if (by_label < 0) by_label = getenv("NX2_PAD_LAYOUT") && !strcmp(getenv("NX2_PAD_LAYOUT"), "xbox");
    if (nx2_pad_read(&x)) {
        WORD g = x.buttons;
        if (by_label) {
            if (g & 0x1000) b |= B_A;      // Xbox A (bottom)
            if (g & 0x2000) b |= B_B;      // Xbox B (right)
            if (g & 0x4000) b |= B_X;      // Xbox X (left)
            if (g & 0x8000) b |= B_Y;      // Xbox Y (top)
        } else {   // by position, as on a Switch controller
            if (g & 0x2000) b |= B_A;      // Xbox B (right)
            if (g & 0x1000) b |= B_B;      // Xbox A (bottom)
            if (g & 0x8000) b |= B_X;      // Xbox Y (top)
            if (g & 0x4000) b |= B_Y;      // Xbox X (left)
        }
        if (g & 0x0100) b |= B_L;
        if (g & 0x0200) b |= B_R;
        if (x.lt > 64) b |= B_ZL;
        if (x.rt > 64) b |= B_ZR;
        if (g & 0x0010) b |= B_PLUS;   // Start
        if (g & 0x0020) b |= B_MINUS;  // Back
        if (g & 0x0040) b |= B_LS;
        if (g & 0x0080) b |= B_RS;
        if (g & 0x0001) b |= B_UP;
        if (g & 0x0002) b |= B_DOWN;
        if (g & 0x0004) b |= B_LEFT;
        if (g & 0x0008) b |= B_RIGHT;
        if (!ax) ax = deadzone(x.lx, 7849);
        if (!ay) ay = deadzone(x.ly, 7849);
        if (!bx) bx = deadzone(x.rx, 8689);
        if (!by) by = deadzone(x.ry, 8689);
    }
    record(b, ax, ay, bx, by);
    if (nscript < 0) script_load();
    {   // NX2_INPUT_LIVE=<file> (tests): lines appended while the game runs are steps relative to the present now
        // ("0-60:LsUp" holds the stick up for the next 61 presents); the present number goes to <file>.frame
        static const char *lp = (const char *)1;
        static long done;
        static uint64_t last_check;
        if (lp == (const char *)1) lp = getenv("NX2_INPUT_LIVE");
        if (lp && *lp && nx2_vk_frame >= last_check + 5) {
            last_check = nx2_vk_frame;
            char fp[600];
            snprintf(fp, sizeof fp, "%s.frame", lp);
            FILE *ff = fopen(fp, "w");
            if (ff) fprintf(ff, "%llu%c", (unsigned long long)nx2_vk_frame, 10), fclose(ff);
            FILE *f = fopen(lp, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long n = ftell(f);
                if (n > done) {
                    char *t = calloc(1, (size_t)(n - done) + 1);
                    fseek(f, done, SEEK_SET);
                    if (t && fread(t, 1, (size_t)(n - done), f) == (size_t)(n - done)) {
                        char *ln = t;
                        while (ln && *ln) {
                            char *nl = strchr(ln, 10);
                            if (!nl) break;   // a line still being written: next time
                            *nl = 0;
                            done += (long)(nl - ln) + 1;
                            char *end;
                            Script s = {0};
                            uint64_t a = strtoull(ln, &end, 10), z = a;
                            if (*end == '-') z = strtoull(end + 1, &end, 10);
                            if (*end == ':' && nscript < SCRIPT_MAX) {
                                s.from = nx2_vk_frame + a, s.to = nx2_vk_frame + z;
                                const char *p = end + 1;
                                while (*p && *p != 13 && *p != ';') {
                                    const char *q = p;
                                    while (*q && *q != '+' && *q != ';' && *q != 13) q++;
                                    s.buttons |= button_named(p, (size_t)(q - p));
                                    p = *q == '+' ? q + 1 : q;
                                }
                                script[nscript++] = s;
                                rt_log("hid: live step %llu-%llu %#llx", (unsigned long long)s.from, (unsigned long long)s.to,
                                       (unsigned long long)s.buttons);
                            }
                            ln = nl + 1;
                        }
                    }
                    free(t);
                }
                fclose(f);
            }
        }
    }
    for (int i = 0; i < nscript; i++)
        if (nx2_vk_frame >= script[i].from && nx2_vk_frame <= script[i].to) {
            uint64_t sb = script[i].buttons;
            b |= sb & ~(0x1ffull << 55);
            int32_t full = sb & S_SLOW ? 8192 : 32767;
            if (sb & S_LS_UP) ay = full;
            if (sb & S_LS_DOWN) ay = -full;
            if (sb & S_LS_LEFT) ax = -full;
            if (sb & S_LS_RIGHT) ax = full;
            if (sb & S_RS_UP) by = full;
            if (sb & S_RS_DOWN) by = -full;
            if (sb & S_RS_LEFT) bx = -full;
            if (sb & S_RS_RIGHT) bx = full;
        }
    // stick directions as buttons (bits 16-19 left stick, 20-23 right stick), as the controller reports them
    const int32_t t = 16384;
    if (ax < -t) b |= 1u << 16;
    if (ay > t) b |= 1u << 17;
    if (ax > t) b |= 1u << 18;
    if (ay < -t) b |= 1u << 19;
    if (bx < -t) b |= 1u << 20;
    if (by > t) b |= 1u << 21;
    if (bx > t) b |= 1u << 22;
    if (by < -t) b |= 1u << 23;
    *buttons = b, *lx = ax, *ly = ay, *rx = bx, *ry = by;
}

// ---- overrides
static void ov_initialize_npad(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    enabled = 1;
    plat_mutex_unlock(&lock);
    rt_log("hid: controllers initialized (one connected full-key controller: keyboard / XInput)");
}

static void ov_supported_ids(Cpu *c) {   // void SetSupportedNpadIdType(const NpadIdType *ids, size_t count)
    const uint32_t *ids = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    int ok = 0;
    for (uint64_t i = 0; ids && i < n && i < 16; i++) ok |= ids[i] == 0;
    plat_mutex_lock(&lock);
    allowed = ok;
    plat_mutex_unlock(&lock);
}

static void ov_supported_styles(Cpu *c) {   // void SetSupportedNpadStyleSet(NpadStyleSet)
    plat_mutex_lock(&lock);
    styles = (uint32_t)arg_x(c, 0);
    plat_mutex_unlock(&lock);
}

static void ov_style_set(Cpu *c) {   // NpadStyleSet GetNpadStyleSet(const NpadIdType &)
    const uint32_t *id = arg_p(c, 0);
    ret_x(c, enabled && allowed && id && *id == 0 ? styles & STYLE_FULL_KEY : 0);
}

static int32_t last_ry;   // the right stick's vertical axis, for the motion sensor below
int32_t nx2_hid_stick_ry(void) { return __atomic_load_n(&last_ry, __ATOMIC_RELAXED); }   // also for nx2_pad.c

// int GetNpadStates(<State> *out, int count, const NpadIdType &id): the newest state only
static void npad_states(Cpu *c, uint32_t want) {
    NpadState *out = arg_p(c, 0);
    int max = (int)arg_x(c, 1);
    const uint32_t *id = arg_p(c, 2);
    if (max <= 0 || !out || !id) {
        ret_x(c, 0);
        return;
    }
    plat_mutex_lock(&lock);
    NpadState s = {.sampling = ++sample};
    int live = want == STYLE_FULL_KEY && enabled && allowed && *id == 0 && (styles & want);
    plat_mutex_unlock(&lock);
    if (live) {
        s.attributes = ATTR_CONNECTED;
        poll(&s.buttons, &s.lx, &s.ly, &s.rx, &s.ry);
        __atomic_store_n(&last_ry, s.ry, __ATOMIC_RELAXED);
        static uint64_t last;
        if (s.buttons != last) {
            static int n;
            if (n++ < 40) rt_log("hid: buttons %#llx (present #%llu)", (unsigned long long)s.buttons,
                                 (unsigned long long)nx2_vk_frame);
            last = s.buttons;
        }
    }
    *out = s;
    ret_x(c, 1);
}
static void ov_states_full_key(Cpu *c) { npad_states(c, STYLE_FULL_KEY); }
static void ov_states_other(Cpu *c) { npad_states(c, 0); }

static void ov_nop(Cpu *c) { (void)c; }

static void ov_mouse_states(Cpu *c) {   // int GetMouseStates(MouseState *out, int count): not connected
    MouseState *out = arg_p(c, 0);
    if ((int)arg_x(c, 1) <= 0 || !out) {
        ret_x(c, 0);
        return;
    }
    plat_mutex_lock(&lock);
    *out = (MouseState){.sampling = ++sample};
    plat_mutex_unlock(&lock);
    ret_x(c, 1);
}

static void ov_keyboard_state(Cpu *c) {   // void GetKeyboardState(KeyboardState *out): no key down
    KeyboardState *out = arg_p(c, 0);
    if (!out) return;
    plat_mutex_lock(&lock);
    *out = (KeyboardState){.sampling = ++sample};
    plat_mutex_unlock(&lock);
}

// ---- vibration devices and the six-axis sensor (TotK Recomp's ov_input.c handle layout: byte 0 the style index,
// byte 1 the controller id, byte 2 the device). Vibration is accepted and not played yet; the sensor reads at rest.
typedef struct { float amp_low, freq_low, amp_high, freq_high; } VibrationValue;
typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], dir[9];
    uint32_t attributes, reserved;
} SixAxisState;
static const VibrationValue vib_neutral = {0, 160, 0, 320};
static int vib_allowed = 1;

static void ov_vibration_handles(Cpu *c) {   // int GetVibrationDeviceHandles(handles *, int, const NpadIdType &, styles)
    uint32_t *out = arg_p(c, 0), style = (uint32_t)arg_x(c, 3);
    int max = (int)arg_x(c, 1);
    const uint32_t *idp = arg_p(c, 2);
    uint32_t id = idp ? *idp : 0;
    static const struct { uint8_t bit, n; uint32_t base; } t[] = {
        {0, 2, 0x3}, {1, 2, 0x4}, {2, 2, 0x5}, {3, 1, 0x6}, {4, 1, 0x10007}, {5, 1, 0x8}, {10, 1, 0xd}, {8, 2, 0x4},
        {30, 2, 0x21}, {29, 2, 0x20}};
    int n = 0;
    for (size_t i = 0; out && max > 0 && i < sizeof t / sizeof *t && !n; i++) {
        if (!(style >> t[i].bit & 1)) continue;
        n = t[i].n < max ? t[i].n : max;
        for (int k = 0; k < n; k++) out[k] = (t[i].base | id << 8) + ((uint32_t)k << 16);
    }
    ret_x(c, (uint64_t)n);
}

static void ov_six_axis_handles(Cpu *c) {   // int GetSixAxisSensorHandles(handles *, int, const NpadIdType &, styles)
    uint32_t *out = arg_p(c, 0), style = (uint32_t)arg_x(c, 3);
    int max = (int)arg_x(c, 1);
    const uint32_t *idp = arg_p(c, 2);
    uint32_t id = idp ? *idp : 0, base = 0, n = 1;
    if (style & 1) base = 0x20003;
    else if (style & 2) base = 0x20004;
    else if (style & 4) base = 0x5, n = 2;
    else if (style & 8) base = 0x6;
    else n = 0;
    if (!out || max <= 0) n = 0;
    if (n > (uint32_t)max) n = (uint32_t)max;
    for (uint32_t k = 0; k < n; k++) out[k] = (base | id << 8) + (k << 16);
    ret_x(c, n);
}

static void ov_result_ok(Cpu *c) { ret_x(c, 0); }
static void ov_vib_actual(Cpu *c) {   // Result GetActualVibrationValue(VibrationValue *, const handle &)
    VibrationValue *out = arg_p(c, 0);
    if (out) *out = vib_neutral;
    ret_x(c, 0);
}
static void ov_vib_info(Cpu *c) {   // Result GetVibrationDeviceInfo(VibrationDeviceInfo *, const handle &)
    uint32_t *out = arg_p(c, 0);
    const uint32_t *h = arg_p(c, 1);
    if (out) out[0] = 1, out[1] = h && (*h >> 16 & 0xff) == 1 ? 2 : 1;   // linear actuator, left / right
    ret_x(c, 0);
}
static void ov_vib_mounted(Cpu *c) {   // Result IsVibrationDeviceMounted(bool *, const handle &)
    uint8_t *out = arg_p(c, 0);
    if (out) *out = 1;
    ret_x(c, 0);
}
static void ov_vib_permitted(Cpu *c) {   // Result IsVibrationPermitted(bool *)
    uint8_t *out = arg_p(c, 0);
    if (out) *out = (uint8_t)vib_allowed;
    ret_x(c, 0);
}
// The game's vibration values (amplitudes of a low and a high frequency band per Joy-Con / controller side) to the
// controller's motors: the low band to the strong motor, the high band to the weak one, both sides together.
static float vib_amp[2][2];   // [side][low, high]
static void vib_apply(const uint32_t *h, const VibrationValue *v) {
    if (!h || !v) return;
    int side = (*h >> 16 & 0xff) == 1;
    vib_amp[side][0] = v->amp_low, vib_amp[side][1] = v->amp_high;
    static int nlog;
    if (nlog++ < 4) rt_log("hid: vibration %.2f / %.2f on side %d", v->amp_low, v->amp_high, side);
    void nx2_pad_rumble(float strong, float weak);
    float lo = vib_amp[0][0] > vib_amp[1][0] ? vib_amp[0][0] : vib_amp[1][0];
    float hi = vib_amp[0][1] > vib_amp[1][1] ? vib_amp[0][1] : vib_amp[1][1];
    nx2_pad_rumble(vib_allowed ? lo : 0, vib_allowed ? hi : 0);
}
static void ov_vib_send(Cpu *c) {   // Result SendVibrationValue(const handle &, const VibrationValue &)
    vib_apply(arg_p(c, 0), arg_p(c, 1));
    ret_x(c, 0);
}
static void ov_vib_send_n(Cpu *c) {   // Result SendVibrationValues(const handle *, const VibrationValue *, int)
    const uint32_t *h = arg_p(c, 0);
    const VibrationValue *v = arg_p(c, 1);
    int n = (int)arg_x(c, 2);
    for (int i = 0; h && v && i < n && i < 8; i++) vib_apply(h + i, v + i);
    ret_x(c, 0);
}
static void ov_vib_permit(Cpu *c) {   // Result PermitVibration(bool)
    vib_allowed = (arg_x(c, 0) & 0xff) != 0;
    ret_x(c, 0);
}

// The motion sensor: level, gravity down the controller's z axis. Splatoon aims up and down with the controller's
// motion by default (the right stick then only turns sideways), so the right stick's vertical axis is reported as
// the controller tilting: angular velocity about x, in turns per second. NX2_GYRO_STICK=<percent> (default 100;
// 0: the sensor at rest, for players who switch motion controls off in the game).
static int six_fill(SixAxisState *s, int max) {
    static uint64_t calls;
    calls++;
    if (!(calls & (calls - 1))) rt_log("hid: motion sensor read %llu times (max %d)", (unsigned long long)calls, max);
    if (max <= 0 || !s) return 0;
    static int pct = -1;
    if (pct < 0) pct = getenv("NX2_GYRO_STICK") ? atoi(getenv("NX2_GYRO_STICK")) : 100;
    // The samples go through the sensor fusion (motion_fusion.c), so direction, angle, rotation and gravity agree as a
    // real controller's do: the stick tilts the controller about its x axis (top edge up for the stick up).
    int nx2_pad_has_motion(void);
    if (nx2_pad_has_motion()) {   // a DualSense / DualShock 4: its own gyro and accelerometer (nx2_pad.c)
        int n = motion_read(motion_global(), (MotionState *)s, max, plat_now_ns());
        static uint64_t next_log;
        static int mlog = -1;   // NX2_MOTION_LOG=1 (debug): the newest state once a second
        if (mlog < 0) mlog = getenv("NX2_MOTION_LOG") && *getenv("NX2_MOTION_LOG") == '1';
        if (mlog && n > 0 && plat_now_ns() > next_log) {
            next_log = plat_now_ns() + 1000000000ull;
            const MotionState *q = (const MotionState *)s;
            rt_log("hid: motion acc %.2f %.2f %.2f G, gyro %.3f %.3f %.3f turns/s, angle %.3f %.3f %.3f, %d samples", q->acc[0],
                   q->acc[1], q->acc[2], q->gyro[0], q->gyro[1], q->gyro[2], q->angle[0], q->angle[1], q->angle[2], n);
        }
        if (n > 0) return n;
    }
    static MotionFusion *m;
    static MotionState prev = {.acc = {0, 0, -1}};
    plat_mutex_lock(&lock);
    if (!m) m = motion_new();
    uint64_t now = plat_now_ns();
    double rate = pct > 0 ? (double)__atomic_load_n(&last_ry, __ATOMIC_RELAXED) / 32767.0 * 2.0 * (double)pct / 100.0 : 0;
    double gyro[3] = {rate, 0, 0}, acc[3] = {-prev.dir[2], -prev.dir[5], -prev.dir[8]};   // gravity in the controller's frame
    if (!prev.sampling) acc[0] = acc[1] = 0, acc[2] = -1;
    motion_feed(m, acc, gyro, now);
    int n = motion_read(m, (MotionState *)s, max, now);
    if (n > 0) prev = *(MotionState *)s;
    plat_mutex_unlock(&lock);
    return n;
}
// Result detail::GetSixAxisSensorStates(int *count, SixAxisSensorState *out, int max, const handle &)
static void ov_six_states_detail(Cpu *c) {
    int32_t *count = arg_p(c, 0);
    int n = six_fill(arg_p(c, 1), (int)arg_x(c, 2));
    if (count) *count = n;
    ret_x(c, 0);
}
// Result detail::GetSixAxisSensorState(SixAxisSensorState *out, const handle &)
static void ov_six_state_detail(Cpu *c) {
    six_fill(arg_p(c, 0), 1);
    ret_x(c, 0);
}
// int GetSixAxisSensorStates(SixAxisSensorState *out, int count, const handle &)
static void ov_six_axis_states(Cpu *c) { ret_x(c, (uint64_t)six_fill(arg_p(c, 0), (int)arg_x(c, 1))); }

// Result ShowControllerSupport(ControllerSupportResultInfo *out, const ControllerSupportArg &): one player, No. 1
static void ov_controller_support(Cpu *c) {
    int32_t *out = arg_p(c, 0);
    if (out) out[0] = 1, out[1] = 0;
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn3hid14InitializeNpadEv", ov_initialize_npad);
HOST_OVERRIDE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", ov_supported_ids);
HOST_OVERRIDE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", ov_supported_styles);
HOST_OVERRIDE("_ZN2nn3hid15GetNpadStyleSetERKj", ov_style_set);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", ov_states_full_key);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", ov_states_other);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", ov_states_other);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", ov_states_other);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", ov_states_other);
HOST_OVERRIDE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", ov_nop);
HOST_OVERRIDE("_ZN2nn3hid14DisconnectNpadERKj", ov_nop);
HOST_OVERRIDE("_ZN2nn3hid15InitializeMouseEv", ov_nop);
HOST_OVERRIDE("_ZN2nn3hid18InitializeKeyboardEv", ov_nop);
HOST_OVERRIDE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", ov_mouse_states);
HOST_OVERRIDE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", ov_keyboard_state);
HOST_OVERRIDE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
              ov_six_axis_states);
HOST_OVERRIDE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
              ov_controller_support);
HOST_OVERRIDE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_"
              "12NpadStyleTagEEE", ov_vibration_handles);
HOST_OVERRIDE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_"
              "12NpadStyleTagEEE", ov_six_axis_handles);
HOST_OVERRIDE("_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", ov_result_ok);
HOST_OVERRIDE("_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", ov_vib_send);
HOST_OVERRIDE("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi",
              ov_vib_send_n);
HOST_OVERRIDE("_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE",
              ov_vib_actual);
HOST_OVERRIDE("_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE",
              ov_vib_info);
HOST_OVERRIDE("_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE", ov_vib_mounted);
HOST_OVERRIDE("_ZN2nn3hid6detail20IsVibrationPermittedEPb", ov_vib_permitted);
HOST_OVERRIDE("_ZN2nn3hid6detail15PermitVibrationEb", ov_vib_permit);
HOST_OVERRIDE("_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", ov_result_ok);
HOST_OVERRIDE("_ZN2nn3hid6detail17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", ov_result_ok);
HOST_OVERRIDE("_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
              ov_six_states_detail);
HOST_OVERRIDE("_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE",
              ov_six_state_detail);
