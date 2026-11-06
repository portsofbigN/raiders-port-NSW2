// NVN2 textures on the host. The driver computes texture storage through its NvRm GPU layer (memory kinds of the
// real GPU), which has no device behind it here, so storage size, alignment and the Texture objects are host code.
//
// The texture description is read from the game's NVNtextureBuilder through the driver's own builder getters
// (forwarded procs), so the builder layout never has to be known. Layout math is TotK Recomp's nvn_tex.c
// (deko3d-derived Tegra block-linear layout; Ampere keeps the same GOB-based block-linear layout).
// Host texture state lives in a table keyed by the guest NVNtexture pointer.
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nvn_tex.h"
#include "nx2_texels.h"
#include "nx2_nvn.h"
#include "platform/platform.h"

static uint64_t g_tget[16];   // cached driver getter addresses

// Some NVN2 builder getters fill a caller structure through their second argument (NVN1 returned pointers), so
// every getter gets a zeroed per-thread scratch buffer there; getters that take one argument ignore it.
static uint8_t *scratch(void) {
    static _Thread_local uint8_t *buf;
    if (!buf) buf = mem_kernel_alloc(256, 64);
    memset(buf, 0, 256);
    return buf;
}

static uint64_t getter(Cpu *c, int k, const char *name, uint64_t b, uint8_t *out) {
    static int logged[16];
    uint64_t r = nx2_driver_call(c, &g_tget[k], name, 2, b, (uint64_t)(uintptr_t)out, 0, 0);
    if (!logged[k]++) {
        char d[96];
        rt_log("nvn2: texture builder getter %s = %s", name, ldr_describe(g_tget[k], d, sizeof d));
    }
    return r;
}

static nvn_texdesc describe(Cpu *c, uint64_t b) {
    nvn_texdesc d;
    memset(&d, 0, sizeof d);
    uint8_t *out = scratch();
    d.target = (int)getter(c, 0, "nvnTextureBuilderGetTarget", b, out);
    d.format = (int)getter(c, 1, "nvnTextureBuilderGetFormat", b, out);
    d.flags = (int)getter(c, 2, "nvnTextureBuilderGetFlags", b, out);
    d.samples = (int)getter(c, 3, "nvnTextureBuilderGetSamples", b, out);
    d.width = (uint32_t)getter(c, 4, "nvnTextureBuilderGetWidth", b, out);
    d.height = (uint32_t)getter(c, 5, "nvnTextureBuilderGetHeight", b, out);
    d.depth = (uint32_t)getter(c, 6, "nvnTextureBuilderGetDepth", b, out);
    d.levels = (uint32_t)getter(c, 7, "nvnTextureBuilderGetLevels", b, out);
    d.stride = (uint32_t)getter(c, 8, "nvnTextureBuilderGetStride", b, out);
    memset(out, 0, 256);
    // NVN2: GetPackagedTextureLayout(b, NVNpackagedTextureLayout *out); byte 0 holds log2 block height/depth when
    // the game gave packaged data (out stays zero otherwise)
    getter(c, 9, "nvnTextureBuilderGetPackagedTextureLayout", b, out);
    if (out[0]) d.pkg_block_h = (signed char)(out[0] + 1);
    {   // the builder's component swizzle (nvnTextureBuilderSetSwizzle): the labels the game compresses on the GPU
        // (BC5) take alpha from a channel; without it they drew as opaque boxes. Getter: (builder, r*, g*, b*, a*)
        static uint64_t g_swz;
        if (!g_swz) g_swz = nx2_driver_proc(c, "nvnTextureBuilderGetSwizzle");
        memset(out, 0, 64);
        uint64_t o = (uint64_t)(uintptr_t)out;
        rt_call(c, g_swz, 5, b, o, o + 4, o + 8, o + 12);
        const int32_t *sw = (const int32_t *)out;
        int ok = 1;
        for (int i = 0; i < 4; i++) ok &= sw[i] >= 0 && sw[i] <= 5;
        if (ok && (sw[0] | sw[1] | sw[2] | sw[3])) {
            d.swizzle_r = (unsigned)sw[0], d.swizzle_g = (unsigned)sw[1], d.swizzle_b = (unsigned)sw[2];
            d.swizzle_a = (unsigned)sw[3];
            static int nlog;
            if (!(sw[0] == 2 && sw[1] == 3 && sw[2] == 4 && sw[3] == 5) && nlog++ < 12)
                rt_log("nvn2: texture builder swizzle %d %d %d %d (format %#x, %ux%u)", sw[0], sw[1], sw[2], sw[3],
                       d.format, d.width, d.height);
        }
    }
    if (!d.levels) d.levels = 1;
    if (!d.depth) d.depth = 1;
    if (!d.height) d.height = 1;
    return d;
}

