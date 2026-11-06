// Controllers for nx2_hid.c: XInput (Xbox and anything Steam Input presents as one) and Sony controllers read
// straight from their HID reports (DualShock 4, DualSense, DualSense Edge; USB and Bluetooth). Every controller is
// turned into the XInput state layout, so nx2_hid.c maps one format to the Switch buttons. Hot-plug: XInput slots that
// were empty are tried again once a second; Sony controllers are searched for every two seconds by a background
// thread, which then reads their reports. Rumble (nx2_pad_rumble): XInput motors, Sony motors (USB and Bluetooth).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include "host.h"
#include "input.h"
#include "platform/platform.h"

typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPad;   // XINPUT_GAMEPAD
typedef struct { DWORD packet; XPad pad; } XState;
typedef struct { WORD left, right; } XVibration;
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XState *);
typedef DWORD(WINAPI *XInputSetStateFn)(DWORD, XVibration *);

enum {   // XINPUT_GAMEPAD_* buttons
    XB_UP = 0x0001, XB_DOWN = 0x0002, XB_LEFT = 0x0004, XB_RIGHT = 0x0008, XB_START = 0x0010, XB_BACK = 0x0020,
    XB_LTHUMB = 0x0040, XB_RTHUMB = 0x0080, XB_LSHOULDER = 0x0100, XB_RSHOULDER = 0x0200, XB_A = 0x1000,
    XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000,
};

// ---- XInput
static XInputGetStateFn xget;
static XInputSetStateFn xset;
static int xtried, xslot = -1;
static uint64_t xretry_ms;

static int xinput_read(XPad *p) {
    if (!xtried) {
        xtried = 1;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) {
            xget = (XInputGetStateFn)(void *)GetProcAddress(m, "XInputGetState");
            xset = (XInputSetStateFn)(void *)GetProcAddress(m, "XInputSetState");
        }
        rt_log("pad: XInput %s", xget ? "available" : "not found");
    }
    if (!xget) return 0;
    XState s;
    if (xslot >= 0) {
        if (xget((DWORD)xslot, &s) == ERROR_SUCCESS) {
            *p = s.pad;
            return 1;
        }
        rt_log("pad: Xbox controller %d disconnected", xslot);
        xslot = -1;
    }
    uint64_t now = plat_now_ms();
    if (now < xretry_ms) return 0;   // empty slots are slow to ask: once a second
    xretry_ms = now + 1000;
    for (DWORD i = 0; i < 4; i++)
        if (xget(i, &s) == ERROR_SUCCESS) {
            xslot = (int)i;
            rt_log("pad: Xbox controller %d connected", xslot);
            *p = s.pad;
            return 1;
        }
    return 0;
}

// ---- Sony controllers (HID)
static PlatMutex sony_lock;
static XPad sony_state;
static volatile LONG sony_connected;
static HANDLE sony_handle = INVALID_HANDLE_VALUE;
static int sony_kind, sony_bt;   // 4 DualShock 4, 5 DualSense; Bluetooth
static DWORD sony_in_len;        // bytes of one input report (a read returns every report queued since the last)
static uint8_t sony_ex[2];       // last rumble sent (strong, weak)
static wchar_t sony_path[512];   // the device, opened again for writing (a read in progress would hold a write up)
static HANDLE sony_out = INVALID_HANDLE_VALUE;

static SHORT axis(uint8_t v, int invert) {   // 0..255, 128 at rest -> -32768..32767
    int c = (int)v - 128;
    if (invert) c = -c - 1;
    int r = c * 256 + (c > 0 ? c * 2 : 0);
    return (SHORT)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}

static WORD dpad(uint8_t hat) {   // 0 up, clockwise to 7, 8 released
    static const WORD t[9] = {XB_UP, XB_UP | XB_RIGHT, XB_RIGHT, XB_DOWN | XB_RIGHT, XB_DOWN, XB_DOWN | XB_LEFT,
                              XB_LEFT, XB_UP | XB_LEFT, 0};
    return t[hat < 8 ? hat : 8];
}

