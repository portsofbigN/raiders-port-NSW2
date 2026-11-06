// Replay of recorded NVN2 command lists: draw state is tracked command by command (nx2_exec, called from
// nx2_cmd.c for every submitted command), and draws are executed on Vulkan (nx2_vkdraw.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nx2_draw.h"
#include "nx2_nvn.h"
#include "platform/platform.h"

Nx2DrawState nx2_state;
uint64_t nx2_draws;

enum {
    C_NONE, C_SET_TEXTURE_POOL, C_SET_SAMPLER_POOL, C_SET_RT, C_SET_VIEWPORT, C_SET_SCISSOR, C_SET_DEPTH_RANGE,
    C_BIND_PROGRAM, C_BIND_UBO, C_BIND_TEXTURE, C_UPDATE_UBO, C_BIND_ATTRIB, C_BIND_STREAM, C_BIND_VB,
    C_DRAW_ELEMENTS, C_DRAW_ARRAYS, C_BIND_BLEND, C_BIND_COLOR, C_BIND_CHANNEL_MASK, C_BIND_DEPTH, C_BIND_POLYGON,
    C_CLEAR_COLOR, C_DRAW_ELEMENTS_BV, C_SET_BLEND_COLOR, C_DRAW_ELEMENTS_INST, C_DRAW_ARRAYS_INST,
    C_CLEAR_DEPTH, C_BIND_SSBO, C_BIND_IMAGE, C_DISPATCH, C_COPY_TEX, C_BIND_TEXTURES, C_RENDER_ENABLE,
    C_RENDER_COND, C_PUSH_GFX, C_PUSH_CS, C_REPORT_COUNTER, C_RESET_COUNTER, C_COPY_B2T, C_COPY_B2B, C_COPY_T2B, C_REPORT_VALUE,
    C_DRAW_ELEMENTS_INDIRECT, C_DRAW_ARRAYS_INDIRECT, C_SIGNAL_EVENT, C_WAIT_EVENT, C_STENCIL_REF, C_STENCIL_VMASK,
    C_STENCIL_WMASK,
};
static const struct { const char *name; int kind; } names[] = {
    {"SetTexturePool", C_SET_TEXTURE_POOL}, {"SetSamplerPool", C_SET_SAMPLER_POOL},
    {"SetRenderTargets", C_SET_RT}, {"SetViewport", C_SET_VIEWPORT}, {"SetScissor", C_SET_SCISSOR},
    {"SetDepthRange", C_SET_DEPTH_RANGE}, {"BindProgram", C_BIND_PROGRAM}, {"BindUniformBuffer", C_BIND_UBO},
    {"BindTexture", C_BIND_TEXTURE}, {"UpdateUniformBuffer", C_UPDATE_UBO}, {"BindVertexAttribState", C_BIND_ATTRIB},
    {"BindVertexStreamState", C_BIND_STREAM}, {"BindVertexBuffer", C_BIND_VB}, {"DrawElements", C_DRAW_ELEMENTS},
    {"DrawArrays", C_DRAW_ARRAYS}, {"BindBlendState", C_BIND_BLEND}, {"BindColorState", C_BIND_COLOR},
    {"BindChannelMaskState", C_BIND_CHANNEL_MASK}, {"BindDepthStencilState", C_BIND_DEPTH},
    {"BindPolygonState", C_BIND_POLYGON}, {"ClearColor", C_CLEAR_COLOR},
    {"DrawElementsBaseVertex", C_DRAW_ELEMENTS_BV}, {"SetBlendColor", C_SET_BLEND_COLOR},
    {"DrawElementsInstanced", C_DRAW_ELEMENTS_INST}, {"DrawArraysInstanced", C_DRAW_ARRAYS_INST},
    {"ClearDepthStencil", C_CLEAR_DEPTH}, {"BindStorageBuffer", C_BIND_SSBO}, {"BindImage", C_BIND_IMAGE},
    {"DispatchCompute", C_DISPATCH}, {"SetRenderEnable", C_RENDER_ENABLE},
    {"SetRenderEnableConditional", C_RENDER_COND}, {"PushGraphicsConstants", C_PUSH_GFX},
    {"PushComputeConstants", C_PUSH_CS}, {"CopyTextureToTexture", C_COPY_TEX}, {"BindTextures", C_BIND_TEXTURES},
    {"ReportCounter", C_REPORT_COUNTER}, {"ResetCounter", C_RESET_COUNTER},
    {"ReportValue", C_REPORT_VALUE}, {"CopyBufferToTexture", C_COPY_B2T}, {"CopyBufferToBuffer", C_COPY_B2B}, {"CopyTextureToBuffer", C_COPY_T2B},
    {"DrawElementsIndirect", C_DRAW_ELEMENTS_INDIRECT}, {"DrawArraysIndirect", C_DRAW_ARRAYS_INDIRECT},
    {"SignalEvent", C_SIGNAL_EVENT}, {"WaitEvent", C_WAIT_EVENT}, {"SetStencilRef", C_STENCIL_REF},
    {"SetStencilValueMask", C_STENCIL_VMASK}, {"SetStencilMask", C_STENCIL_WMASK},
};
// Resolved per proc the first time it is replayed: the game asks for some procs only later (DrawElementsIndirect
// first in gameplay), so a table built once at the first replay left them out and they were dropped.
static int kinds[2048];
static uint8_t kinds_known[2048];