// ---- builder queries
static void tb_storage_size(Cpu *c) {   // size_t nvnTextureBuilderGetStorageSize(const NVNtextureBuilder *)
    nvn_texdesc d = describe(c, c->x[0]);
    nvn_texlayout L;
    nvn_tex_layout(&d, &L);
    if (!L.bpb) rt_log("nvn2: TextureBuilderGetStorageSize: unknown format %#x", d.format);
    c->x[0] = L.size;
}
static void tb_storage_alignment(Cpu *c) {
    nvn_texdesc d = describe(c, c->x[0]);
    nvn_texlayout L;
    nvn_tex_layout(&d, &L);
    c->x[0] = L.align;
}
// Storage classes are compatibility identifiers between textures that may alias one storage; as in TotK, every
// non-buffer texture is class 1 and buffer textures class 0.
static void tb_storage_class(Cpu *c) { c->x[0] = describe(c, c->x[0]).target == 0xb ? 0 : 1; }
static void tb_zcull_size(Cpu *c) { c->x[0] = 0; }   // no ZCull storage: the host keeps no ZCull data

// ---- textures
typedef struct {
    uint64_t obj, serial, pool;
    int64_t offset;
    nvn_texdesc d;
    nvn_texlayout L;
} Tex;
#define MAX_TEX (1 << 17)
static Tex *texs;
static int ntex;
static uint64_t serials;
static PlatMutex lock;

static Tex *find(uint64_t obj) {   // under lock
    for (int i = ntex - 1; i >= 0; i--)
        if (texs[i].obj == obj) return &texs[i];
    return NULL;
}

// The whole host description is also kept inside the guest NVNtexture (the game copies textures by value, as TotK
// Recomp found for its NVN objects); getters read it from there. 0xC0 bytes is NVN1's NVNtexture size, so the record
// stays below that.
#define TEX_MAGIC 0x54325856u   // "VX2T"
typedef struct { uint32_t magic, pad; Tex t; } TexInPlace;
_Static_assert(sizeof(TexInPlace) <= 0xC0, "texture record must fit in an NVNtexture");

static Tex get(uint64_t obj) {
    const TexInPlace *ip = (const TexInPlace *)(uintptr_t)obj;
    if (!obj || ip->magic != TEX_MAGIC) rt_fatal("nvn2: texture %#llx is not initialized", (unsigned long long)obj);
    return ip->t;
}

// NVNboolean nvnTextureInitialize(NVNtexture *, const NVNtextureBuilder *)
static void t_initialize(Cpu *c) {
    static uint64_t g_pool, g_off;
    uint64_t obj = c->x[0], b = c->x[1];
    Tex t = {obj, 0, nx2_driver_call(c, &g_pool, "nvnTextureBuilderGetMemoryPool", 1, b, 0, 0, 0),
             (int64_t)nx2_driver_call(c, &g_off, "nvnTextureBuilderGetMemoryOffset", 1, b, 0, 0, 0), describe(c, b)};
    nvn_tex_layout(&t.d, &t.L);
    plat_mutex_lock(&lock);
    if (!texs && !(texs = calloc(MAX_TEX, sizeof *texs))) rt_fatal("nvn2: texture table");
    Tex *slot = find(obj);
    if (!slot) {
        if (ntex == MAX_TEX) rt_fatal("nvn2: more than %d textures", MAX_TEX);
        slot = &texs[ntex++];
    }
    t.serial = ++serials;
    *slot = t;
    *(TexInPlace *)(uintptr_t)obj = (TexInPlace){TEX_MAGIC, 0, t};
    int n = ntex;
    plat_mutex_unlock(&lock);
    if (n <= 32 || !(n & 1023))
        rt_log("nvn2: texture %#llx #%llu: %ux%ux%u levels %u format %#x target %d flags %#x, %#llx bytes at pool %#llx+%#llx",
               (unsigned long long)obj, (unsigned long long)t.serial, t.d.width, t.d.height, t.d.depth, t.d.levels,
               t.d.format, t.d.target, t.d.flags, (unsigned long long)t.L.size, (unsigned long long)t.pool,
               (unsigned long long)t.offset);
    c->x[0] = 1;
}

