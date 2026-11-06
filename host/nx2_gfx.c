// Graphics bring-up for Switch 2 and an NVN2 tracing layer (stage 5, first step).
//
// main reaches graphics through the same entry points as Switch 1 NVN games (TotK Recomp ov_gfx.c replaces them):
//   nv::SetGraphicsAllocator / nv::InitializeGraphics   recorded here, so the Switch 2 graphics system services
//                                                       (gs, gsv, nvsrv, syncpt, gpu) are never booted
//   nn::vi::*                                           display, layer, native window and vsync event identities
//   nvnBootstrapLoader / nvnDeviceGetProcAddress        hand out NVN2 procs
//
// Every NVN2 proc is, for now, a tracer: it logs its name and first arguments on its first calls and returns a
// neutral value (true for *Initialize, else 0). That shows the order of the game's NVN2 calls and the sizes and
// enums it uses before any of TotK's NVN front end (written against Switch 1 NVN) is relied on.
// NX2_NVN_TRACE=<n> logs the first n calls of each proc (default 3).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nx2_nvn.h"
#include "nx2_texels.h"
#include "platform/platform.h"
#include <windows.h>   // VirtualAlloc: address ranges for virtual memory pools

typedef struct { uint64_t alloc, free, realloc, user; } NvAllocator;
static NvAllocator g_alloc, g_devtools_alloc;
static uint64_t g_gfx_memory, g_gfx_memory_size;
static PlatMutex lock;

// ---- nv::
static void ov_graphics_allocator(Cpu *c) {
    g_alloc = (NvAllocator){c->x[0], c->x[1], c->x[2], c->x[3]};
    rt_log("nv: graphics allocator alloc %#llx free %#llx realloc %#llx user %#llx", (unsigned long long)c->x[0],
           (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
}
static void ov_devtools_allocator(Cpu *c) { g_devtools_alloc = (NvAllocator){c->x[0], c->x[1], c->x[2], c->x[3]}; }
static void ov_initialize_graphics(Cpu *c) {
    g_gfx_memory = c->x[0];
    g_gfx_memory_size = c->x[1];
    rt_log("nv: InitializeGraphics(%#llx, %#llx): the Switch 2 graphics system is not booted; NVN2 is host code",
           (unsigned long long)c->x[0], (unsigned long long)c->x[1]);
}

// ---- nn::vi: identities are zeroed blocks in the guest-visible kernel region, in case SDK code reads them
static void *display, *layer, *window;
static void ov_vi_initialize(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    if (!display) display = mem_kernel_alloc(256, 16), layer = mem_kernel_alloc(256, 16), window = mem_kernel_alloc(256, 16);
    plat_mutex_unlock(&lock);
    rt_log("vi: initialized (display %p, layer %p, native window %p)", display, layer, window);
}
static void out_ptr(Cpu *c, void *p) {
    void **out = (void **)(uintptr_t)c->x[0];
    if (out) *out = p;
    c->x[0] = 0;
}
static void ov_open_display(Cpu *c) { out_ptr(c, display); }
static void ov_create_layer(Cpu *c) { out_ptr(c, layer); }
static void ov_native_window(Cpu *c) { out_ptr(c, window); }
static void ov_vsync_event(Cpu *c) {   // Result GetDisplayVsyncEvent(os::SystemEventType *, Display *)
    kern_bind_system_event(c, c->x[0], 1);
    c->x[0] = 0;
}
static void ov_vi_bool_false(Cpu *c) {   // Query*(bool *out, ...): HDR / VRR not offered yet
    uint8_t *out = (uint8_t *)(uintptr_t)c->x[0];
    if (out) *out = 0;
    c->x[0] = 0;
}
static void ov_vi_ok(Cpu *c) { c->x[0] = 0; }

HOST_OVERRIDE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_graphics_allocator);
HOST_OVERRIDE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_devtools_allocator);
HOST_OVERRIDE("_ZN2nv18InitializeGraphicsEPvm", ov_initialize_graphics);
HOST_OVERRIDE("_ZN2nn2vi10InitializeEv", ov_vi_initialize);
HOST_OVERRIDE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", ov_open_display);
HOST_OVERRIDE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", ov_create_layer);
HOST_OVERRIDE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", ov_native_window);
HOST_OVERRIDE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", ov_vsync_event);
HOST_OVERRIDE("_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", ov_vi_bool_false);
HOST_OVERRIDE("_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", ov_vi_bool_false);
HOST_OVERRIDE("_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", ov_vi_ok);
HOST_OVERRIDE("_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", ov_vi_ok);
HOST_OVERRIDE("_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", ov_vi_ok);

// ---- NVN2 procs. The game's loader asks nvnDeviceGetProcAddress for every proc by name (more names than the
// modules hold as separate strings), so procs are slots: SLOTS synthetic addresses registered before hostfn_seal,
// each given a name the first time the game asks for it.
//
// Each slot runs one of:
//   host     a host implementation from host_procs[] (the procs that touch the GPU or the OS: device, queue,
//            memory, command submission, window), built on Vulkan
//   forward  the real NVN2 driver's code in sdk (recompiled with the game), for procs that only read or write
//            NVN structures (builders, state objects, getters). Their layouts are then exactly the driver's.
//   stub     neither: logged, returns 0 (true for *Initialize)
// The real addresses come from the driver itself: the sdk's nvnBootstrapLoader gives its nvnDeviceGetProcAddress,
// which is asked for each name (device NULL, as for bootstrap-level lookups).
// NX2_NVN_TRACE=<n> logs the first n calls of each proc; NX2_NVN_STUB=<name,...> forces procs to stubs.
#define SLOTS 2048
typedef struct { uint64_t addr; const char *name; uint64_t calls; uint64_t real; GuestFn host; } Slot;
static Slot slots[SLOTS];
static int nnamed;
static int trace_limit = 3;
static PlatMutex slot_lock;
static uint64_t real_gpa;   // the driver's nvnDeviceGetProcAddress