static int kind_of(uint32_t proc) {
    if (proc >= 2048) return C_NONE;
    if (!kinds_known[proc]) {
        const char *full = nx2_proc_name_at((int)proc);
        kinds[proc] = C_NONE;
        if (full && !strncmp(full, "nvnCommandBuffer", 16))
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (!strcmp(full + 16, names[i].name)) kinds[proc] = names[i].kind;
        kinds_known[proc] = 1;
    }
    return kinds[proc];
}

static void hexdump(const char *what, const void *p, size_t n) {
    char hex[3 * 64 + 1] = {0};
    if (n > 64) n = 64;
    for (size_t b = 0; b < n; b++) snprintf(hex + 3 * b, 4, "%02x ", ((const uint8_t *)p)[b]);
    rt_log("      %s: %s", what, hex);
}

// Texture handle -> texture object through the bound texture pool (NVN: texture id in bits 0-19, sampler id above).
int nx2_resolve_texture(uint64_t handle, uint64_t *tex, uint64_t *sampler) {
    uint64_t view, sview;
    if (!nx2_dpool_lookup(nx2_state.texture_pool, (int)(handle & 0xFFFFF), tex, &view)) return 0;
    if (!nx2_dpool_lookup(nx2_state.sampler_pool, (int)(handle >> 20 & 0xFFF), sampler, &sview)) *sampler = 0;
    return 1;
}

