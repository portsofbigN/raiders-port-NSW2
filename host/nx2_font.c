// Built-in system fonts (nn::pl). On Switch 2 they come from the console's firmware through pl:u (shared memory plus
// per-font offset and size, Eglantine NX2_NOTES row 36), which is not part of the game dump. Until the player's own
// firmware fonts can be used, every font type gets a TrueType font from this PC's Windows fonts folder, loaded once
// into guest-visible memory (used locally only; NX2_FONT=<path> picks another file).
//
// Result layout, as the SDK's GetBuiltInFontData writes it (sdk+0x59a670): u8 available, u8 flag, then at +8 the
// font's address and at +16 its size.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

static uint8_t *font;
static uint64_t font_size;
static PlatOnce once;

static void load(void *arg) {
    (void)arg;
    const char *path = getenv("NX2_FONT");
    if (!path) path = "C:/Windows/Fonts/arial.ttf";
    int64_t n = plat_file_size(path);
    if (n <= 0) {
        rt_log("pl: no font at %s; built-in fonts reported unavailable", path);
        return;
    }
    PlatFile *f = plat_file_open(path);
    uint8_t *p = f ? mem_kernel_alloc((uint64_t)n, 4096) : NULL;
    if (!p || plat_file_pread(f, p, (uint32_t)n, 0) != n) rt_fatal("pl: cannot read %s", path);
    plat_file_close(f);
    font = p;
    font_size = (uint64_t)n;
    rt_log("pl: built-in fonts served from %s (%lld bytes)", path, (long long)n);
}

// void nn::pl::GetBuiltInFontData(BuiltInFontData{Ounce,Nx} *out, BuiltInFontType{Ounce,Nx} type, bool)
static void ov_builtin_font(Cpu *c) {
    plat_once(&once, load, NULL);
    uint8_t *out = (uint8_t *)(uintptr_t)c->x[0];
    static int logged[64];
    int type = (int)c->x[1];
    if (type >= 0 && type < 64 && !logged[type]++) rt_log("pl: GetBuiltInFontData type %d", type);
    if (!out) return;
    memset(out, 0, 24);
    if (!font) return;
    out[0] = 1;
    uint64_t a = (uint64_t)(uintptr_t)font, n = font_size;
    memcpy(out + 8, &a, 8);
    memcpy(out + 16, &n, 8);
}

// nn::Result nn::pl::RequestLoadNxFont(void *buffer, size_t size): the fonts are already resident
static void ov_request_load(Cpu *c) { c->x[0] = 0; }

HOST_OVERRIDE("_ZN2nn2pl18GetBuiltInFontDataEPNS0_20BuiltInFontDataOunceENS0_20BuiltInFontTypeOunceEb", ov_builtin_font);
HOST_OVERRIDE("_ZN2nn2pl18GetBuiltInFontDataEPNS0_17BuiltInFontDataNxENS0_17BuiltInFontTypeNxEb", ov_builtin_font);
HOST_OVERRIDE("_ZN2nn2pl17RequestLoadNxFontEPvm", ov_request_load);
