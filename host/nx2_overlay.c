// Performance overlay (NX2_OVERLAY=off|fps|full, F1 cycles): a small box in the top-left corner of the window with
// the frame rate and, in full mode, frame times, CPU and GPU time, draws, shader builds, memory and resolution.
// The text is drawn with GDI into a BGRA bitmap (the swapchain's byte order) four times a second; nx2_vk.c copies
// that bitmap onto every presented image. Frame statistics come from the presents (nx2_overlay_present) and from the
// renderer's counters (nx2_stat_* in nx2_vkdraw.c / nx2_cmd.c).
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#include "host.h"
#include "platform/platform.h"

extern uint64_t nx2_stat_submit_ns, nx2_stat_wait_ns, nx2_stat_pipes;
uint64_t nx2_stat_draws(void);

static volatile LONG mode = -1;   // 0 off, 1 fps, 2 full
static const char *gpu_name = "";
static uint64_t (*vram_used)(uint64_t *budget);

void nx2_overlay_cycle(void) {
    if (mode < 0) mode = 1;
    InterlockedExchange(&mode, (mode + 1) % 3);
}

static int get_mode(void) {
    if (mode < 0) {
        const char *e = getenv("NX2_OVERLAY");
        LONG m = 1;
        if (e && !strcmp(e, "off")) m = 0;
        if (e && !strcmp(e, "full")) m = 2;
        InterlockedCompareExchange(&mode, m, -1);
    }
    return (int)mode;
}

void nx2_overlay_info(const char *gpu, uint64_t (*vram)(uint64_t *budget)) { gpu_name = gpu, vram_used = vram; }

// ---- frame statistics
#define HIST 256
static uint64_t frame_ns[HIST];   // the last frame intervals
static int nhist, hpos;
static uint64_t last_present;

// ---- the bitmap
static HDC dc;
static HBITMAP bmp;
static HFONT font;
static uint32_t *bits;
static uint32_t bw, bh, font_px;
static int bitmap_ready;
static uint64_t last_draw;
static uint32_t box_w, box_h;   // the part of the bitmap in use

static void ensure_bitmap(uint32_t fpx) {
    if (bmp && font_px == fpx) return;
    if (!dc) dc = CreateCompatibleDC(NULL);
    if (bmp) DeleteObject(bmp), bmp = NULL;
    if (font) DeleteObject(font), font = NULL;
    font_px = fpx;
    bw = fpx * 34, bh = fpx * 12;
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), (LONG)bw, -(LONG)bh, 1, 32, BI_RGB}};
    void *p = NULL;
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, NULL, 0);
    bits = p;
    SelectObject(dc, bmp);
    font = CreateFontW(-(int)fpx, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY,
                       FIXED_PITCH, L"Consolas");
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
}

static void line(int *y, COLORREF c, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SetTextColor(dc, c);
    TextOutA(dc, (int)(font_px / 2), *y, buf, (int)strlen(buf));
    SIZE sz;
    GetTextExtentPoint32A(dc, buf, (int)strlen(buf), &sz);
    if ((uint32_t)sz.cx + font_px > box_w) box_w = (uint32_t)sz.cx + font_px;
    *y += (int)(font_px * 1.25);
}