// The common part of both pads' reports: sticks at s[0..3]; face buttons and hat at s[b0], shoulders, triggers,
// share / options and stick clicks at s[b0 + 1]; analog triggers at s[lt], s[rt]. Positional mapping to the Xbox
// layout: cross -> A (bottom), circle -> B (right), square -> X (left), triangle -> Y (top).
static void parse(const uint8_t *s, int b0, int lt, int rt, XPad *o) {
    memset(o, 0, sizeof *o);
    o->lx = axis(s[0], 0), o->ly = axis(s[1], 1), o->rx = axis(s[2], 0), o->ry = axis(s[3], 1);
    uint8_t f = s[b0], g = s[b0 + 1];
    WORD b = dpad(f & 15);
    if (f & 0x10) b |= XB_X;   // square
    if (f & 0x20) b |= XB_A;   // cross
    if (f & 0x40) b |= XB_B;   // circle
    if (f & 0x80) b |= XB_Y;   // triangle
    if (g & 0x01) b |= XB_LSHOULDER;
    if (g & 0x02) b |= XB_RSHOULDER;
    if (g & 0x10) b |= XB_BACK;    // share / create
    if (g & 0x20) b |= XB_START;   // options
    if (g & 0x40) b |= XB_LTHUMB;
    if (g & 0x80) b |= XB_RTHUMB;
    o->buttons = b;
    o->lt = s[lt], o->rt = s[rt];
}

static int sony_report(const uint8_t *r, DWORD n, XPad *o) {
    if (sony_kind == 4) {
        if (r[0] == 0x01 && n >= 10) { parse(r + 1, 4, 7, 8, o); return 1; }    // USB, or Bluetooth's basic report
        if (r[0] == 0x11 && n >= 12) { parse(r + 3, 4, 7, 8, o); return 1; }    // Bluetooth full report
    } else {   // the length does not tell the reports apart: Windows pads every Bluetooth report to 78 bytes
        if (r[0] == 0x01 && !sony_bt && n >= 64) { parse(r + 1, 7, 4, 5, o); return 1; }   // USB
        if (r[0] == 0x31 && n >= 64) { parse(r + 2, 7, 4, 5, o); return 1; }               // Bluetooth full report
        if (r[0] == 0x01 && sony_bt && n >= 10) { parse(r + 1, 4, 7, 8, o); return 1; }    // Bluetooth basic report
    }
    return 0;
}

