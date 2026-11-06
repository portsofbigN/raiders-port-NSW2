// NVN2 buffers and events on the host. Both live in memory pools, which are host objects (nx2_gfx.c), so the
// driver's own code would read pool fields it never wrote: the game's "GpuBuffer" heaps got a wrong base and size
// from the forwarded nvnBufferGetAddress / GetSize and failed their first allocation.
//
// The builders stay the driver's (their getters are forwarded); the objects keep a small host record in place,
// since the game copies NVN objects by value. Observed object strides: NVNbuffer 0x50 bytes, NVNevent 0x40.
#include <string.h>

#include "host.h"
#include "nx2_nvn.h"

// ---- buffers
#define BUF_MAGIC 0x42325856u   // "VX2B"
typedef struct { uint32_t magic, serial; uint64_t pool, offset, size, addr; } BufRec;
_Static_assert(sizeof(BufRec) <= 0x50, "buffer record must fit in an NVNbuffer");
static uint32_t buf_serial;

static BufRec buf(uint64_t obj) {
    const BufRec *r = (const BufRec *)(uintptr_t)obj;
    if (!obj || r->magic != BUF_MAGIC) return (BufRec){0};   // never initialized: zero, as the driver would read
    return *r;
}

// NVNboolean nvnBufferInitialize(NVNbuffer *, const NVNbufferBuilder *)
static void b_initialize(Cpu *c) {
    static uint64_t g_pool, g_off, g_size;
    uint64_t obj = c->x[0], b = c->x[1];
    BufRec r = {BUF_MAGIC, __atomic_add_fetch(&buf_serial, 1, __ATOMIC_RELAXED),
                nx2_driver_call(c, &g_pool, "nvnBufferBuilderGetMemoryPool", 1, b, 0, 0, 0),
                nx2_driver_call(c, &g_off, "nvnBufferBuilderGetMemoryOffset", 1, b, 0, 0, 0),
                nx2_driver_call(c, &g_size, "nvnBufferBuilderGetSize", 1, b, 0, 0, 0), 0};
    r.addr = nx2_pool_memory(r.pool) + r.offset;
    *(BufRec *)(uintptr_t)obj = r;
    if (r.serial <= 8 || !(r.serial % 2048))
        rt_log("nvn2: buffer %#llx #%u: pool %#llx +%#llx, %#llx bytes at %#llx", (unsigned long long)obj, r.serial,
               (unsigned long long)r.pool, (unsigned long long)r.offset, (unsigned long long)r.size,
               (unsigned long long)r.addr);
    c->x[0] = 1;
}
static void b_finalize(Cpu *c) { ((BufRec *)(uintptr_t)c->x[0])->magic = 0; }
static void b_nop(Cpu *c) { (void)c; }
static void b_address(Cpu *c) { c->x[0] = buf(c->x[0]).addr; }   // GPU address == CPU address
static void b_pool(Cpu *c) { c->x[0] = buf(c->x[0]).pool; }
static void b_offset(Cpu *c) { c->x[0] = buf(c->x[0]).offset; }
static void b_size(Cpu *c) { c->x[0] = buf(c->x[0]).size; }
static void b_debug_id(Cpu *c) { c->x[0] = buf(c->x[0]).serial; }

// ---- events: a 32-bit value in pool memory, written by the GPU (command buffer Signal) or the CPU
#define EV_MAGIC 0x45325856u   // "VX2E"
typedef struct { uint32_t magic, pad; uint64_t addr, pool, offset; } EvRec;
_Static_assert(sizeof(EvRec) <= 0x40, "event record must fit in an NVNevent");