typedef struct { const char *name; GuestFn fn; } HostProc;
static void hp_get_proc_address(Cpu *c);
static void hp_device_initialize(Cpu *c);
static void hp_device_finalize(Cpu *c);
static void hp_pool_initialize(Cpu *c);
static void hp_pool_finalize(Cpu *c);
static void hp_pool_map(Cpu *c);
static void hp_pool_nop(Cpu *c);
static void hp_pool_address(Cpu *c);
static void hp_pool_size(Cpu *c);
static void hp_pool_flags(Cpu *c);
static void hp_pool_map_virtual(Cpu *c);
static void hp_dpool_initialize(Cpu *c);
static void hp_dpool_finalize(Cpu *c);
static void hp_dpool_register(Cpu *c);
static void hp_dpool_register_texture(Cpu *c);
static void hp_register_sampler(Cpu *c);
static void hp_register_sampler_builder(Cpu *c);
static void hp_dpool_memory_pool(Cpu *c);
static void hp_dpool_offset(Cpu *c);
static void hp_dpool_size(Cpu *c);
static void hp_dpool_write_table(Cpu *c);
static void hp_ret0(Cpu *c);
static void hp_ret1(Cpu *c);
static void hp_queue_submit(Cpu *c);
static void hp_queue_present(Cpu *c);
static void hp_queue_finish(Cpu *c);
static void hp_queue_fence_sync(Cpu *c);
static void hp_sync_initialize(Cpu *c);
static void hp_sync_wait(Cpu *c);
static void hp_window_initialize(Cpu *c);
static void hp_window_acquire(Cpu *c);
static void hp_window_num_textures(Cpu *c);
static void hp_window_get_interval(Cpu *c);
static void hp_window_set_interval(Cpu *c);
static void hp_window_get_native(Cpu *c);
static const HostProc host_procs[] = {
    // Device fast-clear registration: the driver fills the GPU's ZBC table (NvRmGpuDeviceZbcAdd*); Vulkan clears
    // need no registered values, so these only succeed.
    {"nvnDeviceRegisterFastClearColor", hp_ret1},
    {"nvnDeviceRegisterFastClearColori", hp_ret1},
    {"nvnDeviceRegisterFastClearColorui", hp_ret1},
    {"nvnDeviceRegisterFastClearDepth", hp_ret1},
    {"nvnDeviceRegisterFastClearStencil", hp_ret1},
    {"nvnDeviceRegisterFastClearColorIndex", hp_ret1},
    {"nvnDeviceRegisterFastClearColoriIndex", hp_ret1},
    {"nvnDeviceRegisterFastClearColoruiIndex", hp_ret1},
    {"nvnDeviceRegisterFastClearDepthIndex", hp_ret1},
    {"nvnDeviceRegisterFastClearStencilIndex", hp_ret1},
    // Queue, Sync, Window: null back end for now (submits counted, fences signalled, presents counted)
    {"nvnQueueInitialize", hp_ret1},
    {"nvnQueueFinalize", hp_ret0},
    {"nvnQueueSetDebugLabel", hp_ret0},
    {"nvnQueueGetError", hp_ret0},
    {"nvnQueueGetTotalCommandMemoryUsed", hp_ret0},
    {"nvnQueueGetTotalControlMemoryUsed", hp_ret0},
    {"nvnQueueGetTotalComputeMemoryUsed", hp_ret0},
    {"nvnQueueGetTotalRayTracingMemoryUsed", hp_ret0},
    {"nvnQueueResetMemoryUsageCounts", hp_ret0},
    {"nvnQueueSubmitCommands", hp_queue_submit},
    {"nvnQueueFlush", hp_ret0},
    {"nvnQueueFinish", hp_queue_finish},
    {"nvnQueuePresentTexture", hp_queue_present},
    {"nvnQueueFenceSync", hp_queue_fence_sync},
    {"nvnQueueWaitSync", hp_ret1},
    {"nvnSyncInitialize", hp_sync_initialize},
    {"nvnSyncFinalize", hp_ret0},
    {"nvnSyncSetDebugLabel", hp_ret0},
    {"nvnSyncWait", hp_sync_wait},
    {"nvnWindowInitialize", hp_window_initialize},
    {"nvnWindowFinalize", hp_ret0},
    {"nvnWindowSetDebugLabel", hp_ret0},
    {"nvnWindowAcquireTexture", hp_window_acquire},
    {"nvnWindowGetNativeWindow", hp_window_get_native},
    {"nvnWindowGetPresentInterval", hp_window_get_interval},
    {"nvnWindowSetPresentInterval", hp_window_set_interval},
    {"nvnWindowSetCrop", hp_ret0},
    {"nvnWindowGetCrop", hp_ret0},
    {"nvnWindowSetHdrDisplayMetadata", hp_ret0},
    {"nvnWindowSetHdrContentMetadata", hp_ret0},
    {"nvnWindowGetHdrDisplayMetadata", hp_ret0},
    {"nvnWindowGetHdrContentMetadata", hp_ret0},
    {"nvnWindowSetNumActiveTextures", hp_ret0},
    {"nvnWindowGetNumActiveTextures", hp_window_num_textures},
    {"nvnWindowGetNumTextures", hp_window_num_textures},
    {"nvnTexturePoolInitialize", hp_dpool_initialize},
    {"nvnTexturePoolSetDebugLabel", hp_pool_nop},
    {"nvnTexturePoolFinalize", hp_dpool_finalize},
    {"nvnTexturePoolRegisterTexture", hp_dpool_register_texture},
    {"nvnTexturePoolRegisterImage", hp_dpool_register_texture},
    {"nvnTexturePoolGetMemoryPool", hp_dpool_memory_pool},
    {"nvnTexturePoolWriteDescriptorTableData", hp_dpool_write_table},
    {"nvnTexturePoolGetMemoryOffset", hp_dpool_offset},
    {"nvnTexturePoolGetSize", hp_dpool_size},
    {"nvnSamplerPoolInitialize", hp_dpool_initialize},
    {"nvnSamplerPoolSetDebugLabel", hp_pool_nop},
    {"nvnSamplerPoolFinalize", hp_dpool_finalize},
    {"nvnSamplerPoolRegisterSampler", hp_register_sampler},
    {"nvnSamplerPoolRegisterSamplerBuilder", hp_register_sampler_builder},
    {"nvnSamplerPoolWriteDescriptorTableData", hp_dpool_write_table},
    {"nvnSamplerPoolGetMemoryPool", hp_dpool_memory_pool},
    {"nvnSamplerPoolGetMemoryOffset", hp_dpool_offset},
    {"nvnSamplerPoolGetSize", hp_dpool_size},
    {"nvnMemoryPoolInitialize", hp_pool_initialize},
    {"nvnMemoryPoolFinalize", hp_pool_finalize},
    {"nvnMemoryPoolSetDebugLabel", hp_pool_nop},
    {"nvnMemoryPoolMap", hp_pool_map},
    {"nvnMemoryPoolFlushMappedRange", hp_pool_nop},
    {"nvnMemoryPoolInvalidateMappedRange", hp_pool_nop},
    {"nvnMemoryPoolGetBufferAddress", hp_pool_address},
    {"nvnMemoryPoolMapVirtual", hp_pool_map_virtual},
    {"nvnMemoryPoolGetSize", hp_pool_size},
    {"nvnMemoryPoolGetFlags", hp_pool_flags},
    {"nvnDeviceGetProcAddress", hp_get_proc_address},
    {"nvnDeviceInitialize", hp_device_initialize},
    {"nvnDeviceFinalize", hp_device_finalize},
};

static GuestFn host_proc(const char *name) {
    for (size_t i = 0; i < sizeof host_procs / sizeof *host_procs; i++)
        if (!strcmp(host_procs[i].name, name)) return host_procs[i].fn;
    for (int i = 0; i < nx2_tex_nprocs; i++)
        if (!strcmp(nx2_tex_procs[i].name, name)) return nx2_tex_procs[i].fn;
    for (int i = 0; i < nx2_prog_nprocs; i++)
        if (!strcmp(nx2_prog_procs[i].name, name)) return nx2_prog_procs[i].fn;
    for (int i = 0; i < nx2_buf_nprocs; i++)
        if (!strcmp(nx2_buf_procs[i].name, name)) return nx2_buf_procs[i].fn;
    return nx2_cmd_proc(name);
}

static int forced_stub(const char *name) {
    const char *list = getenv("NX2_NVN_STUB");
    size_t n = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, ',');
        size_t m = e ? (size_t)(e - p) : strlen(p);
        if (m == n && !strncmp(p, name, n)) return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

