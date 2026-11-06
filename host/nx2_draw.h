// Draw state of the NVN2 replay (nx2_draw.c) and the Vulkan draw back end (nx2_vkdraw.c).
#pragma once
#include <stdint.h>

#include "nx2_nvn.h"

#define NX2_MAX_UBO 16
#define NX2_MAX_TEX 32
#define NX2_MAX_VB 16

typedef struct { uint64_t addr, size; } Nx2Range;
typedef struct { int format, offset, stream; } Nx2Attrib;
typedef struct { int stride, divisor; } Nx2Stream;
typedef struct { int nattrib, nstream; Nx2Attrib a[16]; Nx2Stream s[16]; } Nx2VertexInputState;

// Render state objects as the driver's getters report them (decoded at record time, nx2_cmd.c). Blob layout of
// the Bind*State commands: Nx2StateBlob.
enum { NX2_ST_BLEND = 1, NX2_ST_COLOR, NX2_ST_CHANNEL, NX2_ST_DEPTH, NX2_ST_POLYGON };
typedef struct { int kind; int v[11]; } Nx2StateBlob;
typedef struct {
    uint8_t src, dst, srca, dsta, eq, eqa;   // NVNblendFunc / NVNblendEquation
} Nx2Blend;
typedef struct {
    Nx2Blend blend[8];
    uint8_t blend_enable;   // bit per colour target
    uint8_t mask[8];        // channel mask per target: bit 0 R .. bit 3 A
    uint8_t depth_test, depth_write, depth_func;
    uint8_t cull, front_ccw, polygon_mode;
    uint8_t stencil_test, sfunc[2], sop[2][3];   // front, back: NVNstencilFunc; NVNstencilOp on fail, depth fail, pass
} Nx2Raster;

typedef struct {
    uint64_t texture_pool, sampler_pool;
    Nx2Raster rs;
    Nx2RenderTargets rt;
    int viewport[4], scissor[4];
    float depth_range[2];
    uint64_t program;
    uint32_t program_stages;
    Nx2Range ubo[6][NX2_MAX_UBO];   // per NVN shader stage: vertex, fragment, geometry, tess control, tess eval, compute
    uint64_t tex[6][NX2_MAX_TEX];   // texture handles
    Nx2Range ssbo[6][NX2_MAX_UBO];  // storage buffers
    uint64_t image[6][8];           // image handles (image id in the texture pool)
    Nx2VertexInputState vi;
    Nx2Range vb[NX2_MAX_VB];
    float blend_color[4];
    uint8_t push_gfx[256], push_cs[256];   // Push{Graphics,Compute}Constants data, by byte offset
    uint8_t stencil_ref[2], stencil_vmask[2], stencil_wmask[2];   // SetStencilRef / ValueMask / Mask: front, back
} Nx2DrawState;

extern Nx2DrawState nx2_state;

// mode: NVNdrawPrimitive; index_type: NVNindexType (-1: DrawArrays); index_addr: GPU (= CPU) address of the indices
// base_instance: the first instance (per-instance vertex data start there; the shader's instance id does not include it)
typedef struct { int mode, index_type, count; uint64_t index_addr; int first, base_instance, base_vertex, instances; } Nx2Draw;

void nx2_exec(uint32_t proc, const uint64_t *x, const float *f, const uint8_t *blob, uint32_t blob_n, int dump);
int nx2_resolve_texture(uint64_t handle, uint64_t *tex, uint64_t *sampler);
void nx2_vk_draw(const Nx2Draw *d);
void nx2_vk_clear_color(int index, const float *rgba, int mask);
void nx2_vk_clear_depth(float depth, int depth_mask, int stencil, int stencil_mask);
void nx2_vk_dispatch(uint32_t x, uint32_t y, uint32_t z);
// CopyTextureToTexture as recorded: NVNcopyRegion {x, y, z, width, height, depth} of source and destination
typedef struct { uint64_t src, dst; int32_t sr[6], dr[6]; int flags, pad; } Nx2Copy;
void nx2_vk_copy_texture(const Nx2Copy *c);
// DrawElementsIndirect / DrawArraysIndirect: the draw's numbers read by the GPU at `args` (NVN's indirect layouts are
// Vulkan's); SignalEvent: the GPU writes the event's value at `addr` after the work before it.
void nx2_vk_draw_indirect(const Nx2Draw *d, uint64_t args);
void nx2_vk_signal_event(uint64_t addr, int mode, uint32_t value);
// samples-passed counters: an occlusion query from the reset to the report; 1 if the GPU writes the count at addr
void nx2_vk_counter_reset(void);
int nx2_vk_counter_report(uint64_t addr);
uint64_t nx2_event_addr(uint64_t event);   // nx2_buf.c: where an NVNevent's value lives
void nx2_vk_submit_end(void);                            // end of one nvnQueueSubmitCommands
int nx2_vk_present_texture(uint64_t obj, uint64_t frame);   // 1: presented from its GPU image

extern uint64_t nx2_draws;
