// NVN2 programs on the host, and shader capture. The driver's nvnProgramInitializeFromBuilder needs its GPU state,
// so programs are host objects. Their shader stages are captured here for translation (Eglantine's
// shader_recompiler, third_party/eglantine): each stage's code (GPU address == CPU address) and control section are
// written to work/shaders/ as <program serial>-<stage index>.code / .ctrl, with the raw NVNshaderData entries
// logged for the first programs so the NVN2 layout of that structure can be confirmed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nx2_nvn.h"
#include "platform/platform.h"

#define MAX_STAGES 8
#define DATA_SIZE 16          // NVN1 NVNshaderData: { NVNbufferAddress data; const void *control; }
#define CODE_WINDOW 0x40000   // bytes of code captured per stage (cut at the end of its memory pool)
#define CTRL_WINDOW 0x1000

typedef struct { uint64_t builder; int count; uint8_t data[MAX_STAGES][DATA_SIZE]; } BuilderShaders;
#define MAX_BUILDERS 64
static BuilderShaders builders[MAX_BUILDERS];
static PlatMutex lock;
static uint64_t programs;

static void save(const char *path, const void *p, uint64_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, (size_t)n, f);
    fclose(f);
}

// Programs are recorded in place (the game may copy the object): {magic, serial} at offset 0; the stages live in a
// host table indexed by serial.
#define PROG_MAGIC 0x50325850u   // "PX2P"
#define MAX_PROGRAMS 65536
static Nx2Program table[MAX_PROGRAMS];

int nx2_prog_get(uint64_t obj, Nx2Program *out) {
    const uint32_t *p = (const uint32_t *)(uintptr_t)obj;
    if (!obj || p[0] != PROG_MAGIC || p[1] == 0 || p[1] >= MAX_PROGRAMS) return 0;
    *out = table[p[1]];
    return out->serial == p[1];
}

static int no_capture = -1;

static void capture(uint64_t program, int count, const uint8_t (*data)[DATA_SIZE]) {
    uint64_t serial = __atomic_add_fetch(&programs, 1, __ATOMIC_RELAXED);
    if (serial < MAX_PROGRAMS) {
        Nx2Program *t = &table[serial];
        t->count = count < MAX_STAGES ? count : MAX_STAGES;
        for (int i = 0; i < t->count; i++) {
            memcpy(&t->code[i], data[i], 8);
            memcpy(&t->ctrl[i], data[i] + 8, 8);
            uint64_t end = nx2_pool_end(t->code[i]);
            t->code_size[i] = end > t->code[i] ? end - t->code[i] : 0;
            t->const_off[i] = t->const_size[i] = 0;
            const uint32_t *ctl = (const uint32_t *)(uintptr_t)t->ctrl[i];
            if (t->ctrl[i] && t->ctrl[i] < (1ull << 39) && ctl[0] == 0x98761234u && ctl[0x44 / 4] >= ctl[0x40 / 4] &&
                ctl[0x44 / 4] <= t->code_size[i]) {
                t->const_off[i] = ctl[0x40 / 4];
                t->const_size[i] = ctl[0x44 / 4] - ctl[0x40 / 4];
            }
        }
        t->serial = (uint32_t)serial;
        uint32_t *p = (uint32_t *)(uintptr_t)program;
        p[0] = PROG_MAGIC, p[1] = (uint32_t)serial;
    }
    // NX2_SHADER_CAPTURE=1 (debug): the stages as files in shaders/ (the in-game translator reads them from memory)
    if (no_capture < 0) {
        const char *e = getenv("NX2_SHADER_CAPTURE");
        no_capture = !(e && *e == '1');
    }
    if (no_capture) return;
    plat_file_mkdir_p("shaders");
    for (int i = 0; i < count && i < MAX_STAGES; i++) {
        uint64_t code, ctrl;
        memcpy(&code, data[i], 8);
        memcpy(&ctrl, data[i] + 8, 8);
        if (serial <= 4) {
            char hex[3 * DATA_SIZE + 1] = {0};
            for (int k = 0; k < DATA_SIZE; k++) snprintf(hex + 3 * k, 4, "%02x ", data[i][k]);
            rt_log("nvn2: program %#llx #%llu stage %d: NVNshaderData %s", (unsigned long long)program,
                   (unsigned long long)serial, i, hex);
        }
        char path[128];
        uint64_t end = nx2_pool_end(code);
        uint64_t n = end > code ? end - code : 0;
        if (n > CODE_WINDOW) n = CODE_WINDOW;
        if (n) {
            snprintf(path, sizeof path, "shaders/%05llu-%d.code", (unsigned long long)serial, i);
            save(path, (const void *)(uintptr_t)code, n);
        }
        if (ctrl && ctrl < (1ull << 39)) {
            snprintf(path, sizeof path, "shaders/%05llu-%d.ctrl", (unsigned long long)serial, i);
            save(path, (const void *)(uintptr_t)ctrl, CTRL_WINDOW);
        }
        if (serial <= 4 || !(serial % 500))
            rt_log("nvn2: program #%llu stage %d: code %#llx (%#llx bytes captured), control %#llx",
                   (unsigned long long)serial, i, (unsigned long long)code, (unsigned long long)n,
                   (unsigned long long)ctrl);
    }
}