static uint64_t proc_address(Cpu *c, const char *name) {
    if (!name || strncmp(name, "nvn", 3)) return 0;
    plat_mutex_lock(&slot_lock);
    for (int i = 0; i < nnamed; i++)
        if (!strcmp(slots[i].name, name)) {
            plat_mutex_unlock(&slot_lock);
            return slots[i].addr;
        }
    if (nnamed == SLOTS) rt_fatal("nvn2: more than %d procs", SLOTS);
    Slot *s = &slots[nnamed++];
    s->name = strdup(name);
    plat_mutex_unlock(&slot_lock);
    s->host = host_proc(name);
    if (!s->host && real_gpa && !forced_stub(name)) s->real = rt_call(c, real_gpa, 2, (uint64_t)0, (uint64_t)(uintptr_t)s->name);
    static FILE *names;   // every proc the game asks for, in order of first request, with how it is served
    static int want = -1;   // NX2_PROC_LIST=1 (debug): the list in nvn2_procs.txt
    if (want < 0) want = getenv("NX2_PROC_LIST") && *getenv("NX2_PROC_LIST") == '1';
    if (want && !names) names = fopen("nvn2_procs.txt", "w");
    if (names) fprintf(names, "%s %s\n", name, s->host ? "host" : s->real ? "forward" : "stub"), fflush(names);
    return s->addr;
}

static Slot *slot_at(uint64_t pc) {   // slots are registered in order, so their addresses ascend
    int lo = 0, hi = SLOTS;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (slots[mid].addr < pc) lo = mid + 1;
        else hi = mid;
    }
    return lo < SLOTS && slots[lo].addr == pc ? &slots[lo] : NULL;
}

int nx2_proc_index(uint64_t pc) {
    Slot *s = slot_at(pc);
    return s ? (int)(s - slots) : -1;
}
int nx2_proc_count(void) { return __atomic_load_n(&nnamed, __ATOMIC_ACQUIRE); }
const char *nx2_proc_name_at(int index) { return index >= 0 && index < nnamed ? slots[index].name : "?"; }
int nx2_proc_index_by_name(const char *name) {
    for (int i = 0; i < nnamed; i++)
        if (!strcmp(slots[i].name, name)) return i;
    return -1;
}

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && !strcmp(s + n - m, tail);
}

// The last RING NVN2 calls (any thread), printed by the crash reporter (sp_main.c) through nx2_ring_dump.
#define RING 256
typedef struct { int slot; uint64_t x0, x1, x2, x3, ret, lr; } RingEntry;
static RingEntry ring[RING];
static uint64_t ring_next;

void nx2_ring_dump(void) {
    uint64_t end = __atomic_load_n(&ring_next, __ATOMIC_ACQUIRE), start = end > RING ? end - RING : 0;
    rt_logf("last %llu NVN2 calls (oldest first):\n", (unsigned long long)(end - start));
    for (uint64_t i = start; i < end; i++) {
        const RingEntry *e = &ring[i % RING];
        char lr[96];
        rt_logf("  %-48s (%#llx, %#llx, %#llx, %#llx) -> %#llx  from %s\n", nx2_proc_name_at(e->slot),
                (unsigned long long)e->x0, (unsigned long long)e->x1, (unsigned long long)e->x2,
                (unsigned long long)e->x3, (unsigned long long)e->ret, ldr_describe(e->lr, lr, sizeof lr));
    }
}

static void nvn_proc_body(Cpu *c);
static uint64_t proc_ns[SLOTS], proc_calls[SLOTS];   // time in each proc (all threads), for the frame profile
static void nvn_proc(Cpu *c) {
    uint64_t k = __atomic_fetch_add(&ring_next, 1, __ATOMIC_ACQ_REL);
    RingEntry *e = &ring[k % RING];
    int idx = nx2_proc_index(c->pc);
    *e = (RingEntry){idx, c->x[0], c->x[1], c->x[2], c->x[3], 0, c->x[30]};
    uint64_t t0 = plat_now_ns();
    nvn_proc_body(c);
    if (idx >= 0 && idx < SLOTS) {
        __atomic_add_fetch(&proc_ns[idx], plat_now_ns() - t0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&proc_calls[idx], 1, __ATOMIC_RELAXED);
    }
    e->ret = c->x[0];
}

// The procs that took the most time since the previous call (all threads), per frame over n frames.
void nx2_proc_prof_report(double n) {
    uint64_t total = 0, calls = 0;
    for (int i = 0; i < SLOTS; i++) total += proc_ns[i], calls += proc_calls[i];
    rt_log("prof:   NVN calls %.1f ms/frame (%.0f calls/frame); the slowest:", total * 1e-6 / n, calls / n);
    for (int k = 0; k < 8; k++) {
        int best = -1;
        for (int i = 0; i < SLOTS; i++)
            if (proc_ns[i] && (best < 0 || proc_ns[i] > proc_ns[best])) best = i;
        if (best < 0) break;
        rt_log("prof:     %6.2f ms/frame %7.0f calls  %s", proc_ns[best] * 1e-6 / n, proc_calls[best] / n,
               nx2_proc_name_at(best));
        proc_ns[best] = 0;
    }
    rt_log("prof:   the most called:");
    static int all = -1;   // NX2_PROC_ALL=1 (debug): every proc called, not the first ten
    if (all < 0) all = getenv("NX2_PROC_ALL") && *getenv("NX2_PROC_ALL") == '1';
    for (int k = 0; k < (all ? SLOTS : 10); k++) {
        int best = -1;
        for (int i = 0; i < SLOTS; i++)
            if (proc_calls[i] && (best < 0 || proc_calls[i] > proc_calls[best])) best = i;
        if (best < 0) break;
        rt_log("prof:     %7.0f calls/frame  %s", proc_calls[best] / n, nx2_proc_name_at(best));
        proc_calls[best] = 0;
    }
    memset(proc_ns, 0, sizeof proc_ns), memset(proc_calls, 0, sizeof proc_calls);
}