static void t_finalize(Cpu *c) {
    plat_mutex_lock(&lock);
    Tex *t = find(c->x[0]);
    if (t) *t = texs[--ntex];
    plat_mutex_unlock(&lock);
}
static void t_nop(Cpu *c) { (void)c; }
static void t_flags(Cpu *c) { c->x[0] = (uint32_t)get(c->x[0]).d.flags; }
static void t_target(Cpu *c) { c->x[0] = (uint32_t)get(c->x[0]).d.target; }
static void t_width(Cpu *c) { c->x[0] = get(c->x[0]).d.width; }
static void t_height(Cpu *c) { c->x[0] = get(c->x[0]).d.height; }
static void t_depth(Cpu *c) { c->x[0] = get(c->x[0]).d.depth; }
static void t_levels(Cpu *c) { c->x[0] = get(c->x[0]).d.levels; }
static void t_format(Cpu *c) { c->x[0] = (uint32_t)get(c->x[0]).d.format; }
static void t_samples(Cpu *c) { c->x[0] = (uint32_t)get(c->x[0]).d.samples; }
static void t_stride(Cpu *c) { c->x[0] = get(c->x[0]).d.stride; }
static void t_pool(Cpu *c) { c->x[0] = get(c->x[0]).pool; }
static void t_offset(Cpu *c) { c->x[0] = (uint64_t)get(c->x[0]).offset; }
static void t_size(Cpu *c) { c->x[0] = get(c->x[0]).L.size; }
static void t_class(Cpu *c) { c->x[0] = get(c->x[0]).d.target == 0xb ? 0 : 1; }
static void t_debug_id(Cpu *c) { c->x[0] = get(c->x[0]).serial; }
// NVNtextureAddress nvnTextureGetTextureAddress(const NVNtexture *): its storage (GPU address == CPU address)
static void t_address(Cpu *c) {
    Tex t = get(c->x[0]);
    c->x[0] = nx2_pool_memory(t.pool) + (uint64_t)t.offset;
}
// void nvnTexture{Write,Read}Texels[Strided](const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *,
//                                           void *data[, ptrdiff_t rowStride, ptrdiff_t imageStride])
// The storage is the game's own pool memory (GPU address == CPU address), so texels are copied straight between the
// caller's linear rows and the block-linear storage. The base level comes from the view, through the driver's
// nvnTextureViewGetLevels (the view is the driver's object). 3D textures copy slice by slice; array and cube
// textures layer by layer (1D arrays keep layers in y).
typedef struct { int x, y, z, w, h, d; } CopyRegion;   // NVNcopyRegion: offsets, then sizes

static void texels(Cpu *c, int write, int strided) {
    static uint64_t g_levels;
    Tex t = get(c->x[0]);
    uint64_t view = c->x[1];
    CopyRegion r = *(const CopyRegion *)(uintptr_t)c->x[2];
    uint8_t *data = (uint8_t *)(uintptr_t)c->x[3];
    unsigned level = 0;
    if (view) {
        int *out = (int *)scratch();
        if (nx2_driver_call(c, &g_levels, "nvnTextureViewGetLevels", 3, view, (uint64_t)(uintptr_t)out,
                            (uint64_t)(uintptr_t)(out + 1), 0) & 1)
            level = (unsigned)out[0];
    }
    int bw, bh, bpb = nvn_fmt_bpb(t.d.format, &bw, &bh);
    if (!bpb) {
        rt_log("nvn2: %sTexels: unknown format %#x", write ? "Write" : "Read", t.d.format);
        return;
    }
    uint64_t wb = (uint64_t)((r.w + bw - 1) / bw) * (uint64_t)bpb, hrows = (uint64_t)((r.h + bh - 1) / bh);
    uint64_t row = strided && c->x[4] ? c->x[4] : wb;
    uint64_t img = strided && c->x[5] ? c->x[5] : row * hrows;
    uint8_t *storage = (uint8_t *)(uintptr_t)(nx2_pool_memory(t.pool) + (uint64_t)t.offset);
    uint32_t x = (uint32_t)(r.x / bw) * (uint32_t)bpb, y = (uint32_t)(r.y / bh);
    int failed = 0;
    if (t.d.target == 2) {   // 3D: depth slices
        for (int k = 0; k < (r.d > 0 ? r.d : 1); k++)
            failed |= nvn_texels_box(&t.d, level, 0, x, y, (uint32_t)(r.z + k), (uint32_t)wb, (uint32_t)hrows, storage,
                                     data + (uint64_t)k * img, row, !write);
    } else if (t.d.target == 3) {   // 1D array: layers in y
        for (int k = 0; k < (r.h > 0 ? r.h : 1); k++)
            failed |= nvn_texels_box(&t.d, level, (unsigned)(r.y + k), x, 0, 0, (uint32_t)wb, 1, storage,
                                     data + (uint64_t)k * row, row, !write);
    } else {   // 2D, arrays and cube maps: layers in z
        for (int k = 0; k < (r.d > 0 ? r.d : 1); k++)
            failed |= nvn_texels_box(&t.d, level, (unsigned)(r.z + k), x, y, 0, (uint32_t)wb, (uint32_t)hrows, storage,
                                     data + (uint64_t)k * img, row, !write);
    }
    if (failed) {
        static int n;
        if (n++ < 8)
            rt_log("nvn2: %sTexels on texture #%llu (%ux%ux%u target %d format %#x) region %d,%d,%d %dx%dx%d level %u "
                   "not copied", write ? "Write" : "Read", (unsigned long long)t.serial, t.d.width, t.d.height,
                   t.d.depth, t.d.target, t.d.format, r.x, r.y, r.z, r.w, r.h, r.d, level);
    }
}
void nx2_vk_texture_dirty(uint64_t obj);
// texels() leaves its result in x0: the texture is taken before
static void t_write_texels(Cpu *c) { uint64_t t = c->x[0]; texels(c, 1, 0); nx2_vk_texture_dirty(t); }
static void t_write_texels_strided(Cpu *c) { uint64_t t = c->x[0]; texels(c, 1, 1); nx2_vk_texture_dirty(t); }
static void t_read_texels(Cpu *c) { texels(c, 0, 0); }
static void t_read_texels_strided(Cpu *c) { texels(c, 0, 1); }