static EvRec ev(uint64_t obj) {
    const EvRec *r = (const EvRec *)(uintptr_t)obj;
    if (!obj || r->magic != EV_MAGIC) return (EvRec){0};
    return *r;
}
// NVNboolean nvnEventInitialize(NVNevent *, const NVNeventBuilder *)
static void e_initialize(Cpu *c) {
    static uint64_t g_pool, g_off;
    uint64_t obj = c->x[0], b = c->x[1];
    EvRec r = {EV_MAGIC, 0, 0, nx2_driver_call(c, &g_pool, "nvnEventBuilderGetMemoryPool", 1, b, 0, 0, 0),
               nx2_driver_call(c, &g_off, "nvnEventBuilderGetMemoryOffset", 1, b, 0, 0, 0)};
    r.addr = r.pool ? nx2_pool_memory(r.pool) + r.offset : 0;
    *(EvRec *)(uintptr_t)obj = r;
    c->x[0] = 1;
}
static void e_finalize(Cpu *c) { ((EvRec *)(uintptr_t)c->x[0])->magic = 0; }
static void e_value(Cpu *c) {   // uint32_t nvnEventGetValue(const NVNevent *)
    EvRec r = ev(c->x[0]);
    c->x[0] = r.addr ? __atomic_load_n((uint32_t *)(uintptr_t)r.addr, __ATOMIC_ACQUIRE) : 0;
}
static void e_signal(Cpu *c) {   // void nvnEventSignal(NVNevent *, NVNeventSignalMode mode, uint32_t value)
    EvRec r = ev(c->x[0]);
    if (!r.addr) return;
    uint32_t *p = (uint32_t *)(uintptr_t)r.addr, v = (uint32_t)c->x[2];
    if (c->x[1] == 1) __atomic_add_fetch(p, v, __ATOMIC_ACQ_REL);   // NVN_EVENT_SIGNAL_MODE_ADD
    else __atomic_store_n(p, v, __ATOMIC_RELEASE);                  // NVN_EVENT_SIGNAL_MODE_WRITE
}
uint64_t nx2_event_addr(uint64_t event) { return ev(event).addr; }
static void e_pool(Cpu *c) { c->x[0] = ev(c->x[0]).pool; }

// The device's depth mode (NVN_DEPTH_MODE_NEAR_IS_MINUS_W 0, NEAR_IS_ZERO 1) and window origin (LOWER_LEFT 0,
// UPPER_LEFT 1): recorded for the translation of the shaders (nx2_vkdraw.c), then set in the driver as well.
int nx2_depth_mode = -1, nx2_window_origin = -1;
static void d_depth_mode(Cpu *c) {   // void nvnDeviceSetDepthMode(NVNdevice *, NVNdepthMode)
    static uint64_t g;
    nx2_depth_mode = (int)(uint32_t)c->x[1];
    rt_log("nvn2: device depth mode %d (%s)", nx2_depth_mode, nx2_depth_mode == 1 ? "near is zero" : "near is -w");
    nx2_driver_call(c, &g, "nvnDeviceSetDepthMode", 2, c->x[0], c->x[1], 0, 0);
}
static void d_window_origin(Cpu *c) {   // void nvnDeviceSetWindowOriginMode(NVNdevice *, NVNwindowOriginMode)
    static uint64_t g;
    nx2_window_origin = (int)(uint32_t)c->x[1];
    rt_log("nvn2: device window origin %d (%s)", nx2_window_origin, nx2_window_origin == 1 ? "upper left" : "lower left");
    nx2_driver_call(c, &g, "nvnDeviceSetWindowOriginMode", 2, c->x[0], c->x[1], 0, 0);
}
static void e_offset(Cpu *c) { c->x[0] = ev(c->x[0]).offset; }

const Nx2HostProc nx2_buf_procs[] = {
    {"nvnBufferInitialize", b_initialize},
    {"nvnBufferSetDebugLabel", b_nop},
    {"nvnBufferFinalize", b_finalize},
    {"nvnBufferMap", b_address},
    {"nvnBufferGetAddress", b_address},
    {"nvnBufferFlushMappedRange", b_nop},
    {"nvnBufferInvalidateMappedRange", b_nop},
    {"nvnBufferGetMemoryPool", b_pool},
    {"nvnBufferGetMemoryOffset", b_offset},
    {"nvnBufferGetSize", b_size},
    {"nvnBufferGetDebugID", b_debug_id},
    {"nvnEventInitialize", e_initialize},
    {"nvnEventFinalize", e_finalize},
    {"nvnEventGetValue", e_value},
    {"nvnEventSignal", e_signal},
    {"nvnEventGetMemoryPool", e_pool},
    {"nvnEventGetMemoryOffset", e_offset},
    {"nvnDeviceSetDepthMode", d_depth_mode},
    {"nvnDeviceSetWindowOriginMode", d_window_origin},
};
const int nx2_buf_nprocs = sizeof nx2_buf_procs / sizeof *nx2_buf_procs;