static void nvn_proc_body(Cpu *c) {
    Slot *s = slot_at(c->pc);
    const char *name = s && s->name ? s->name : "?";
    uint64_t n = s ? __atomic_add_fetch(&s->calls, 1, __ATOMIC_RELAXED) : 0;
    if (n <= (uint64_t)trace_limit) {
        char lr[128];
        rt_log("nvn2: %s %s(%#llx, %#llx, %#llx, %#llx) #%llu from %s",
               !s ? "?" : s->host ? "host" : s->real ? "fwd" : "stub", name, (unsigned long long)c->x[0],
               (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
               (unsigned long long)n, ldr_describe(c->x[30], lr, sizeof lr));
    }
    if (s && s->host) {
        s->host(c);
    } else if (s && s->real) {
        c->pc = s->real;   // the driver's code returns to the game's caller (x30 untouched)
        rt_dispatch(c);
    } else {
        c->x[0] = ends_with(name, "Initialize") ? 1 : 0;
    }
}

// NVNproc nvnDeviceGetProcAddress(const NVNdevice *, const char *name): our slot for the name
static void hp_get_proc_address(Cpu *c) { c->x[0] = proc_address(c, (const char *)(uintptr_t)c->x[1]); }

// ---- Device. nvnDeviceGetInteger stays forwarded: the driver answers every property from constants
// (NX2_NVN_PROBE_INTEGERS=1 lists them; API version 231.39).
static uint64_t g_device;

// NVNboolean nvnDeviceInitialize(NVNdevice *, const NVNdeviceBuilder *)
static void hp_device_initialize(Cpu *c) {
    g_device = c->x[0];
    rt_log("nvn2: device %#llx initialized on the host (builder %#llx)", (unsigned long long)c->x[0],
           (unsigned long long)c->x[1]);
    c->x[0] = 1;
}
static void hp_device_finalize(Cpu *c) { (void)c; g_device = 0; }

// The driver's own implementation of a proc (for builder getters a host proc reads its builder through).
uint64_t nx2_driver_proc(Cpu *c, const char *name) {
    uint64_t a = real_gpa ? rt_call(c, real_gpa, 2, (uint64_t)0, (uint64_t)(uintptr_t)name) : 0;
    if (!a) rt_fatal("nvn2: the driver has no %s", name);
    return a;
}
#define driver_proc nx2_driver_proc

uint64_t nx2_driver_call(Cpu *c, uint64_t *cache, const char *name, int n, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3) {
    uint64_t f = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
    if (!f) __atomic_store_n(cache, f = nx2_driver_proc(c, name), __ATOMIC_RELEASE);
    return rt_call(c, f, n, a0, a1, a2, a3);
}

// ---- MemoryPool. The pool's memory is the game's storage; Switch 2 uses GPU address == CPU address for
// registered memory (Eglantine NX2_NOTES row 30), so the buffer address is the storage address. Host state lives in
// a table keyed by the guest object, so the guest's NVNmemoryPool bytes are never interpreted.
typedef struct { uint64_t obj, mem, size; uint32_t flags; } Pool;
#define MAX_POOLS 8192
static Pool pools[MAX_POOLS];
static int npools;
static PlatMutex pool_lock;
static uint64_t pool_bytes;
static volatile uint64_t pool_gen = 1;   // bumped on every change of the table (address lookup caches)

static Pool *pool_find(uint64_t obj) {   // under pool_lock
    for (int i = 0; i < npools; i++)
        if (pools[i].obj == obj) return &pools[i];
    return NULL;
}

// Host state is also written into the guest NVNmemoryPool itself (as TotK Recomp does for its NVN objects): the
// game copies pool objects by value, so a copy must carry its state. The table above only serves address lookups.
#define POOL_MAGIC 0x50325856u   // "VX2P"
typedef struct { uint32_t magic, flags; uint64_t mem, size; } PoolInPlace;

// An object that was never initialized reads as all zero, as with the driver: the game probes pools embedded in its
// resource files with nvnMemoryPoolGetBufferAddress and initializes them when it gets 0 (main+0x323d00c).
static Pool pool_get(uint64_t obj) {
    const PoolInPlace *ip = (const PoolInPlace *)(uintptr_t)obj;
    if (!obj || ip->magic != POOL_MAGIC) return (Pool){0};
    return (Pool){obj, ip->mem, ip->size, ip->flags};
}

// NVNboolean nvnMemoryPoolInitialize(NVNmemoryPool *, const NVNmemoryPoolBuilder *)
static void hp_pool_initialize(Cpu *c) {
    uint64_t obj = c->x[0], b = c->x[1];
    static uint64_t get_mem, get_size, get_flags;
    if (!get_mem) {
        get_mem = driver_proc(c, "nvnMemoryPoolBuilderGetMemory");
        get_size = driver_proc(c, "nvnMemoryPoolBuilderGetSize");
        get_flags = driver_proc(c, "nvnMemoryPoolBuilderGetFlags");
    }
    Pool p = {obj, rt_call(c, get_mem, 1, b), rt_call(c, get_size, 1, b), (uint32_t)rt_call(c, get_flags, 1, b)};
    if (!p.mem && p.size) {   // a virtual pool: an address range of its own until MapVirtual gives it storage
        void *r = VirtualAlloc(NULL, p.size, MEM_RESERVE, PAGE_NOACCESS);
        p.mem = (uint64_t)(uintptr_t)r;
    }
    *(PoolInPlace *)(uintptr_t)obj = (PoolInPlace){POOL_MAGIC, p.flags, p.mem, p.size};
    plat_mutex_lock(&pool_lock);
    Pool *slot = pool_find(obj);
    if (!slot) {
        if (npools == MAX_POOLS) rt_fatal("nvn2: more than %d memory pools", MAX_POOLS);
        slot = &pools[npools++];
    }
    *slot = p;
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    pool_bytes += p.size;
    uint64_t total = pool_bytes;
    int n = npools;
    plat_mutex_unlock(&pool_lock);
    if (n <= 64 || !(n & 63))
        rt_log("nvn2: memory pool %#llx: %#llx+%#llx flags %#x (%d pools, %.1f MiB)", (unsigned long long)obj,
               (unsigned long long)p.mem, (unsigned long long)p.size, p.flags, n, total / 1048576.0);
    c->x[0] = 1;
}

static void hp_pool_finalize(Cpu *c) {
    uint64_t base = 0, size = 0;
    plat_mutex_lock(&pool_lock);
    Pool *p = pool_find(c->x[0]);
    if (p) base = p->mem, size = p->size, pool_bytes -= p->size, *p = pools[--npools];
    __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
    plat_mutex_unlock(&pool_lock);
    if (base) nx2_vk_pool_released(base, size);
}
static void hp_pool_nop(Cpu *c) { (void)c; }
uint64_t nx2_pool_memory(uint64_t pool) { return pool_get(pool).mem; }
// The replay looks up every vertex / index / storage buffer here (~5k lookups a frame over thousands of pools): the
// last pools found are cached per thread until the table changes.
// Pools sorted by address for nx2_pool_range (pool_lock held exclusively): start, end, position in the table, and
// the largest end up to each entry (pools may overlap: virtual pools on physical ones). A linear scan over ~3000
// pools for every vertex, index and storage buffer was 8% of the render thread in gameplay.
typedef struct { uint64_t mem, end; int idx; } SortedPool;
static SortedPool sorted[MAX_POOLS];
static uint64_t sorted_maxend[MAX_POOLS];
static int nsorted;
static uint64_t sorted_gen;
static int by_mem(const void *a, const void *b) {
    uint64_t x = ((const SortedPool *)a)->mem, y = ((const SortedPool *)b)->mem;
    return x < y ? -1 : x > y;
}
static void pools_sort(void) {
    nsorted = 0;
    for (int i = 0; i < npools; i++)
        if (pools[i].mem && pools[i].size) sorted[nsorted++] = (SortedPool){pools[i].mem, pools[i].mem + pools[i].size, i};
    qsort(sorted, (size_t)nsorted, sizeof *sorted, by_mem);
    for (int i = 0; i < nsorted; i++)
        sorted_maxend[i] = i && sorted_maxend[i - 1] > sorted[i].end ? sorted_maxend[i - 1] : sorted[i].end;
    sorted_gen = pool_gen;
}

int nx2_pool_range(uint64_t addr, uint64_t *base, uint64_t *size) {
    static __thread struct { uint64_t gen, mem, size; } hit[4];
    static __thread int next;
    uint64_t gen = __atomic_load_n(&pool_gen, __ATOMIC_ACQUIRE);
    for (int k = 0; k < 4; k++)
        if (hit[k].gen == gen && addr >= hit[k].mem && addr < hit[k].mem + hit[k].size) {
            *base = hit[k].mem, *size = hit[k].size;
            return 1;
        }
    int ok = 0;
    plat_mutex_lock_shared(&pool_lock);
    gen = pool_gen;
    if (sorted_gen != gen) {   // the table changed: sort it again (exclusively)
        plat_mutex_unlock_shared(&pool_lock);
        plat_mutex_lock(&pool_lock);
        if (sorted_gen != pool_gen) pools_sort();
        plat_mutex_unlock(&pool_lock);
        plat_mutex_lock_shared(&pool_lock);
        gen = pool_gen;
    }
    if (sorted_gen == gen && nsorted) {
        // the last pool starting at or below addr, then back while an earlier one may still reach addr; of the pools
        // containing it, the one first in the table (what the linear scan returned)
        int lo = 0, hi = nsorted - 1, at = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (sorted[mid].mem <= addr) at = mid, lo = mid + 1;
            else hi = mid - 1;
        }
        int best = -1;
        for (int j = at; j >= 0 && sorted_maxend[j] > addr; j--)
            if (sorted[j].end > addr && (best < 0 || sorted[j].idx < sorted[best].idx)) best = j;
        if (best >= 0) *base = sorted[best].mem, *size = sorted[best].end - sorted[best].mem, ok = 1;
    } else {
        for (int i = 0; i < npools && !ok; i++)
            if (addr >= pools[i].mem && addr < pools[i].mem + pools[i].size) *base = pools[i].mem, *size = pools[i].size, ok = 1;
    }
    plat_mutex_unlock_shared(&pool_lock);
    if (ok) {
        int k = next++ & 3;
        hit[k].gen = gen, hit[k].mem = *base, hit[k].size = *size;
    }
    return ok;
}

