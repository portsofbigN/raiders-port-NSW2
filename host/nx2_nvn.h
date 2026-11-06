// Shared parts of the NVN2 layer (nx2_gfx.c procs, nx2_tex.c textures).
#pragma once
#include <stdint.h>

#include "cpu.h"

typedef struct { const char *name; GuestFn fn; } Nx2HostProc;

// The real driver's implementation of a proc (recompiled sdk code); fatal if the driver has none.
uint64_t nx2_driver_proc(Cpu *c, const char *name);
// Calls the driver's proc `name` with up to 4 arguments (cached lookup per call site through *cache).
uint64_t nx2_driver_call(Cpu *c, uint64_t *cache, const char *name, int n, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3);
// Storage address of an initialized memory pool (GPU address == CPU address); fatal if unknown.
uint64_t nx2_pool_memory(uint64_t pool);

// Host procs of nx2_tex.c, searched after nx2_gfx.c's own table.
extern const Nx2HostProc nx2_tex_procs[];
extern const int nx2_tex_nprocs;

// Slot index of the NVN2 proc at a synthetic address (nx2_gfx.c), -1 if none; its name.
int nx2_proc_index(uint64_t pc);
const char *nx2_proc_name_at(int index);
// nx2_cmd.c: the host proc for an nvnCommandBuffer* name (lifecycle, or the generic recorder), NULL otherwise.
GuestFn nx2_cmd_proc(const char *name);
void nx2_cmd_submitted(int count, const uint64_t *handles);
void nx2_cmd_stats(uint64_t *recorded, uint64_t *submitted, uint64_t *unknown_handles);
// End address of the memory pool that holds addr (0 if none) (nx2_gfx.c).
uint64_t nx2_pool_end(uint64_t addr);
// The memory pool holding addr: its storage range; 0 if none (nx2_gfx.c).
int nx2_pool_range(uint64_t addr, uint64_t *base, uint64_t *size);
// nx2_vkdraw.c: a pool's storage is released (its imported Vulkan memory must go).
void nx2_vk_pool_released(uint64_t base, uint64_t size);
// Host procs of nx2_prog.c.
extern const Nx2HostProc nx2_prog_procs[];
extern const int nx2_prog_nprocs;
// Host procs of nx2_buf.c (buffers, events).
extern const Nx2HostProc nx2_buf_procs[];
extern const int nx2_buf_nprocs;

// nx2_gfx.c: slot index of a proc name the game asked for, -1 if it never did.
int nx2_proc_index_by_name(const char *name);
int nx2_proc_count(void);   // procs the game has asked for so far
// nx2_vk.c: present one RGBA8 image (rows of w * 4 bytes); frame counts presents (screenshots, NX2_SHOT).
void nx2_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t frame);
#include "nvn_tex.h"
// nx2_tex.c: the description and storage of an initialized texture; 0 if obj is not one.
int nx2_tex_get(uint64_t obj, nvn_texdesc *d, uint8_t **storage, uint64_t *size);
// nx2_cmd.c: what SetRenderTargets bound, copied at record time.
// view_format: the NVNformat of the colour target's texture view (0: the texture's own), e.g. RGBA8_SRGB on an
// RGBA8 texture, which encodes linear values to sRGB as they are written.
// view_layer / view_level: the first layer (3D textures: the slice) and the mip level the view selects; a game
// renders the slices of a 3D look-up table as 8 colour targets, one view per slice.
typedef struct {
    int count, pad;
    uint64_t color[8];
    uint64_t depth;
    int view_format[8];
    int view_layer[8], view_level[8];
    int depth_layer, depth_level;
} Nx2RenderTargets;
// nx2_prog.c: shader stages of a program (code address and control pointer each); 0 if obj is not a host program.
// const_off / const_size: the stage's constant data (constant buffer c1: compiler constants, jump tables), at
// code + const_off, from its control section (magic 0x98761234; begin at +0x40, end at +0x44).
typedef struct {
    uint32_t serial;
    int count;
    uint64_t code[8], ctrl[8], code_size[8];
    uint32_t const_off[8], const_size[8];
} Nx2Program;
int nx2_prog_get(uint64_t obj, Nx2Program *out);
// nx2_gfx.c: texture or sampler pool registration of id: (object, view or 0); 0 if none.
int nx2_dpool_lookup(uint64_t pool, int id, uint64_t *obj, uint64_t *view);
// What the NVNtextureView of a texture registration selects, decoded with the driver's getters when the game
// registered it (the view object is often on the stack). has_* 0: the texture's own value.
typedef struct {
    uint8_t has_swizzle, has_format, has_target, has_levels, has_layers, pad[3];
    uint8_t swizzle[4];   // NVNtextureSwizzle r, g, b, a
    int format, target, base_level, levels, min_layer, layers;
} Nx2TexView;
int nx2_dpool_view(uint64_t pool, int id, Nx2TexView *out);
// A sampler registration decoded with the driver's getters (NVN enums; valid 0: nothing known).
typedef struct {
    uint8_t valid, min, mag, compare_mode;
    uint8_t wrap[3], compare_func;
    float lod_min, lod_max, lod_bias, aniso;
    float border[4];
} Nx2Sampler;
int nx2_dpool_sampler(uint64_t pool, int id, Nx2Sampler *out);