static void debug_draw(void) {
    Nx2DrawState *s = &nx2_state;
    Nx2Program p;
    if (nx2_prog_get(s->program, &p)) {
        rt_log("      program %#llx: serial %u, %d stages", (unsigned long long)s->program, p.serial, p.count);
        for (int i = 0; i < p.count; i++) {
            const uint32_t *h = p.code_size[i] ? (const uint32_t *)(uintptr_t)p.code[i] : NULL;
            rt_log("        stage %d: code %#llx (%#llx bytes to pool end), control %#llx, header %08x %08x", i,
                   (unsigned long long)p.code[i], (unsigned long long)p.code_size[i], (unsigned long long)p.ctrl[i],
                   h ? h[0] : 0, h ? h[1] : 0);
        }
    } else {
        rt_log("      program %#llx is not a host program", (unsigned long long)s->program);
    }
    for (int st = 0; st < 6; st++)
        for (int i = 0; i < 4; i++) {
            if (s->ubo[st][i].size) {
                rt_log("      ubo stage %d [%d]: %#llx size %#llx", st, i, (unsigned long long)s->ubo[st][i].addr,
                       (unsigned long long)s->ubo[st][i].size);
                hexdump("data", (const void *)(uintptr_t)s->ubo[st][i].addr, 64);
            }
            if (s->tex[st][i]) {
                uint64_t t = 0, smp = 0;
                int ok = nx2_resolve_texture(s->tex[st][i], &t, &smp);
                nvn_texdesc d;
                uint8_t *storage;
                uint64_t size;
                if (ok && nx2_tex_get(t, &d, &storage, &size))
                    rt_log("      texture stage %d [%d]: handle %#llx -> %#llx %ux%ux%u format %#x target %d levels %d "
                           "flags %#x, sampler %#llx", st, i, (unsigned long long)s->tex[st][i],
                           (unsigned long long)t, d.width, d.height, d.depth, d.format, d.target, d.levels, d.flags,
                           (unsigned long long)smp);
                else
                    rt_log("      texture stage %d [%d]: handle %#llx not resolved (%#llx)", st, i,
                           (unsigned long long)s->tex[st][i], (unsigned long long)t);
            }
        }
    for (int i = 0; i < 4; i++)
        if (s->vb[i].size) {
            const float *v = (const float *)(uintptr_t)s->vb[i].addr;
            rt_log("      vertex buffer %d: %#llx size %#llx: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", i,
                   (unsigned long long)s->vb[i].addr, (unsigned long long)s->vb[i].size, v[0], v[1], v[2], v[3], v[4],
                   v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
        }
    for (int i = 0; i < s->vi.nattrib; i++)
        if (s->vi.a[i].format)
            rt_log("      attrib %d: format %#x offset %d stream %d", i, s->vi.a[i].format, s->vi.a[i].offset,
                   s->vi.a[i].stream);
    for (int i = 0; i < s->vi.nstream; i++)
        if (s->vi.s[i].stride)
            rt_log("      stream %d: stride %d divisor %d", i, s->vi.s[i].stride, s->vi.s[i].divisor);
    for (int i = 0; i < s->rt.count; i++) {
        nvn_texdesc d;
        uint8_t *storage;
        uint64_t size;
        if (nx2_tex_get(s->rt.color[i], &d, &storage, &size))
            rt_log("      render target %d: %#llx %ux%u format %#x", i, (unsigned long long)s->rt.color[i], d.width,
                   d.height, d.format);
    }
}

int nx2_vk_tracing(void);

// Rendering enable (SetRenderEnable / SetRenderEnableConditional): while off, draws and clears are dropped. The
// conditional form compares the two counter reports at the address ({u64 value, u64 timestamp} each, 16 bytes
// apart) when the command executes: mode 0 renders if they are equal, mode 1 if they differ.
static int render_off;
static uint64_t samples_base;   // ResetCounter(SAMPLES_PASSED): the draw count it starts from

void nx2_exec(uint32_t proc, const uint64_t *x, const float *f, const uint8_t *blob, uint32_t blob_n, int dump) {
    Nx2DrawState *s = &nx2_state;
    if (nx2_vk_tracing()) {
        const char *nm = nx2_proc_name_at((int)proc) + 16;
        static int all = -1;   // NX2_TRACE_ALL=1: every command of the traced frame
        if (all < 0) all = getenv("NX2_TRACE_ALL") != NULL;
        if (all && !strcmp(nm, "SetRenderTargets") && blob) {
            const Nx2RenderTargets *t = (const Nx2RenderTargets *)blob;
            rt_log("trace   SetRenderTargets n %d: %llx %llx depth %llx", t->count, (unsigned long long)t->color[0],
                   (unsigned long long)t->color[1], (unsigned long long)t->depth);
        } else if (all || !strncmp(nm, "Push", 4) || !strncmp(nm, "Copy", 4) || !strcmp(nm, "BindUniformBuffer") ||
            !strcmp(nm, "BindProgram") || !strcmp(nm, "BindTextures"))
            rt_log("trace   %s %llx %llx %llx %llx %llx %llx", nm, (unsigned long long)x[1], (unsigned long long)x[2],
                   (unsigned long long)x[3], (unsigned long long)x[4], (unsigned long long)x[5],
                   (unsigned long long)x[6]);
    }
    static int defaults;
    if (!defaults) {   // NVN defaults: all channels written, blending off (ONE, ZERO, ADD), filled polygons
        defaults = 1;
        for (int t = 0; t < 8; t++) s->rs.mask[t] = 0xf, s->rs.blend[t] = (Nx2Blend){2, 1, 2, 1, 1, 1};
        s->rs.polygon_mode = 2, s->rs.depth_func = 8, s->rs.front_ccw = 1;
        for (int f = 0; f < 2; f++) {   // stencil: always, keep, masks all bits
            s->rs.sfunc[f] = 8, s->rs.sop[f][0] = s->rs.sop[f][1] = s->rs.sop[f][2] = 1;
            s->stencil_vmask[f] = s->stencil_wmask[f] = 0xff;
        }
    }
    int k0 = kind_of(proc);
    if (k0 == C_DRAW_ELEMENTS_INDIRECT || k0 == C_DRAW_ARRAYS_INDIRECT) {
        static int n;
        if (n++ < 6)
            rt_log("nvn2 replay: %s (%#llx %#llx %#llx %#llx)%s", k0 == C_DRAW_ELEMENTS_INDIRECT ? "DrawElementsIndirect" : "DrawArraysIndirect",
                   (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3], (unsigned long long)x[4],
                   render_off ? ", rendering off" : "");
    }
    if (render_off && (k0 == C_DRAW_ELEMENTS || k0 == C_DRAW_ELEMENTS_BV || k0 == C_DRAW_ARRAYS ||
                       k0 == C_DRAW_ELEMENTS_INST || k0 == C_DRAW_ARRAYS_INST || k0 == C_CLEAR_COLOR ||
                       k0 == C_CLEAR_DEPTH || k0 == C_DRAW_ELEMENTS_INDIRECT || k0 == C_DRAW_ARRAYS_INDIRECT))
        return;
    switch (k0) {
    case C_SET_TEXTURE_POOL: s->texture_pool = x[1]; break;
    case C_SET_SAMPLER_POOL: s->sampler_pool = x[1]; break;
    case C_SET_RT:
        if (blob && blob_n >= sizeof s->rt) memcpy(&s->rt, blob, sizeof s->rt);
        break;
    case C_SET_VIEWPORT: s->viewport[0] = (int)x[1], s->viewport[1] = (int)x[2], s->viewport[2] = (int)x[3],
                         s->viewport[3] = (int)x[4]; break;
    case C_SET_SCISSOR: s->scissor[0] = (int)x[1], s->scissor[1] = (int)x[2], s->scissor[2] = (int)x[3],
                        s->scissor[3] = (int)x[4]; break;
    case C_SET_DEPTH_RANGE: s->depth_range[0] = f[0], s->depth_range[1] = f[1]; break;
    case C_BIND_PROGRAM: s->program = x[1], s->program_stages = (uint32_t)x[2]; break;
    case C_BIND_UBO:   // (cb, stage, index, addr, size)
        if (x[1] < 6 && x[2] < NX2_MAX_UBO) s->ubo[x[1]][x[2]] = (Nx2Range){x[3], x[4]};
        break;
    case C_BIND_TEXTURE:   // (cb, stage, index, handle)
        if (x[1] < 6 && x[2] < NX2_MAX_TEX) s->tex[x[1]][x[2]] = x[3];
        break;
    case C_UPDATE_UBO:   // (cb, addr, size, offset, dataSize, data): the GPU writes the data into the buffer in order
        if (blob && x[3] + blob_n <= x[2]) memcpy((void *)(uintptr_t)(x[1] + x[3]), blob, blob_n);
        break;
    case C_BIND_ATTRIB:
        if (blob && blob_n == sizeof s->vi) {
            const Nx2VertexInputState *v = (const Nx2VertexInputState *)blob;
            memcpy(s->vi.a, v->a, sizeof s->vi.a);
            s->vi.nattrib = v->nattrib;
        }
        break;
    case C_BIND_STREAM:
        if (blob && blob_n == sizeof s->vi) {
            const Nx2VertexInputState *v = (const Nx2VertexInputState *)blob;
            memcpy(s->vi.s, v->s, sizeof s->vi.s);
            s->vi.nstream = v->nstream;
        }
        break;
    case C_BIND_VB:   // (cb, index, addr, size)
        if (x[1] < NX2_MAX_VB) s->vb[x[1]] = (Nx2Range){x[2], x[3]};
        break;
    case C_BIND_BLEND: case C_BIND_COLOR: case C_BIND_CHANNEL_MASK: case C_BIND_DEPTH: case C_BIND_POLYGON: {
        if (!blob || blob_n != sizeof(Nx2StateBlob)) break;
        const Nx2StateBlob *b = (const Nx2StateBlob *)blob;
        Nx2Raster *r = &s->rs;
        switch (b->kind) {
        case NX2_ST_BLEND:
            if (b->v[0] >= 0 && b->v[0] < 8)
                r->blend[b->v[0]] = (Nx2Blend){(uint8_t)b->v[1], (uint8_t)b->v[2], (uint8_t)b->v[3], (uint8_t)b->v[4],
                                               (uint8_t)b->v[5], (uint8_t)b->v[6]};
            break;
        case NX2_ST_COLOR: r->blend_enable = (uint8_t)b->v[0]; break;
        case NX2_ST_CHANNEL: for (int t = 0; t < 8; t++) r->mask[t] = (uint8_t)b->v[t]; break;
        case NX2_ST_DEPTH:
            r->depth_test = (uint8_t)b->v[0], r->depth_write = (uint8_t)b->v[1], r->depth_func = (uint8_t)b->v[2];
            r->stencil_test = (uint8_t)b->v[3];
            for (int f = 0; f < 2; f++) {
                r->sfunc[f] = (uint8_t)b->v[4 + f];
                for (int k = 0; k < 3; k++) r->sop[f][k] = (uint8_t)(b->v[6 + f] >> (8 * k));
            }
            break;
        case NX2_ST_POLYGON: r->cull = (uint8_t)b->v[0], r->front_ccw = (uint8_t)b->v[1],
                             r->polygon_mode = (uint8_t)b->v[2]; break;
        }
        break;
    }
    case C_COPY_TEX:
        if (blob && blob_n == sizeof(Nx2Copy)) nx2_vk_copy_texture((const Nx2Copy *)blob);
        break;
    case C_BIND_TEXTURES:   // (cb, stage, first, count, const NVNtextureHandle *): handles copied at record time
        if (blob && x[1] < 6)
            for (uint32_t i = 0; i < blob_n / 8 && x[2] + i < NX2_MAX_TEX; i++)
                memcpy(&s->tex[x[1]][x[2] + i], blob + 8 * i, 8);
        break;
    case C_PUSH_GFX: case C_PUSH_CS: {   // (cb, ?, offset, size, data): data copied at record time
        uint8_t *dst = k0 == C_PUSH_GFX ? s->push_gfx : s->push_cs;
        if (blob && x[2] + blob_n <= 256) memcpy(dst + x[2], blob, blob_n);
        break;
    }
    case C_RENDER_ENABLE:   // (cb, NVNboolean enable)
        render_off = !(x[1] & 0xff);
        break;
    case C_REPORT_COUNTER: {   // (cb, NVNcounterType, NVNbufferAddress): {u64 counter, u64 timestamp} written there
        // In command order, so the conditional rendering that compares reports sees them: the game clears a target
        // "once" by rendering if two reports differ and then reporting a timestamp (counter 0) over the first.
        // Timestamps are in GPU ticks (19.2 MHz); samples passed is approximated by the draws since the reset (the
        // GPU's result is not known when the commands are replayed), other counters read 0.
        uint64_t *d = (uint64_t *)(uintptr_t)x[2];
        {   // NX2_COUNTER_LOG=1 (debug): how many reports of each counter type, every 600 presents
            static int on = -1;
            static uint64_t by_type[32], last_frame;
            if (on < 0) on = getenv("NX2_COUNTER_LOG") && *getenv("NX2_COUNTER_LOG") == '1';
            if (on) {
                by_type[(uint32_t)x[1] & 31]++;
                extern uint64_t nx2_vk_frame;
                if (nx2_vk_frame >= last_frame + 600) {
                    last_frame = nx2_vk_frame;
                    char line[400];
                    size_t o = 0;
                    for (int t = 0; t < 32; t++)
                        if (by_type[t]) o += (size_t)snprintf(line + o, sizeof line - o, " type %d: %llu", t, (unsigned long long)by_type[t]);
                    rt_log("nvn2 replay: counter reports by present %llu:%s", (unsigned long long)nx2_vk_frame, line);
                    memset(by_type, 0, sizeof by_type);
                }
            }
        }
        if (d && !((uintptr_t)d & 7)) {
            uint32_t type = (uint32_t)x[1];
            d[0] = type == 1 ? nx2_draws - samples_base : 0;
            static int64_t fake = -2;   // NX2_SAMPLES_FAKE=<n> (debug): every samples-passed report reads n
            if (fake == -2) fake = getenv("NX2_SAMPLES_FAKE") ? atoll(getenv("NX2_SAMPLES_FAKE")) : -1;
            if (type == 1 && fake >= 0) d[0] = (uint64_t)fake;
            d[1] = plat_now_ns() * 12 / 625;
            if (type == 1 && fake < 0) nx2_vk_counter_report(x[2]);   // the GPU's count replaces the estimate
            static int rtrap = -1;   // NX2_READ_TRAP=2 (debug): who reads the sample counts the GPU reports
            if (rtrap < 0) rtrap = getenv("NX2_READ_TRAP") && *getenv("NX2_READ_TRAP") == '2';
            extern uint64_t nx2_vk_frame;
            if (rtrap && type == 1 && nx2_vk_frame > 10700) {
                void nx2_rtrap_arm(uint64_t base, uint64_t size, const char *what);
                char w[96];
                snprintf(w, sizeof w, "samples report %#llx (value %llu)", (unsigned long long)x[2], (unsigned long long)d[0]);
                nx2_rtrap_arm(x[2], 16, w);
            }
        }
        break;
    }
    case C_COPY_B2T: case C_COPY_B2B: case C_COPY_T2B: {   // not executed yet: counted
        static uint64_t n[3];
        int k = k0 == C_COPY_B2T ? 0 : k0 == C_COPY_B2B ? 1 : 2;
        n[k]++;
        if (n[k] <= 6 || !(n[k] & (n[k] - 1)))
            rt_log("nx2: %s #%llu (%llx %llx %llx %llx %llx) not executed",
                   k == 0 ? "CopyBufferToTexture" : k == 1 ? "CopyBufferToBuffer" : "CopyTextureToBuffer",
                   (unsigned long long)n[k], (unsigned long long)x[1], (unsigned long long)x[2],
                   (unsigned long long)x[3], (unsigned long long)x[4], (unsigned long long)x[5]);
        break;
    }
    case C_REPORT_VALUE: {   // (cb, uint32_t value, NVNbufferAddress): the value written there, in command order
        uint32_t *d = (uint32_t *)(uintptr_t)x[2];
        if (d && !((uintptr_t)d & 3)) __atomic_store_n(d, (uint32_t)x[1], __ATOMIC_RELEASE);
        static int n;
        if (n++ < 8) rt_log("nvn2 replay: ReportValue %#x to %#llx", (uint32_t)x[1], (unsigned long long)x[2]);
        break;
    }
    case C_RESET_COUNTER:   // (cb, NVNcounterType)
        if ((uint32_t)x[1] == 1) samples_base = nx2_draws, nx2_vk_counter_reset();
        break;
    case C_RENDER_COND: {   // (cb, NVNconditionalRenderMode mode, NVNbufferAddress counters)
        const uint64_t *c = (const uint64_t *)(uintptr_t)x[2];
        int equal = c ? c[0] == c[2] : 1;
        render_off = (int)x[1] == 0 ? !equal : equal;
        if (nx2_vk_tracing())
            rt_log("trace   conditional mode %d: %llu vs %llu -> %s", (int)x[1], c ? (unsigned long long)c[0] : 0ull,
                   c ? (unsigned long long)c[2] : 0ull, render_off ? "OFF" : "on");
        break;
    }
    case C_BIND_SSBO:   // (cb, stage, index, addr, size)
        if (x[1] < 6 && x[2] < NX2_MAX_UBO) s->ssbo[x[1]][x[2]] = (Nx2Range){x[3], x[4]};
        break;
    case C_BIND_IMAGE:   // (cb, stage, index, NVNimageHandle)
        if (x[1] < 6 && x[2] < 8) s->image[x[1]][x[2]] = x[3];
        break;
    case C_DISPATCH:   // (cb, groupsX, groupsY, groupsZ)
        if (dump) {
            Nx2Program p;
            if (nx2_prog_get(s->program, &p))
                rt_log("      dispatch %u x %u x %u: program serial %u (%d stages), code %#llx, control %#llx, "
                       "constants +%#x (%#x bytes)", (unsigned)x[1], (unsigned)x[2], (unsigned)x[3], p.serial, p.count,
                       (unsigned long long)p.code[0], (unsigned long long)p.ctrl[0], p.const_off[0], p.const_size[0]);
            if (nx2_prog_get(s->program, &p) && p.ctrl[0])
                for (int o = 0; o < 0x1b0; o += 32) {
                    const uint32_t *w = (const uint32_t *)(uintptr_t)(p.ctrl[0] + o);
                    rt_log("        ctrl+%03x: %08x %08x %08x %08x %08x %08x %08x %08x", o, w[0], w[1], w[2], w[3], w[4],
                           w[5], w[6], w[7]);
                }
        }
        nx2_vk_dispatch((uint32_t)x[1], (uint32_t)x[2], (uint32_t)x[3]);
        break;
    case C_CLEAR_DEPTH:   // (cb, float depth, NVNboolean depthMask, int stencil, int stencilMask)
        nx2_vk_clear_depth(f[0], (int)(x[1] & 0xff), (int)x[2], (int)x[3]);
        break;
    case C_SET_BLEND_COLOR: if (blob) memcpy(s->blend_color, blob, 16); break;
    case C_DRAW_ELEMENTS:   // (cb, mode, type, count, indexBuffer)
    case C_DRAW_ELEMENTS_BV: {   // (cb, mode, type, count, indexBuffer, baseVertex)
        Nx2Draw d = {(int)x[1], (int)x[2], (int)x[3], x[4], 0, 0,
                     kind_of(proc) == C_DRAW_ELEMENTS_BV ? (int)x[5] : 0, 1};
        if (dump) {
            const uint8_t *ib = (const uint8_t *)(uintptr_t)x[4];
            rt_log("      draw elements mode %d type %d count %d", d.mode, d.index_type, d.count);
            hexdump("indices", ib, 24);
            debug_draw();
        }
        nx2_draws++;
        nx2_vk_draw(&d);
        break;
    }
    case C_DRAW_ELEMENTS_INST: {   // (cb, mode, type, count, indexBuffer, baseVertex, baseInstance, instances)
        Nx2Draw d = {(int)x[1], (int)x[2], (int)x[3], x[4], 0, (int)x[6], (int)x[5], (int)x[7]};
        if (dump) debug_draw();
        nx2_draws++;
        nx2_vk_draw(&d);
        break;
    }
    case C_DRAW_ARRAYS_INST: {   // (cb, mode, first, count, baseInstance, instances)
        Nx2Draw d = {(int)x[1], -1, (int)x[3], 0, (int)x[2], (int)x[4], 0, (int)x[5]};
        if (dump) debug_draw();
        nx2_draws++;
        nx2_vk_draw(&d);
        break;
    }
    case C_DRAW_ELEMENTS_INDIRECT: {   // (cb, mode, type, indexBuffer, indirectBuffer): the GPU decides the count
        Nx2Draw d = {(int)x[1], (int)x[2], 0, x[3], 0, 0, 0, 1};
        if (dump) debug_draw();
        nx2_draws++;
        nx2_vk_draw_indirect(&d, x[4]);
        break;
    }
    case C_DRAW_ARRAYS_INDIRECT: {   // (cb, mode, indirectBuffer)
        Nx2Draw d = {(int)x[1], -1, 0, 0, 0, 0, 0, 1};
        if (dump) debug_draw();
        nx2_draws++;
        nx2_vk_draw_indirect(&d, x[2]);
        break;
    }
    case C_STENCIL_REF: case C_STENCIL_VMASK: case C_STENCIL_WMASK: {   // (cb, NVNface (1 front, 2 back, 3 both), value)
        uint8_t *t = k0 == C_STENCIL_REF ? s->stencil_ref : k0 == C_STENCIL_VMASK ? s->stencil_vmask : s->stencil_wmask;
        if (x[1] & 1) t[0] = (uint8_t)x[2];
        if (x[1] & 2) t[1] = (uint8_t)x[2];
        break;
    }
    case C_SIGNAL_EVENT: {   // (cb, const NVNevent *, NVNeventSignalMode, NVNeventSignalLocation, int flags, uint32_t value)
        // The GPU writes the value after the work before it: the game reads the event to know when GPU results
        // (ink under the player, hits) are in its memory.
        uint64_t a = nx2_event_addr(x[1]);
        static int n;
        if (n++ < 8)
            rt_log("nvn2 replay: SignalEvent %#llx (value at %#llx) mode %d location %d flags %#x value %#x",
                   (unsigned long long)x[1], (unsigned long long)a, (int)x[2], (int)x[3], (unsigned)x[4], (unsigned)x[5]);
        nx2_vk_signal_event(a, (int)x[2], (uint32_t)x[5]);
        break;
    }
    case C_WAIT_EVENT: {   // (cb, const NVNevent *, NVNeventWaitMode, uint32_t value): one queue in order, nothing to wait
        static int n;
        if (n++ < 4)
            rt_log("nvn2 replay: WaitEvent %#llx mode %d value %#x (value now %#x)", (unsigned long long)x[1], (int)x[2],
                   (unsigned)x[3], nx2_event_addr(x[1]) ? *(const uint32_t *)(uintptr_t)nx2_event_addr(x[1]) : 0u);
        break;
    }
    case C_DRAW_ARRAYS: {   // (cb, mode, first, count)
        Nx2Draw d = {(int)x[1], -1, (int)x[3], 0, (int)x[2], 0, 0, 1};
        if (dump) debug_draw();
        nx2_draws++;
        nx2_vk_draw(&d);
        break;
    }
    case C_CLEAR_COLOR:
        if (blob) nx2_vk_clear_color((int)x[1], (const float *)blob, (int)x[3]);   // (cb, index, color *, mask)
        break;
    default: {   // commands the replay ignores: each named once
        static uint8_t seen[2048];
        if (proc < 2048 && !seen[proc]) {
            seen[proc] = 1;
            rt_log("nvn2 replay: %s not executed", nx2_proc_name_at((int)proc));
        }
        break;
    }
    }
}