static void render(uint32_t win_w, uint32_t win_h, uint32_t img_w, uint32_t img_h, uint64_t now) {
    int m = get_mode();
    uint32_t fpx = win_h / 45;
    if (fpx < 13) fpx = 13;
    if (fpx > 40) fpx = 40;
    ensure_bitmap(fpx);
    if (!bits) return;
    // the interval since the last drawing: frame times, renderer counters, process times
    static uint64_t t0, s0, w0, d0, p0, c0;
    uint64_t dt = now - t0;
    uint64_t sub = nx2_stat_submit_ns, wait = nx2_stat_wait_ns, draws = nx2_stat_draws(), pipes = nx2_stat_pipes;
    FILETIME ct, et, kt, ut;
    GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut);
    uint64_t cpu = (((uint64_t)kt.dwHighDateTime << 32 | kt.dwLowDateTime) +
                    ((uint64_t)ut.dwHighDateTime << 32 | ut.dwLowDateTime)) * 100;
    int n = nhist < 120 ? nhist : 120;
    double sum = 0, mx = 0, mn = 1e18;
    for (int i = 0; i < n; i++) {
        double f = (double)frame_ns[(hpos - 1 - i + HIST) % HIST] * 1e-6;
        sum += f, mx = f > mx ? f : mx, mn = f < mn ? f : mn;
    }
    double avg = n ? sum / n : 0, fps = avg > 0 ? 1000.0 / avg : 0;
    double frames = avg > 0 && dt ? (double)dt * 1e-6 / avg : 1;
    if (frames < 1) frames = 1;
    for (uint32_t i = 0; i < bw * bh; i++) bits[i] = 0xff16141a;   // background (BGRA)
    box_w = 0;
    int y = (int)(font_px / 3);
    COLORREF fc = fps >= 57 ? RGB(110, 240, 120) : fps >= 45 ? RGB(250, 220, 90) : RGB(255, 100, 90);
    line(&y, fc, "FPS %5.1f   %5.2f ms", fps, avg);
    if (m == 2 && t0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        double cpu_pct = dt ? 100.0 * (double)(cpu - c0) / (double)dt / si.dwNumberOfProcessors : 0;
        static int en = -1;   // NX2_UI_LANG (the launcher's language)
        if (en < 0) en = getenv("NX2_UI_LANG") && !strcmp(getenv("NX2_UI_LANG"), "en");
        line(&y, RGB(230, 230, 235), en ? "Frame   min %5.2f  max %5.2f ms" : "Quadro  min %5.2f  max %5.2f ms", mn, mx);
        line(&y, RGB(230, 230, 235), en ? "Render CPU %5.2f ms  GPU wait %5.2f ms" : "Render CPU %5.2f ms  espera GPU %5.2f ms",
             (double)(sub - s0) * 1e-6 / frames, (double)(wait - w0) * 1e-6 / frames);
        line(&y, RGB(230, 230, 235), en ? "Draws %5.0f/frame  new shaders %llu" : "Draws %5.0f/quadro  shaders novos %llu",
             (double)(draws - d0) / frames, (unsigned long long)(pipes - p0));
        PROCESS_MEMORY_COUNTERS_EX pm = {sizeof pm};
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
        uint64_t budget = 0, used = vram_used ? vram_used(&budget) : 0;
        line(&y, RGB(230, 230, 235), "VRAM %5.0f / %5.0f MiB  RAM %5.0f MiB", used / 1048576.0, budget / 1048576.0,
             pm.WorkingSetSize / 1048576.0);
        line(&y, RGB(230, 230, 235), en ? "CPU %4.0f%%  image %ux%u  window %ux%u" : "CPU %4.0f%%  imagem %ux%u  janela %ux%u",
             cpu_pct, img_w, img_h, win_w, win_h);
        line(&y, RGB(170, 175, 190), "%s  Vulkan", gpu_name);
    }
    t0 = now, s0 = sub, w0 = wait, d0 = draws, p0 = pipes, c0 = cpu;
    box_h = (uint32_t)y + font_px / 3;
    if (box_w > bw) box_w = bw;
    if (box_h > bh) box_h = bh;
    GdiFlush();
    for (uint32_t i = 0; i < bw * bh; i++) bits[i] |= 0xff000000u;   // GDI leaves alpha 0 where it drew
    bitmap_ready = 1;
}

// Every present: records the frame interval. Returns the overlay bitmap to copy at the window's top-left corner
// (BGRA rows of *stride pixels, *w x *h), or NULL when the overlay is off.
const uint32_t *nx2_overlay_present(uint32_t win_w, uint32_t win_h, uint32_t img_w, uint32_t img_h, uint32_t *w,
                                    uint32_t *h, uint32_t *stride) {
    uint64_t now = plat_now_ns();
    if (last_present) {
        frame_ns[hpos] = now - last_present;
        hpos = (hpos + 1) % HIST;
        if (nhist < HIST) nhist++;
    }
    last_present = now;
    if (!get_mode()) return NULL;
    static int last_mode = -1;
    if (!bitmap_ready || now - last_draw > 250000000ull || last_mode != get_mode() || font_px != (win_h / 45 < 13 ? 13 : win_h / 45 > 40 ? 40 : win_h / 45)) {
        render(win_w, win_h, img_w, img_h, now);
        last_draw = now, last_mode = get_mode();
    }
    if (!bitmap_ready || box_w + 8 > win_w || box_h + 8 > win_h) return NULL;
    *w = box_w, *h = box_h, *stride = bw;
    return bits;
}