// For the back end: the description and storage of an initialized texture; 0 if obj is not one.
int nx2_tex_get(uint64_t obj, nvn_texdesc *d, uint8_t **storage, uint64_t *size) {
    const TexInPlace *ip = (const TexInPlace *)(uintptr_t)obj;
    if (!obj || ip->magic != TEX_MAGIC) return 0;
    *d = ip->t.d;
    *storage = (uint8_t *)(uintptr_t)(nx2_pool_memory(ip->t.pool) + (uint64_t)ip->t.offset);
    *size = ip->t.L.size;
    return 1;
}

const Nx2HostProc nx2_tex_procs[] = {
    {"nvnTextureBuilderGetStorageSize", tb_storage_size},
    {"nvnTextureBuilderGetStorageAlignment", tb_storage_alignment},
    {"nvnTextureBuilderGetStorageClass", tb_storage_class},
    {"nvnTextureBuilderGetRawStorageClass", tb_storage_class},
    {"nvnTextureBuilderGetZCullStorageSize", tb_zcull_size},
    {"nvnTextureInitialize", t_initialize},
    {"nvnTextureFinalize", t_finalize},
    {"nvnTextureSetDebugLabel", t_nop},
    {"nvnTextureGetZCullStorageSize", tb_zcull_size},
    {"nvnTextureGetStorageClass", t_class},
    {"nvnTextureGetRawStorageClass", t_class},
    {"nvnTextureGetFlags", t_flags},
    {"nvnTextureGetTarget", t_target},
    {"nvnTextureGetWidth", t_width},
    {"nvnTextureGetHeight", t_height},
    {"nvnTextureGetDepth", t_depth},
    {"nvnTextureGetLevels", t_levels},
    {"nvnTextureGetFormat", t_format},
    {"nvnTextureGetSamples", t_samples},
    {"nvnTextureGetStride", t_stride},
    {"nvnTextureGetTextureAddress", t_address},
    {"nvnTextureGetMemoryPool", t_pool},
    {"nvnTextureGetMemoryOffset", t_offset},
    {"nvnTextureGetStorageSize", t_size},
    {"nvnTextureGetDebugID", t_debug_id},
    {"nvnTextureWriteTexels", t_write_texels},
    {"nvnTextureWriteTexelsStrided", t_write_texels_strided},
    {"nvnTextureReadTexels", t_read_texels},
    {"nvnTextureReadTexelsStrided", t_read_texels_strided},
    {"nvnTextureFlushTexels", t_nop},
    {"nvnTextureInvalidateTexels", t_nop},
};
const int nx2_tex_nprocs = sizeof nx2_tex_procs / sizeof *nx2_tex_procs;