uint64_t nx2_pool_end(uint64_t addr) {
    uint64_t end = 0;
    plat_mutex_lock_shared(&pool_lock);
    for (int i = 0; i < npools && !end; i++)
        if (addr >= pools[i].mem && addr < pools[i].mem + pools[i].size) end = pools[i].mem + pools[i].size;
    plat_mutex_unlock_shared(&pool_lock);
    return end;
}
static void zero_pool_note(Cpu *c, const char *what) {   // a query on a pool object the host never initialized
    static int n;
    if (n++ < 16) {
        char lr[128];
        rt_log("nvn2: %s on uninitialized pool %#llx -> 0 (from %s)", what, (unsigned long long)c->x[0],
               ldr_describe(c->x[30], lr, sizeof lr));
    }
}
static void hp_pool_map(Cpu *c) {
    uint64_t m = pool_get(c->x[0]).mem;
    if (!m) zero_pool_note(c, "Map");
    c->x[0] = m;
}
static void hp_pool_address(Cpu *c) {
    uint64_t m = pool_get(c->x[0]).mem;
    if (!m) zero_pool_note(c, "GetBufferAddress");
    c->x[0] = m;
}
static void hp_pool_size(Cpu *c) { c->x[0] = pool_get(c->x[0]).size; }
static void hp_pool_flags(Cpu *c) { c->x[0] = pool_get(c->x[0]).flags; }
// NVNboolean nvnMemoryPoolMapVirtual(NVNmemoryPool *virtual, int count, const NVNmappingRequest *requests)
// NVN1 NVNmappingRequest: { const NVNmemoryPool *physicalPool; int64 physicalOffset; int64 virtualOffset;
// uint64 size; int storageClass } (40 bytes). The game maps a whole physical pool into a virtual pool of the same
// size (main+0x31f70b4); when a request covers the virtual pool from offset 0, the virtual pool takes the physical
// storage's address, so its GPU and CPU addresses are real. Other mappings are recorded in the log for now.
typedef struct { uint64_t phys_pool; int64_t phys_off, virt_off; uint64_t size; int32_t storage_class, pad; } MapReq;

static void hp_pool_map_virtual(Cpu *c) {
    uint64_t vobj = c->x[0];
    int count = (int)c->x[1];
    const MapReq *req = (const MapReq *)(uintptr_t)c->x[2];
    PoolInPlace *v = (PoolInPlace *)(uintptr_t)vobj;
    for (int i = 0; i < count; i++) {
        Pool ph = pool_get(req[i].phys_pool);
        static int logged;
        if (logged++ < 8)
            rt_log("nvn2: MapVirtual %#llx: physical pool %#llx (storage %#llx) +%#llx -> virtual +%#llx, %#llx bytes, "
                   "class %d", (unsigned long long)vobj, (unsigned long long)req[i].phys_pool,
                   (unsigned long long)ph.mem, (unsigned long long)req[i].phys_off,
                   (unsigned long long)req[i].virt_off, (unsigned long long)req[i].size, req[i].storage_class);
        if (v->magic == POOL_MAGIC && ph.mem && req[i].virt_off == 0 && req[i].size >= v->size) {
            v->mem = ph.mem + (uint64_t)req[i].phys_off;
            plat_mutex_lock(&pool_lock);
            Pool *t = pool_find(vobj);
            if (t) t->mem = v->mem;
            __atomic_add_fetch(&pool_gen, 1, __ATOMIC_RELEASE);
            plat_mutex_unlock(&pool_lock);
        } else {
            static int partial;
            if (partial++ < 8) rt_log("nvn2: MapVirtual: partial mapping kept as a log entry only");
        }
    }
    c->x[0] = 1;
}

// ---- TexturePool / SamplerPool: descriptor tables in a memory pool. Registrations are kept per id on the host
// (the texture or sampler object and, for textures, the view), for the Vulkan back end to resolve the handles the
// game builds from them. Both kinds share one table, told apart by which Initialize made them.
typedef struct { uint64_t a, b; Nx2TexView v; Nx2Sampler s; } Desc;
typedef struct { uint64_t obj, mem_pool, offset; int count; Desc *desc; } DPool;
#define MAX_DPOOLS 256
static DPool dpools[MAX_DPOOLS];
static int ndpools;
static PlatMutex dpool_lock;

static DPool *dpool_find(uint64_t obj) {   // under dpool_lock
    for (int i = 0; i < ndpools; i++)
        if (dpools[i].obj == obj) return &dpools[i];
    return NULL;
}