// void nvnProgramBuilderSetShaders(NVNprogramBuilder *, int count, const NVNshaderData *): kept per builder, and
// forwarded so the driver's builder stays consistent for any getter the game calls.
static void pb_set_shaders(Cpu *c) {
    static uint64_t real;
    uint64_t b = c->x[0];
    int count = (int)c->x[1];
    plat_mutex_lock(&lock);
    BuilderShaders *s = NULL;
    for (int i = 0; i < MAX_BUILDERS && !s; i++)
        if (builders[i].builder == b) s = &builders[i];
    for (int i = 0; i < MAX_BUILDERS && !s; i++)
        if (!builders[i].builder) s = &builders[i];
    if (!s) s = &builders[b / 16 % MAX_BUILDERS];   // reuse a slot: builders are short-lived stack objects
    s->builder = b;
    s->count = count < MAX_STAGES ? count : MAX_STAGES;
    memcpy(s->data, (const void *)(uintptr_t)c->x[2], (size_t)s->count * DATA_SIZE);
    plat_mutex_unlock(&lock);
    nx2_driver_call(c, &real, "nvnProgramBuilderSetShaders", 3, c->x[0], c->x[1], c->x[2], 0);
}

// NVNboolean nvnProgramInitializeFromBuilder(NVNprogram *, const NVNprogramBuilder *)
static void p_initialize_from_builder(Cpu *c) {
    uint64_t program = c->x[0], b = c->x[1];
    BuilderShaders s = {0};
    plat_mutex_lock(&lock);
    for (int i = 0; i < MAX_BUILDERS; i++)
        if (builders[i].builder == b) s = builders[i];
    plat_mutex_unlock(&lock);
    if (s.builder) capture(program, s.count, (const uint8_t (*)[DATA_SIZE])s.data);
    else rt_log("nvn2: program %#llx from builder %#llx without shaders", (unsigned long long)program,
                (unsigned long long)b);
    c->x[0] = 1;
}

// NVNboolean nvnProgramSetShaders(NVNprogram *, int count, const NVNshaderData *)
static void p_set_shaders(Cpu *c) {
    capture(c->x[0], (int)c->x[1], (const uint8_t (*)[DATA_SIZE])(uintptr_t)c->x[2]);
    c->x[0] = 1;
}
// nvnProgramSetShadersExt: 24-byte entries {code, control, 8 more bytes} (stage 1 at +24 in the game's calls)
static void p_set_shaders_ext(Cpu *c) {
    int n = (int)c->x[1] < MAX_STAGES ? (int)c->x[1] : MAX_STAGES;
    uint8_t data[MAX_STAGES][DATA_SIZE];
    const uint8_t *src = (const uint8_t *)(uintptr_t)c->x[2];
    for (int i = 0; i < n; i++) memcpy(data[i], src + 24 * i, DATA_SIZE);
    static int logged;
    if (logged++ < 2) {
        char hex[3 * 48 + 1] = {0};
        for (int k = 0; k < 48; k++) snprintf(hex + 3 * k, 4, "%02x ", src[k]);
        rt_log("nvn2: SetShadersExt(%d stages) entries: %s", n, hex);
    }
    capture(c->x[0], n, (const uint8_t (*)[DATA_SIZE])data);
    c->x[0] = 1;
}

static void p_ret1(Cpu *c) { c->x[0] = 1; }
static void p_ret0(Cpu *c) { c->x[0] = 0; }

const Nx2HostProc nx2_prog_procs[] = {
    {"nvnProgramBuilderSetShaders", pb_set_shaders},
    {"nvnProgramInitializeFromBuilder", p_initialize_from_builder},
    {"nvnProgramInitialize", p_ret1},
    {"nvnProgramFinalize", p_ret0},
    {"nvnProgramSetDebugLabel", p_ret0},
    {"nvnProgramSetShaders", p_set_shaders},
    {"nvnProgramSetShadersExt", p_set_shaders_ext},
    {"nvnProgramSetSampleShading", p_ret0},
    {"nvnProgramGetSubtileSize", p_ret0},
    {"nvnProgramSetSubroutineLinkage", p_ret1},
    {"nvnProgramGetShaderGroupHandle", p_ret0},
    {"nvnProgramGetScratchBytesPerWarp", p_ret0},
    {"nvnProgramGetDeviceComputeMemorySize", p_ret0},
};
const int nx2_prog_nprocs = sizeof nx2_prog_procs / sizeof *nx2_prog_procs;