static HANDLE sony_open(void) {
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA id = {sizeof id};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &id); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &id, NULL, 0, &need, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = calloc(1, need);
        if (!d) continue;
        d->cbSize = sizeof *d;
        if (SetupDiGetDeviceInterfaceDetailW(set, &id, d, need, NULL, NULL)) {
            HANDLE h = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                   OPEN_EXISTING, 0, NULL);
            HIDD_ATTRIBUTES a = {sizeof a};
            if (h != INVALID_HANDLE_VALUE && HidD_GetAttributes(h, &a) && a.VendorID == 0x054c) {
                int kind = a.ProductID == 0x05c4 || a.ProductID == 0x09cc || a.ProductID == 0x0ba0 ? 4
                         : a.ProductID == 0x0ce6 || a.ProductID == 0x0df2 ? 5 : 0;
                PHIDP_PREPARSED_DATA pp;
                HIDP_CAPS caps = {0};
                if (kind && HidD_GetPreparsedData(h, &pp)) {
                    HidP_GetCaps(pp, &caps);
                    HidD_FreePreparsedData(pp);
                }
                if (kind && caps.UsagePage == 1 && (caps.Usage == 5 || caps.Usage == 4)) {   // the gamepad collection
                    sony_kind = kind;
                    sony_bt = caps.OutputReportByteLength > 64 || caps.InputReportByteLength > 64;
                    sony_in_len = caps.InputReportByteLength;
                    if (sony_bt) {   // reading the calibration switches Bluetooth to the full report (0x11, 0x31)
                        uint8_t fr[64] = {kind == 4 ? 0x02 : 0x05};
                        HidD_GetFeature(h, fr, kind == 4 ? sizeof fr : 41);
                    }
                    found = h;
                    wcsncpy(sony_path, d->DevicePath, 511);
                    rt_log("pad: %s connected (%s)", kind == 4 ? "DualShock 4" : "DualSense", sony_bt ? "Bluetooth" : "USB");
                    h = INVALID_HANDLE_VALUE;
                }
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        free(d);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// ---- Sony motion sensors: every full report's gyro and accelerometer go to the sensor fusion (motion_fusion.c), which
// the game's six-axis sensor reads (nx2_hid.c). Raw axes are SDL's (x right, y out of the face, z toward the player):
// gyro 16.384 per degree/s, accelerometer 8192 per G; turned into the Switch controller's frame as input.c does. The
// gyro's bias is learned while the controller lies still (no turning faster than ~2 degrees/s for a moment), so the
// camera does not drift. NX2_GYRO=0: off. The right stick's vertical axis adds a turn about x (NX2_GYRO_STICK).
static volatile LONG64 motion_last_ns;   // the last sample fed
int nx2_pad_has_motion(void) {
    uint64_t t = (uint64_t)motion_last_ns;
    return t && plat_now_ns() - t < 100000000ull;
}

static void sony_motion(const uint8_t *r, DWORD n, uint64_t when) {
    static int off = -1, stick = -1;
    if (off < 0) off = getenv("NX2_GYRO") && *getenv("NX2_GYRO") == '0';
    if (stick < 0) stick = getenv("NX2_GYRO_STICK") ? atoi(getenv("NX2_GYRO_STICK")) : 100;
    if (off) return;
    const uint8_t *d = NULL;   // the report's data after the id (and the Bluetooth header)
    int g = 0, a = 0;
    if (sony_kind == 4) {
        if (r[0] == 0x11 && n >= 32) d = r + 3;
        else if (r[0] == 0x01 && !sony_bt && n >= 32) d = r + 1;
        g = 12, a = 18;
    } else {
        if (r[0] == 0x31 && n >= 64) d = r + 2;
        else if (r[0] == 0x01 && !sony_bt && n >= 64) d = r + 1;
        g = 15, a = 21;
    }
    if (!d) return;
    int16_t raw[6];
    for (int i = 0; i < 3; i++) raw[i] = (int16_t)(d[g + 2 * i] | d[g + 2 * i + 1] << 8);
    for (int i = 0; i < 3; i++) raw[3 + i] = (int16_t)(d[a + 2 * i] | d[a + 2 * i + 1] << 8);
    // bias: follows the reading slowly while all three axes stay within ~2 degrees/s of it for 0.25 s
    static double bias[3];
    static int still, have_bias;
    int quiet = 1;
    for (int i = 0; i < 3; i++)
        if (fabs(raw[i] - bias[i]) > (have_bias ? 32.0 : 400.0)) quiet = 0;
    still = quiet ? still + 1 : 0;
    if (still > 60) {
        double k = have_bias ? 0.01 : 0.2;
        for (int i = 0; i < 3; i++) bias[i] += (raw[i] - bias[i]) * k;
        if (!have_bias && still > 120) {
            have_bias = 1;
            rt_log("pad: motion sensor bias %.1f %.1f %.1f (raw units)", bias[0], bias[1], bias[2]);
        }
    }
    {
        static int mlog = -1;
        static uint64_t nlog;
        if (mlog < 0) mlog = getenv("NX2_MOTION_LOG") && *getenv("NX2_MOTION_LOG") == '1';
        if (mlog && !(nlog++ % 250))
            rt_log("pad: raw report %#x (%lu bytes): gyro %d %d %d accel %d %d %d, ry %d", r[0], (unsigned long)n, raw[0], raw[1], raw[2],
                   raw[3], raw[4], raw[5], sony_state.ry);
    }
    const double dps = 1.0 / 16.384 * 3.14159265358979 / 180.0;   // raw -> rad/s
    double sg[3] = {(raw[0] - bias[0]) * dps, (raw[1] - bias[1]) * dps, (raw[2] - bias[2]) * dps};
    double sa[3] = {raw[3] / 8192.0, raw[4] / 8192.0, raw[5] / 8192.0};
    double acc[3] = {-sa[0], sa[2], -sa[1]}, gyro[3] = {sg[0], -sg[2], sg[1]};   // SDL axes -> Switch axes (input.c)
    if (stick > 0) {   // the right stick's vertical axis (any input: keyboard too) as a turn about x, as without a gyro
        int32_t nx2_hid_stick_ry(void);
        double ry = nx2_hid_stick_ry() / 32767.0;
        if (fabs(ry) > 0.12) gyro[0] += ry * 2.0 * stick / 100.0;
    }
    uint64_t last = (uint64_t)motion_last_ns;
    if (when <= last) when = last + 1000;
    motion_feed(motion_global(), acc, gyro, when);
    InterlockedExchange64(&motion_last_ns, (LONG64)when);
    static int logged;
    if (!logged++) rt_log("pad: motion sensors on (%s)", sony_kind == 4 ? "DualShock 4" : "DualSense");
}

static DWORD WINAPI sony_thread(LPVOID arg) {
    (void)arg;
    uint8_t buf[600];
    for (;;) {
        if (sony_handle == INVALID_HANDLE_VALUE) {
            HANDLE h = sony_open();
            if (h == INVALID_HANDLE_VALUE) {
                Sleep(2000);
                continue;
            }
            sony_handle = h;
        }
        DWORD n = 0;
        if (!ReadFile(sony_handle, buf, sizeof buf, &n, NULL)) {
            rt_log("pad: Sony controller disconnected");
            InterlockedExchange(&sony_connected, 0);
            sony_ex[0] = sony_ex[1] = 0;
            plat_mutex_lock(&sony_lock);
            CloseHandle(sony_handle);
            sony_handle = INVALID_HANDLE_VALUE;
            plat_mutex_unlock(&sony_lock);
            continue;
        }
        DWORD at = 0;   // several reports queued: the newest for the buttons, every one for the motion sensors
        if (sony_in_len && n >= 2 * sony_in_len) {
            DWORD cnt = n / sony_in_len;
            uint64_t now = plat_now_ns();
            for (DWORD k = 0; k < cnt; k++) sony_motion(buf + k * sony_in_len, sony_in_len, now - (uint64_t)(cnt - 1 - k) * 4000000ull);
            at = (cnt - 1) * sony_in_len, n = sony_in_len;
        } else {
            sony_motion(buf, n, plat_now_ns());
        }
        XPad p;
        if (sony_report(buf + at, n, &p)) {
            static int log = -1;   // NX2_PAD_LOG=1 (debug): each change of the buttons, with the report it came in
            static WORD last = 0xFFFF;
            if (log < 0) log = getenv("NX2_PAD_LOG") && *getenv("NX2_PAD_LOG") == '1';
            if (log && (p.buttons != last || (p.lt > 128) != (sony_state.lt > 128) || (p.rt > 128) != (sony_state.rt > 128)))
                rt_log("pad: report %#x (%lu bytes): buttons %#06x, triggers %u %u", buf[at], (unsigned long)n, p.buttons,
                       p.lt, p.rt);
            last = p.buttons;
            plat_mutex_lock(&sony_lock);
            sony_state = p;
            plat_mutex_unlock(&sony_lock);
            InterlockedExchange(&sony_connected, 1);
        }
    }
    return 0;
}

static uint32_t crc32_bt(const uint8_t *p, size_t n) {   // Bluetooth output reports: CRC-32 of 0xA2 then the report
    uint32_t c = 0xFFFFFFFFu;
    uint8_t seed = 0xA2;
    for (size_t i = 0; i <= n; i++) {
        c ^= i ? p[i - 1] : seed;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

static void sony_rumble(uint8_t strong, uint8_t weak) {   // USB and Bluetooth
    if (!sony_connected || (sony_ex[0] == strong && sony_ex[1] == weak)) return;
    sony_ex[0] = strong, sony_ex[1] = weak;
    uint8_t r[80] = {0};
    DWORD n = 0;
    if (sony_bt) {   // 78 bytes ending in the CRC
        static uint8_t seq;
        if (sony_kind == 4) {
            r[0] = 0x11, r[1] = 0xC0, r[3] = 0x01, r[6] = weak, r[7] = strong;   // HID + CRC; flags: motors
        } else {
            r[0] = 0x31, r[1] = (uint8_t)(seq << 4), r[2] = 0x10;   // sequence, tag
            r[3] = 0x03, r[5] = weak, r[6] = strong;   // flags: compatible vibration + haptics select
            seq = (seq + 1) & 15;
        }
        uint32_t c = crc32_bt(r, 74);
        memcpy(r + 74, &c, 4);
        n = 78;
    } else if (sony_kind == 4) {
        r[0] = 0x05, r[1] = 0x01, r[4] = weak, r[5] = strong;   // flags: motors
        n = 32;
    } else {
        r[0] = 0x02, r[1] = 0x03, r[3] = weak, r[4] = strong;   // flags: compatible vibration + haptics select
        n = 48;
    }
    if (sony_out == INVALID_HANDLE_VALUE)
        sony_out = CreateFileW(sony_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD done;
    if (sony_out != INVALID_HANDLE_VALUE && !WriteFile(sony_out, r, n, &done, NULL)) {
        CloseHandle(sony_out);   // unplugged: opened again with the next controller
        sony_out = INVALID_HANDLE_VALUE;
    }
}

// ---- interface for nx2_hid.c
static int pads_off = -1;

// The first connected controller's state in the XInput layout; 0 when none (or NX2_CONTROLLER=keyboard).
int nx2_pad_read(XPad *p) {
    if (pads_off < 0) {
        const char *e = getenv("NX2_CONTROLLER");
        pads_off = e && !strcmp(e, "keyboard");
        if (!pads_off) CreateThread(NULL, 0, sony_thread, NULL, 0, NULL);
        rt_log("pad: controllers %s", pads_off ? "off (keyboard only)" : "on (Xbox / XInput, DualShock 4, DualSense)");
    }
    if (pads_off) return 0;
    if (xinput_read(p)) return 1;
    if (sony_connected) {
        plat_mutex_lock(&sony_lock);
        *p = sony_state;
        plat_mutex_unlock(&sony_lock);
        return 1;
    }
    return 0;
}

// Rumble: strong (low frequency) and weak (high frequency) motors, 0..1, scaled by NX2_RUMBLE (percent, default 100).
void nx2_pad_rumble(float strong, float weak) {
    static int scale = -1;
    if (scale < 0) scale = getenv("NX2_RUMBLE") ? atoi(getenv("NX2_RUMBLE")) : 100;
    if (pads_off > 0 || scale <= 0) return;
    float k = (float)scale / 100.0f;
    strong = strong * k > 1 ? 1 : strong * k, weak = weak * k > 1 ? 1 : weak * k;
    if (strong < 0) strong = 0;
    if (weak < 0) weak = 0;
    static WORD last[2] = {1, 1};
    WORD s16 = (WORD)(strong * 65535), w16 = (WORD)(weak * 65535);
    if (xset && xslot >= 0 && (s16 != last[0] || w16 != last[1])) {
        XVibration v = {s16, w16};
        xset((DWORD)xslot, &v);
        last[0] = s16, last[1] = w16;
    }
    sony_rumble((uint8_t)(strong * 255), (uint8_t)(weak * 255));
}