// NVNboolean nvn{Texture,Sampler}PoolInitialize(pool *, const NVNmemoryPool *, ptrdiff_t offset, int numDescriptors)
static void hp_dpool_initialize(Cpu *c) {
    int count = (int)c->x[3];
    Desc *d = calloc(count > 0 ? (size_t)count : 1, sizeof *d);
    if (!d) rt_fatal("nvn2: descriptor pool of %d entries", count);
    plat_mutex_lock(&dpool_lock);
    DPool *p = dpool_find(c->x[0]);
    if (!p) {
        if (ndpools == MAX_DPOOLS) rt_fatal("nvn2: more than %d descriptor pools", MAX_DPOOLS);
        p = &dpools[ndpools++];
    } else {
        free(p->desc);
    }
    *p = (DPool){c->x[0], c->x[1], c->x[2], count, d};
    plat_mutex_unlock(&dpool_lock);
    rt_log("nvn2: descriptor pool %#llx: %d entries in memory pool %#llx at +%#llx", (unsigned long long)c->x[0], count,
           (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    c->x[0] = 1;
}

static void hp_dpool_finalize(Cpu *c) {
    plat_mutex_lock(&dpool_lock);
    DPool *p = dpool_find(c->x[0]);
    if (p) free(p->desc), *p = dpools[--ndpools];
    plat_mutex_unlock(&dpool_lock);
}

// void Register{Texture,Image}(pool *, int id, const NVNtexture *, const NVNtextureView *)
// void Register{Sampler,SamplerBuilder}(pool *, int id, const NVNsampler{,Builder} *)
static void decode_view(Cpu *c, uint64_t view, Nx2TexView *v) {
    memset(v, 0, sizeof *v);
    if (!view) return;
    static _Thread_local int32_t *out;
    if (!out) out = mem_kernel_alloc(64, 64);
    uint64_t o = (uint64_t)(uintptr_t)out;
    static uint64_t g_sw, g_fmt, g_tgt, g_lv, g_ly;
    if (!__atomic_load_n(&g_sw, __ATOMIC_ACQUIRE))   // 5 arguments: called through rt_call directly
        __atomic_store_n(&g_sw, nx2_driver_proc(c, "nvnTextureViewGetSwizzle"), __ATOMIC_RELEASE);
    static int mask = -1;
    if (mask < 0) mask = getenv("NX2_VIEW_DECODE_MASK") ? atoi(getenv("NX2_VIEW_DECODE_MASK")) : 31;
    memset(out, 0, 64);
    if ((mask & 1) && rt_call(c, g_sw, 5, view, o, o + 4, o + 8, o + 12) & 0xff) {
        v->has_swizzle = 1;
        for (int i = 0; i < 4; i++) v->swizzle[i] = (uint8_t)out[i];
    }
    if ((mask & 2) && nx2_driver_call(c, &g_fmt, "nvnTextureViewGetFormat", 2, view, o + 16, 0, 0) & 0xff)
        v->has_format = 1, v->format = out[4];
    if ((mask & 4) && nx2_driver_call(c, &g_tgt, "nvnTextureViewGetTarget", 2, view, o + 20, 0, 0) & 0xff)
        v->has_target = 1, v->target = out[5];
    if ((mask & 8) && nx2_driver_call(c, &g_lv, "nvnTextureViewGetLevels", 3, view, o + 24, o + 28, 0) & 0xff)
        v->has_levels = 1, v->base_level = out[6], v->levels = out[7];
    if ((mask & 16) && nx2_driver_call(c, &g_ly, "nvnTextureViewGetLayers", 3, view, o + 32, o + 36, 0) & 0xff)
        v->has_layers = 1, v->min_layer = out[8], v->layers = out[9];
    static int logged;
    if (logged < 12 && (v->has_swizzle || v->has_format || v->has_target || v->has_levels || v->has_layers)) {
        logged++;
        rt_log("nvn2: texture view: swizzle %d (%d %d %d %d) format %d (%#x) target %d (%d) levels %d (%d+%d) "
               "layers %d (%d+%d)", v->has_swizzle, v->swizzle[0], v->swizzle[1], v->swizzle[2], v->swizzle[3],
               v->has_format, v->format, v->has_target, v->target, v->has_levels, v->base_level, v->levels,
               v->has_layers, v->min_layer, v->layers);
    }
}

// Register{Texture,Image}: the view is decoded now (it may live on the caller's stack)
static void hp_dpool_register_texture(Cpu *c) {
    // arguments first: the getter calls in decode_view clobber x0-x18
    uint64_t pool = c->x[0], tex = c->x[2], view = c->x[3];
    int id = (int)c->x[1];
    Nx2TexView v;
    static int off = -1;
    if (off < 0) off = getenv("NX2_NO_VIEW_DECODE") != NULL;
    if (off) memset(&v, 0, sizeof v);
    else decode_view(c, view, &v);
    plat_mutex_lock(&dpool_lock);
    DPool *p = dpool_find(pool);
    int ok = p && id >= 0 && id < p->count;
    if (ok) p->desc[id] = (Desc){tex, view, v, {0}};
    plat_mutex_unlock(&dpool_lock);
    if (!ok) rt_log("nvn2: register id %d in texture pool %#llx out of range", id, (unsigned long long)pool);
}

int nx2_dpool_view(uint64_t pool, int id, Nx2TexView *out) {
    plat_mutex_lock_shared(&dpool_lock);
    DPool *p = dpool_find(pool);
    int ok = p && id >= 0 && id < p->count && p->desc[id].a;
    if (ok) *out = p->desc[id].v;
    plat_mutex_unlock_shared(&dpool_lock);
    return ok;
}

// Register{Sampler,SamplerBuilder}(pool, id, object): the sampler's state, read with the matching getters
static float vfloat(const Cpu *c) {   // a float return value (s0)
    float f;
    memcpy(&f, &c->v[0], 4);
    return f;
}
static void decode_sampler(Cpu *c, uint64_t obj, int builder, Nx2Sampler *o) {
    memset(o, 0, sizeof *o);
    if (!obj) return;
    static _Thread_local int32_t *out;
    if (!out) out = mem_kernel_alloc(64, 64);
    uint64_t q = (uint64_t)(uintptr_t)out;
    static uint64_t g[2][7];
    static const char *names[2][7] = {
        {"nvnSamplerGetMinMagFilter", "nvnSamplerGetWrapMode", "nvnSamplerGetLodClamp", "nvnSamplerGetLodBias",
         "nvnSamplerGetCompare", "nvnSamplerGetMaxAnisotropy", "nvnSamplerGetBorderColor"},
        {"nvnSamplerBuilderGetMinMagFilter", "nvnSamplerBuilderGetWrapMode", "nvnSamplerBuilderGetLodClamp",
         "nvnSamplerBuilderGetLodBias", "nvnSamplerBuilderGetCompare", "nvnSamplerBuilderGetMaxAnisotropy",
         "nvnSamplerBuilderGetBorderColor"}};
    uint64_t *gg = g[builder];
    memset(out, 0, 64);
    nx2_driver_call(c, &gg[0], names[builder][0], 3, obj, q, q + 4, 0);
    nx2_driver_call(c, &gg[1], names[builder][1], 4, obj, q + 8, q + 12, q + 16);
    nx2_driver_call(c, &gg[2], names[builder][2], 3, obj, q + 20, q + 24, 0);
    o->min = (uint8_t)out[0], o->mag = (uint8_t)out[1];
    o->wrap[0] = (uint8_t)out[2], o->wrap[1] = (uint8_t)out[3], o->wrap[2] = (uint8_t)out[4];
    memcpy(&o->lod_min, &out[5], 4);
    memcpy(&o->lod_max, &out[6], 4);
    nx2_driver_call(c, &gg[3], names[builder][3], 1, obj, 0, 0, 0);
    o->lod_bias = vfloat(c);
    memset(out, 0, 16);
    nx2_driver_call(c, &gg[4], names[builder][4], 3, obj, q, q + 4, 0);
    o->compare_mode = (uint8_t)out[0], o->compare_func = (uint8_t)out[1];
    nx2_driver_call(c, &gg[5], names[builder][5], 1, obj, 0, 0, 0);
    o->aniso = vfloat(c);
    memset(out, 0, 16);
    nx2_driver_call(c, &gg[6], names[builder][6], 2, obj, q, 0, 0);
    memcpy(o->border, out, 16);
    o->valid = 1;
    static int logged;
    if (logged++ < 10)
        rt_log("nvn2: sampler: min %d mag %d wrap %d %d %d lod %g..%g bias %g compare %d/%d aniso %g border %g %g %g %g",
               o->min, o->mag, o->wrap[0], o->wrap[1], o->wrap[2], o->lod_min, o->lod_max, o->lod_bias,
               o->compare_mode, o->compare_func, o->aniso, o->border[0], o->border[1], o->border[2], o->border[3]);
}

int nx2_dpool_sampler(uint64_t pool, int id, Nx2Sampler *out) {
    plat_mutex_lock_shared(&dpool_lock);
    DPool *p = dpool_find(pool);
    int ok = p && id >= 0 && id < p->count && p->desc[id].s.valid;
    if (ok) *out = p->desc[id].s;
    plat_mutex_unlock_shared(&dpool_lock);
    return ok;
}

static void register_sampler(Cpu *c, int builder) {
    uint64_t pool = c->x[0], obj = c->x[2];   // arguments first: the getters clobber x0-x18
    int id = (int)c->x[1];
    Nx2Sampler smp;
    decode_sampler(c, obj, builder, &smp);
    plat_mutex_lock(&dpool_lock);
    DPool *p = dpool_find(pool);
    int ok = p && id >= 0 && id < p->count;
    if (ok) p->desc[id] = (Desc){obj, 0, {0}, smp};
    plat_mutex_unlock(&dpool_lock);
    if (!ok) rt_log("nvn2: register id %d in sampler pool %#llx out of range", id, (unsigned long long)pool);
}
static void hp_register_sampler(Cpu *c) { register_sampler(c, 0); }
static void hp_register_sampler_builder(Cpu *c) { register_sampler(c, 1); }

static void hp_dpool_register(Cpu *c) {
    plat_mutex_lock(&dpool_lock);
    DPool *p = dpool_find(c->x[0]);
    int id = (int)c->x[1], ok = p && id >= 0 && id < p->count;
    if (ok) p->desc[id] = (Desc){c->x[2], 0, {0}, {0}};
    plat_mutex_unlock(&dpool_lock);
    if (!ok) rt_log("nvn2: register id %d in descriptor pool %#llx out of range", id, (unsigned long long)c->x[0]);
}

static DPool dpool_get(uint64_t obj) {
    plat_mutex_lock_shared(&dpool_lock);
    DPool *p = dpool_find(obj);
    DPool r = p ? *p : (DPool){0};
    plat_mutex_unlock_shared(&dpool_lock);
    if (!r.obj) rt_fatal("nvn2: descriptor pool %#llx is not initialized", (unsigned long long)obj);
    return r;
}
int nx2_dpool_lookup(uint64_t pool, int id, uint64_t *obj, uint64_t *view) {
    plat_mutex_lock_shared(&dpool_lock);
    DPool *p = dpool_find(pool);
    int ok = p && id >= 0 && id < p->count && p->desc[id].a;
    if (ok) *obj = p->desc[id].a, *view = p->desc[id].b;
    plat_mutex_unlock_shared(&dpool_lock);
    return ok;
}
static void hp_dpool_memory_pool(Cpu *c) { c->x[0] = dpool_get(c->x[0]).mem_pool; }
static void hp_dpool_offset(Cpu *c) { c->x[0] = dpool_get(c->x[0]).offset; }
static void hp_dpool_size(Cpu *c) { c->x[0] = (uint64_t)dpool_get(c->x[0]).count; }
static void hp_dpool_write_table(Cpu *c) {   // NVN2: bulk descriptor writes; arguments still to be identified
    static int n;
    if (n++ < 4)
        rt_log("nvn2: DescriptorTableData write (%#llx, %#llx, %#llx, %#llx) not interpreted yet",
               (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
               (unsigned long long)c->x[3]);
}

// ---- Queue, Sync, Window: null back end. Nothing is drawn yet; this lets the game's frame loop run so its
// command-buffer recording and per-frame NVN2 use can be observed before the Vulkan back end replays them.
static void hp_ret0(Cpu *c) { c->x[0] = 0; }
static void hp_ret1(Cpu *c) { c->x[0] = 1; }

static uint64_t g_submits, g_presents, g_present_t0;
static void hp_queue_submit(Cpu *c) {   // void nvnQueueSubmitCommands(NVNqueue *, int count, const NVNcommandHandle *)
    int n = (int)c->x[1];
    void nx2_sampler_here(void);
    nx2_sampler_here();   // NX2_SAMPLE_PROF: profile this (the render) thread
    __atomic_add_fetch(&g_submits, (uint64_t)n, __ATOMIC_RELAXED);
    // The replay and the GPU wait are host work: the virtual core goes to other guest threads meanwhile (on the
    // console the submit returns at once; holding the core here stalled the threads that share it).
    static int keep = -1;
    if (keep < 0) keep = getenv("NX2_SUBMIT_KEEP_CORE") != NULL;
    int away = keep ? 0 : kern_core_away();
    if (n > 0 && c->x[2]) nx2_cmd_submitted(n, (const uint64_t *)(uintptr_t)c->x[2]);
    kern_core_back(&away);
}
// ---- syncs. The Vulkan back end submits without waiting (nx2_vkdraw.c slots): a sync fenced on the queue keeps the
// serial of the last submission, and waiting on it waits for that submission (the virtual core is free meanwhile).
uint64_t nx2_vk_submitted_serial(void);
int nx2_vk_wait_serial(uint64_t serial, uint64_t timeout_ns);
#define SYNC_SLOTS 4096
static struct { uint64_t obj, serial; } g_syncs[SYNC_SLOTS];
static PlatMutex g_sync_lock;
static void sync_set(uint64_t obj, uint64_t serial) {
    plat_mutex_lock(&g_sync_lock);
    uint64_t h = (obj >> 3) * 0x9e3779b97f4a7c15ull >> 52;
    for (uint64_t i = 0; i < SYNC_SLOTS; i++) {
        uint64_t k = (h + i) & (SYNC_SLOTS - 1);
        if (!g_syncs[k].obj || g_syncs[k].obj == obj) {
            g_syncs[k].obj = obj, g_syncs[k].serial = serial;
            break;
        }
    }
    plat_mutex_unlock(&g_sync_lock);
}
static uint64_t sync_get(uint64_t obj) {
    uint64_t r = 0;
    plat_mutex_lock(&g_sync_lock);
    uint64_t h = (obj >> 3) * 0x9e3779b97f4a7c15ull >> 52;
    for (uint64_t i = 0; i < SYNC_SLOTS; i++) {
        uint64_t k = (h + i) & (SYNC_SLOTS - 1);
        if (!g_syncs[k].obj) break;
        if (g_syncs[k].obj == obj) {
            r = g_syncs[k].serial;
            break;
        }
    }
    plat_mutex_unlock(&g_sync_lock);
    return r;
}
// NVNboolean nvnSyncInitialize(NVNsync *, NVNdevice *)
static void hp_sync_initialize(Cpu *c) { sync_set(c->x[0], 0), c->x[0] = 1; }
// void nvnQueueFenceSync(NVNqueue *, NVNsync *, NVNsyncCondition, int flags)
static void hp_queue_fence_sync(Cpu *c) { sync_set(c->x[1], nx2_vk_submitted_serial()); }
// NVNsyncWaitResult nvnSyncWait(const NVNsync *, uint64_t timeoutNs): 0 already signalled, 1 condition satisfied,
// 2 timeout expired
static void hp_sync_wait(Cpu *c) {
    uint64_t serial = sync_get(c->x[0]), timeout = c->x[1];
    if (!serial) {
        c->x[0] = 0;
        return;
    }
    if (nx2_vk_wait_serial(serial, 0)) {
        c->x[0] = 0;
        return;
    }
    if (!timeout) {
        c->x[0] = 2;
        return;
    }
    int away = kern_core_away();
    int done = nx2_vk_wait_serial(serial, timeout);
    kern_core_back(&away);
    c->x[0] = done ? 1 : 2;
}
// void nvnQueueFinish(NVNqueue *)
static void hp_queue_finish(Cpu *c) {
    (void)c;
    int away = kern_core_away();
    nx2_vk_wait_serial(0, UINT64_MAX);
    kern_core_back(&away);
}

typedef struct { uint64_t obj, native; int textures, interval, next; uint64_t tex[8]; } Win;
static Win g_win;

// The window texture is converted from block-linear to linear rows and handed to the presenter (nx2_vk.c).
static void present_window_texture(int index, uint64_t frame) {
    if (index < 0 || index >= g_win.textures || index >= 8) return;
    nvn_texdesc d;
    uint8_t *storage;
    uint64_t size;
    if (!nx2_tex_get(g_win.tex[index], &d, &storage, &size)) return;
    extern int nx2_vk_present_texture(uint64_t obj, uint64_t frame);
    if (!getenv("NX2_TEST_PATTERN") && nx2_vk_present_texture(g_win.tex[index], frame)) return;
    static uint8_t *rgba;
    static size_t rgba_n;
    size_t need = (size_t)d.width * d.height * 4;
    if (rgba_n < need) free(rgba), rgba = malloc(need), rgba_n = need;
    if (!rgba) return;
    const char *tp = getenv("NX2_TEST_PATTERN");
    if (tp && *tp == '1') {   // a gradient written into the tiled storage, to check the conversion and presenter
        for (uint32_t y = 0; y < d.height; y++)
            for (uint32_t x = 0; x < d.width; x++) {
                uint8_t *q = rgba + ((size_t)y * d.width + x) * 4;
                q[0] = (uint8_t)(x * 255 / d.width), q[1] = (uint8_t)(y * 255 / d.height);
                q[2] = (uint8_t)(frame * 4), q[3] = 255;
            }
        nvn_swizzle(&d, 0, 0, rgba, (size_t)d.width * 4, storage);
    }
    if (nvn_deswizzle(&d, 0, 0, storage, rgba, (size_t)d.width * 4)) return;
    nx2_present_rgba(rgba, d.width, d.height, frame);
}

static void hp_queue_present(Cpu *c) {   // void nvnQueuePresentTexture(NVNqueue *, NVNwindow *, int index)
    uint64_t n = __atomic_add_fetch(&g_presents, 1, __ATOMIC_RELAXED);
    int away = kern_core_away();   // presenting (and pacing to 60 fps) is host work: the core is free meanwhile
    present_window_texture((int)c->x[2], n);
    kern_core_back(&away);
    if (n == 60) {
        void nx2_cmd_histogram(void);
        nx2_cmd_histogram();
    }
    uint64_t now = plat_now_ns();
    if (n == 1) g_present_t0 = now;
    if (n == 1 || n % 60 == 0) {
        uint64_t rec, sub, unk, rts, clears, noclr;
        nx2_cmd_stats(&rec, &sub, &unk);
        void nx2_cmd_replay_stats(uint64_t *, uint64_t *, uint64_t *);
        nx2_cmd_replay_stats(&rts, &clears, &noclr);
        extern uint64_t nx2_draws;
        rt_log("nvn2 replay so far: %llu SetRenderTargets, %llu ClearColor (%llu without a target), %llu draws",
               (unsigned long long)rts, (unsigned long long)clears, (unsigned long long)noclr,
               (unsigned long long)nx2_draws);
        rt_log("nvn2: present #%llu (texture %d; %llu handles submitted, %llu commands recorded, %llu submitted, "
               "%llu unknown handles; %.1f fps since the first)",
               (unsigned long long)n, (int)c->x[2], (unsigned long long)g_submits, (unsigned long long)rec,
               (unsigned long long)sub, (unsigned long long)unk, n > 1 ? (n - 1) / ((now - g_present_t0) / 1e9) : 0.0);
    }
}

static void hp_window_initialize(Cpu *c) {   // NVNboolean nvnWindowInitialize(NVNwindow *, const NVNwindowBuilder *)
    static uint64_t get_num, get_native;
    static _Thread_local uint8_t *out;
    if (!out) out = mem_kernel_alloc(256, 64);
    uint64_t b = c->x[1], obj = c->x[0];
    // NVN2 builder getters may return through a caller structure (second argument), as some texture ones do: pass
    // one and log both, the return value and what was written. (Window getters return directly: 3 textures.)
    memset(out, 0, 256);
    uint64_t r_num = nx2_driver_call(c, &get_num, "nvnWindowBuilderGetNumTextures", 2, b, (uint64_t)(uintptr_t)out, 0, 0);
    int out_num = *(int *)out;
    memset(out, 0, 256);
    uint64_t r_native = nx2_driver_call(c, &get_native, "nvnWindowBuilderGetNativeWindow", 2, b, (uint64_t)(uintptr_t)out, 0, 0);
    uint64_t out_native = *(uint64_t *)out;
    int textures = r_num > 0 && r_num <= 8 ? (int)r_num : out_num > 0 && out_num <= 8 ? out_num : 3;
    g_win = (Win){obj, out_native ? out_native : r_native, textures, 1, 0, {0}};
    static uint64_t get_tex;   // const NVNtexture *nvnWindowBuilderGetTexture(const NVNwindowBuilder *, int)
    for (int i = 0; i < textures && i < 8; i++)
        g_win.tex[i] = nx2_driver_call(c, &get_tex, "nvnWindowBuilderGetTexture", 2, b, (uint64_t)i, 0, 0);
    rt_log("nvn2: window %#llx: %d textures", (unsigned long long)obj, textures);
    c->x[0] = 1;
}
// NVNwindowAcquireTextureResult nvnWindowAcquireTexture(NVNwindow *, NVNsync *, int *index)
static void hp_window_acquire(Cpu *c) {
    int *idx = (int *)(uintptr_t)c->x[2];
    if (idx) *idx = g_win.textures > 0 ? g_win.next++ % g_win.textures : 0;
    c->x[0] = 0;   // success
}
static void hp_window_num_textures(Cpu *c) { c->x[0] = (uint64_t)g_win.textures; }
static void hp_window_get_interval(Cpu *c) { c->x[0] = (uint64_t)g_win.interval; }
static void hp_window_set_interval(Cpu *c) { g_win.interval = (int)c->x[1]; }
static void hp_window_get_native(Cpu *c) { c->x[0] = g_win.native; }

// void *nvnBootstrapLoader(const char *name)
static void ov_nvn_bootstrap(Cpu *c) {
    const char *name = (const char *)(uintptr_t)c->x[0];
    static int first = 1;
    if (first) {
        first = 0;
        rt_boot_event("nvn", c, name ? name : "(null)");
        uint64_t loader = ldr_symbol("nvnBootstrapLoader");   // the sdk's own, which main's import no longer reaches
        real_gpa = loader ? rt_call(c, loader, 1, (uint64_t)(uintptr_t)"nvnDeviceGetProcAddress") : 0;
        char d[128];
        rt_log("nvn2: driver nvnBootstrapLoader %#llx, nvnDeviceGetProcAddress %s", (unsigned long long)loader,
               real_gpa ? ldr_describe(real_gpa, d, sizeof d) : "not found: every proc without a host version is a stub");
        // NX2_NVN_PROBE_INTEGERS=1: ask the driver's nvnDeviceGetInteger about a zeroed device for each property,
        // to learn which device values it keeps as constants (the Switch 2 GPU's limits and alignments).
        const char *probe = getenv("NX2_NVN_PROBE_INTEGERS");
        if (real_gpa && probe && *probe == '1') {
            uint64_t gi = rt_call(c, real_gpa, 2, (uint64_t)0, (uint64_t)(uintptr_t)"nvnDeviceGetInteger");
            uint8_t *dev = mem_kernel_alloc(0x10000, 64);
            int *out = mem_kernel_alloc(16, 16);
            for (int k = 0; k < 0x100; k++) {
                *out = 0x5a5a5a5a;
                rt_call(c, gi, 3, (uint64_t)(uintptr_t)dev, (uint64_t)k, (uint64_t)(uintptr_t)out);
                rt_log("nvn2 probe: GetInteger(%#x) = %d (%#x)", k, *out, *out);
            }
        }
    }
    c->x[0] = proc_address(c, name);
}
HOST_OVERRIDE("nvnBootstrapLoader", ov_nvn_bootstrap);

// Synthetic addresses before hostfn_seal (sp_main.c seals after ldr_load).
__attribute__((constructor)) static void nx2_nvn_register(void) {
    // Each slot needs its own label: hostfn_register gives a repeated name the address it already has.
    static char labels[SLOTS][16];
    for (int i = 0; i < SLOTS; i++) {
        snprintf(labels[i], sizeof labels[i], "nvn2 proc %d", i);
        slots[i].addr = hostfn_register(labels[i], nvn_proc);
    }
    const char *t = getenv("NX2_NVN_TRACE");
    if (t) trace_limit = atoi(t);
}

