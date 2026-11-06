// Vulkan execution of the NVN2 replay (state in nx2_draw.c). Each nvnQueueSubmitCommands becomes one Vulkan command
// buffer, submitted and waited for at the end of the submit (simple and safe while the back end grows; frame
// overlap comes later).
//
// - Images: one VkImage per NVN texture object. A texture the GPU never wrote is uploaded from its storage
//   (block-linear -> linear) the first time a draw samples it; render targets are GPU images from then on.
// - Programs: translated in game (shader/nx2_xlate.cpp); the NVN bindings reach the shaders as Eglantine lays
//   them out for this title: uniform buffer binding i of a stage is constant buffer c(3 + i), texture binding i is
//   the handle at c2[0x20 + 8 i].
// - Pipelines: per program, vertex input, render target formats and texture types; dynamic rendering.
// Images stay in VK_IMAGE_LAYOUT_GENERAL (no cost on NVIDIA; textures may also be render targets); a full memory
// barrier separates render passes, copies and clears.
// Not yet: blend / depth / raster state (opaque, no depth test), samplers (linear clamp), storage buffers and
// images, compute dispatches, copies.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "nx2_astc.h"
#include "nx2_draw.h"
#include "nx2_nvn.h"
#include "nx2_texels.h"
#include "nx2_vk.h"
#include "nx2_vkfmt.h"
#include "nx2_xlate.h"
#include "platform/platform.h"

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) rt_fatal("vulkan: %s failed (%d)", #x, (int)r_); } while (0)

static Nx2VkCtx *V;
static VkDevice D;
static PlatMutex lock;
static int disabled = -1;   // NX2_GPU=0: no Vulkan execution (null back end)

// ---- per-submit command buffer, staging ring, descriptor pool
static VkCommandPool cpool;
static VkCommandBuffer cb;   // the current slot's
static VkFence fence;
static int recording, rendering;
#define STAGE_SIZE (384ull << 20)   // NSLOT parts of SLOT_STAGE
static VkBuffer stage_buf;
static VkDeviceMemory stage_mem;
static uint8_t *stage_ptr;
static VkDeviceSize stage_off;
static VkDescriptorPool dpool;
static VkSampler sampler_linear, sampler_nearest;
static VkDeviceSize ubo_align = 256, ssbo_align = 64;
static VkPhysicalDeviceVulkan12Properties props12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};

typedef struct Img Img;
static void destroy_img(Img *m);
static Img *rt_image(int i);   // the image colour target i is drawn into
typedef struct Imp {
    uint64_t base, size;
    VkDeviceMemory mem;
    VkBuffer buf;
    int failed;
    struct Imp *next;
    VkBuffer dbuf;          // video-memory mirror (see mirror_of)
    VkDeviceMemory dmem;
    int mstate;             // mirror: 0 not tried, 1 in use, -1 not possible
    int mlist;              // index in mirrors[], -1 none
    uint64_t msynced, mused;   // mirror epochs: last sync, last use
    uint64_t *mhash;           // NX2_MIRROR_VERIFY: a hash per page at the last sync
    uint8_t *mgpu;             // NX2_MIRROR_VERIFY: pages the GPU wrote back since the last sync (mirror updated too)
} Imp;
static Imp *last_imp;       // the pool of the last buffer_at
#define IMP_BUCKETS 1024
static Imp *imps[IMP_BUCKETS];
static int nimports, import_off = -1;
static uint64_t import_align = 4096, import_bytes;
static PFN_vkGetMemoryHostPointerPropertiesEXT get_host_props;

// Frame-time profile (logged every 300 presents): GPU waits, pipeline builds, submits (nx2_cmd.c), pacing (nx2_vk.c)
uint64_t nx2_prof_wait_ns, nx2_prof_pipe_ns, nx2_prof_submit_ns, nx2_prof_pace_ns, nx2_prof_flushes, nx2_prof_pipes;
static uint64_t prof_xlate_ns, prof_vkpipe_ns;
// Counters for the performance overlay (nx2_overlay.c), never reset: it takes differences over its own interval.
uint64_t nx2_stat_submit_ns, nx2_stat_wait_ns, nx2_stat_pipes;
volatile int nx2_dump_request;   // set by nx2_vk.c (NX2_DUMP_ON_BLACK)   // pipeline builds: shader translation, driver pipeline creation
// CPU time of the draw phases (TSC ticks), in the "prof:" report
#include <x86intrin.h>
enum { DP_START, DP_PIPE, DP_CONST, DP_IMGS, DP_DESC, DP_VTX, DP_CMD, DP_UPLOAD, DP_LOOKUP, DP_IMAGE, DP_LAYOUT, DP_N };
static uint64_t dp_ssbo_bytes, dp_ssbo_n;
static uint64_t th_n, th_bytes, th_big, th_skipped;   // CPU texture change checks (storage_hash) for "prof:"
static uint32_t th_big_w, th_big_h, th_big_f;   // written storage buffers through the device ring (copied in and out)
static uint64_t dp_ticks[DP_N], dp_t, dp_vtx_copied, dp_vtx_copies, dp_idx_copied, dp_vtx_inplace, dp_idx_inplace;
static const char *const dp_name[DP_N] = {"targets", "pipeline", "constants", "textures", "descriptors", "vertices", "commands",
                                          "(textures: uploads/hashes", "lookups", "images", "layouts)"};
#define DP_MARK(k) do { uint64_t t_ = __rdtsc(); dp_ticks[k] += t_ - dp_t; dp_t = t_; } while (0)
// Storage buffers a shader writes run in device memory: the game's memory is imported host memory, where every
// GPU access (atomics above all: a depth max over 920k pixels took 46 ms) crosses the bus. Copied in before the
// draw or dispatch, back after it (ssbo_writeback). The ring is free again after each flush.
#define DEV_SIZE (64ull << 20)
static VkBuffer dev_buf;
static VkDeviceMemory dev_mem;
static VkDeviceSize dev_off;
static struct { VkBuffer dst; struct Imp *imp; VkDeviceSize dst_off, src_off, size; } ssbo_back[64];
static int nssbo_back;
// Storage buffers a shader writes in place (in the game's memory: larger than the device ring takes, or the ring full)
// whose pool has a video-memory mirror by then: the written range is copied into the mirror after the draw
// (ssbo_writeback).
static struct { struct Imp *imp; VkDeviceSize off, size; } mfix[64];
static int nmfix;

// ---- submission slots. flush() submits the recorded work and goes on with the next slot without waiting: the GPU
// runs one submission while the next is replayed. A slot's staging part, device ring part, descriptor pool and the
// objects that died while it was recorded (imports, images) are reused or destroyed once its fence has signalled
// (slot_retire). The game waits through its sync objects (nx2_vk_wait_serial). NX2_SYNC_SUBMIT=1: every flush waits.
#define NSLOT 3
#define SLOT_STAGE (STAGE_SIZE / NSLOT)
typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    VkDescriptorPool dpool;
    int pending;
    uint64_t serial;
    struct { VkDeviceSize off; uint64_t addr, size; } wb_[1024];   // staged storage buffers to copy back
    int nwb_;
    struct Imp *dead_imps_[256];
    int ndead_imps_;
    Img *dead_[1024];   // replaced images
    int ndead_;
} Slot;
static Slot slots_[NSLOT];
static int cur;
static uint64_t submit_serial, done_serial;
#define wb (slots_[cur].wb_)
#define nwb (slots_[cur].nwb_)
#define dead_imps (slots_[cur].dead_imps_)
#define ndead_imps (slots_[cur].ndead_imps_)
#define dead (slots_[cur].dead_)
#define ndead (slots_[cur].ndead_)
static VkDeviceSize stage_base(void) { return (VkDeviceSize)cur * SLOT_STAGE; }
static VkDeviceSize dev_base(void) { return (VkDeviceSize)cur * DEV_SIZE; }

// NX2_GPU_PROF=<present>: GPU time of every draw and dispatch of that present (timestamps), summed by program
#define GP_QUERIES 8192
static VkQueryPool gp_pool;
static uint32_t gp_nq;
static struct { uint32_t serial, w, h, compute; } gp_tag[GP_QUERIES / 2];
static double gp_ms[65536];
static uint32_t gp_count[65536], gp_w[65536], gp_h[65536], gp_cs[65536];
static float gp_period;
extern uint64_t nx2_vk_frame;
static int gp_on(void) {
    static int64_t want = -2;
    if (want == -2) want = getenv("NX2_GPU_PROF") ? atoll(getenv("NX2_GPU_PROF")) : -1;
    return want >= 0 && (int64_t)nx2_vk_frame + 1 == want;
}
static int gp_begin(void) {   // a query index, -1 when not profiling
    if (!gp_pool || !gp_on() || gp_nq + 2 > GP_QUERIES) return -1;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gp_pool, gp_nq);
    return (int)gp_nq;
}
static void gp_end(int q, uint32_t serial, uint32_t w, uint32_t h, int compute) {
    if (q < 0) return;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gp_pool, (uint32_t)q + 1);
    gp_tag[q / 2].serial = serial, gp_tag[q / 2].w = w, gp_tag[q / 2].h = h, gp_tag[q / 2].compute = compute;
    gp_nq += 2;
}
static void gp_collect(void) {   // after the fence
    if (!gp_nq) return;
    static uint64_t ts[GP_QUERIES];
    if (vkGetQueryPoolResults(D, gp_pool, 0, gp_nq, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        for (uint32_t i = 0; i < gp_nq; i += 2) {
            uint32_t sr = gp_tag[i / 2].serial & 0xffff;
            gp_ms[sr] += (double)(ts[i + 1] - ts[i]) * gp_period * 1e-6;
            gp_count[sr]++, gp_w[sr] = gp_tag[i / 2].w, gp_h[sr] = gp_tag[i / 2].h, gp_cs[sr] = gp_tag[i / 2].compute;
        }
    gp_nq = 0;
}
static void gp_report(void) {
    double total = 0;
    for (int i = 0; i < 65536; i++) total += gp_ms[i];
    if (total <= 0) return;
    rt_log("gpu prof: %.2f ms in draws and dispatches (summed; they may overlap)", total);
    for (int k = 0; k < 25; k++) {
        int best = -1;
        for (int i = 0; i < 65536; i++)
            if (gp_ms[i] > 0 && (best < 0 || gp_ms[i] > gp_ms[best])) best = i;
        if (best < 0) break;
        rt_log("gpu prof:  %7.2f ms  %s #%d x%u  target %ux%u", gp_ms[best], gp_cs[best] ? "dispatch" : "draw    ", best,
               gp_count[best], gp_w[best], gp_h[best]);
        gp_ms[best] = 0;
    }
    memset(gp_ms, 0, sizeof gp_ms), memset(gp_count, 0, sizeof gp_count);
}

static void begin_cb(void) {
    if (recording) return;
    VKCHECK(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKCHECK(vkBeginCommandBuffer(cb, &bi));
    recording = 1;
    {   // what earlier submissions wrote (they may still run) before anything here
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                             NULL, 0, NULL);
    }
    if (gp_pool && gp_on()) vkCmdResetQueryPool(cb, gp_pool, 0, GP_QUERIES), gp_nq = 0;
    void occ_new_chunk(void);
    occ_new_chunk();
}

static void occ_cut(void);
static void end_rendering(void) {
    if (!rendering) return;
    occ_cut();   // a query begun inside this rendering ends with it
    vkCmdEndRendering(cb);
    rendering = 0;
}

// Waits for slot k's submission (if any) and frees what it held. Under lock.
static void slot_retire(int k) {
    Slot *sl = &slots_[k];
    if (sl->pending) {
        uint64_t t0 = plat_now_ns();
        VKCHECK(vkWaitForFences(D, 1, &sl->fence, VK_TRUE, UINT64_MAX));
        nx2_prof_wait_ns += plat_now_ns() - t0, nx2_stat_wait_ns += plat_now_ns() - t0;
        VKCHECK(vkResetFences(D, 1, &sl->fence));
        sl->pending = 0;
        if (sl->serial > done_serial) done_serial = sl->serial;
        gp_collect();
    }
    for (int i = 0; i < sl->nwb_; i++)
        memcpy((void *)(uintptr_t)sl->wb_[i].addr, stage_ptr + sl->wb_[i].off, sl->wb_[i].size);
    sl->nwb_ = 0;
    for (int i = 0; i < sl->ndead_imps_; i++) {
        if (sl->dead_imps_[i]->dbuf) vkDestroyBuffer(D, sl->dead_imps_[i]->dbuf, NULL);
        if (sl->dead_imps_[i]->dmem) vkFreeMemory(D, sl->dead_imps_[i]->dmem, NULL);
        vkDestroyBuffer(D, sl->dead_imps_[i]->buf, NULL);
        vkFreeMemory(D, sl->dead_imps_[i]->mem, NULL);
        free(sl->dead_imps_[i]);
    }
    sl->ndead_imps_ = 0;
    for (int i = 0; i < sl->ndead_; i++) destroy_img(sl->dead_[i]);
    sl->ndead_ = 0;
}

static int sync_submit(void) {   // NX2_SYNC_SUBMIT=1, or NX2_SYNC_FROM=<present>: from that present on
    static int on = -1;
    static int64_t from = -1;
    if (on < 0) {
        on = getenv("NX2_SYNC_SUBMIT") != NULL;
        from = getenv("NX2_SYNC_FROM") ? atoll(getenv("NX2_SYNC_FROM")) : -1;
    }
    extern uint64_t nx2_vk_frame;
    return on || (from >= 0 && (int64_t)nx2_vk_frame >= from);
}

static void wait_all(void) {   // every submission done, every slot free (under lock)
    for (int i = 1; i <= NSLOT; i++) slot_retire((cur + i) % NSLOT);
}

// Submits what was recorded and goes on with the next slot, once that slot's previous submission is done.
static int since_flush;   // draws and dispatches recorded since the last submission
static VkQueryPool occ_pool;   // samples-passed counters (nx2_vk_counter_reset / _report below)
static int occ_active = -1;
static uint64_t occ_broken;
// reports waiting for their copy: done together at the end of the submission (one wait for the queries, not one each)
static struct { uint32_t q; VkBuffer buf; VkDeviceSize off; } occ_copy[1024];
static int nocc_copy;
static void occ_copy_all(void) {
    for (int i = 0; i < nocc_copy; i++)
        vkCmdCopyQueryPoolResults(cb, occ_pool, occ_copy[i].q, 1, occ_copy[i].buf, occ_copy[i].off, 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    nocc_copy = 0;
}
static void occ_note_draw(uint32_t serial, const Nx2Raster *R, int has_depth, uint32_t w, uint32_t h);
static void occ_note_skip(void);
static void occ_dump(void);
static void flush(void) {
    since_flush = 0;
    if (!recording) return;
    end_rendering();
    if (occ_active >= 0) {   // a query cannot outlive its command buffer: that count falls back to the estimate
        vkCmdEndQuery(cb, occ_pool, (uint32_t)occ_active);
        occ_active = -1;
        occ_broken++;
    }
    occ_copy_all();
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    nx2_vk_queue_lock();
    VKCHECK(vkQueueSubmit(V->queue, 1, &si, fence));
    nx2_vk_queue_unlock();
    nx2_prof_flushes++;
    slots_[cur].pending = 1;
    slots_[cur].serial = ++submit_serial;
    recording = 0;
    nssbo_back = 0;
    cur = (cur + 1) % NSLOT;
    slot_retire(cur);
    cb = slots_[cur].cb, fence = slots_[cur].fence, dpool = slots_[cur].dpool;
    stage_off = 0;
    dev_off = 0;
    VKCHECK(vkResetDescriptorPool(D, dpool, 0));
    if (sync_submit() || (gp_pool && gp_on())) wait_all();
}

// Work goes to the GPU every NX2_FLUSH_DRAWS draws and dispatches (default 200; 0: only where the game submits), so
// the GPU runs while the rest of the frame is still being replayed. With the GPU waited for at each present
// (NX2_FRAMES_AHEAD=0), a frame sent in two submissions left ~7 ms of GPU work to wait for after the last one.
static void flush_chunk(void) {
    static int every = -1;
    if (every < 0) every = getenv("NX2_FLUSH_DRAWS") ? atoi(getenv("NX2_FLUSH_DRAWS")) : 200;
    if (every > 0 && ++since_flush >= every && occ_active < 0) flush();   // not while a samples count runs
}

// flush, then wait for everything: for reading GPU results on the CPU
static void flush_wait(void) {
    flush();
    wait_all();
}

// The serial of the last submission (nvnQueueFenceSync), and waiting for one (nvnSyncWait, nvnQueueFinish: serial 0
// is everything submitted so far). 1: done, 0: timed out. Any thread; the lock is not held while waiting.
uint64_t nx2_vk_submitted_serial(void) { return __atomic_load_n(&submit_serial, __ATOMIC_ACQUIRE); }
int nx2_vk_wait_serial(uint64_t serial, uint64_t timeout_ns) {
    if (!V || disabled) return 1;
    uint64_t end = timeout_ns == UINT64_MAX ? UINT64_MAX : plat_now_ns() + timeout_ns;
    plat_mutex_lock(&lock);
    if (!serial) serial = submit_serial;
    while (done_serial < serial) {
        int oldest = -1;
        for (int k = 0; k < NSLOT; k++)
            if (slots_[k].pending && (oldest < 0 || slots_[k].serial < slots_[oldest].serial)) oldest = k;
        if (oldest < 0) break;
        VkFence f = slots_[oldest].fence;
        uint64_t sr = slots_[oldest].serial;
        plat_mutex_unlock(&lock);
        uint64_t now = plat_now_ns();
        VkResult r = vkWaitForFences(D, 1, &f, VK_TRUE, end == UINT64_MAX ? UINT64_MAX : end > now ? end - now : 0);
        plat_mutex_lock(&lock);
        if (r == VK_TIMEOUT) {
            plat_mutex_unlock(&lock);
            return 0;
        }
        if (slots_[oldest].pending && slots_[oldest].serial == sr) slot_retire(oldest);
    }
    plat_mutex_unlock(&lock);
    return 1;
}

static VkDeviceSize stage_alloc(VkDeviceSize n, VkDeviceSize align) {   // an offset in stage_buf (this slot's part)
    VkDeviceSize off = (stage_off + align - 1) & ~(align - 1);
    if (off + n > SLOT_STAGE) {
        if (n > SLOT_STAGE) rt_fatal("vulkan: staging request of %llu bytes", (unsigned long long)n);
        flush();
        begin_cb();
        off = 0;
    }
    stage_off = off + n;
    return stage_base() + off;
}

static void create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer *b,
                          VkDeviceMemory *m) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VKCHECK(vkCreateBuffer(D, &bi, NULL, b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(D, *b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               nx2_vk_memory_type(mr.memoryTypeBits, props)};
    VKCHECK(vkAllocateMemory(D, &ai, NULL, m));
    VKCHECK(vkBindBufferMemory(D, *b, *m, 0));
}

static void init(void) {
    V = nx2_vk();
    D = V->dev;
    VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                  VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V->qfamily};
    VKCHECK(vkCreateCommandPool(D, &pc, NULL, &cpool));
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool,
                                      VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int k = 0; k < NSLOT; k++) {
        VKCHECK(vkAllocateCommandBuffers(D, &ca, &slots_[k].cb));
        VKCHECK(vkCreateFence(D, &fc, NULL, &slots_[k].fence));
    }
    cb = slots_[0].cb, fence = slots_[0].fence;
    {   // staging (constants, copied buffers): video memory the CPU writes (resizable BAR) when it is large enough,
        // else system memory. Opt-in (NX2_STAGE_BAR=1): it did not lower GPU time in gameplay and the CPU writes of
        // the constants got ~10 ms a frame slower.
        VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkMemoryPropertyFlags want = hv;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(V->phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount && getenv("NX2_STAGE_BAR"); i++) {
            VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if ((f & (hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) == (hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size >= (VkDeviceSize)STAGE_SIZE * 2) {
                want = hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
                break;
            }
        }
        rt_log("vkdraw: staging in %s", want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "video memory (BAR)" : "system memory");
        create_buffer(STAGE_SIZE,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      want, &stage_buf, &stage_mem);
    }
    VKCHECK(vkMapMemory(D, stage_mem, 0, VK_WHOLE_SIZE, 0, (void **)&stage_ptr));
    if (!getenv("NX2_SSBO_HOST"))   // NX2_SSBO_HOST=1: written storage buffers stay in host memory (the old way)
        create_buffer(DEV_SIZE * NSLOT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dev_buf, &dev_mem);
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
    };
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 16384, 4, sizes};
    for (int k = 0; k < NSLOT; k++) VKCHECK(vkCreateDescriptorPool(D, &dp, NULL, &slots_[k].dpool));
    dpool = slots_[0].dpool;
    VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    VKCHECK(vkCreateSampler(D, &sc, NULL, &sampler_linear));
    sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
    sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VKCHECK(vkCreateSampler(D, &sc, NULL, &sampler_nearest));
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    props12.pNext = &hostp;
    VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &props12};
    vkGetPhysicalDeviceProperties2(V->phys, &props);
    ubo_align = props.properties.limits.minUniformBufferOffsetAlignment;
    import_align = hostp.minImportedHostPointerAlignment ? hostp.minImportedHostPointerAlignment : 4096;
    ssbo_align = props.properties.limits.minStorageBufferOffsetAlignment;
    rt_log("vkdraw: float controls: denorm flush f16 %d f32 %d, preserve f16 %d f32 %d, signed zero/inf/nan f16 %d "
           "f32 %d f64 %d", props12.shaderDenormFlushToZeroFloat16, props12.shaderDenormFlushToZeroFloat32,
           props12.shaderDenormPreserveFloat16, props12.shaderDenormPreserveFloat32,
           props12.shaderSignedZeroInfNanPreserveFloat16, props12.shaderSignedZeroInfNanPreserveFloat32,
           props12.shaderSignedZeroInfNanPreserveFloat64);
    rt_log("vkdraw: ready (staging %llu MiB)", (unsigned long long)(STAGE_SIZE >> 20));
    if (getenv("NX2_GPU_PROF")) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, GP_QUERIES};
        if (vkCreateQueryPool(D, &qi, NULL, &gp_pool) != VK_SUCCESS) gp_pool = VK_NULL_HANDLE;
        gp_period = props.properties.limits.timestampPeriod;
    }
}

static int active(void) {
    if (disabled < 0) {
        const char *e = getenv("NX2_GPU");
        disabled = e && *e == '0';
    }
    if (disabled) return 0;
    if (!V) init();
    return 1;
}

static void barrier_all(void) {   // a full memory barrier: correct, not fast
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
                         NULL, 0, NULL);
}

// ---- imported memory pools: vertex, index and storage buffers read the game's memory in place

static Imp *import_pool(uint64_t base, uint64_t size) {
    Imp **slot = &imps[(base >> 12) % IMP_BUCKETS];
    for (Imp *m = *slot; m; m = m->next)
        if (m->base == base && m->size == size) return m->failed ? NULL : m;
    Imp *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->base = base, m->size = size, m->mlist = -1;
    m->next = *slot;
    *slot = m;
    m->failed = 1;
    if (base % import_align || size % import_align || nimports > 3000) return NULL;
    if (!get_host_props) get_host_props = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(D, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!get_host_props || get_host_props(D, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                          (void *)(uintptr_t)base, &hp) != VK_SUCCESS || !hp.memoryTypeBits)
        return NULL;
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (vkCreateBuffer(D, &bi, NULL, &m->buf) != VK_SUCCESS) return NULL;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(D, m->buf, &mr);
    uint32_t bits = mr.memoryTypeBits & hp.memoryTypeBits, type = 0;
    while (type < 32 && !(bits & (1u << type))) type++;
    VkImportMemoryHostPointerInfoEXT ih = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)base};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ih, size, type};
    if (type == 32 || vkAllocateMemory(D, &ai, NULL, &m->mem) != VK_SUCCESS ||
        vkBindBufferMemory(D, m->buf, m->mem, 0) != VK_SUCCESS) {
        vkDestroyBuffer(D, m->buf, NULL);
        if (m->mem) vkFreeMemory(D, m->mem, NULL);
        m->buf = VK_NULL_HANDLE, m->mem = VK_NULL_HANDLE;
        static int nlog;
        if (nlog++ < 4) rt_log("vkdraw: pool %#llx (+%#llx) could not be imported", (unsigned long long)base,
                               (unsigned long long)size);
        return NULL;
    }
    m->failed = 0;
    nimports++, import_bytes += size;
    if (nimports <= 8 || nimports % 100 == 0)
        rt_log("vkdraw: %d memory pools imported (%.1f MiB)", nimports, import_bytes / 1048576.0);
    return m;
}

// The game's memory at addr..addr+size as a buffer range, without a copy; 0 when it cannot be (then copy).
static int buffer_at(uint64_t addr, uint64_t size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    if (import_off < 0) import_off = getenv("NX2_NO_IMPORT") != NULL;
    uint64_t base, psize;
    if (import_off || !addr || !nx2_pool_range(addr, &base, &psize) || addr + size > base + psize) return 0;
    if ((addr - base) % align) return 0;
    Imp *m = import_pool(base, psize);
    if (!m) return 0;
    *buf = m->buf, *off = addr - base;
    last_imp = m;
    return 1;
}

// ---- video-memory mirrors of the game's memory pools
// The game's vertex, index and storage data live in its memory pools, which are host memory imported into Vulkan: the
// GPU read them over PCIe (gameplay: ~200 MiB of vertex ranges a frame, 24 ms of GPU time against 8 ms from video
// memory). A pool read that way gets a device-local mirror. The game's heap is reserved write-watched (memory.c), so
// once per submit the pages the CPU wrote since the last sync are copied into the mirrors (one barrier for all);
// storage buffers the GPU writes back into a pool are copied into its mirror too (ssbo_writeback).
// NX2_NO_MIRROR=1: read the pools in place (the old way).
#define MIRROR_MAX 4096
#define MIRROR_BUDGET (4ull << 30)
#define MIRROR_IDLE 64      // submits a mirror stays in the per-submit sync after its last use
static Imp *mirrors[MIRROR_MAX];
static int nmirrors;
static uint64_t mirror_epoch = 1, mirror_bytes, mirror_synced_epoch;
static uint64_t *mpages;
static uint64_t mpages_cap;
static uint64_t mp_copied, mp_syncs, mp_regions;   // statistics for "prof:"
static void end_rendering(void);
static void barrier_all(void);

static int mirror_pages_room(uint64_t size) {
    uint64_t need = size / 4096 + 1;
    if (need <= mpages_cap) return 1;
    uint64_t *np = realloc(mpages, need * sizeof *np);
    if (!np) return 0;
    mpages = np, mpages_cap = need;
    return 1;
}

// The written pages of m since its last sync, as copy commands (the caller has ended rendering and placed barriers).
// 0 when the pool cannot be watched any more (the caller then drops the mirror).
static uint64_t page_hash(const uint64_t *w) {
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < 512; i++) h = (h ^ w[i]) * 0x100000001b3ull;
    return h;
}

static int mirror_copy_written(Imp *m) {
    if (!mirror_pages_room(m->size)) return 0;
    // NX2_MIRROR_VERIFY=1 (debug, slow): after the write-watch query every page is hashed, then the write marks are
    // read again without clearing them (writes during the hashing). A page whose contents changed since the last sync
    // without a mark in either query is logged (the mirror would have missed it).
    static int verify = -1;
    if (verify < 0) verify = getenv("NX2_MIRROR_VERIFY") != NULL;
    uint64_t npg = m->size / 4096;
    uint64_t n = mem_written_pages(m->base, m->size, mpages, mpages_cap);
    if (n == UINT64_MAX) return 0;
    uint64_t *cur = NULL, *pk = NULL, npk = 0;
    if (verify) {
        cur = malloc(npg * sizeof *cur);
        pk = malloc((npg + 1) * sizeof *pk);
        for (uint64_t i = 0; cur && i < npg; i++) cur[i] = page_hash((const uint64_t *)(uintptr_t)(m->base + i * 4096));
        npk = pk ? mem_written_peek(m->base, m->size, pk, npg + 1) : 0;
        if (npk == UINT64_MAX) npk = 0;
    }
    if (cur) {
        if (m->mhash) {
            uint64_t k = 0, missed = 0, first = 0;
            for (uint64_t i = 0; i < npg; i++) {
                uint64_t a = m->base + i * 4096;
                while (k < n && mpages[k] < a) k++;
                int dirty = k < n && mpages[k] == a;
                for (uint64_t j = 0; !dirty && j < npk; j++) dirty = pk[j] == a;
                if (!dirty && cur[i] != m->mhash[i] && !(m->mgpu && m->mgpu[i])) {
                    if (!missed) first = a;
                    missed++;
                }
            }
            static int trap = -1;
            if (trap < 0) trap = getenv("NX2_MIRROR_TRAP") != NULL;
            void nx2_mtrap_arm(uint64_t base, uint64_t size);
            if (trap && missed) nx2_mtrap_arm(m->base, m->size);   // who writes it (logged by nx2_mtrap.c)
            static int nlog;
            const char *nx2_mem_describe(uint64_t a, char *buf, int n);
            char md[160];
            if (missed && nlog < 200) {   // does a write still get marked now?
                volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)first;
                uint32_t v = *w;
                __sync_val_compare_and_swap(w, v, v);
                uint64_t got[64];
                uint64_t ng = mem_written_peek(m->base, m->size, got, 64), hit = 0;
                for (uint64_t j = 0; ng != UINT64_MAX && j < ng; j++) hit |= got[j] == first;
                rt_log("vkdraw: mirror verify: probe write at %#llx %s (%lld marks now)", (unsigned long long)first,
                       hit ? "marked" : "NOT marked", (long long)ng);
            }
            if (missed && nlog++ < 200)
                rt_log("vkdraw: mirror verify: pool %#llx (+%#llx): %llu pages changed without a write mark (first %#llx), "
                       "%llu marked; epoch %llu frame %llu; memory %s", (unsigned long long)m->base, (unsigned long long)m->size,
                       (unsigned long long)missed, (unsigned long long)first, (unsigned long long)n,
                       (unsigned long long)mirror_epoch, (unsigned long long)nx2_vk_frame,
                       nx2_mem_describe(first, md, sizeof md));
        }
        free(m->mhash);
        m->mhash = cur;
        // m->mgpu is kept: the GPU writes land after this sync's hashing (asynchronous submission)
    }
    free(pk);
    static VkBufferCopy rg[1024];
    int nr = 0;
    for (uint64_t i = 0; i < n;) {
        uint64_t a = mpages[i], e = a + 4096;
        for (i++; i < n && mpages[i] == e; i++) e += 4096;
        if (a < m->base) a = m->base;
        if (e > m->base + m->size) e = m->base + m->size;
        if (e <= a) continue;
        rg[nr++] = (VkBufferCopy){a - m->base, a - m->base, e - a};
        mp_copied += e - a;
        if (nr == 1024) vkCmdCopyBuffer(cb, m->buf, m->dbuf, (uint32_t)nr, rg), mp_regions += nr, nr = 0;
    }
    if (nr) vkCmdCopyBuffer(cb, m->buf, m->dbuf, (uint32_t)nr, rg), mp_regions += nr;
    m->msynced = mirror_epoch;
    return 1;
}

static void mirror_drop(Imp *m) {   // back to reads in place (the buffers are freed with the pool)
    m->mstate = -1;
    if (m->mlist >= 0) {
        mirrors[m->mlist] = mirrors[--nmirrors];
        mirrors[m->mlist]->mlist = m->mlist;
        m->mlist = -1;
    }
}

// Once per submit (at the first mirrored read): every mirror used lately gets the pages written since its last sync.
static void mirror_sync_all(void) {
    mirror_synced_epoch = mirror_epoch;
    int any = 0;
    for (int i = 0; i < nmirrors; i++) {
        Imp *m = mirrors[i];
        if (m->mused + MIRROR_IDLE < mirror_epoch) continue;
        if (!any) end_rendering(), barrier_all(), any = 1;
        if (!mirror_copy_written(m)) mirror_drop(m), i--;
    }
    if (any) barrier_all();
    mp_syncs++;
}

// The device-local mirror of the pool m to read instead of m->buf, or VK_NULL_HANDLE (read in place).
static VkBuffer mirror_of(Imp *m) {
    static int off = -1;
    if (off < 0) off = getenv("NX2_NO_MIRROR") != NULL;
    if (off || !m || m->failed || m->mstate < 0) return VK_NULL_HANDLE;
    if (mirror_synced_epoch != mirror_epoch) mirror_sync_all();
    if (m->mstate < 0) return VK_NULL_HANDLE;
    m->mused = mirror_epoch;
    if (m->mstate == 1) {
        if (m->msynced != mirror_epoch) {   // idle for a while: out of the per-submit sync
            end_rendering();
            barrier_all();
            if (!mirror_copy_written(m)) {
                mirror_drop(m);
                return VK_NULL_HANDLE;
            }
            barrier_all();
        }
        return m->dbuf;
    }
    // first use: a mirror when the pool is write-watched and fits the budget, and no other mirror covers its memory
    // (pools may overlap: virtual pools mapped onto physical ones; the sync of one mirror clears the write marks of the
    // shared pages, which the other would then miss)
    m->mstate = -1;
    for (int i = 0; i < nmirrors; i++) {
        const Imp *o = mirrors[i];
        if (o->base < m->base + m->size && m->base < o->base + o->size) {
            static int nlog;
            if (nlog++ < 16)
                rt_log("vkdraw: pool %#llx (+%#llx) overlaps mirrored pool %#llx (+%#llx): read in place",
                       (unsigned long long)m->base, (unsigned long long)m->size, (unsigned long long)o->base,
                       (unsigned long long)o->size);
            return VK_NULL_HANDLE;
        }
    }
    if (nmirrors == MIRROR_MAX || mirror_bytes + m->size > MIRROR_BUDGET || !mirror_pages_room(m->size) ||
        mem_written_pages(m->base, m->size, mpages, mpages_cap) == UINT64_MAX)   // also clears the old writes
        return VK_NULL_HANDLE;
    {   // the write marks must work for this memory: some pools never get any (seen in gameplay: written every frame
        // by the game's memcpy, 0 marks, so the mirror went stale and frames came out black). Test: a locked
        // compare-exchange of the first word of the first and last page with its own value is a write that changes
        // nothing; both pages must then be marked.
        uint64_t probe[2] = {m->base, m->base + m->size - 4096}, got[4];
        for (int k = 0; k < 2; k++) {
            volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)probe[k];
            uint32_t v = *w;
            __sync_val_compare_and_swap(w, v, v);
        }
        uint64_t ng = mem_written_peek(m->base, m->size, got, 4);
        int ok = ng != UINT64_MAX && ng >= 1 && got[0] == probe[0] && (probe[1] == probe[0] || (ng >= 2 && got[ng - 1] == probe[1]));
        if (!ok) {
            static int nlog;
            if (nlog++ < 16)
                rt_log("vkdraw: pool %#llx (+%#llx): writes are not tracked (probe: %lld marks): read in place",
                       (unsigned long long)m->base, (unsigned long long)m->size, (long long)ng);
            return VK_NULL_HANDLE;
        }
    }
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = m->size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(D, &bi, NULL, &m->dbuf) != VK_SUCCESS) return m->dbuf = VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(D, m->dbuf, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(D, &ai, NULL, &m->dmem) != VK_SUCCESS) {
        vkDestroyBuffer(D, m->dbuf, NULL);
        m->dbuf = VK_NULL_HANDLE, m->dmem = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    vkBindBufferMemory(D, m->dbuf, m->dmem, 0);
    end_rendering();
    barrier_all();
    VkBufferCopy cp = {0, 0, m->size};
    vkCmdCopyBuffer(cb, m->buf, m->dbuf, 1, &cp);
    barrier_all();
    mp_copied += m->size;
    mirror_bytes += m->size;
    m->mstate = 1, m->msynced = mirror_epoch;
    m->mlist = nmirrors, mirrors[nmirrors++] = m;
    static int nlog;
    if (nlog++ < 8 || !(nmirrors % 64))
        rt_log("vkdraw: pool %#llx (+%#llx) mirrored in video memory (%d mirrors, %.1f MiB)", (unsigned long long)m->base,
               (unsigned long long)m->size, nmirrors, mirror_bytes / 1048576.0);
    return m->dbuf;
}

void nx2_vk_pool_released(uint64_t base, uint64_t size) {
    if (!V) return;
    plat_mutex_lock(&lock);
    Imp **slot = &imps[(base >> 12) % IMP_BUCKETS];
    for (Imp **p = slot; *p; p = &(*p)->next)
        if ((*p)->base == base && (*p)->size == size) {
            Imp *m = *p;
            *p = m->next;
            if (m->mlist >= 0) mirror_drop(m);
            if (m->dbuf) mirror_bytes -= m->size;
            if (!m->failed) {
                if (ndead_imps < 256) {   // still referenced by submitted or open work: freed with this slot
                    dead_imps[ndead_imps++] = m;
                } else {
                    flush_wait();
                    if (m->dbuf) vkDestroyBuffer(D, m->dbuf, NULL);
                    if (m->dmem) vkFreeMemory(D, m->dmem, NULL);
                    vkDestroyBuffer(D, m->buf, NULL);
                    vkFreeMemory(D, m->mem, NULL);
                    free(m);
                }
                nimports--, import_bytes -= size;
            } else {
                free(m);
            }
            break;
        }
    plat_mutex_unlock(&lock);
}

// ---- images
struct Img {
    uint64_t obj;
    nvn_texdesc d;
    uint8_t *storage;
    Nx2VkFormat f;
    VkFormat fmt;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view, att_view;
    Img *enc;        // block-compressed image rendered through a block-sized view: the uncompressed image drawn into
    int enc_level;
    // attachment views of render target views: another format of the class, one layer (3D: slice), one level
    struct AttSel { VkImageView view; VkFormat fmt; uint16_t layer, level; } *att_sel;
    int natt_sel;
    VkImageView typed[7];   // sampled views by VkImageViewType, made when a shader reads another type
    struct { uint64_t key; VkImageView view; } vcache[16];   // views of registered NVN texture views
    VkImageView sview[7];    // storage views by VkImageViewType (identity swizzle, UNORM for sRGB formats)
    VkComponentMapping cm;
    VkImageViewType view_type;
    VkImageType type;
    VkImageCreateFlags flags;
    VkImageUsageFlags usage;
    VkImageLayout layout;
    VkImageAspectFlags aspect, all_aspect;   // sampled aspect; every aspect (barriers, clears, attachments)
    uint32_t layers, levels, w, h, depth;
    int gpu, uploaded, failed;
    volatile int dirty;          // WriteTexels since the last upload
    uint64_t size, hash, hash_frame;   // CPU storage: contents hash, checked once a frame
    int frozen_ok;                     // unused (kept for the slices below)
    uint64_t *slice_hash;              // large storage: a hash per 2 MiB region it covers (tex_changed)
    uint8_t *slice_frozen;             // that region was frozen when its hash was taken
    int nslices;
    Img *next;
};
#define IMG_BUCKETS 4096
static Img *imgs[IMG_BUCKETS];
static uint64_t img_count, img_bytes;

// An image the GPU wrote: its CPU storage is stale from then on (no more uploads). 3D and layered images are logged
// with the place that marked them (the first few), to follow look-up tables and arrays the game renders.
extern uint64_t nx2_vk_frame;
#define mark_gpu(img) mark_gpu_at((img), __LINE__)
static void mark_gpu_at(Img *m, int line) {
    if (!m->gpu && (m->depth > 1 || m->layers > 1)) {
        static int n;
        if (n++ < 24)
            rt_log("vkdraw: %ux%ux%u (layers %u, format %#x) image %#llx written by the GPU (vkdraw line %d, frame %llu)",
                   m->w, m->h, m->depth, m->layers, m->d.format, (unsigned long long)m->obj, line,
                   (unsigned long long)nx2_vk_frame);
    }
    m->gpu = 1;
    static int rtrap = -1;   // NX2_READ_TRAP=1 (debug, nx2_mtrap.c): who reads what the GPU wrote, from memory
    if (rtrap < 0) rtrap = getenv("NX2_READ_TRAP") && *getenv("NX2_READ_TRAP") == '1';
    if (rtrap && m->storage && m->size && m->size <= (4u << 20) && nx2_vk_frame > 600) {
        void nx2_rtrap_arm(uint64_t base, uint64_t size, const char *what);
        char w[96];
        snprintf(w, sizeof w, "image %#llx %ux%ux%u fmt %#x", (unsigned long long)m->obj, m->w, m->h, m->depth, m->d.format);
        nx2_rtrap_arm((uint64_t)(uintptr_t)m->storage, m->size, w);
    }
}

static void destroy_img(Img *m) {
    for (int i = 0; i < 16; i++)
        if (m->vcache[i].view) vkDestroyImageView(D, m->vcache[i].view, NULL);
    for (int i = 0; i < m->natt_sel; i++)
        if (m->att_sel[i].view) vkDestroyImageView(D, m->att_sel[i].view, NULL);
    free(m->att_sel);
    for (int i = 0; i < 7; i++) {
        if (m->typed[i]) vkDestroyImageView(D, m->typed[i], NULL);
        if (m->sview[i]) vkDestroyImageView(D, m->sview[i], NULL);
    }
    if (m->view) vkDestroyImageView(D, m->view, NULL);
    if (m->att_view) vkDestroyImageView(D, m->att_view, NULL);
    free(m->slice_hash), free(m->slice_frozen);
    if (m->enc) destroy_img(m->enc);
    if (m->img) vkDestroyImage(D, m->img, NULL);
    if (m->mem) vkFreeMemory(D, m->mem, NULL);
    free(m);
}

static void img_layout(Img *m, VkImageLayout to) {
    if (m->layout == to) return;
    end_rendering();
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, m->layout, to,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, m->img,
                              {m->all_aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
    m->layout = to;
}

static VkComponentSwizzle swz(unsigned nvn) {   // NVN_TEXTURE_SWIZZLE_ZERO 0, ONE 1, R 2, G 3, B 4, A 5
    static const VkComponentSwizzle t[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                            VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                            VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    return nvn < 6 ? t[nvn] : VK_COMPONENT_SWIZZLE_IDENTITY;
}

static VkFormat srgb_to_unorm(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}

static Img *create_img(uint64_t obj, const nvn_texdesc *d, uint8_t *storage) {
    Img *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("vkdraw: image record");
    m->obj = obj, m->d = *d, m->storage = storage;
    if (obj) {
        nvn_texdesc dd;
        uint8_t *st;
        nx2_tex_get(obj, &dd, &st, &m->size);
    }
    if (!nx2_vk_format(d->format, &m->f) || m->f.expand_rgb) {
        static int n;
        if (n++ < 16) rt_log("vkdraw: texture %#llx format %#x not supported yet", (unsigned long long)obj, d->format);
        m->failed = 1;
        return m;
    }
    m->fmt = m->f.vk;
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    m->levels = d->levels ? d->levels : 1;
    m->layers = L.layers ? L.layers : 1;
    m->w = d->width ? d->width : 1, m->h = d->height ? d->height : 1, m->depth = 1;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageCreateFlags flags = 0;
    switch (d->target) {
    case 0: type = VK_IMAGE_TYPE_1D, m->view_type = VK_IMAGE_VIEW_TYPE_1D, m->h = 1; break;
    case 3: type = VK_IMAGE_TYPE_1D, m->view_type = VK_IMAGE_VIEW_TYPE_1D_ARRAY, m->h = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D, m->view_type = VK_IMAGE_VIEW_TYPE_3D, m->depth = d->depth ? d->depth : 1,
            m->layers = 1; break;
    case 4: case 6: m->view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case 8: m->view_type = VK_IMAGE_VIEW_TYPE_CUBE, flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    case 9: m->view_type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    default: m->view_type = m->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; break;
    }
    if (type == VK_IMAGE_TYPE_2D && m->layers >= 6 && !(m->layers % 6) && m->w == m->h)
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;   // NVN views may read 2D arrays as cubes
    if ((flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && (m->layers % 6 || m->w != m->h)) {
        rt_log("vkdraw: cube texture %#llx %ux%u with %u layers made a 2D array", (unsigned long long)obj, m->w, m->h,
               m->layers);
        flags = 0;
        m->view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    m->aspect = m->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (m->f.stencil && !m->f.depth) m->aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    m->all_aspect = m->aspect | (m->f.depth && m->f.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(V->phys, m->fmt, &fp);
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    {   // colour targets may be written through views of another format of their class (sRGB <-> UNORM)
        int bw = 1, bh = 1;
        nvn_fmt_bpb(d->format, &bw, &bh);
        if (bw == 1 && !m->f.depth && !m->f.stencil) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    VkFormat unorm = srgb_to_unorm(m->fmt);
    if (unorm != m->fmt) {   // sRGB images are written as UNORM by compute shaders: a mutable format
        VkFormatProperties up;
        vkGetPhysicalDeviceFormatProperties(V->phys, unorm, &up);
        if (up.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) {
            usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        }
    }
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;   // its slices are rendered to through 2D views
    m->usage = usage;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.flags = flags;
    ii.imageType = type;
    ii.format = m->fmt;
    ii.extent = (VkExtent3D){m->w, m->h, m->depth};
    ii.mipLevels = m->levels;
    ii.arrayLayers = m->layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (vkCreateImage(D, &ii, NULL, &m->img) != VK_SUCCESS) {
        rt_log("vkdraw: texture %#llx (%ux%ux%u format %#x target %d) could not be created",
               (unsigned long long)obj, m->w, m->h, m->depth, d->format, d->target);
        m->failed = 1;
        return m;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(D, m->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKCHECK(vkAllocateMemory(D, &ai, NULL, &m->mem));
    VKCHECK(vkBindImageMemory(D, m->img, m->mem, 0));
    img_count++, img_bytes += mr.size;
    VkComponentMapping cm = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    // the description's swizzle is not baked in: one image serves textures that differ only there, and each binding
    // applies its texture's (or view's) swizzle in its own view (bind_resources, img_view_nvn)
    if (m->f.alpha_one) cm.a = VK_COMPONENT_SWIZZLE_ONE;
    m->cm = cm, m->type = type, m->flags = flags;
    static int nlog;
    if (nlog++ < 16)
        rt_log("vkdraw: image %#llx %ux%ux%u fmt %#x target %d levels %u layers %u, swizzle %u %u %u %u",
               (unsigned long long)obj, m->w, m->h, m->depth, d->format, d->target, m->levels, m->layers,
               d->swizzle_r, d->swizzle_g, d->swizzle_b, d->swizzle_a);
    // views in the image's own format must not claim STORAGE when only the UNORM alias can be a storage image
    VkImageViewUsageCreateInfo vuse = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
                                       usage & ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, srgb_to_unorm(m->fmt) != m->fmt ? &vuse : NULL};
    vi.image = m->img;
    vi.viewType = m->view_type;
    vi.format = m->fmt;
    vi.components = m->f.depth ? (VkComponentMapping){0} : cm;
    vi.subresourceRange = (VkImageSubresourceRange){m->aspect, 0, m->levels, 0, m->layers};
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) VKCHECK(vkCreateImageView(D, &vi, NULL, &m->view));
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;   // 3D: slice 0 (the image is 2D-array compatible)
        vi.components = (VkComponentMapping){0};
        vi.subresourceRange = (VkImageSubresourceRange){m->all_aspect, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(D, &vi, NULL, &m->att_view));
    }
    m->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return m;
}

extern uint64_t nx2_vk_frame;
// Images are keyed by their memory and description, not by the NVN texture object: the game's transient render
// targets re-initialize the same objects every frame with other sizes and formats, and several objects describe
// the same memory. Each (memory, description) keeps its own image; up to 16 per address, least recently used
// retired first. The bucket is the storage address.
#define IMG_PER_ADDR 16
static inline unsigned img_bucket(const uint8_t *storage, uint64_t obj) {
    return storage ? (unsigned)(((uintptr_t)storage >> 9) % IMG_BUCKETS) : (unsigned)((obj >> 3) % IMG_BUCKETS);
}

// Same image: the description apart from the component swizzle, which belongs to views (a texture drawn into
// and the texture sampled may differ only there: the labels the game compresses on the GPU do).
static int desc_same(const nvn_texdesc *a, const nvn_texdesc *b) {
    nvn_texdesc x = *a, y = *b;
    x.swizzle_r = x.swizzle_g = x.swizzle_b = x.swizzle_a = 0;
    y.swizzle_r = y.swizzle_g = y.swizzle_b = y.swizzle_a = 0;
    return !memcmp(&x, &y, sizeof x);
}

static Img *img_find(uint64_t obj, int create) {
    nvn_texdesc d;
    uint8_t *storage;
    uint64_t size;
    if (!obj || !nx2_tex_get(obj, &d, &storage, &size)) return NULL;
    Img **slot = &imgs[img_bucket(storage, obj)];
    int variants = 0;
    Img **oldest = NULL;
    for (Img **p = slot; *p; p = &(*p)->next) {
        Img *m = *p;
        if (m->storage != storage || (!storage && m->obj != obj)) continue;
        if (desc_same(&m->d, &d)) {
            if (p != slot) {   // most recently used first
                *p = m->next;
                m->next = *slot;
                *slot = m;
            }
            m->obj = obj;
            return m;
        }
        variants++;
        oldest = p;   // the last match is the least recently used
    }
    if (!create) return NULL;
    if (variants >= IMG_PER_ADDR && oldest) {
        Img *m = *oldest;
        static int nev;
        if (nev++ < 32)
            rt_log("vkdraw: memory %p: image %ux%u fmt %#x retired (frame %llu)", (void *)storage, m->w, m->h,
                   m->d.format, (unsigned long long)nx2_vk_frame);
        *oldest = m->next;
        if (ndead < (int)(sizeof dead / sizeof *dead)) dead[ndead++] = m;
        else destroy_img(m);   // only when the GPU is idle enough: flush first
    }
    Img *m = create_img(obj, &d, storage);
    m->next = *slot;
    *slot = m;
    return m;
}

static Img *img_get(uint64_t obj) { return img_find(obj, 1); }

uint64_t nx2_vk_frame;

// NX2_DUMP_TEX=<object, hex>: level 0 of that BC4 texture, as deswizzled, written to tex_<object>.png (grey)
static void dump_bc4(const Img *m, const uint8_t *blocks, uint32_t w, uint32_t h) {
    uint8_t *rgba = calloc((size_t)w * h, 4);
    if (!rgba) return;
    uint32_t bx = (w + 3) / 4;
    for (uint32_t by = 0; by < (h + 3) / 4; by++)
        for (uint32_t x = 0; x < bx; x++) {
            const uint8_t *b = blocks + 8 * ((size_t)by * bx + x);
            uint8_t pal[8] = {b[0], b[1]};
            if (b[0] > b[1]) for (int i = 1; i < 7; i++) pal[i + 1] = (uint8_t)(((7 - i) * b[0] + i * b[1]) / 7);
            else {
                for (int i = 1; i < 5; i++) pal[i + 1] = (uint8_t)(((5 - i) * b[0] + i * b[1]) / 5);
                pal[6] = 0, pal[7] = 255;
            }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int t = 0; t < 16; t++) {
                uint32_t px = x * 4 + t % 4, py = by * 4 + t / 4;
                if (px >= w || py >= h) continue;
                uint8_t v = pal[bits >> (3 * t) & 7];
                uint8_t *o = rgba + ((size_t)py * w + px) * 4;
                o[0] = o[1] = o[2] = v, o[3] = 255;
            }
        }
    char path[64];
    snprintf(path, sizeof path, "tex_%llx.png", (unsigned long long)m->obj);
    nx2_write_png(path, rgba, w, h, 1);
    rt_log("vkdraw: texture %#llx (%ux%u, levels %u, flags %#x, block height %d) written to %s",
           (unsigned long long)m->obj, w, h, m->levels, m->d.flags, m->d.pkg_block_h, path);
    free(rgba);
}

static uint64_t storage_hash(const uint8_t *p, uint64_t n) {
    const uint64_t *w = (const uint64_t *)p;
    // up to 1 MiB every word; larger storage one word in four (a glyph written into a 20 MiB cache still changes
    // several of the sampled words: each of its rows spans more than 32 bytes). Four independent lanes: the CPU
    // works on them in parallel (one serial chain was 6.5% of the render thread in gameplay).
    uint64_t step = n <= (1u << 20) ? 1 : 4, words = n / 8, i = 0;
    uint64_t h0 = 0x9e3779b97f4a7c15ull ^ n, h1 = 0xc2b2ae3d27d4eb4full, h2 = 0x165667b19e3779f9ull, h3 = 0x27d4eb2f165667c5ull;
    for (; i + 3 * step < words; i += 4 * step) {
        h0 = (h0 ^ w[i]) * 0x100000001b3ull + (h0 >> 29);
        h1 = (h1 ^ w[i + step]) * 0x100000001b3ull + (h1 >> 29);
        h2 = (h2 ^ w[i + 2 * step]) * 0x100000001b3ull + (h2 >> 29);
        h3 = (h3 ^ w[i + 3 * step]) * 0x100000001b3ull + (h3 >> 29);
    }
    for (; i < words; i += step) h0 = (h0 ^ w[i]) * 0x100000001b3ull + (h0 >> 29);
    return h0 ^ (h1 * 3) ^ (h2 * 5) ^ (h3 * 7);
}

// Large CPU-written textures (the 20 MiB glyph cache) are not hashed every frame while unchanged: their storage's
// 2 MiB regions are frozen read-only (memory.c mem_freeze; the game's first write thaws a region, file reads thaw
// first), and a texture whose regions are all still frozen at the next check did not change. Freezing comes before
// the hash or the upload, so a write after it is seen at the next check. NX2_TEX_FREEZE=0: hash every frame.
// 28 MiB a frame were hashed in gameplay (6% of the render thread).
#define SLICE (2ull << 20)
static int tex_freeze_on(void) {
    static int on = -1;
    if (on < 0) on = !getenv("NX2_TEX_FREEZE") || strcmp(getenv("NX2_TEX_FREEZE"), "0");
    return on;
}
// Large storage, region by region: a region frozen since its hash was taken is unchanged; the others are frozen again
// (before hashing: a later write thaws them) and hashed. A region that cannot be frozen (a file read nearby keeps it
// thawed) is hashed every time, alone. 1: something changed (or the first time).
static int tex_changed(Img *m) {
    uint64_t start = (uint64_t)(uintptr_t)m->storage, end = start + m->size, r0 = start & ~(SLICE - 1);
    int n = (int)((end - r0 + SLICE - 1) / SLICE), changed = 0, fresh = 0;
    if (m->nslices != n) {
        free(m->slice_hash), free(m->slice_frozen);
        m->slice_hash = calloc((size_t)n, sizeof *m->slice_hash);
        m->slice_frozen = calloc((size_t)n, 1);
        if (!m->slice_hash || !m->slice_frozen) {
            m->nslices = 0;
            return 1;
        }
        m->nslices = n, fresh = 1;
    }
    for (int i = 0; i < n; i++) {
        uint64_t r = r0 + (uint64_t)i * SLICE, lo = r > start ? r : start, hi = r + SLICE < end ? r + SLICE : end;
        if (!fresh && m->slice_frozen[i] && mem_frozen(r)) {
            th_skipped++;
            continue;
        }
        m->slice_frozen[i] = (uint8_t)mem_freeze(r);
        uint64_t h = storage_hash((const uint8_t *)(uintptr_t)lo, hi - lo);
        th_n++, th_bytes += hi - lo;
        if (fresh || h != m->slice_hash[i]) changed = 1, m->slice_hash[i] = h;
    }
    return changed || fresh;
}

// CPU contents -> image (textures the GPU writes are never uploaded). Uploaded again when the game wrote texels
// (WriteTexels) or, for textures up to 64 MiB, when the storage changed (glyph caches and other CPU-filled textures
// are written in place); the contents are hashed at most once a frame.
static void img_upload(Img *m) {
    if (m->failed || m->gpu || !m->storage) return;
    if (m->uploaded) {
        static int off = -1;
        if (off < 0) off = getenv("NX2_NO_REUPLOAD") != NULL;
        if (off) return;
        int changed = m->dirty;
        int bw = 1, bh = 1;
        nvn_fmt_bpb(m->d.format, &bw, &bh);
        // compressed (BC / ASTC) and depth textures are loaded once; the others are checked once a frame (the game
        // adds glyphs to its font cache and draws labels with them in the same frame; every 16 frames lost letters)
        static int every = -1;
        if (every < 0) every = getenv("NX2_TEX_CHECK_EVERY") ? atoi(getenv("NX2_TEX_CHECK_EVERY")) : 1;
        int checkable = bw == 1 && !m->f.depth && m->size && m->size <= (64u << 20);
        if (!changed && checkable && nx2_vk_frame >= m->hash_frame + (uint64_t)every && m->size >= SLICE / 2 &&
            tex_freeze_on()) {
            m->hash_frame = nx2_vk_frame;   // large: region by region (tex_changed)
            changed = tex_changed(m);
        } else if (!changed && checkable && nx2_vk_frame >= m->hash_frame + (uint64_t)every) {
            m->hash_frame = nx2_vk_frame;
            th_n++, th_bytes += m->size;
            if (m->size > th_big) th_big = m->size, th_big_w = m->w, th_big_h = m->h, th_big_f = m->d.format;
            uint64_t h = storage_hash(m->storage, m->size);
            changed = h != m->hash;
            m->hash = h;
        }
        if (!changed) return;
    } else if (m->size && m->size <= (64u << 20)) {
        if (m->size >= SLICE / 2 && tex_freeze_on()) tex_changed(m);   // the slices' first hashes (and freezes)
        else m->hash = storage_hash(m->storage, m->size);
        m->hash_frame = nx2_vk_frame;
    }
    m->dirty = 0;
    m->uploaded = 1;
    begin_cb();
    end_rendering();
    img_layout(m, VK_IMAGE_LAYOUT_GENERAL);
    int bw, bh, bpb = nvn_fmt_bpb(m->d.format, &bw, &bh);
    static uint8_t *blocks;   // ASTC: raw blocks, decoded to RGBA8 into the staging ring
    static size_t blocks_n;
    for (uint32_t level = 0; level < m->levels; level++) {
        uint32_t w = m->w >> level ? m->w >> level : 1, h = m->h >> level ? m->h >> level : 1;
        uint32_t z = m->depth >> level ? m->depth >> level : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        size_t bytes = pitch * rows * z;
        for (uint32_t layer = 0; layer < m->layers; layer++) {
            if (m->f.astc_bw) {
                if (blocks_n < bytes) free(blocks), blocks = malloc(bytes), blocks_n = bytes;
                VkDeviceSize off = stage_alloc((VkDeviceSize)w * h * 4 * z, 16);
                if (!blocks || nvn_deswizzle(&m->d, level, layer, m->storage, blocks, pitch)) continue;
                for (uint32_t sl = 0; sl < z; sl++)
                    astc_decode_ldr(blocks + sl * pitch * rows, w, h, m->f.astc_bw, m->f.astc_bh, m->f.astc_srgb,
                                    stage_ptr + off + (size_t)sl * w * h * 4);
                VkBufferImageCopy cp = {off, 0, 0, {m->aspect, level, layer, 1}, {0, 0, 0}, {w, h, z}};
                vkCmdCopyBufferToImage(cb, stage_buf, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
                continue;
            }
            VkDeviceSize off = stage_alloc(bytes, 16);
            int deswz = nvn_deswizzle(&m->d, level, layer, m->storage, stage_ptr + off, pitch);
            static const char *dt = (const char *)1;
            if (dt == (const char *)1) dt = getenv("NX2_DUMP_TEX");
            static int ndumped;
            if (!deswz && dt && !level && !layer && m->d.format == 0x49 && ndumped < 40 &&
                (!strcmp(dt, "all") || strtoull(dt, NULL, 16) == (m->obj & 0xfffffffffull)))
            {
                ndumped++, dump_bc4(m, stage_ptr + off, w, h);
                char rp[64];   // the raw storage too, for offline layout experiments
                snprintf(rp, sizeof rp, "tex_%llx.raw", (unsigned long long)m->obj);
                FILE *f = fopen(rp, "wb");
                if (f) fwrite(m->storage, 1, (size_t)m->size, f), fclose(f);
            }
            if (deswz) {
                static int n;
                if (n++ < 8)
                    rt_log("vkdraw: texture %#llx level %u layer %u not converted", (unsigned long long)m->obj, level,
                           layer);
                continue;
            }
            VkBufferImageCopy cp = {off, 0, 0, {m->aspect, level, layer, 1}, {0, 0, 0}, {w, h, z}};
            vkCmdCopyBufferToImage(cb, stage_buf, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        }
    }
}

// A sampled view of the type a shader declares (NX2X_T_*); VK_NULL_HANDLE when the image cannot be seen that way.
static VkImageView img_view(Img *m, int shader_type) {
    VkImageViewType t;
    switch (shader_type) {
    case NX2X_T_1D: t = VK_IMAGE_VIEW_TYPE_1D; break;
    case NX2X_T_ARRAY_1D: t = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
    case NX2X_T_ARRAY_2D: t = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case NX2X_T_3D: t = VK_IMAGE_VIEW_TYPE_3D; break;
    case NX2X_T_CUBE: t = VK_IMAGE_VIEW_TYPE_CUBE; break;
    case NX2X_T_ARRAY_CUBE: t = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; break;
    default: t = VK_IMAGE_VIEW_TYPE_2D; break;
    }
    if (t == m->view_type) return m->view;
    if (m->typed[t]) return m->typed[t];
    int one_d = m->type == VK_IMAGE_TYPE_1D, three_d = m->type == VK_IMAGE_TYPE_3D;
    int cube = t == VK_IMAGE_VIEW_TYPE_CUBE || t == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    if ((t == VK_IMAGE_VIEW_TYPE_1D || t == VK_IMAGE_VIEW_TYPE_1D_ARRAY) != one_d) return VK_NULL_HANDLE;
    if ((t == VK_IMAGE_VIEW_TYPE_3D) != three_d) return VK_NULL_HANDLE;
    if (cube && !(m->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) return VK_NULL_HANDLE;
    uint32_t layers = t == VK_IMAGE_VIEW_TYPE_2D || t == VK_IMAGE_VIEW_TYPE_1D || t == VK_IMAGE_VIEW_TYPE_3D ? 1
                      : t == VK_IMAGE_VIEW_TYPE_CUBE ? 6
                      : t == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? m->layers / 6 * 6 : m->layers;
    VkImageViewUsageCreateInfo vuse = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vuse};
    vi.image = m->img;
    vi.viewType = t;
    vi.format = m->fmt;
    vi.components = m->f.depth ? (VkComponentMapping){0} : m->cm;
    vi.subresourceRange = (VkImageSubresourceRange){m->aspect, 0, m->levels, 0, layers};
    if (vkCreateImageView(D, &vi, NULL, &m->typed[t]) != VK_SUCCESS) m->typed[t] = VK_NULL_HANDLE;
    return m->typed[t];
}

static VkImageView img_storage_view(Img *m, int shader_type) {
    if (!m || m->failed || !(m->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_NULL_HANDLE;
    VkImageViewType t;
    switch (shader_type) {
    case NX2X_T_1D: t = VK_IMAGE_VIEW_TYPE_1D; break;
    case NX2X_T_ARRAY_1D: t = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
    case NX2X_T_ARRAY_2D: case NX2X_T_CUBE: case NX2X_T_ARRAY_CUBE: t = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case NX2X_T_3D: t = VK_IMAGE_VIEW_TYPE_3D; break;
    default: t = VK_IMAGE_VIEW_TYPE_2D; break;
    }
    if (m->sview[t]) return m->sview[t];
    if ((t == VK_IMAGE_VIEW_TYPE_3D) != (m->type == VK_IMAGE_TYPE_3D)) return VK_NULL_HANDLE;
    if ((t == VK_IMAGE_VIEW_TYPE_1D || t == VK_IMAGE_VIEW_TYPE_1D_ARRAY) != (m->type == VK_IMAGE_TYPE_1D))
        return VK_NULL_HANDLE;
    uint32_t layers = t == VK_IMAGE_VIEW_TYPE_2D_ARRAY || t == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? m->layers : 1;
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_STORAGE_BIT};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vu};
    vi.image = m->img;
    vi.viewType = t;
    vi.format = srgb_to_unorm(m->fmt);
    vi.subresourceRange = (VkImageSubresourceRange){m->aspect, 0, 1, 0, layers};
    if (vkCreateImageView(D, &vi, NULL, &m->sview[t]) != VK_SUCCESS) m->sview[t] = VK_NULL_HANDLE;
    return m->sview[t];
}

// A sampled view with an NVN texture view's swizzle and mip range applied (falls back to img_view).
static VkImageView img_view_nvn(Img *m, int shader_type, const Nx2TexView *v) {
    static int off = -1;
    if (off < 0) off = getenv("NX2_NO_VIEWS") != NULL;
    if (off || !v || (!v->has_swizzle && !v->has_levels)) return img_view(m, shader_type);
    VkImageView base = img_view(m, shader_type);   // checks that the type is possible
    if (!base) return VK_NULL_HANDLE;
    uint32_t lv0 = v->has_levels && (uint32_t)v->base_level < m->levels ? (uint32_t)v->base_level : 0;
    uint32_t nlv = v->has_levels && v->levels > 0 ? (uint32_t)v->levels : m->levels - lv0;
    if (lv0 + nlv > m->levels) nlv = m->levels - lv0;
    uint32_t sw = v->has_swizzle ? (uint32_t)v->swizzle[0] | v->swizzle[1] << 4 | v->swizzle[2] << 8 | v->swizzle[3] << 12
                                 : 0xffff;
    if (sw == (2u | 3u << 4 | 4u << 8 | 5u << 12) && lv0 == 0 && nlv == m->levels) return base;   // identity
    uint64_t key = (uint64_t)sw | (uint64_t)shader_type << 16 | (uint64_t)lv0 << 24 | (uint64_t)nlv << 32;
    for (int i = 0; i < 16; i++)
        if (m->vcache[i].view && m->vcache[i].key == key) return m->vcache[i].view;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, VK_IMAGE_USAGE_SAMPLED_BIT};
    vi.pNext = &vu;
    vi.image = m->img;
    // the view type img_view chose for this shader type
    vi.viewType = base == m->view ? m->view_type : VK_IMAGE_VIEW_TYPE_2D;
    for (int t = 0; t < 7; t++)
        if (m->typed[t] == base) vi.viewType = (VkImageViewType)t;
    vi.format = m->fmt;
    vi.components = v->has_swizzle ? (VkComponentMapping){swz(v->swizzle[0]), swz(v->swizzle[1]), swz(v->swizzle[2]),
                                                          swz(v->swizzle[3])}
                                   : m->cm;
    if (m->f.depth) vi.components = (VkComponentMapping){0};
    uint32_t layers = vi.viewType == VK_IMAGE_VIEW_TYPE_2D || vi.viewType == VK_IMAGE_VIEW_TYPE_1D ||
                              vi.viewType == VK_IMAGE_VIEW_TYPE_3D ? 1
                      : vi.viewType == VK_IMAGE_VIEW_TYPE_CUBE ? 6
                      : vi.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? m->layers / 6 * 6 : m->layers;
    vi.subresourceRange = (VkImageSubresourceRange){m->aspect, lv0, nlv, 0, layers};
    int slot = 0;
    while (slot < 16 && m->vcache[slot].view) slot++;
    if (slot == 16) return base;
    if (vkCreateImageView(D, &vi, NULL, &m->vcache[slot].view) != VK_SUCCESS) return base;
    m->vcache[slot].key = key;
    return m->vcache[slot].view;
}

// ---- samplers: one VkSampler per distinct NVN sampler state
static struct { Nx2Sampler key; VkSampler s; } samplers[512];
static int nsamplers;

static VkSamplerAddressMode vk_wrap(int w) {
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkSampler sampler_for(const Nx2Sampler *k, int integer) {
    if (!k || !k->valid) return integer ? sampler_nearest : sampler_linear;
    Nx2Sampler key = *k;
    if (integer) key.min = 0, key.mag = 0;   // integer formats cannot be filtered
    for (int i = 0; i < nsamplers; i++)
        if (!memcmp(&samplers[i].key, &key, sizeof key)) return samplers[i].s;
    VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = key.mag == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.minFilter = key.min == 1 || key.min == 3 || key.min == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sc.mipmapMode = key.min == 4 || key.min == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.addressModeU = vk_wrap(key.wrap[0]);
    sc.addressModeV = vk_wrap(key.wrap[1]);
    sc.addressModeW = vk_wrap(key.wrap[2]);
    sc.mipLodBias = key.lod_bias;
    sc.minLod = key.lod_min;
    sc.maxLod = key.min <= 1 ? 0.25f : key.lod_max;   // filters without mipmaps read level 0 only
    if (sc.maxLod < sc.minLod) sc.maxLod = sc.minLod;
    if (key.aniso > 1.0f && !integer) sc.anisotropyEnable = VK_TRUE, sc.maxAnisotropy = key.aniso > 16 ? 16 : key.aniso;
    if (key.compare_mode == 1 && key.compare_func >= 1 && key.compare_func <= 8)
        sc.compareEnable = VK_TRUE, sc.compareOp = (VkCompareOp)(key.compare_func - 1);
    int white = key.border[0] >= 1 && key.border[1] >= 1 && key.border[2] >= 1;
    sc.borderColor = white ? (key.border[3] >= 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK)
                     : key.border[3] >= 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s;
    if (nsamplers < 48)
        rt_log("vkdraw: sampler %d: min %d mag %d wrap %d %d %d lod %g..%g bias %g aniso %g compare %d/%d", nsamplers,
               key.min, key.mag, key.wrap[0], key.wrap[1], key.wrap[2], key.lod_min, key.lod_max, key.lod_bias,
               key.aniso, key.compare_mode, key.compare_func);
    if (nsamplers == 512 || vkCreateSampler(D, &sc, NULL, &s) != VK_SUCCESS) return sampler_linear;
    samplers[nsamplers].key = key, samplers[nsamplers].s = s, nsamplers++;
    return s;
}

// ---- dummies for unbound textures: a 1x1 2D image with 6 layers (2D, arrays, cubes), a 1D and a 3D one
static Img *dummies[3];
static Img *dummy_of(int shader_type) {
    int k = shader_type == NX2X_T_1D || shader_type == NX2X_T_ARRAY_1D ? 1 : shader_type == NX2X_T_3D ? 2 : 0;
    if (dummies[k]) return dummies[k];
    nvn_texdesc d = {0};
    d.format = 0x25, d.width = d.height = d.depth = d.levels = 1;
    d.target = k == 1 ? 3 : k == 2 ? 2 : 9;
    if (k == 0) d.depth = 6;
    Img *m = create_img(0, &d, NULL);
    begin_cb();
    img_layout(m, VK_IMAGE_LAYOUT_GENERAL);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    end_rendering();
    vkCmdClearColorImage(cb, m->img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    mark_gpu(m);
    dummies[k] = m;
    return m;
}
static Img *dummy(void) { return dummy_of(NX2X_T_2D); }

static void dump_images(void);

// ---- shader stages: Shader::Stage <-> NVN stage (vertex 0, fragment 1, geometry 2, tess control 3, tess eval 4,
// compute 5)
static int nvn_stage_of(int shader_stage) {
    static const int t[6] = {0, 3, 4, 2, 1, 5};
    return shader_stage >= 0 && shader_stage < 6 ? t[shader_stage] : 0;
}

static int target_type(int target) {   // NVNtextureTarget -> NX2X_T_*
    switch (target) {
    case 0: return NX2X_T_1D;
    case 1: case 5: return NX2X_T_2D;
    case 2: return NX2X_T_3D;
    case 3: return NX2X_T_ARRAY_1D;
    case 4: case 6: return NX2X_T_ARRAY_2D;
    case 7: return NX2X_T_2D_RECT;
    case 8: return NX2X_T_CUBE;
    case 9: return NX2X_T_ARRAY_CUBE;
    case 10: return NX2X_T_BUFFER;
    default: return NX2X_T_2D;
    }
}

static uint64_t tex_handle(int nvn_stage, uint32_t cbuf_offset) {
    if (cbuf_offset < 0x20) return 0;
    uint32_t slot = (cbuf_offset - 0x20) / 8;
    return slot < NX2_MAX_TEX ? nx2_state.tex[nvn_stage][slot] : 0;
}

static int handle_texture(uint64_t handle, nvn_texdesc *d) {
    uint64_t tex, smp;
    uint8_t *storage;
    uint64_t size;
    return handle && nx2_resolve_texture(handle, &tex, &smp) && nx2_tex_get(tex, d, &storage, &size);
}

// The program's constant data per shader stage (c1), for the translator and the draws.
typedef struct { const uint8_t *data[6]; uint32_t size[6]; } ConstData;

static void const_data(const Nx2Program *p, ConstData *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < p->count; i++) {
        if (!p->code[i] || p->code_size[i] < 0x90) continue;
        uint32_t w = *(const uint32_t *)(uintptr_t)p->code[i];
        int type = (w >> 10) & 0xf, graphics = ((w & 0x1f) == 1 || (w & 0x1f) == 2) && type >= 1 && type <= 5;
        int st = graphics ? type - 1 : 5;   // Shader::Stage
        c->data[st] = (const uint8_t *)(uintptr_t)(p->code[i] + p->const_off[i]);
        c->size[st] = p->const_size[i];
    }
}

static uint32_t cb_cbuf_value(void *user, int stage, uint32_t index, uint32_t offset) {
    const ConstData *c = user;
    if (index == 2) return (uint32_t)tex_handle(nvn_stage_of(stage), offset);
    if (index == 1 && stage >= 0 && stage < 6 && c && offset + 4 <= c->size[stage]) {
        uint32_t v;
        memcpy(&v, c->data[stage] + offset, 4);
        return v;
    }
    return 0;
}
static int cb_texture_type(void *user, int stage, uint32_t handle) {
    (void)user, (void)stage;
    nvn_texdesc d;
    return handle_texture(handle, &d) ? target_type(d.target) : NX2X_T_2D;
}
static int cb_texture_integer(void *user, int stage, uint32_t handle) {
    (void)user, (void)stage;
    nvn_texdesc d;
    Nx2VkFormat f;
    return handle_texture(handle, &d) && nx2_vk_format(d.format, &f) && f.integer;
}

// ---- pipelines
#define MAX_STAGES 5
typedef struct {
    uint32_t serial;
    int topology;
    int ncolor;
    VkFormat color[8];
    VkFormat depth;
    Nx2Raster rs;
    uint8_t tex_types[MAX_STAGES][16];   // type of each texture descriptor, in descriptor order
    Nx2Attrib attrib[16];
    Nx2Stream stream[16];
} PipeKey;

typedef struct Pipe {
    PipeKey key;
    int failed;
    int nst;
    Nx2XStage st[MAX_STAGES];   // descriptors (SPIR-V freed after pipeline creation)
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkDescriptorSetLayout dsl;
    int ndesc;
    uint32_t loads_generic;
    int compute;
    uint32_t push_off, push_size;   // compute: where PushComputeConstants data goes in c0
    int16_t slot[MAX_STAGES][96];   // NVN binding index of each descriptor (-1: unknown)
    volatile long ready;            // 1: pipe usable; 0: the driver still compiles it (NX2_ASYNC_SHADERS); -1 failed
    struct Pipe *next;
} Pipe;

// NX2_ASYNC_SHADERS=1: the driver's compilation of a new graphics pipeline (vkCreateGraphicsPipelines, ~30 ms the
// first time a shader is seen on this machine) runs on a background thread (nx2_jobs.c) and the draws using it are
// skipped until it is ready: no frame stalls, but an object or effect can appear a few frames late the first time.
// The translation stays here (it reads the current draw state). Default: wait (no gaps). Not offered in the launcher:
// in Splatoon the ink painted on the ground is gameplay (the game reads it back from the GPU), and a skipped ink draw
// changed where the player ended up in the same scripted run (virtual time) - tests only.
static int async_shaders(void) {
    static int on = -1;
    if (on < 0) on = getenv("NX2_ASYNC_SHADERS") && atoi(getenv("NX2_ASYNC_SHADERS")) > 0;
    return on;
}
typedef struct {   // a pipeline creation with all it points to, for the background compile
    struct Pipe *pp;
    int n;
    VkPipelineShaderStageCreateInfo stages[MAX_STAGES];
    VkVertexInputAttributeDescription va[16];
    VkVertexInputBindingDescription vb[16];
    VkPipelineVertexInputStateCreateInfo vis;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vps;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cbs;
    VkDynamicState dyn[3];
    VkPipelineDynamicStateCreateInfo dys;
    VkPipelineRenderingCreateInfo ri;
    VkGraphicsPipelineCreateInfo gp;
} PipeJob;
static volatile long async_built, async_failed;

// NVN binding index of each descriptor. Graphics: textures at c2[0x20 + 8 i]; storage buffers {u64 address,
// u32 size} at c0[0x110 + 16 i] (vertex) / c0[0x510 + 16 i] (fragment), as GLSLC lays out the driver bank (TotK
// Recomp vk_exec_draw.c). Compute: the program's own compact c0 layout from 0x20 (storage buffers 16 bytes,
// textures and images 8 bytes each, in binding order), so a descriptor's index is its rank within its kind.
// Compute c0 layout from the control section (counts per kind at +0xb0 / +0xb4, the region's end at +0xba):
// from 0x20, 16 bytes per uniform buffer after the first, 16 per storage buffer, 8 per texture, 8 per image, each
// kind's slots in binding order (unused lower slots keep their place). 0 when the words do not add up.
static int compute_bases(const Nx2Program *p, uint32_t base[6]) {
    if (!p || !p->ctrl[0]) return 0;
    uint32_t wb0 = *(const uint32_t *)(uintptr_t)(p->ctrl[0] + 0xb0), wb4 = *(const uint32_t *)(uintptr_t)(p->ctrl[0] + 0xb4);
    uint32_t wb8 = *(const uint32_t *)(uintptr_t)(p->ctrl[0] + 0xb8);
    uint32_t nubo = wb0 >> 8 & 0xff, nssbo = wb0 >> 16 & 0xff, ntex = wb0 >> 24 & 0xff, nimg = wb4 >> 16 & 0xff;
    uint32_t end = wb8 >> 16 & 0xffff;
    (void)nubo;
    memset(base, 0, 6 * sizeof *base);
    // the region ends at `end`; what precedes the storage buffers varies by program, so count back from the end
    if (end < 8 * nimg + 8 * ntex + 16 * nssbo) return 0;   // the region may start below 0x20
    base[NX2X_IMAGE] = end - 8 * nimg;
    base[NX2X_TEXTURE] = base[NX2X_IMAGE] - 8 * ntex;
    base[NX2X_SSBO] = base[NX2X_TEXTURE] - 16 * nssbo;
    return 1;
}
static const Nx2Program *slots_prog;   // the program assign_slots works on (compute layout)

// Push constants of a compute program: (control +0xa9) words, in c0 right before its storage buffers.
static int push_region(const Nx2Program *p, uint32_t *off, uint32_t *size) {
    uint32_t b[6];
    if (!compute_bases(p, b)) return 0;
    uint32_t words = *(const uint32_t *)(uintptr_t)(p->ctrl[0] + 0xa8) >> 8 & 0xff;
    uint32_t span = (4 * words + 15) & ~15u;   // the block is 16-byte aligned: it ends at or below the buffers
    if (!words || b[NX2X_SSBO] < span) return 0;
    *off = b[NX2X_SSBO] - span, *size = 4 * words;
    return 1;
}

static void assign_slots(Pipe *pp) {
    for (int s = 0; s < pp->nst; s++) {
        const Nx2XStage *st = &pp->st[s];
        uint32_t lo[6];
        for (int k = 0; k < 6; k++) lo[k] = ~0u;
        for (int i = 0; i < st->ndesc; i++)
            if (st->desc[i].kind != NX2X_CBUF && st->desc[i].cbuf_offset < lo[st->desc[i].kind])
                lo[st->desc[i].kind] = st->desc[i].cbuf_offset;
        for (int i = 0; i < st->ndesc && i < 96; i++) {
            const Nx2XDesc *d = &st->desc[i];
            int slot = -1;
            if (st->stage == 5) {
                uint32_t stride = d->kind == NX2X_SSBO ? 16 : 8, cb[6];
                int exact = compute_bases(slots_prog, cb);
                if (!exact && i == 0) {
                    static int nlog;
                    if (nlog++ < 8)
                        rt_log("vkdraw: compute program #%u: control counts do not match, slots by rank", pp->key.serial);
                }
                if (d->kind != NX2X_CBUF && d->cbuf_index == 0)
                    slot = exact && d->cbuf_offset >= cb[d->kind] ? (int)((d->cbuf_offset - cb[d->kind]) / stride)
                                                                 : (int)((d->cbuf_offset - lo[d->kind]) / stride);
            } else if (d->kind == NX2X_TEXTURE && d->cbuf_index == 2 && d->cbuf_offset >= 0x20) {
                slot = (int)((d->cbuf_offset - 0x20) / 8);
            } else if (d->kind == NX2X_SSBO && d->cbuf_index == 0) {
                uint32_t base = st->stage == 0 ? 0x110 : st->stage == 4 ? 0x510 : 0;
                if (base && d->cbuf_offset >= base && d->cbuf_offset < base + 0x100) slot = (int)((d->cbuf_offset - base) / 16);
            }
            pp->slot[s][i] = (int16_t)slot;
        }
    }
}
#define PIPE_BUCKETS 4096
static Pipe *pipes[PIPE_BUCKETS];
static uint64_t pipe_count, pipe_failed;

static uint64_t key_hash(const PipeKey *k) {   // 8 bytes at a time (the key is zeroed first, padding included)
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 1469598103934665603ull;
    size_t i = 0;
    for (; i + 8 <= sizeof *k; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 31;
    }
    for (; i < sizeof *k; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static VkFormat vertex_format(int f) {
    switch (f) {   // 3-component and packed formats the texture table widens
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    default: break;
    }
    Nx2VkFormat vf;
    return nx2_vk_format(f, &vf) ? vf.vk : VK_FORMAT_UNDEFINED;
}

static VkShaderStageFlagBits vk_stage(int shader_stage) {
    switch (shader_stage) {
    case 0: return VK_SHADER_STAGE_VERTEX_BIT;
    case 1: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case 2: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case 3: return VK_SHADER_STAGE_GEOMETRY_BIT;
    case 4: return VK_SHADER_STAGE_FRAGMENT_BIT;
    default: return VK_SHADER_STAGE_COMPUTE_BIT;
    }
}

static VkDescriptorType desc_type(int kind) {
    switch (kind) {
    case NX2X_CBUF: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case NX2X_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case NX2X_TEXBUF: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case NX2X_IMGBUF: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    case NX2X_TEXTURE: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}

static VkPrimitiveTopology vk_topology(int mode) {
    switch (mode) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static int input_topology(int mode) {   // Shader::InputTopology
    return mode == 0 ? 0 : mode >= 1 && mode <= 3 ? 1 : 3;
}

// Graphics stages of a program in pipeline order (vertex, tess control, tess eval, geometry, fragment).
// The bytes of a stage that hold its code, for the translation cache (nx2_xcache.c): up to its constant data, which
// follows the code; without constants, what is readable (capped). Hashing more than the code only costs cache hits.
static size_t stage_code_len(const Nx2Program *p, const void *code, size_t size) {
    for (int i = 0; i < p->count; i++)
        if ((const void *)(uintptr_t)p->code[i] == code && p->const_off[i] >= 0x90 && p->const_off[i] <= size)
            return p->const_off[i];
    return size < (1u << 20) ? size : (1u << 20);
}
int nx2_xcache_translate(int, const void *const *, const size_t *, const size_t *, int, const Nx2XOptions *, Nx2XStage *);

static int program_stages(const Nx2Program *p, const void **code, size_t *size) {
    int n = 0;
    for (int want = 1; want <= 5; want++)
        for (int i = 0; i < p->count; i++) {
            if (!p->code[i] || p->code_size[i] < 0x90) continue;
            uint32_t w = *(const uint32_t *)(uintptr_t)p->code[i];
            if ((w & 0x1f) != 1 && (w & 0x1f) != 2) continue;
            if (((w >> 10) & 0xf) != (uint32_t)want) continue;
            code[n] = (const void *)(uintptr_t)p->code[i];
            size[n] = p->code_size[i];
            n++;
        }
    return n;
}

static VkBlendFactor blend_factor(int f) {   // NVNblendFunc
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 0xA: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0xB: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp blend_op(int e) {   // NVNblendEquation
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkShaderModule module(const Nx2XStage *s) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, s->words * 4, s->spirv};
    VkShaderModule m;
    VKCHECK(vkCreateShaderModule(D, &mi, NULL, &m));
    return m;
}

static void pipe_job(void *arg) {   // a worker thread (nx2_jobs.c): the driver's compile of one pipeline
    PipeJob *j = arg;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult res = vkCreateGraphicsPipelines(D, VK_NULL_HANDLE, 1, &j->gp, NULL, &p);
    for (int s = 0; s < j->n; s++) vkDestroyShaderModule(D, j->stages[s].module, NULL);
    j->pp->pipe = p;
    __atomic_store_n(&j->pp->ready, res == VK_SUCCESS ? 1 : -1, __ATOMIC_RELEASE);
    __atomic_add_fetch(res == VK_SUCCESS ? &async_built : &async_failed, 1, __ATOMIC_RELAXED);
    free(j);
}

static int build_pipeline(Pipe *pp, const Nx2Program *prog, const Nx2Draw *dr) {
    const void *code[MAX_STAGES];
    size_t size[MAX_STAGES];
    int n = program_stages(prog, code, size);
    if (!n) {
        rt_log("vkdraw: program #%u has no graphics stages", prog->serial);
        return 0;
    }
    ConstData cdata;
    const_data(prog, &cdata);
    Nx2XOptions opt = {0};
    opt.user = &cdata;
    opt.cbuf_value = cb_cbuf_value;
    opt.texture_type = cb_texture_type;
    opt.texture_integer = cb_texture_integer;
    opt.topology = input_topology(dr->mode);
    // NVN's default depth mode is NEAR_IS_MINUS_W (OpenGL clip space, z in [-w, w]): converted to Vulkan's [0, w]
    opt.convert_depth = 1;
    opt.float_controls_set = 1;
    opt.denorm_flush16 = props12.shaderDenormFlushToZeroFloat16;
    opt.denorm_flush32 = props12.shaderDenormFlushToZeroFloat32;
    opt.denorm_preserve16 = props12.shaderDenormPreserveFloat16;
    opt.denorm_preserve32 = props12.shaderDenormPreserveFloat32;
    opt.szinf_nan16 = props12.shaderSignedZeroInfNanPreserveFloat16;
    opt.szinf_nan32 = props12.shaderSignedZeroInfNanPreserveFloat32;
    opt.szinf_nan64 = props12.shaderSignedZeroInfNanPreserveFloat64;
    for (int k = 0; k < 32; k++) opt.attrib_types[k] = 5;   // disabled
    for (int k = 0; k < 16; k++) {
        if (!pp->key.attrib[k].format) continue;
        Nx2VkFormat vf;
        int integer = nx2_vk_format(pp->key.attrib[k].format, &vf) && vf.integer;
        opt.attrib_types[k] = integer ? (vf.sint ? 1 : 2) : 0;
    }
    for (int k = 0; k < 8; k++) {
        Nx2VkFormat cf = {0};
        Img *rt = k < nx2_state.rt.count ? rt_image(k) : NULL;
        int integer = rt && nx2_vk_format(rt->d.format, &cf) && cf.integer;
        opt.color_types[k] = integer ? (cf.sint ? 1 : 2) : 0;
    }
    uint64_t tx = plat_now_ns();
    size_t len[MAX_STAGES];
    for (int s = 0; s < n; s++) len[s] = stage_code_len(prog, code[s], size[s]);
    int r = nx2_xcache_translate(n, code, size, len, 0, &opt, pp->st);
    prof_xlate_ns += plat_now_ns() - tx;
    if (r) {
        rt_log("vkdraw: program #%u stage %d not translated: %s", prog->serial, r - 1, pp->st[r - 1].error);
        return 0;
    }
    pp->nst = n;
    {   // NX2_DUMP_SPIRV=<serial>: that program's translated stages as spv_<serial>_<stage>.spv (debug)
        static char want_list[256] = {1};   // a comma-separated list of serials
        if (want_list[0] == 1) snprintf(want_list, sizeof want_list, ",%s,", getenv("NX2_DUMP_SPIRV") ? getenv("NX2_DUMP_SPIRV") : "");
        char me[16];
        snprintf(me, sizeof me, ",%u,", prog->serial);
        int want = strstr(want_list, me) ? (int)prog->serial : -1;
        if ((int)prog->serial == want)
            for (int ns = 0; ns < 6; ns++)   // and the constant buffers bound at its first draw
                for (int k = 0; k < NX2_MAX_UBO; k++) {
                    Nx2Range r = nx2_state.ubo[ns][k];
                    if (!r.addr || !r.size) continue;
                    const float *fv = (const float *)(uintptr_t)r.addr;
                    char line[1600];
                    size_t o = 0;
                    for (uint64_t i = 0; i < r.size / 4 && i < 96 && o < sizeof line - 32; i++)
                        o += (size_t)snprintf(line + o, sizeof line - o, "%s%g", i % 4 ? " " : " | ", fv[i]);
                    rt_log("spirv dump #%u: nvn stage %d ubo %d (%#llx bytes):%s", prog->serial, ns, k, (unsigned long long)r.size, line);
                }
        if ((int)prog->serial == want)
            for (int s = 0; s < n; s++) {
                char path[64];
                snprintf(path, sizeof path, "spv_%u_%d.spv", prog->serial, pp->st[s].stage);
                FILE *f = fopen(path, "wb");
                if (f) fwrite(pp->st[s].spirv, 4, pp->st[s].words, f), fclose(f);
            }
    }
    assign_slots(pp);
    // descriptor set layout: every stage's descriptors (bindings are unique across stages)
    VkDescriptorSetLayoutBinding b[160];
    int nb = 0;
    for (int s = 0; s < n; s++) {
        pp->st[s].stage = pp->st[s].stage;
        for (int i = 0; i < pp->st[s].ndesc && nb < 160; i++) {
            const Nx2XDesc *dd = &pp->st[s].desc[i];
            b[nb++] = (VkDescriptorSetLayoutBinding){dd->binding, desc_type(dd->kind), dd->count,
                                                     (VkShaderStageFlags)vk_stage(pp->st[s].stage), NULL};
        }
    }
    pp->ndesc = nb;
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0,
                                          (uint32_t)nb, b};
    VKCHECK(vkCreateDescriptorSetLayout(D, &dl, NULL, &pp->dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16};   // render area (Eglantine RenderAreaInfo)
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &pp->dsl, 1, &pcr};
    VKCHECK(vkCreatePipelineLayout(D, &pl, NULL, &pp->layout));

    VkPipelineShaderStageCreateInfo stages[MAX_STAGES];
    for (int s = 0; s < n; s++)
        stages[s] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                      vk_stage(pp->st[s].stage), module(&pp->st[s]), "main", NULL};
    // NX2_DEBUG_PROG=<serial> with NX2_DEBUG_VS / NX2_DEBUG_FS=<file.spv>: that program's stage replaced (tests)
    const char *dp = getenv("NX2_DEBUG_PROG");
    if (dp && (uint32_t)atoi(dp) == prog->serial)
        for (int s = 0; s < n; s++) {
            const char *f = getenv(pp->st[s].stage == 0 ? "NX2_DEBUG_VS" : pp->st[s].stage == 4 ? "NX2_DEBUG_FS" : "-");
            FILE *fp = f ? fopen(f, "rb") : NULL;
            if (!fp) continue;
            static uint32_t words[65536];
            size_t nb = fread(words, 1, sizeof words, fp);
            fclose(fp);
            Nx2XStage tmp = pp->st[s];
            tmp.spirv = words, tmp.words = nb / 4;
            vkDestroyShaderModule(D, stages[s].module, NULL);
            stages[s].module = module(&tmp);
            rt_log("vkdraw: program #%u stage %d replaced by %s", prog->serial, pp->st[s].stage, f);
        }
    // vertex input: the generics the vertex shader reads, from the bound attribute states
    VkVertexInputAttributeDescription va[16];
    VkVertexInputBindingDescription vb[16];
    int nva = 0, nvb = 0;
    uint32_t streams = 0;
    pp->loads_generic = pp->st[0].loads_generic;
    for (int k = 0; k < 16; k++) {
        if (!(pp->loads_generic & (1u << k)) || !pp->key.attrib[k].format) continue;
        VkFormat vf = vertex_format(pp->key.attrib[k].format);
        if (vf == VK_FORMAT_UNDEFINED) {   // the shader then reads (0, 0, 0, 1): flat normals / texture coordinates
            static uint32_t seen[8];
            static int nseen;
            int known = 0;
            for (int i = 0; i < nseen; i++) known |= seen[i] == (uint32_t)pp->key.attrib[k].format;
            if (!known && nseen < 8) {
                seen[nseen++] = (uint32_t)pp->key.attrib[k].format;
                rt_log("vkdraw: vertex attribute format %#x not supported (program #%u, attribute %d): dropped",
                       pp->key.attrib[k].format, prog->serial, k);
            }
            continue;
        }
        int st = pp->key.attrib[k].stream & 15;
        va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)k, (uint32_t)st, vf,
                                                        (uint32_t)pp->key.attrib[k].offset};
        streams |= 1u << st;
    }
    for (int st = 0; st < 16; st++)
        if (streams & (1u << st))
            vb[nvb++] = (VkVertexInputBindingDescription){(uint32_t)st, (uint32_t)pp->key.stream[st].stride,
                                                          pp->key.stream[st].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                                     : VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vis = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
                                                (uint32_t)nvb, vb, (uint32_t)nva, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 vk_topology(dr->mode), VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL,
                                             1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    const Nx2Raster *R = &pp->key.rs;
    int y_flip(void);
    rs.polygonMode = R->polygon_mode == 0 ? VK_POLYGON_MODE_POINT : R->polygon_mode == 1 ? VK_POLYGON_MODE_LINE
                                                                                         : VK_POLYGON_MODE_FILL;
    rs.cullMode = getenv("NX2_NO_CULL") ? VK_CULL_MODE_NONE : (VkCullModeFlags)(R->cull & 3);
    // NVN decides winding with y up (OpenGL). With the flipped viewport (y_flip) Vulkan sees the same winding;
    // without it, Vulkan's framebuffer y points down, which reverses every winding.
    if (y_flip()) rs.frontFace = R->front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    else rs.frontFace = R->front_ccw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = R->depth_test != 0;
    ds.depthWriteEnable = R->depth_write != 0;
    ds.depthCompareOp = R->depth_func >= 1 && R->depth_func <= 8 ? (VkCompareOp)(R->depth_func - 1)
                                                                 : VK_COMPARE_OP_ALWAYS;
    {   // NX2_ZEQUAL=lequal|gequal: depth "equal" (colour passes over a depth pre-pass) relaxed, for tests
        static int relax = -1;
        if (relax < 0) {
            const char *e = getenv("NX2_ZEQUAL");
            relax = !e ? 0 : !strcmp(e, "lequal") ? 1 : !strcmp(e, "gequal") ? 2 : 0;
        }
        if (relax && ds.depthCompareOp == VK_COMPARE_OP_EQUAL)
            ds.depthCompareOp = relax == 1 ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_GREATER_OR_EQUAL;
    }
    // stencil (masks and reference are dynamic: SetStencilRef / ValueMask / Mask); NVN functions and ops are Vulkan's + 1
    ds.stencilTestEnable = R->stencil_test != 0;
    for (int f = 0; f < 2; f++) {
        VkStencilOpState *so = f ? &ds.back : &ds.front;
        so->compareOp = R->sfunc[f] >= 1 && R->sfunc[f] <= 8 ? (VkCompareOp)(R->sfunc[f] - 1) : VK_COMPARE_OP_ALWAYS;
        VkStencilOp *op[3] = {&so->failOp, &so->depthFailOp, &so->passOp};
        for (int k = 0; k < 3; k++) *op[k] = R->sop[f][k] >= 1 && R->sop[f][k] <= 8 ? (VkStencilOp)(R->sop[f][k] - 1) : VK_STENCIL_OP_KEEP;
        so->compareMask = so->writeMask = 0xff;
    }
    VkPipelineColorBlendAttachmentState cba[8];
    for (int k = 0; k < 8; k++) {
        memset(&cba[k], 0, sizeof cba[k]);
        cba[k].colorWriteMask = R->mask[k] & 0xf;
        if (R->blend_enable & (1u << k)) {
            const Nx2Blend *b = &R->blend[k];
            cba[k].blendEnable = VK_TRUE;
            cba[k].srcColorBlendFactor = blend_factor(b->src);
            cba[k].dstColorBlendFactor = blend_factor(b->dst);
            cba[k].srcAlphaBlendFactor = blend_factor(b->srca);
            cba[k].dstAlphaBlendFactor = blend_factor(b->dsta);
            cba[k].colorBlendOp = blend_op(b->eq);
            cba[k].alphaBlendOp = blend_op(b->eqa);
        }
    }
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = (uint32_t)pp->key.ncolor;
    cbs.pAttachments = cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dys = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 3, dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = (uint32_t)pp->key.ncolor;
    ri.pColorAttachmentFormats = pp->key.color;
    ri.depthAttachmentFormat = pp->key.depth;
    if (pp->key.depth == VK_FORMAT_D32_SFLOAT_S8_UINT || pp->key.depth == VK_FORMAT_D24_UNORM_S8_UINT)
        ri.stencilAttachmentFormat = pp->key.depth;
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    gp.stageCount = (uint32_t)n;
    gp.pStages = stages;
    gp.pVertexInputState = &vis;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &dys;
    gp.layout = pp->layout;
    if (async_shaders()) {   // the compile goes to a worker with copies of everything the create info points to
        PipeJob *j = calloc(1, sizeof *j);
        if (j) {
            j->pp = pp, j->n = n;
            memcpy(j->stages, stages, sizeof stages), memcpy(j->va, va, sizeof va), memcpy(j->vb, vb, sizeof vb);
            memcpy(j->cba, cba, sizeof cba), memcpy(j->dyn, dyn, sizeof dyn);
            j->vis = vis, j->ia = ia, j->vps = vps, j->rs = rs, j->ms = ms, j->ds = ds, j->cbs = cbs, j->dys = dys;
            j->ri = ri, j->gp = gp;
            j->vis.pVertexBindingDescriptions = j->vb, j->vis.pVertexAttributeDescriptions = j->va;
            j->cbs.pAttachments = j->cba, j->dys.pDynamicStates = j->dyn;
            j->ri.pColorAttachmentFormats = pp->key.color;   // in the pipeline record: stays
            j->gp.pNext = &j->ri, j->gp.pStages = j->stages, j->gp.pVertexInputState = &j->vis;
            j->gp.pInputAssemblyState = &j->ia, j->gp.pViewportState = &j->vps, j->gp.pRasterizationState = &j->rs;
            j->gp.pMultisampleState = &j->ms, j->gp.pDepthStencilState = &j->ds, j->gp.pColorBlendState = &j->cbs;
            j->gp.pDynamicState = &j->dys;
            nx2_xlate_free(pp->st, n);
            pp->ready = 0;
            void nx2_job_submit(void (*fn)(void *), void *arg);
            nx2_job_submit(pipe_job, j);
            pipe_count++;
            return 1;
        }
    }
    uint64_t tv = plat_now_ns();
    VkResult res = vkCreateGraphicsPipelines(D, VK_NULL_HANDLE, 1, &gp, NULL, &pp->pipe);
    prof_vkpipe_ns += plat_now_ns() - tv;
    for (int s = 0; s < n; s++) vkDestroyShaderModule(D, stages[s].module, NULL);
    nx2_xlate_free(pp->st, n);
    if (res != VK_SUCCESS) {
        rt_log("vkdraw: pipeline of program #%u failed (%d)", prog->serial, (int)res);
        return 0;
    }
    pipe_count++;
    pp->ready = 1;
    if (pipe_count <= 40 || pipe_count % 100 == 0)
        rt_log("vkdraw: pipeline %llu: program #%u, %d stages, %d descriptors, %d colour targets",
               (unsigned long long)pipe_count, prog->serial, n, nb, pp->key.ncolor);
    return 1;
}

// Texture types of a program's texture descriptors with the bound textures (part of the pipeline key).
typedef struct { uint32_t serial; int nst; int stage[MAX_STAGES]; int ntex[MAX_STAGES]; uint32_t off[MAX_STAGES][16]; }
    ProgTex;
#define PROGTEX_SLOTS 65536
static ProgTex *progtex[PROGTEX_SLOTS];

// What a render target view selects: its format (0: the texture's own), first layer (3D: slice) and mip level.
typedef struct { int fmt, layer, level; } RtSel;
static VkImageView att_view_sel(Img *m, RtSel sel, VkFormat *fmt_out);
static RtSel rt_sel(int i) {
    RtSel r = {nx2_state.rt.view_format[i], nx2_state.rt.view_layer[i], nx2_state.rt.view_level[i]};
    return r;
}

static Pipe *pipe_for(const Nx2Program *prog, const Nx2Draw *dr) {
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.serial = prog->serial;
    k.topology = dr->mode;
    k.ncolor = nx2_state.rt.count;
    for (int i = 0; i < k.ncolor; i++) {
        Img *m = rt_image(i);
        VkFormat vf = VK_FORMAT_UNDEFINED;
        if (m && !m->failed && m->att_view) att_view_sel(m, rt_sel(i), &vf);
        k.color[i] = vf;
    }
    while (k.ncolor > 0 && k.color[k.ncolor - 1] == VK_FORMAT_UNDEFINED) k.ncolor--;   // depth-only passes
    Img *dm = nx2_state.rt.depth ? img_get(nx2_state.rt.depth) : NULL;
    k.depth = dm && !dm->failed && dm->f.depth ? dm->fmt : VK_FORMAT_UNDEFINED;
    k.rs = nx2_state.rs;
    for (int i = k.ncolor; i < 8; i++) memset(&k.rs.blend[i], 0, sizeof k.rs.blend[i]), k.rs.mask[i] = 0;
    k.rs.blend_enable &= (uint8_t)((1u << k.ncolor) - 1);
    for (int i = 0; i < k.ncolor; i++)
        if (!(k.rs.blend_enable & (1u << i))) memset(&k.rs.blend[i], 0, sizeof k.rs.blend[i]);
    if (k.depth == VK_FORMAT_UNDEFINED) k.rs.depth_test = k.rs.depth_write = k.rs.depth_func = 0;
    if (k.depth != VK_FORMAT_D32_SFLOAT_S8_UINT && k.depth != VK_FORMAT_D24_UNORM_S8_UINT)   // no stencil to test
        k.rs.stencil_test = 0;
    if (!k.rs.stencil_test) memset(k.rs.sfunc, 0, sizeof k.rs.sfunc), memset(k.rs.sop, 0, sizeof k.rs.sop);
    memcpy(k.attrib, nx2_state.vi.a, sizeof k.attrib);
    for (int i = nx2_state.vi.nattrib; i < 16; i++) memset(&k.attrib[i], 0, sizeof k.attrib[i]);
    memcpy(k.stream, nx2_state.vi.s, sizeof k.stream);
    ProgTex *pt = prog->serial < PROGTEX_SLOTS ? progtex[prog->serial] : NULL;
    if (pt)
        for (int s = 0; s < pt->nst; s++)
            for (int i = 0; i < pt->ntex[s] && i < 16; i++) {
                nvn_texdesc d;
                k.tex_types[s][i] = (uint8_t)(handle_texture(tex_handle(pt->stage[s], pt->off[s][i]), &d)
                                                  ? target_type(d.target) : NX2X_T_2D);
            }
    uint64_t h = key_hash(&k);
    Pipe **slot = &pipes[h % PIPE_BUCKETS];
    for (Pipe *p = *slot; p; p = p->next)
        if (!memcmp(&p->key, &k, sizeof k)) return p->failed ? NULL : p;
    Pipe *pp = calloc(1, sizeof *pp);
    if (!pp) rt_fatal("vkdraw: pipeline record");
    pp->key = k;
    pp->next = *slot;
    *slot = pp;
    uint64_t tp = plat_now_ns();
    int built = build_pipeline(pp, prog, dr);
    nx2_prof_pipe_ns += plat_now_ns() - tp, nx2_prof_pipes++, nx2_stat_pipes++;
    if (!built) {
        pp->failed = 1;
        pipe_failed++;
        return NULL;
    }
    if (!pt && prog->serial < PROGTEX_SLOTS) {   // remember where the program reads texture handles
        pt = calloc(1, sizeof *pt);
        pt->serial = prog->serial;
        pt->nst = pp->nst;
        for (int s = 0; s < pp->nst; s++) {
            pt->stage[s] = nvn_stage_of(pp->st[s].stage);
            for (int i = 0; i < pp->st[s].ndesc; i++)
                if (pp->st[s].desc[i].kind == NX2X_TEXTURE && pt->ntex[s] < 16)
                    pt->off[s][pt->ntex[s]++] = pp->st[s].desc[i].cbuf_offset;
        }
        progtex[prog->serial] = pt;
    }
    return pp;
}

// The level and layer a render target view selects, within the image (out of range: 0).
static void sel_clamp(const Img *m, RtSel *s) {
    if (s->level < 0 || (uint32_t)s->level >= m->levels) s->level = 0;
    uint32_t n = m->type == VK_IMAGE_TYPE_3D ? (m->depth >> s->level ? m->depth >> s->level : 1) : m->layers;
    if (m->type == VK_IMAGE_TYPE_3D && !(m->flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) n = 1;
    if (s->layer < 0 || (uint32_t)s->layer >= n) s->layer = 0;
}

// The attachment view of an NVN render target view: the image's own format or another of its class (mutable
// images), one layer (3D: one slice) and one mip level. Views other than the image's first are kept per image.
static VkImageView att_view_sel(Img *m, RtSel sel, VkFormat *fmt_out) {
    VkFormat vf = m->fmt;
    Nx2VkFormat f;
    if (sel.fmt && nx2_vk_format(sel.fmt, &f) && f.vk != m->fmt && (m->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))
        vf = f.vk;
    sel_clamp(m, &sel);
    *fmt_out = m->fmt;
    if (vf == m->fmt && !sel.layer && !sel.level) return m->att_view;
    for (int i = 0; i < m->natt_sel; i++)
        if (m->att_sel[i].fmt == vf && m->att_sel[i].layer == sel.layer && m->att_sel[i].level == sel.level) {
            *fmt_out = vf;
            return m->att_sel[i].view;
        }
    if (m->natt_sel >= 1024) return m->att_view;
    if (!(m->natt_sel & 15)) {
        struct AttSel *g = realloc(m->att_sel, (size_t)(m->natt_sel + 16) * sizeof *g);
        if (!g) return m->att_view;
        m->att_sel = g;
    }
    int depthy = m->f.depth || m->f.stencil;
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
                                     depthy ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                            : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vu};
    vi.image = m->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = vf;
    vi.subresourceRange =
        (VkImageSubresourceRange){m->all_aspect, (uint32_t)sel.level, 1, (uint32_t)sel.layer, 1};
    VkImageView v;
    if (vkCreateImageView(D, &vi, NULL, &v) != VK_SUCCESS) return m->att_view;
    struct AttSel *a = &m->att_sel[m->natt_sel++];
    a->view = v, a->fmt = vf, a->layer = (uint16_t)sel.layer, a->level = (uint16_t)sel.level;
    static int nlog;
    if (nlog++ < 16)
        rt_log("vkdraw: render target %#llx (%ux%ux%u, %u layers) written as format %d, layer %d, level %d",
               (unsigned long long)m->obj, m->w, m->h, m->depth, m->layers, (int)vf, sel.layer, sel.level);
    *fmt_out = vf;
    return v;
}

// The game sets NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT (Direct3D's convention): clip-space y = +1 is the first row of
// a render target, where Vulkan puts y = -1. Viewports are flipped (negative height) to match; without it every
// draw lands upside down relative to the hardware, and passes that sample by interpolated coordinates and passes
// that sample by window position (gl_FragCoord) disagree about where things are. NX2_YFLIP=0: no flip.
int y_flip(void) {
    static int on = -1;
    if (on < 0) on = !(getenv("NX2_YFLIP") && *getenv("NX2_YFLIP") == '0');
    return on;
}

// GPU texture compression: the game renders BC4 / BC5 textures (labels, look-up tables) through RGBA16UI / RGBA32UI
// views whose texels are the 4x4 blocks. Vulkan cannot render into a compressed image, so such a target is drawn
// into an uncompressed image of the view's format and size (m->enc) and copied into the compressed one, whose
// blocks have the same bytes, after the draw or clear (enc_flush).
static Img *enc_pending[8];
static int nenc_pending;
static Img *rt_image(int i) {   // the image colour target i is drawn into
    Img *m = img_get(nx2_state.rt.color[i]);
    if (!m || m->failed || m->att_view) return m;
    int bw = 1, bh = 1, vbw = 1, vbh = 1, vf = nx2_state.rt.view_format[i];
    int bpb = nvn_fmt_bpb(m->d.format, &bw, &bh);
    if (bw == 1 || !vf || nvn_fmt_bpb(vf, &vbw, &vbh) != bpb || vbw != 1) return m;
    int lvl = nx2_state.rt.view_level[i] >= 0 && (uint32_t)nx2_state.rt.view_level[i] < m->levels
                  ? nx2_state.rt.view_level[i] : 0;
    uint32_t w = (m->w >> lvl ? m->w >> lvl : 1), h = (m->h >> lvl ? m->h >> lvl : 1);
    w = (w + bw - 1) / bw, h = (h + bh - 1) / bh;
    if (!m->enc || m->enc->d.format != vf || m->enc->w != w || m->enc->h != h) {
        if (m->enc) {
            if (ndead < (int)(sizeof dead / sizeof *dead)) dead[ndead++] = m->enc;
            else destroy_img(m->enc);
        }
        nvn_texdesc d = {0};
        d.target = 1, d.format = vf, d.width = w, d.height = h, d.depth = 1, d.levels = 1;
        m->enc = create_img(0, &d, NULL);
        static int nlog;
        if (nlog++ < 8)
            rt_log("vkdraw: compressed target %#llx (%ux%u format %#x) drawn through a %ux%u image of format %#x, level %d",
                   (unsigned long long)m->obj, m->w, m->h, m->d.format, w, h, vf, lvl);
        if (m->enc->failed || !m->enc->att_view) return m;
    }
    m->enc_level = lvl;
    int known = 0;
    for (int k = 0; k < nenc_pending; k++) known |= enc_pending[k] == m;
    if (!known && nenc_pending < 8) enc_pending[nenc_pending++] = m;
    return m->enc;
}
static Img *g_bound_tex, *g_bound_wimg;   // (bind_resources)
static void enc_flush(void) {   // the drawn blocks into the compressed images
    if (!nenc_pending) return;
    static int nlog;
    if (nlog++ < 4000 && enc_pending[0]->enc && enc_pending[0]->enc->w == 100) {
        const Img *src = g_bound_tex;   // the texture the encoder read: what the CPU memory under it holds
        uint64_t nz = 0, hsh = 0;
        if (src && src->storage)
            for (uint64_t k = 0; k < src->size; k++) nz += src->storage[k] != 0, hsh = hsh * 31 + src->storage[k];
        rt_log("vkdraw: compressed target %#llx encoded on frame %llu from %ux%u gpu %d: CPU memory %llu nonzero bytes "
               "of %llu, hash %016llx", (unsigned long long)enc_pending[0]->obj, (unsigned long long)nx2_vk_frame + 1,
               src ? src->w : 0, src ? src->h : 0, src ? src->gpu : -1, (unsigned long long)nz,
               (unsigned long long)(src ? src->size : 0), (unsigned long long)hsh);
    }
    end_rendering();
    barrier_all();
    for (int k = 0; k < nenc_pending; k++) {
        Img *m = enc_pending[k];
        img_layout(m->enc, VK_IMAGE_LAYOUT_GENERAL);
        img_layout(m, VK_IMAGE_LAYOUT_GENERAL);
        VkImageCopy cp = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                          {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m->enc_level, 0, 1}, {0, 0, 0},
                          {m->enc->w, m->enc->h, 1}};
        vkCmdCopyImage(cb, m->enc->img, VK_IMAGE_LAYOUT_GENERAL, m->img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
        mark_gpu(m);
    }
    barrier_all();
    nenc_pending = 0;
}

// ---- render targets
static Img *cur_rt[8], *cur_depth;
static int cur_nrt;
static uint32_t cur_w, cur_h;

static uint32_t lvl_dim(uint32_t d, int level) { return d >> level ? d >> level : 1; }

// sel: what each colour target's view selects (format, layer, level); dsel: the depth view's layer and level
static void begin_rendering(Img **rts, const RtSel *sel, int n, Img *depth, RtSel dsel) {
    static RtSel cur_sel[8], cur_dsel;
    if (rendering && n == cur_nrt && depth == cur_depth && !memcmp(rts, cur_rt, n * sizeof *rts) &&
        !memcmp(cur_sel, sel, n * sizeof *sel) && !memcmp(&cur_dsel, &dsel, sizeof dsel))
        return;
    memcpy(cur_sel, sel, n * sizeof *sel);
    cur_dsel = dsel;
    end_rendering();
    VkRenderingAttachmentInfo att[8];
    uint32_t w = 1u << 30, h = 1u << 30;
    for (int i = 0; i < n; i++) {
        att[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkFormat vf;
        att[i].imageView = rts[i] ? att_view_sel(rts[i], sel[i], &vf) : VK_NULL_HANDLE;
        att[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        att[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (rts[i]) {
            RtSel s = sel[i];
            sel_clamp(rts[i], &s);
            if (lvl_dim(rts[i]->w, s.level) < w) w = lvl_dim(rts[i]->w, s.level);
            if (lvl_dim(rts[i]->h, s.level) < h) h = lvl_dim(rts[i]->h, s.level);
        }
    }
    RtSel ds = dsel;
    if (depth) sel_clamp(depth, &ds);
    if (w == 1u << 30 && depth) w = lvl_dim(depth->w, ds.level), h = lvl_dim(depth->h, ds.level);
    if (w == 1u << 30) w = h = 1;
    barrier_all();
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = (VkRect2D){{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = (uint32_t)n;
    ri.pColorAttachments = att;
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth) {
        VkFormat dvf;
        da.imageView = att_view_sel(depth, ds, &dvf);
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = &da;
        if (depth->f.stencil) ri.pStencilAttachment = &da;
    }
    vkCmdBeginRendering(cb, &ri);
    cur_depth = depth;
    rendering = 1;
    memcpy(cur_rt, rts, n * sizeof *rts);
    cur_nrt = n;
    cur_w = w, cur_h = h;
}

static uint64_t g_draws_done, g_draws_skipped;
uint64_t nx2_stat_draws(void) { return g_draws_done; }

static void skip(const char *why) {
    g_draws_skipped++;
    occ_note_skip();
    static int n;
    if (n++ < 24) rt_log("vkdraw: draw skipped: %s", why);
}

// (written storage buffers: wb[], copied back to guest memory in flush)

static VkImageView img_storage_view(Img *m, int shader_type);

static Img *resolve_image_handle(uint64_t handle) {   // NVNimageHandle: an image id in the texture pool
    uint64_t obj, view;
    if (!handle || !nx2_dpool_lookup(nx2_state.texture_pool, (int)(handle & 0xFFFFF), &obj, &view)) return NULL;
    return img_get(obj);
}

// NX2_TRACE_FRAME=<n>: one line per clear / draw / dispatch of present n with what it reads and writes.
static char trace_buf[2048];
int nx2_vk_tracing(void);
static int tracing(void) { return nx2_vk_tracing(); }
int nx2_vk_tracing(void) {
    static int64_t want = -2;
    if (want == -2) want = getenv("NX2_TRACE_FRAME") ? atoll(getenv("NX2_TRACE_FRAME")) : -1;
    if (nx2_dump_request) return 1;   // NX2_DUMP_ON_BLACK: the frame whose images are dumped
    return want >= 0 && (int64_t)nx2_vk_frame + 1 == want;   // the commands submitted before present n
}
static int img_desc(char *o, size_t n, const char *tag, const Img *m) {
    if (!m) return snprintf(o, n, " %s:none", tag);
    if (m->depth > 1 || m->layers > 1) {   // 3D or layered: the raw storage too (tex3d_<object>.bin), for inspection
        char path[64];
        snprintf(path, sizeof path, "tex3d_%llx.bin", (unsigned long long)m->obj & 0xfffffff);
        FILE *f = m->storage && m->size && m->size <= (16u << 20) ? fopen(path, "wb") : NULL;
        if (f) fwrite(m->storage, 1, (size_t)m->size, f), fclose(f);
    }
    return snprintf(o, n, " %s:%llx(%ux%ux%u L%u t%d f%d%s @%llx)", tag, (unsigned long long)m->obj & 0xfffffff, m->w,
                    m->h, m->depth, m->layers, m->d.target, (int)m->fmt, m->gpu ? " gpu" : "",
                    (unsigned long long)(uintptr_t)m->storage);
}

static Img *g_bound_tex, *g_bound_wimg;

static void ssbo_writeback(void) {   // after a draw or dispatch: written storage buffers back to the game's memory
    if (!nssbo_back && !nmfix) return;
    end_rendering();
    barrier_all();
    for (int i = 0; i < nmfix; i++) {
        const struct Imp *mi = mfix[i].imp;
        if (mi->mstate != 1 || !mi->dbuf) continue;
        VkBufferCopy cp = {mfix[i].off, mfix[i].off, mfix[i].size};
        vkCmdCopyBuffer(cb, mi->buf, mi->dbuf, 1, &cp);
        static int nlog;
        if (nlog++ < 8)
            rt_log("vkdraw: storage buffer written in place in mirrored pool %#llx (+%#llx, %#llx bytes): mirror updated",
                   (unsigned long long)mi->base, (unsigned long long)mfix[i].off, (unsigned long long)mfix[i].size);
    }
    nmfix = 0;
    for (int i = 0; i < nssbo_back; i++) {
        VkBufferCopy cp = {ssbo_back[i].src_off, ssbo_back[i].dst_off, ssbo_back[i].size};
        vkCmdCopyBuffer(cb, dev_buf, ssbo_back[i].dst, 1, &cp);
        // every mirror covering the written memory, found by address: pools overlap (the write may go through a pool
        // that is not the mirrored one)
        const struct Imp *wi = ssbo_back[i].imp;
        if (!wi) continue;
        uint64_t a0 = wi->base + ssbo_back[i].dst_off, a1 = a0 + ssbo_back[i].size;
        for (int k = 0; k < nmirrors; k++) {
            Imp *mi = mirrors[k];
            if (mi->base >= a1 || a0 >= mi->base + mi->size || !mi->dbuf) continue;
            uint64_t lo = a0 > mi->base ? a0 : mi->base, hi = a1 < mi->base + mi->size ? a1 : mi->base + mi->size;
            VkBufferCopy mc = {ssbo_back[i].src_off + (lo - a0), lo - mi->base, hi - lo};
            vkCmdCopyBuffer(cb, dev_buf, mi->dbuf, 1, &mc);
            if (mi != wi) {
                static int nlog;
                if (nlog++ < 8)
                    rt_log("vkdraw: storage write through pool %#llx lands in mirrored pool %#llx (+%#llx): mirror updated",
                           (unsigned long long)wi->base, (unsigned long long)mi->base, (unsigned long long)(lo - mi->base));
            }
            if (mi->mhash) {   // NX2_MIRROR_VERIFY: written by the GPU, not a missed write
                if (!mi->mgpu) mi->mgpu = calloc(mi->size / 4096, 1);
                for (uint64_t pg = (lo - mi->base) / 4096; mi->mgpu && pg <= (hi - 1 - mi->base) / 4096; pg++) mi->mgpu[pg] = 1;
            }
        }
    }
    barrier_all();
    nssbo_back = 0;
}

// Descriptors of a pipeline from the current NVN bindings; 0 when something cannot be bound yet.
static int bind_resources(Pipe *pp, const ConstData *cdata, VkDescriptorSet *set_out) {
    Nx2DrawState *s = &nx2_state;
    // pass 1, images: uploads and layout changes may flush the staging ring (which frees descriptor sets)
    Img *imgs_of[MAX_STAGES][96];
    static Nx2TexView views_of[MAX_STAGES][96];
    static Nx2Sampler smp_of[MAX_STAGES][96];
    for (int st = 0; st < pp->nst && st < MAX_STAGES; st++)   // the slots this pipeline uses
        memset(imgs_of[st], 0, sizeof imgs_of[st][0] * (size_t)(pp->st[st].ndesc < 96 ? pp->st[st].ndesc : 96));
    for (int st = 0; st < pp->nst; st++) {
        int ns = nvn_stage_of(pp->st[st].stage);
        for (int i = 0; i < pp->st[st].ndesc && i < 96; i++) {
            const Nx2XDesc *dd = &pp->st[st].desc[i];
            int slot = pp->slot[st][i];
            if (dd->kind == NX2X_TEXBUF || dd->kind == NX2X_IMGBUF) {
                static int nlog;
                if (nlog++ < 8) rt_log("vkdraw: texel buffer at c%u[%#x] not supported yet", dd->cbuf_index, dd->cbuf_offset);
                return 0;
            }
            Img *m = NULL;
            if (dd->kind == NX2X_TEXTURE) {
                uint64_t t_l = __rdtsc();
                uint64_t handle = slot >= 0 && slot < NX2_MAX_TEX ? s->tex[ns][slot] : 0, tex = 0, smp = 0;
                m = handle && nx2_resolve_texture(handle, &tex, &smp) ? img_get(tex) : NULL;
                if (m && (m->failed || !m->view)) m = NULL;
                if (!m || !nx2_dpool_view(s->texture_pool, (int)(handle & 0xFFFFF), &views_of[st][i]))
                    memset(&views_of[st][i], 0, sizeof views_of[st][i]);
                nvn_texdesc td;   // the texture object's own swizzle, when the view sets none (images ignore it)
                uint8_t *tst;
                uint64_t tsz;
                if (m && !views_of[st][i].has_swizzle && nx2_tex_get(tex, &td, &tst, &tsz) &&
                    (td.swizzle_r | td.swizzle_g | td.swizzle_b | td.swizzle_a)) {
                    Nx2TexView *tv = &views_of[st][i];
                    tv->has_swizzle = 1;
                    tv->swizzle[0] = (uint8_t)td.swizzle_r, tv->swizzle[1] = (uint8_t)td.swizzle_g;
                    tv->swizzle[2] = (uint8_t)td.swizzle_b, tv->swizzle[3] = (uint8_t)td.swizzle_a;
                }
                if (!m || !nx2_dpool_sampler(s->sampler_pool, (int)(handle >> 20 & 0xFFF), &smp_of[st][i]))
                    memset(&smp_of[st][i], 0, sizeof smp_of[st][i]);
                uint64_t t_u = __rdtsc();
                dp_ticks[DP_LOOKUP] += t_u - t_l;
                if (m) img_upload(m);
                dp_ticks[DP_UPLOAD] += __rdtsc() - t_u;
            } else if (dd->kind == NX2X_IMAGE) {
                uint64_t t_i = __rdtsc();
                m = slot >= 0 && slot < 8 ? resolve_image_handle(s->image[ns][slot]) : NULL;
                if (m && (m->failed || !img_storage_view(m, dd->type))) {
                    static int nlog;
                    if (nlog++ < 8)
                        rt_log("vkdraw: image %#llx (format %#x) cannot be a storage image", (unsigned long long)m->obj,
                               m->d.format);
                    m = NULL;
                }
                if (m) {
                    img_upload(m);
                    if (dd->is_written) mark_gpu(m);
                }
                dp_ticks[DP_IMAGE] += __rdtsc() - t_i;
            } else {
                continue;
            }
            uint64_t t_y = __rdtsc();
            if (!m) m = dummy_of(dd->type);
            img_layout(m, VK_IMAGE_LAYOUT_GENERAL);
            imgs_of[st][i] = m;
            dp_ticks[DP_LAYOUT] += __rdtsc() - t_y;
        }
    }
    DP_MARK(DP_IMGS);
    if (stage_off > SLOT_STAGE - (32ull << 20)) {   // room for the buffers without a flush
        flush();
        begin_cb();
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool, 1, &pp->dsl};
    if (pp->ndesc && vkAllocateDescriptorSets(D, &dai, &set) != VK_SUCCESS) {
        flush();
        begin_cb();
        VKCHECK(vkAllocateDescriptorSets(D, &dai, &set));
    }
    static VkWriteDescriptorSet wr[160];
    static VkDescriptorImageInfo ii[160];
    static VkDescriptorBufferInfo bi[160];
    int nw = 0;
    for (int st = 0; st < pp->nst; st++) {
        int ns = nvn_stage_of(pp->st[st].stage), sst = pp->st[st].stage;
        for (int i = 0; i < pp->st[st].ndesc && i < 96 && nw < 160; i++) {
            const Nx2XDesc *dd = &pp->st[st].desc[i];
            int slot = pp->slot[st][i];
            VkWriteDescriptorSet *w = &wr[nw];
            *w = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, dd->binding, 0, 1,
                                        desc_type(dd->kind)};
            if (dd->kind == NX2X_CBUF) {
                uint64_t n = dd->index < 18 && pp->st[st].cbuf_used_size[dd->index] ? pp->st[st].cbuf_used_size[dd->index]
                                                                                     : 0x10000;
                n = (n + 15) & ~15ull;
                if (n > 0x10000) n = 0x10000;
                VkDeviceSize off = stage_alloc(n, ubo_align);
                uint8_t *dst = stage_ptr + off;
                // zeroed below only where nothing is copied (the driver bank is filled sparsely: zeroed whole)
                if (dd->index == 0) memset(dst, 0, n);
                if (dd->index == 0 && pp->compute && pp->push_size && pp->push_off + pp->push_size <= n)
                    memcpy(dst + pp->push_off, s->push_cs, pp->push_size);   // compute push constants
                if (dd->index == 0) {   // the driver bank: storage buffer table and handles where the shader reads them
                    for (int j = 0; j < pp->st[st].ndesc && j < 96; j++) {
                        const Nx2XDesc *e = &pp->st[st].desc[j];
                        int sl = pp->slot[st][j];
                        if (e->cbuf_index != 0 || sl < 0 || e->kind == NX2X_CBUF) continue;
                        uint8_t entry[16] = {0};
                        size_t en = 8;
                        if (e->kind == NX2X_SSBO && sl < NX2_MAX_UBO) {
                            uint32_t sz = (uint32_t)s->ssbo[ns][sl].size;
                            memcpy(entry, &s->ssbo[ns][sl].addr, 8);
                            memcpy(entry + 8, &sz, 4);
                            en = 16;
                        } else if (e->kind == NX2X_TEXTURE && sl < NX2_MAX_TEX) {
                            memcpy(entry, &s->tex[ns][sl], 8);
                        } else if (e->kind == NX2X_IMAGE && sl < 8) {
                            memcpy(entry, &s->image[ns][sl], 8);
                        }
                        if (e->cbuf_offset + en <= n) memcpy(dst + e->cbuf_offset, entry, en);
                    }
                } else {
                    Nx2Range r = {0, 0};
                    if (dd->index == 1 && sst < 6) r = (Nx2Range){(uint64_t)(uintptr_t)cdata->data[sst], cdata->size[sst]};
                    else if (dd->index >= 3 && dd->index - 3 < NX2_MAX_UBO) r = s->ubo[ns][dd->index - 3];
                    uint64_t copied = r.addr && r.size ? (r.size < n ? r.size : n) : 0;
                    if (copied) memcpy(dst, (const void *)(uintptr_t)r.addr, copied);
                    if (copied < n) memset(dst + copied, 0, n - copied);
                }
                bi[nw] = (VkDescriptorBufferInfo){stage_buf, off, n};
                w->pBufferInfo = &bi[nw];
            } else if (dd->kind == NX2X_SSBO) {
                Nx2Range r = slot >= 0 && slot < NX2_MAX_UBO ? s->ssbo[ns][slot] : (Nx2Range){0, 0};
                VkBuffer ib;
                VkDeviceSize ioff;
                if (dd->is_written && dev_buf && r.addr && r.size && r.size <= (16u << 20) && nssbo_back < 64 &&
                    buffer_at(r.addr, r.size, ssbo_align, &ib, &ioff)) {   // written: a device copy (see DEV_SIZE)
                    VkDeviceSize doff = (dev_off + ssbo_align - 1) & ~(ssbo_align - 1);
                    if (doff + r.size <= DEV_SIZE) {
                        dev_off = doff + r.size;
                        end_rendering();
                        barrier_all();
                        VkBufferCopy cp = {ioff, dev_base() + doff, r.size};
                        vkCmdCopyBuffer(cb, ib, dev_buf, 1, &cp);
                        dp_ssbo_bytes += r.size, dp_ssbo_n++;
                        barrier_all();
                        ssbo_back[nssbo_back].dst = ib, ssbo_back[nssbo_back].dst_off = ioff;
                        ssbo_back[nssbo_back].imp = last_imp;   // its mirror, if it has one by the copy back
                        ssbo_back[nssbo_back].src_off = dev_base() + doff, ssbo_back[nssbo_back++].size = r.size;
                        bi[nw] = (VkDescriptorBufferInfo){dev_buf, dev_base() + doff, r.size};
                        w->pBufferInfo = &bi[nw];
                        nw++;
                        continue;
                    }
                }
                if (r.addr && r.size && buffer_at(r.addr, r.size, ssbo_align, &ib, &ioff)) {   // in place: no copy back
                    if (!dd->is_written) {
                        VkBuffer mb = mirror_of(last_imp);
                        if (mb) ib = mb;
                    } else if (last_imp && last_imp->mstate >= 0) {
                        // a shader writes this pool in place: no mirror for it (read in place from now on, so reads
                        // see the GPU's writes). NX2_MIRROR_FOLLOW=1: keep the mirror and copy the written range into it.
                        static int follow = -1;
                        if (follow < 0) follow = getenv("NX2_MIRROR_FOLLOW") != NULL;
                        if (follow && nmfix < 64) {
                            mfix[nmfix].imp = last_imp, mfix[nmfix].off = ioff, mfix[nmfix++].size = r.size;
                        } else {
                            static int nlog;
                            if (nlog++ < 16)
                                rt_log("vkdraw: pool %#llx (+%#llx) is written in place by shaders: not mirrored",
                                       (unsigned long long)last_imp->base, (unsigned long long)last_imp->size);
                            mirror_drop(last_imp);
                            for (int k = 0; k < nmirrors; k++)   // nor any pool overlapping the written memory
                                if (mirrors[k]->base < r.addr + r.size && r.addr < mirrors[k]->base + mirrors[k]->size)
                                    mirror_drop(mirrors[k]), k--;
                        }
                    }
                    bi[nw] = (VkDescriptorBufferInfo){ib, ioff, r.size};
                    w->pBufferInfo = &bi[nw];
                    nw++;
                    continue;
                }
                uint64_t n = r.size ? (r.size + 15) & ~15ull : 256;
                if (n > (64u << 20)) n = 64u << 20;
                VkDeviceSize off = stage_alloc(n, ssbo_align);
                memset(stage_ptr + off, 0, n);
                if (r.addr && r.size) memcpy(stage_ptr + off, (const void *)(uintptr_t)r.addr, r.size < n ? r.size : n);
                if (dd->is_written && r.addr && r.size && nwb < (int)(sizeof wb / sizeof *wb))
                    wb[nwb].off = off, wb[nwb].addr = r.addr, wb[nwb].size = r.size < n ? r.size : n, nwb++;
                bi[nw] = (VkDescriptorBufferInfo){stage_buf, off, n};
                w->pBufferInfo = &bi[nw];
            } else {
                Img *m = imgs_of[st][i];
                int storage = dd->kind == NX2X_IMAGE;
                VkImageView view = storage ? img_storage_view(m, dd->type) : img_view_nvn(m, dd->type, &views_of[st][i]);
                if (!view) {
                    m = dummy_of(dd->type);
                    view = storage ? img_storage_view(m, dd->type) : img_view(m, dd->type);
                }
                ii[nw] = (VkDescriptorImageInfo){storage ? VK_NULL_HANDLE : sampler_for(&smp_of[st][i], m->f.integer),
                                                 view, VK_IMAGE_LAYOUT_GENERAL};
                w->pImageInfo = &ii[nw];
                if (dd->count > 1) w->descriptorCount = 1;   // arrays: element 0 only for now
            }
            nw++;
        }
    }
    if (nw) vkUpdateDescriptorSets(D, (uint32_t)nw, wr, 0, NULL);
    *set_out = set;
    g_bound_tex = g_bound_wimg = NULL;   // the first sampled texture and written image (compute upscaler fallback)
    for (int st = 0; st < pp->nst; st++)
        for (int i = 0; i < pp->st[st].ndesc && i < 96; i++) {
            const Nx2XDesc *dd = &pp->st[st].desc[i];
            if (dd->kind == NX2X_TEXTURE && !g_bound_tex) g_bound_tex = imgs_of[st][i];
            if (dd->kind == NX2X_IMAGE && dd->is_written && !g_bound_wimg) g_bound_wimg = imgs_of[st][i];
        }
    if (tracing()) {
        size_t o = 0;
        trace_buf[0] = 0;
        for (int st = 0; st < pp->nst; st++)
            for (int i = 0; i < pp->st[st].ndesc && i < 96 && o < sizeof trace_buf - 64; i++) {
                const Nx2XDesc *dd = &pp->st[st].desc[i];
                if (dd->kind == NX2X_TEXTURE)
                {
                    o += (size_t)img_desc(trace_buf + o, sizeof trace_buf - o, "tex", imgs_of[st][i]);
                    const Nx2TexView *tv = &views_of[st][i];
                    if (imgs_of[st][i] && o < sizeof trace_buf - 40)   // the view's mip levels / layers, if it sets them
                        o += (size_t)snprintf(trace_buf + o, sizeof trace_buf - o, "[lv %d+%d of %u%s sw %d:%d%d%d%d tex %d%d%d%d]",
                                              tv->has_levels ? tv->base_level : 0, tv->has_levels ? tv->levels : 0,
                                              imgs_of[st][i]->levels, tv->has_layers ? " layers set" : "",
                                              tv->has_swizzle, tv->swizzle[0], tv->swizzle[1], tv->swizzle[2],
                                              tv->swizzle[3], imgs_of[st][i]->d.swizzle_r, imgs_of[st][i]->d.swizzle_g,
                                              imgs_of[st][i]->d.swizzle_b, imgs_of[st][i]->d.swizzle_a);
                }
                else if (dd->kind == NX2X_IMAGE)
                    o += (size_t)img_desc(trace_buf + o, sizeof trace_buf - o, dd->is_written ? "IMGw" : "img",
                                          imgs_of[st][i]);
                else if (dd->kind == NX2X_SSBO)
                    o += (size_t)snprintf(trace_buf + o, sizeof trace_buf - o, " ssbo%s", dd->is_written ? "w" : "");
                else if (dd->kind == NX2X_CBUF) {
                    int ns = nvn_stage_of(pp->st[st].stage);
                    int bound = dd->index >= 3 && dd->index - 3 < NX2_MAX_UBO && nx2_state.ubo[ns][dd->index - 3].size;
                    o += (size_t)snprintf(trace_buf + o, sizeof trace_buf - o, " s%d:c%u[%#x]%s", pp->st[st].stage,
                                          dd->index, dd->index < 18 ? pp->st[st].cbuf_used_size[dd->index] : 0,
                                          dd->index >= 3 && !bound ? "UNBOUND" : "");
                }
            }
    }
    return 1;
}

static uint64_t g_indirect;   // nx2_vk_draw_indirect: the address of the draw's numbers (the GPU reads them)

void nx2_vk_draw(const Nx2Draw *dr) {
    if (!active()) return;
    plat_mutex_lock(&lock);
    Nx2DrawState *s = &nx2_state;
    dp_t = __rdtsc();
    Nx2Program prog;
    if (!nx2_prog_get(s->program, &prog)) {
        skip("program unknown");
        goto out;
    }
    begin_cb();
    // render targets
    Img *rts[8] = {0};
    int nrt = s->rt.count > 8 ? 8 : s->rt.count;
    for (int i = 0; i < nrt; i++) {
        rts[i] = rt_image(i);
        if (rts[i] && (rts[i]->failed || !rts[i]->att_view)) rts[i] = NULL;
    }
    // depth-only passes (shadow maps) have no colour target: drawn with the depth target alone
    Img *depth_only = NULL;
    if (!nrt || !rts[0]) {
        Img *dz = s->rt.depth ? img_get(s->rt.depth) : NULL;
        if (nrt <= 1 && dz && !dz->failed && dz->att_view && dz->f.depth) {
            depth_only = dz;
            nrt = 0;
        } else {
            if (tracing())
                rt_log("trace draw #%u SKIPPED: no colour target (%d targets, colour %#llx, depth %#llx)", prog.serial,
                       nrt, (unsigned long long)s->rt.color[0], (unsigned long long)s->rt.depth);
            static int nlog;
            if (nlog++ < 8) {
                Img *c0 = nrt ? img_get(s->rt.color[0]) : NULL;
                rt_log("vkdraw: draw of #%u skipped: colour target %#llx (view format %#x, %d targets, mask %x) unusable (%s; %ux%ux%u layers %u target %d "
                       "format %#x failed %d)", prog.serial, (unsigned long long)s->rt.color[0], s->rt.view_format[0], s->rt.count, s->rs.mask[0],
                       !nrt ? "none" : rts[0] == NULL ? "image failed / no attachment view" : "?", c0 ? c0->w : 0,
                       c0 ? c0->h : 0, c0 ? c0->depth : 0, c0 ? c0->layers : 0, c0 ? c0->d.target : -1,
                       c0 ? c0->d.format : 0, c0 ? c0->failed : -1);
            }
            skip("no colour target");
            goto out;
        }
    }
    Img *size_ref = rts[0] ? rts[0] : depth_only;
    {   // NX2_DUMP_BEFORE=<serial>: images saved before that program's first draw on the screenshot frame
        static int done, init;
        static const char *db, *sf;
        if (!init) init = 1, db = getenv("NX2_DUMP_BEFORE"), sf = getenv("NX2_SHOT_FRAME");
        extern uint64_t nx2_vk_frame;
        if (db && sf && !done && (uint32_t)atoi(db) == prog.serial && nx2_vk_frame + 1 >= strtoull(sf, NULL, 10)) {
            done = 1;
            dump_images();
            rt_log("vkdraw: images dumped before #%u on frame %llu; exiting", prog.serial,
                   (unsigned long long)nx2_vk_frame + 1);
            fflush(stdout), fflush(stderr);
            plat_exit(0);   // the replay cannot go on after the dump's flush (it crashed); the log stays whole
        }
    }
    static char skip_list[256] = {1};
    if (skip_list[0] == 1) {
        const char *e = getenv("NX2_SKIP_PROGRAMS");
        snprintf(skip_list, sizeof skip_list, ",%s,", e ? e : "");
    }
    if (skip_list[1] != ',') {
        char me[16];
        snprintf(me, sizeof me, ",%u,", prog.serial);
        if (strstr(skip_list, me)) goto out;
    }
    {   // NX2_SKIP_COUNTS=<n,...>: draws of those vertex/index counts skipped (program numbers change between runs)
        static char counts[512] = {1};
        if (counts[0] == 1) {
            const char *e = getenv("NX2_SKIP_COUNTS");
            snprintf(counts, sizeof counts, ",%s,", e ? e : "");
        }
        if (counts[1] != ',') {
            char cn[24];
            snprintf(cn, sizeof cn, ",%d,", dr->count);
            if (strstr(counts, cn)) goto out;
        }
    }
    DP_MARK(DP_START);
    Pipe *pp = pipe_for(&prog, dr);
    if (!pp) {
        skip("no pipeline");
        goto out;
    }
    if (__atomic_load_n(&pp->ready, __ATOMIC_ACQUIRE) != 1) {   // NX2_ASYNC_SHADERS: still compiling (or failed)
        static uint64_t nskip;
        nskip++;
        goto out;
    }
    DP_MARK(DP_PIPE);
    ConstData cdata;
    const_data(&prog, &cdata);
    DP_MARK(DP_CONST);
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!bind_resources(pp, &cdata, &set)) {
        skip("resources not bound (texel buffers)");
        goto out;
    }
    DP_MARK(DP_DESC);
    // vertex and index data
    VkBuffer vbufs[16];
    VkDeviceSize voffs[16];
    uint32_t streams = 0;
    for (int k = 0; k < 16; k++)
        if ((pp->loads_generic & (1u << k)) && pp->key.attrib[k].format) streams |= 1u << (pp->key.attrib[k].stream & 15);
    for (int st = 0; st < 16; st++)
        if (streams & (1u << st)) {
            Nx2Range r = s->vb[st];
            if (r.addr && r.size && buffer_at(r.addr, r.size, 1, &vbufs[st], &voffs[st])) {   // in place
                dp_vtx_inplace += r.size;
                VkBuffer mb = mirror_of(last_imp);
                if (mb) vbufs[st] = mb;
                continue;
            }
            uint64_t n = r.size ? r.size : 16;
            if (n > (64u << 20)) n = 64u << 20;
            VkDeviceSize off = stage_alloc(n, 16);
            if (r.addr) memcpy(stage_ptr + off, (const void *)(uintptr_t)r.addr, n);
            else memset(stage_ptr + off, 0, n);
            vbufs[st] = stage_buf, voffs[st] = off;
            dp_vtx_copied += n, dp_vtx_copies++;
        }
    VkDeviceSize ioff = 0;
    VkBuffer ibuf = stage_buf;
    VkIndexType itype = VK_INDEX_TYPE_UINT16;
    if (g_indirect && dr->index_type == 0) {   // 8-bit indices are widened by a copy, which needs the count
        skip("indirect draw with 8-bit indices");
        goto out;
    }
    if (dr->index_type >= 0 && dr->index_type != 0 &&
        buffer_at(dr->index_addr, g_indirect ? (dr->index_type == 1 ? 2u : 4u) : (uint64_t)dr->count * (dr->index_type == 1 ? 2 : 4),
                  dr->index_type == 1 ? 2 : 4, &ibuf, &ioff)) {   // 16 / 32-bit indices in place, in the game's memory pool
        dp_idx_inplace += (uint64_t)dr->count * (dr->index_type == 1 ? 2 : 4);
        VkBuffer mb = mirror_of(last_imp);
        if (mb) ibuf = mb;
        itype = dr->index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    } else if (g_indirect && dr->index_type >= 0) {
        skip("indirect draw whose indices cannot be read in place");
        goto out;
    } else if (dr->index_type >= 0) {
        ibuf = stage_buf;
        int isz = dr->index_type == 0 ? 1 : dr->index_type == 1 ? 2 : 4;
        uint64_t n = (uint64_t)dr->count * (isz == 1 ? 2 : isz);
        dp_idx_copied += n;
        ioff = stage_alloc(n, 4);
        const uint8_t *src = (const uint8_t *)(uintptr_t)dr->index_addr;
        if (isz == 1) {   // Vulkan has no 8-bit indices without an extension: widen
            uint16_t *o = (uint16_t *)(stage_ptr + ioff);
            for (int i = 0; i < dr->count; i++) o[i] = src[i];
        } else {
            memcpy(stage_ptr + ioff, src, n);
        }
        itype = isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    }
    DP_MARK(DP_VTX);
    // targets, then the draw
    for (int i = 0; i < nrt; i++)
        if (rts[i]) {
            img_layout(rts[i], VK_IMAGE_LAYOUT_GENERAL);
            mark_gpu(rts[i]);
            static int n3d;
            if (rts[i]->depth > 1 && n3d++ < 40)   // 3D targets: how the game addresses their slices
                rt_log("vkdraw: draw of #%u into 3D target %d (%ux%ux%u): mode %d, %d vertices, %d instances, view "
                       "format %#x, frame %llu", prog.serial, i, rts[i]->w, rts[i]->h, rts[i]->depth, dr->mode, dr->count,
                       dr->instances, nx2_state.rt.view_format[i], (unsigned long long)nx2_vk_frame);
        }
    Img *dimg = NULL;
    if (pp->key.depth != VK_FORMAT_UNDEFINED) {
        dimg = img_get(s->rt.depth);
        if (dimg && !dimg->failed && dimg->att_view) {
            img_layout(dimg, VK_IMAGE_LAYOUT_GENERAL);
            mark_gpu(dimg);
        } else {
            dimg = NULL;
        }
    }
    if (!dimg && pp->key.depth != VK_FORMAT_UNDEFINED) {
        skip("depth target missing");
        goto out;
    }
    {
        RtSel sel[8];
        for (int i = 0; i < nrt; i++) sel[i] = rt_sel(i);
        RtSel dsel = {0, s->rt.depth_layer, s->rt.depth_level};
        begin_rendering(rts, sel, nrt, dimg, dsel);
    }
    {   // NX2_DEBUG_COUNT=<n>: vertex input of the first draws with that vertex / index count
        static int dbg_count = -2, dbg_cn;
        if (dbg_count == -2) dbg_count = getenv("NX2_DEBUG_COUNT") ? atoi(getenv("NX2_DEBUG_COUNT")) : -1;
        if (dr->count == dbg_count && dbg_cn++ < 4) {
            char line[1024];
            size_t o = 0;
            for (int k = 0; k < 16 && o < sizeof line - 80; k++)
                if (pp->loads_generic & (1u << k))
                    o += (size_t)snprintf(line + o, sizeof line - o, " a%d:fmt %#x s%d +%d", k, pp->key.attrib[k].format,
                                          pp->key.attrib[k].stream, pp->key.attrib[k].offset);
            for (int st = 0; st < 16 && o < sizeof line - 80; st++)
                if (pp->key.stream[st].stride || s->vb[st].addr)
                    o += (size_t)snprintf(line + o, sizeof line - o, " | s%d stride %d div %d vb %#llx+%#llx", st,
                                          pp->key.stream[st].stride, pp->key.stream[st].divisor,
                                          (unsigned long long)s->vb[st].addr, (unsigned long long)s->vb[st].size);
            rt_log("vkdraw debug count %d (#%u, generic %#x):%s", dr->count, prog.serial, pp->loads_generic, line);
        }
    }
    {   // NX2_DEBUG_PROG=<serial>: the state of that program's first draws
        static int dbg_serial = -2, dbg_n;
        if (dbg_serial == -2) dbg_serial = getenv("NX2_DEBUG_PROG") ? atoi(getenv("NX2_DEBUG_PROG")) : -1;
        if ((int)prog.serial == dbg_serial && dbg_n++ < 3) {
            const Nx2Raster *R = &pp->key.rs;
            rt_log("vkdraw debug #%u: rt %#llx %ux%u fmt %d, depth fmt %d; mask %x blend_en %x (%d %d %d %d eq %d) "
                   "depth %d/%d/%d cull %d front %d poly %d; viewport %d %d %d %d range %g %g scissor %d %d %d %d; "
                   "mode %d count %d itype %d; vertex attribs used %#x", prog.serial,
                   (unsigned long long)size_ref->obj, size_ref->w, size_ref->h, (int)size_ref->fmt, (int)pp->key.depth,
                   R->mask[0], R->blend_enable, R->blend[0].src, R->blend[0].dst, R->blend[0].srca, R->blend[0].dsta,
                   R->blend[0].eq, R->depth_test, R->depth_write, R->depth_func, R->cull, R->front_ccw,
                   R->polygon_mode, s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3],
                   s->depth_range[0], s->depth_range[1], s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3],
                   dr->mode, dr->count, dr->index_type, pp->loads_generic);
            rt_log("vkdraw debug #%u: pipeline %d colour targets (rendering %d), %d stages, masks %x %x, formats %d %d",
                   prog.serial, pp->key.ncolor, nrt, pp->nst, R->mask[0], R->mask[1], (int)pp->key.color[0],
                   (int)pp->key.color[1]);
        }
    }
    occ_note_draw(prog.serial, &pp->key.rs, dimg != NULL, size_ref->w, size_ref->h);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
    vkCmdSetBlendConstants(cb, s->blend_color);
    for (int f = 0; f < 2; f++) {
        VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        vkCmdSetStencilCompareMask(cb, face, s->stencil_vmask[f]);
        vkCmdSetStencilWriteMask(cb, face, s->stencil_wmask[f]);
        vkCmdSetStencilReference(cb, face, s->stencil_ref[f]);
    }
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->layout, 0, 1, &set, 0, NULL);
    float area[4] = {(float)size_ref->w, (float)size_ref->h, 0, 0};
    vkCmdPushConstants(cb, pp->layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16, area);
    VkViewport vp = {(float)s->viewport[0], (float)s->viewport[1], (float)s->viewport[2], (float)s->viewport[3],
                     s->depth_range[0], s->depth_range[1]};
    if (vp.width <= 0 || vp.height <= 0) vp.x = vp.y = 0, vp.width = (float)cur_w, vp.height = (float)cur_h;
    if (y_flip()) vp.y += vp.height, vp.height = -vp.height;   // NVN upper-left origin: clip y = +1 on row 0
    if (vp.minDepth < 0 || vp.minDepth > 1) vp.minDepth = 0;
    if (vp.maxDepth < 0 || vp.maxDepth > 1) vp.maxDepth = 1;
    vkCmdSetViewport(cb, 0, 1, &vp);
    VkRect2D sc = {{s->scissor[0], s->scissor[1]}, {(uint32_t)s->scissor[2], (uint32_t)s->scissor[3]}};
    if (s->scissor[2] <= 0 || s->scissor[3] <= 0) sc = (VkRect2D){{0, 0}, {cur_w, cur_h}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    for (int st = 0; st < 16; st++)
        if (streams & (1u << st)) vkCmdBindVertexBuffers(cb, (uint32_t)st, 1, &vbufs[st], &voffs[st]);
    int instances = dr->instances > 0 ? dr->instances : 1;
    int gq = gp_begin();
    VkBuffer abuf;
    VkDeviceSize aoff;
    if (g_indirect && buffer_at(g_indirect, dr->index_type >= 0 ? 20 : 16, 4, &abuf, &aoff)) {
        if (dr->index_type >= 0) {
            vkCmdBindIndexBuffer(cb, ibuf, ioff, itype);
            vkCmdDrawIndexedIndirect(cb, abuf, aoff, 1, 20);
        } else {
            vkCmdDrawIndirect(cb, abuf, aoff, 1, 16);
        }
        static int n;
        if (n++ < 4) {
            const uint32_t *a = (const uint32_t *)(uintptr_t)g_indirect;
            rt_log("vkdraw: indirect draw #%u (mode %d, index type %d): numbers at %#llx, now %u %u %u %d %u",
                   prog.serial, dr->mode, dr->index_type, (unsigned long long)g_indirect, a[0], a[1], a[2], (int)a[3],
                   dr->index_type >= 0 ? a[4] : 0);
        }
    } else if (g_indirect) {
        static int n;
        if (n++ < 4) rt_log("vkdraw: indirect draw #%u: its numbers at %#llx cannot be read in place", prog.serial,
                            (unsigned long long)g_indirect);
    } else if (dr->index_type >= 0) {
        vkCmdBindIndexBuffer(cb, ibuf, ioff, itype);
        vkCmdDrawIndexed(cb, (uint32_t)dr->count, (uint32_t)instances, 0, dr->base_vertex, (uint32_t)dr->base_instance);
    } else {
        vkCmdDraw(cb, (uint32_t)dr->count, (uint32_t)instances, (uint32_t)dr->first, (uint32_t)dr->base_instance);
    }
    gp_end(gq, prog.serial, cur_w, cur_h, 0);
    {   // NX2_LOG_FINAL=<from present>: the constants of the final 4K output pass (black-frame debug)
        static int64_t from = -2;
        if (from == -2) from = getenv("NX2_LOG_FINAL") ? atoll(getenv("NX2_LOG_FINAL")) : -1;
        if (from >= 0 && (int64_t)nx2_vk_frame + 1 >= from && (int64_t)nx2_vk_frame < from + 200 && nrt == 1 && rts[0] &&
            rts[0]->w == 3840 && rts[0]->fmt == VK_FORMAT_R8G8B8A8_UNORM) {
            for (int ns = 0; ns < 6; ns++) {
                Nx2Range r = s->ubo[ns][0];
                if (!r.addr || !r.size) continue;
                const float *f = (const float *)(uintptr_t)r.addr;
                char line[640];
                size_t o = 0;
                for (int i = 0; i < 24 && (uint64_t)i * 4 < r.size; i++)
                    o += (size_t)snprintf(line + o, sizeof line - o, " %g", f[i]);
                rt_log("final pass: present %llu #%u stage %d ubo %#llx+%#llx:%s", (unsigned long long)nx2_vk_frame + 1,
                       prog.serial, ns, (unsigned long long)r.addr, (unsigned long long)r.size, line);
            }
        }
    }
    static int log_labels = -1;
    if (log_labels < 0) log_labels = getenv("NX2_LOG_LABELS") != NULL;
    if (log_labels && size_ref && size_ref->w == 400 && size_ref->h == 240) {   // debug: label renders
        static int n;
        if (n++ < 200)
            rt_log("label draw: frame %llu program #%u count %d inst %d target %#llx fmt %d @%p", (unsigned long long)nx2_vk_frame + 1,
                   prog.serial, dr->count, instances, (unsigned long long)size_ref->obj, (int)size_ref->fmt, (void *)size_ref->storage);
    }
    ssbo_writeback();
    enc_flush();
    g_draws_done++;
    flush_chunk();
    DP_MARK(DP_CMD);
    if (tracing()) {
        char rt[256];
        size_t o = 0;
        for (int i = 0; i < nrt && o < sizeof rt - 48; i++) o += (size_t)img_desc(rt + o, sizeof rt - o, "RT", rts[i]);
        if (dimg) o += (size_t)img_desc(rt + o, sizeof rt - o, "Z", dimg);
        rt_log("trace draw #%u mode %d n %d inst %d blend %x mask %x ztest %d zfunc %d zwrite %d:%s |%s", prog.serial,
               dr->mode, dr->count, dr->instances, pp->key.rs.blend_enable, pp->key.rs.mask[0], pp->key.rs.depth_test,
               pp->key.rs.depth_func, pp->key.rs.depth_write, rt, trace_buf);
    }
    // NX2_SYNC_DRAWS=1: submit and wait after every draw, naming it first (finds the draw that hangs the GPU)
    static int sync = -1;
    if (sync < 0) sync = getenv("NX2_SYNC_DRAWS") != NULL;
    if (sync) {
        rt_log("vkdraw: draw %llu: program #%u mode %d count %d, %d targets (%ux%u)", (unsigned long long)g_draws_done,
               prog.serial, dr->mode, dr->count, nrt, size_ref->w, size_ref->h);
        flush_wait();
    }
    // NX2_SKIP_PROGRAMS=a,b,...: program serials not drawn
out:
    plat_mutex_unlock(&lock);
}

// ClearColor: NVN clears inside the scissor rectangle and only the channels of mask (bit 0 R .. bit 3 A). A full-
// image clear of all channels is vkCmdClearColorImage; a partial one is vkCmdClearAttachments in a rendering pass
// with that target (channel masks other than 0 and 0xf are cleared as 0xf for now).
void nx2_vk_clear_color(int index, const float *rgba, int mask) {
    if (!active() || !(mask & 0xf)) return;
    plat_mutex_lock(&lock);
    if (index >= 0 && index < nx2_state.rt.count) {
        Img *m = rt_image(index);
        if (m && !m->failed && !m->f.depth) {
            begin_cb();
            end_rendering();
            barrier_all();
            img_layout(m, VK_IMAGE_LAYOUT_GENERAL);
            VkClearColorValue v;
            memcpy(v.float32, rgba, 16);
            if (getenv("NX2_CLEAR_DEBUG")) v.float32[0] = 1, v.float32[1] = 0, v.float32[2] = 1, v.float32[3] = 1;
            static int logged;
            if (logged++ < 6)
                rt_log("vkdraw: clear %#llx (%ux%u) to %g %g %g %g", (unsigned long long)m->obj, m->w, m->h, rgba[0],
                       rgba[1], rgba[2], rgba[3]);
            const int *sc = nx2_state.scissor;
            RtSel sel = rt_sel(index);
            sel_clamp(m, &sel);
            int lw = (int)lvl_dim(m->w, sel.level), lh = (int)lvl_dim(m->h, sel.level);
            int full = sc[2] <= 0 || sc[3] <= 0 || (sc[0] <= 0 && sc[1] <= 0 && sc[0] + sc[2] >= lw &&
                                                    sc[1] + sc[3] >= lh);
            if (m->type == VK_IMAGE_TYPE_3D) sel.layer = 0;   // one array layer: a clear covers every slice
            if (full || !m->att_view) {
                VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)sel.level, 1, (uint32_t)sel.layer, 1};
                vkCmdClearColorImage(cb, m->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
            } else {
                Img *one[1] = {m};
                RtSel one_sel[1] = {rt_sel(index)}, nod = {0, 0, 0};
                begin_rendering(one, one_sel, 1, NULL, nod);
                int x0 = sc[0] < 0 ? 0 : sc[0], y0 = sc[1] < 0 ? 0 : sc[1];
                int x1 = sc[0] + sc[2] > lw ? lw : sc[0] + sc[2];
                int y1 = sc[1] + sc[3] > lh ? lh : sc[1] + sc[3];
                if (x1 > x0 && y1 > y0) {
                    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = v}};
                    VkClearRect cr = {{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, 1};
                    vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
                }
                end_rendering();
            }
            if (tracing()) {
                char d[96];
                img_desc(d, sizeof d, "clear", m);
                rt_log("trace%s to %g %g %g %g mask %#x scissor %d,%d %dx%d", d, rgba[0], rgba[1], rgba[2], rgba[3], mask,
                       sc[0], sc[1], sc[2], sc[3]);
            }
            mark_gpu(m);
            enc_flush();
        }
    }
    plat_mutex_unlock(&lock);
}

void nx2_vk_clear_depth(float depth, int depth_mask, int stencil, int stencil_mask) {
    stencil_mask &= 0xff;
    if (!active() || (!depth_mask && !stencil_mask)) return;
    plat_mutex_lock(&lock);
    Img *m = nx2_state.rt.depth ? img_get(nx2_state.rt.depth) : NULL;
    if (m && !m->failed && m->f.depth) {
        begin_cb();
        end_rendering();
        barrier_all();
        img_layout(m, VK_IMAGE_LAYOUT_GENERAL);
        VkClearDepthStencilValue v = {depth, (uint32_t)stencil & 0xff};
        RtSel ds = {0, nx2_state.rt.depth_layer, nx2_state.rt.depth_level};
        sel_clamp(m, &ds);
        VkImageAspectFlags asp = 0;   // what the masks allow (a partial stencil mask clears all its bits)
        if (depth_mask) asp |= m->all_aspect & VK_IMAGE_ASPECT_DEPTH_BIT;
        if (stencil_mask) asp |= m->all_aspect & VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!asp) {
            plat_mutex_unlock(&lock);
            return;
        }
        VkImageSubresourceRange r = {asp, (uint32_t)ds.level, 1, (uint32_t)ds.layer, 1};
        vkCmdClearDepthStencilImage(cb, m->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        mark_gpu(m);
    }
    plat_mutex_unlock(&lock);
}

void nx2_vk_submit_end(void) {
    if (!V || disabled) return;
    plat_mutex_lock(&lock);
    mirror_epoch++;   // the next submit syncs the mirrors first
    flush();
    plat_mutex_unlock(&lock);
}

static void dump_images(void);

// The window texture rendered on the GPU, if it was: presented from its image.
int nx2_vk_present_texture(uint64_t obj, uint64_t frame) {
    if (rt_trace & RT_TRACE_SCHED) kern_sched_report(frame);   // TOTK_TRACE=sched: time per guest thread
    {
        static int64_t want = -2;
        if (want == -2) want = getenv("NX2_GPU_PROF") ? atoll(getenv("NX2_GPU_PROF")) : -1;
        if ((int64_t)frame == want && V && !disabled) {   // the profiled present: its work is done once flushed
            plat_mutex_lock(&lock);
            flush_wait();
            plat_mutex_unlock(&lock);
            gp_report();
        }
    }
    nx2_vk_frame = frame;
    if (V && !disabled) occ_dump();
    {
        static uint64_t t_last, f_last, d_last;
        uint64_t now = plat_now_ns();
        if (!t_last) t_last = now, f_last = frame;
        static int slow_ms = -1;   // NX2_SLOW_LOG=<ms> (debug): where the time of each frame slower than that went
        if (slow_ms < 0) slow_ms = getenv("NX2_SLOW_LOG") ? atoi(getenv("NX2_SLOW_LOG")) : 0;
        static uint64_t s_now, s_sub, s_wait, s_pipe, s_pipes, s_x, s_vk, s_th, s_mp, s_pace, s_fl;
        if (slow_ms > 0) {
            if (s_now && now - s_now > (uint64_t)slow_ms * 1000000ull)
                rt_log("slow frame %llu: %.1f ms: submits %.1f (GPU waits %.1f, %llu flushes), pipelines %.1f (%llu built: "
                       "translation %.1f, driver %.1f), pacing %.1f; textures hashed %.1f MiB, mirrors copied %.1f MiB",
                       (unsigned long long)frame, (now - s_now) * 1e-6, (nx2_prof_submit_ns - s_sub) * 1e-6,
                       (nx2_prof_wait_ns - s_wait) * 1e-6, (unsigned long long)(nx2_prof_flushes - s_fl),
                       (nx2_prof_pipe_ns - s_pipe) * 1e-6, (unsigned long long)(nx2_prof_pipes - s_pipes),
                       (prof_xlate_ns - s_x) * 1e-6, (prof_vkpipe_ns - s_vk) * 1e-6, (nx2_prof_pace_ns - s_pace) * 1e-6,
                       (th_bytes - s_th) / 1048576.0, (mp_copied - s_mp) / 1048576.0);
            s_now = now, s_sub = nx2_prof_submit_ns, s_wait = nx2_prof_wait_ns, s_pipe = nx2_prof_pipe_ns;
            s_pipes = nx2_prof_pipes, s_x = prof_xlate_ns, s_vk = prof_vkpipe_ns, s_th = th_bytes, s_mp = mp_copied;
            s_pace = nx2_prof_pace_ns, s_fl = nx2_prof_flushes;
        }
        if (frame - f_last >= 300) {
            s_sub = s_wait = s_pipe = s_pipes = s_x = s_vk = s_th = s_mp = s_pace = s_fl = 0;   // the counters restart
            double n = (double)(frame - f_last), ms = 1e-6 / n;
            rt_log("prof: frames %llu-%llu: %.1f ms/frame (%.1f fps): submits %.1f (GPU waits %.1f in %.1f flushes), "
                   "pipelines %.1f (%.1f built), pacing %.1f, rest %.1f; %.0f draws/frame",
                   (unsigned long long)f_last, (unsigned long long)frame, (double)(now - t_last) * ms,
                   n * 1e9 / (double)(now - t_last), nx2_prof_submit_ns * ms, nx2_prof_wait_ns * ms,
                   nx2_prof_flushes / n, nx2_prof_pipe_ns * ms, nx2_prof_pipes / n, nx2_prof_pace_ns * ms,
                   (double)(now - t_last - nx2_prof_submit_ns - nx2_prof_pace_ns) * ms,
                   (double)(g_draws_done - d_last) / n);
            {   // draw phases: TSC ticks to ns with this interval's clock
                static uint64_t tsc_last;
                uint64_t tsc = __rdtsc();
                if (tsc_last) {
                    double per_ns = (double)(tsc - tsc_last) / (double)(now - t_last), tot = 0;
                    char line[512];
                    size_t o = 0;
                    for (int k = 0; k < DP_UPLOAD; k++) tot += (double)dp_ticks[k];
                    for (int k = 0; k < DP_N; k++)
                        o += (size_t)snprintf(line + o, sizeof line - o, " %s %.2f", dp_name[k],
                                              (double)dp_ticks[k] / per_ns * ms);
                    rt_log("prof:   pipeline builds: translation %.2f ms/frame, driver %.2f ms/frame", prof_xlate_ns * ms,
                           prof_vkpipe_ns * ms);
                    prof_xlate_ns = prof_vkpipe_ns = 0;
                    {
                        void nx2_xcache_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *);
                        uint64_t xh, xm, xr, xl;
                        nx2_xcache_stats(&xh, &xm, &xr, &xl);
                        rt_log("prof:   translation cache: %llu hits, %llu translated, %llu refused (other answers); %llu "
                               "loaded", (unsigned long long)xh, (unsigned long long)xm, (unsigned long long)xr,
                               (unsigned long long)xl);
                    }
                    rt_log("prof:   mirrors: %d pools, %.0f MiB; %.2f MiB/frame copied in %.0f regions, %.1f syncs/frame",
                           nmirrors, mirror_bytes / 1048576.0, (double)mp_copied / n / 1048576.0, (double)mp_regions / n,
                           (double)mp_syncs / n);
                    mp_copied = mp_regions = mp_syncs = 0;
                    rt_log("prof:   written storage buffers: %.1f/frame, %.1f MiB/frame copied in and out (device ring)",
                           (double)dp_ssbo_n / n, (double)dp_ssbo_bytes / n / 1048576.0);
                    dp_ssbo_n = dp_ssbo_bytes = 0;
                    rt_log("prof:   texture checks: %.1f/frame hashed (%.1f MiB/frame), %.1f/frame unchanged by frozen "
                           "memory; largest hashed %ux%u format %#x (%.1f MiB)", (double)th_n / n,
                           (double)th_bytes / n / 1048576.0, (double)th_skipped / n, th_big_w, th_big_h, th_big_f,
                           th_big / 1048576.0);
                    th_n = th_bytes = th_big = th_skipped = 0;
                    uint64_t fz, fzs, fth, oth;
                    mem_freeze_stats(&fz, &fzs, &fth, &oth);
                    rt_log("prof:   frozen memory: %llu regions now; per frame %.1f freezes, %.1f thawed by writes, %.1f by "
                           "other means", (unsigned long long)fz, (double)fzs / n, (double)fth / n, (double)oth / n);
                    rt_log("prof:   draws %.2f ms/frame:%s; vertex copies %.0f/frame (%.2f MiB), index copies %.2f MiB; in place: vertex ranges %.1f MiB, indices %.1f MiB",
                           tot / per_ns * ms, line, (double)dp_vtx_copies / n, (double)dp_vtx_copied / n / 1048576.0,
                           (double)dp_idx_copied / n / 1048576.0, (double)dp_vtx_inplace / n / 1048576.0,
                           (double)dp_idx_inplace / n / 1048576.0);
                }
                tsc_last = tsc;
                memset(dp_ticks, 0, sizeof dp_ticks);
                dp_vtx_copied = dp_vtx_copies = dp_idx_copied = dp_vtx_inplace = dp_idx_inplace = 0;
            }
            void nx2_proc_prof_report(double n);
            nx2_proc_prof_report(n);
            t_last = now, f_last = frame, d_last = g_draws_done;
            nx2_prof_wait_ns = nx2_prof_pipe_ns = nx2_prof_submit_ns = nx2_prof_pace_ns = 0;
            nx2_prof_flushes = nx2_prof_pipes = 0;
        }
    }
    if (!V || disabled) return 0;
    plat_mutex_lock(&lock);
    flush();
    {   // The GPU work of a present is finished before the game goes on with the next frame (NX2_FRAMES_AHEAD=0, the
        // default; n: the work of present - n). The game reads what the GPU wrote into its memory during the previous
        // frame, which the console's GPU has long finished; with the mirrors the GPU got fast, the CPU ran ahead and
        // read half-written results (one black frame when the player was splatted; none with this wait, and none
        // before the mirrors, when the GPU was the bottleneck). 8 or more: no wait.
        static int ahead = -2;
        static uint64_t ser[8];
        if (ahead == -2) ahead = getenv("NX2_FRAMES_AHEAD") ? atoi(getenv("NX2_FRAMES_AHEAD")) : 0;
        ser[frame % 8] = submit_serial;
        if (ahead >= 0 && ahead < 8 && frame >= (uint64_t)ahead) {
            uint64_t want = ser[(frame - (uint64_t)ahead) % 8];
            while (done_serial < want) {
                int oldest = -1;
                for (int k = 0; k < NSLOT; k++)
                    if (slots_[k].pending && (oldest < 0 || slots_[k].serial < slots_[oldest].serial)) oldest = k;
                if (oldest < 0) break;
                slot_retire(oldest);
            }
        }
    }
    const char *sf = getenv("NX2_SHOT_FRAME");
    if (getenv("NX2_SHOT_ALL") && sf && strtoull(sf, NULL, 10) == frame) dump_images();
    {   // NX2_BLACK_CHECK_FROM=<present>: from then on, an RGBA8 output whose upper right part is mostly black has
        // its frame's GPU images dumped right away (debug of black frames; the next frame is often fine)
        static int64_t from = -2;
        if (from == -2) from = getenv("NX2_BLACK_CHECK_FROM") ? atoll(getenv("NX2_BLACK_CHECK_FROM")) : -1;
        Img *o = from >= 0 && (int64_t)frame >= from ? img_find(obj, 0) : NULL;
        if (o && o->gpu && !o->failed && o->fmt == VK_FORMAT_R8G8B8A8_UNORM) {
            uint32_t rows = o->h * 45 / 100;
            VkDeviceSize bytes = (VkDeviceSize)o->w * rows * 4;
            begin_cb();
            end_rendering();
            barrier_all();
            img_layout(o, VK_IMAGE_LAYOUT_GENERAL);
            VkDeviceSize off = stage_alloc(bytes, 16);
            VkBufferImageCopy cp = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {o->w, rows, 1}};
            vkCmdCopyImageToBuffer(cb, o->img, VK_IMAGE_LAYOUT_GENERAL, stage_buf, 1, &cp);
            flush_wait();
            const uint8_t *px = stage_ptr + off;
            uint64_t n = 0, black = 0;
            for (uint32_t y = 0; y < rows; y += 8)
                for (uint32_t x = o->w * 28 / 100; x < o->w; x += 8, n++) {
                    const uint8_t *q = px + ((uint64_t)y * o->w + x) * 4;
                    black += q[0] + q[1] + q[2] < 15;
                }
            if (n && black * 5 > n) {
                rt_log("vkdraw: present #%llu is %.0f%% black at the top: dumping its images", (unsigned long long)frame,
                       100.0 * (double)black / (double)n);
                nx2_dump_request = 1;
            }
        }
    }
    if (nx2_dump_request) {   // NX2_DUMP_ON_BLACK (nx2_vk.c) / NX2_BLACK_CHECK_FROM
        flush_wait();
        dump_images();
        rt_log("vkdraw: images of present #%llu dumped (black screen); exiting", (unsigned long long)frame);
        fflush(stdout), fflush(stderr);
        plat_exit(0);
    }
    Img *m = img_find(obj, 0);
    int ok = m && m->gpu && !m->failed;
    if (ok) {
        void nx2_present_source_view(VkImageView v);   // for FSR on the present (nx2_vk.c)
        nx2_present_source_view(img_view(m, NX2X_T_2D));
        nx2_present_image(m->img, &m->layout, m->w, m->h, frame);
    }
    if (frame % 600 == 1)
        rt_log("vkdraw: %llu draws done, %llu skipped, %llu pipelines (%llu failed), %llu images (%.1f MiB)",
               (unsigned long long)g_draws_done, (unsigned long long)g_draws_skipped, (unsigned long long)pipe_count,
               (unsigned long long)pipe_failed, (unsigned long long)img_count, img_bytes / 1048576.0);
    plat_mutex_unlock(&lock);
    return ok;
}

// ---- NX2_SHOT_ALL=1: on the screenshot frame, every 2D image the GPU wrote is saved as gpu_<object>.png
static float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * (float)(1 << e) / 32768.0f;
    return s ? -v : v;
}
static float uf11(uint32_t v, int mbits) {   // unsigned 11/10-bit float (5-bit exponent)
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (!e) return m / (float)(1 << mbits) / 16384.0f;
    if (e == 31) return 65504.0f;
    return (1.0f + m / (float)(1 << mbits)) * (float)(1 << e) / 32768.0f;
}
static uint8_t to8(float v) { return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); }

static void dump_images(void) {
    int n = 0;
    for (int b = 0; b < IMG_BUCKETS; b++)   // small GPU-written 3D images (colour grading tables): raw, every slice
        for (Img *m = imgs[b]; m; m = m->next) {
            if (!m->gpu || m->failed || m->type != VK_IMAGE_TYPE_3D || m->f.depth) continue;
            int bw = 1, bh = 1, bpp = nvn_fmt_bpb(m->d.format, &bw, &bh);
            VkDeviceSize bytes = (VkDeviceSize)m->w * m->h * m->depth * (bpp > 0 ? bpp : 0);
            if (bw != 1 || bh != 1 || !bytes || bytes > (4u << 20)) continue;
            begin_cb();
            end_rendering();
            barrier_all();
            img_layout(m, VK_IMAGE_LAYOUT_GENERAL);
            VkDeviceSize off = stage_alloc(bytes, 16);
            VkBufferImageCopy cp = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {m->w, m->h, m->depth}};
            vkCmdCopyImageToBuffer(cb, m->img, VK_IMAGE_LAYOUT_GENERAL, stage_buf, 1, &cp);
            flush_wait();
            char path[96];
            snprintf(path, sizeof path, "gpu3d_%llx_%ux%ux%u_f%d_vk%d.bin", (unsigned long long)m->obj & 0xfffffff, m->w, m->h,
                     m->depth, m->d.format, (int)m->fmt);
            FILE *f = fopen(path, "wb");
            if (f) fwrite(stage_ptr + off, 1, (size_t)bytes, f), fclose(f);
        }
    for (int b = 0; b < IMG_BUCKETS; b++)
        for (Img *m = imgs[b]; m; m = m->next) {
            if (!m->gpu || m->failed || m->type != VK_IMAGE_TYPE_2D || m->f.depth) continue;
            int bw = 1, bh = 1, bpp = nvn_fmt_bpb(m->d.format, &bw, &bh);   // bytes per texel of the format
            if (bw != 1 || bh != 1 || bpp <= 0 || bpp > 16 || m->f.astc_bw) continue;   // compressed: not dumped
            uint32_t rows = m->h;   // large images: the top rows that fit in half the staging ring
            if ((VkDeviceSize)m->w * rows * bpp > SLOT_STAGE / 2) rows = (uint32_t)(SLOT_STAGE / 2 / ((VkDeviceSize)m->w * bpp));
            if (!rows) continue;
            VkDeviceSize bytes = (VkDeviceSize)m->w * rows * bpp;
            begin_cb();
            end_rendering();
            barrier_all();
            img_layout(m, VK_IMAGE_LAYOUT_GENERAL);
            VkDeviceSize off = stage_alloc(bytes, 16);
            VkBufferImageCopy cp = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {m->w, rows, 1}};
            vkCmdCopyImageToBuffer(cb, m->img, VK_IMAGE_LAYOUT_GENERAL, stage_buf, 1, &cp);
            flush_wait();
            uint8_t *rgba = malloc((size_t)m->w * rows * 4);
            const uint8_t *src = stage_ptr + off;   // flush() resets the ring but keeps its contents
            for (size_t i = 0; i < (size_t)m->w * rows; i++) {
                uint8_t *o = rgba + i * 4;
                o[3] = 255;
                switch (m->fmt) {
                case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
                    memcpy(o, src + i * 4, 4);
                    break;
                case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
                    o[0] = src[i * 4 + 2], o[1] = src[i * 4 + 1], o[2] = src[i * 4];
                    break;
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
                    uint32_t v;
                    memcpy(&v, src + i * 4, 4);
                    o[0] = (uint8_t)((v & 0x3ff) >> 2), o[1] = (uint8_t)((v >> 10 & 0x3ff) >> 2);
                    o[2] = (uint8_t)((v >> 20 & 0x3ff) >> 2);
                    break;
                }
                case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
                    uint32_t v;
                    memcpy(&v, src + i * 4, 4);
                    o[0] = to8(uf11(v & 0x7ff, 6)), o[1] = to8(uf11(v >> 11 & 0x7ff, 6));
                    o[2] = to8(uf11(v >> 22 & 0x3ff, 5));
                    break;
                }
                case VK_FORMAT_R16G16B16A16_SFLOAT: {
                    uint16_t h[4];
                    memcpy(h, src + i * 8, 8);
                    for (int k = 0; k < 3; k++) o[k] = to8(half_to_float(h[k]));
                    break;
                }
                case VK_FORMAT_R8_UNORM: o[0] = o[1] = o[2] = src[i]; break;
                default: {
                    uint32_t v;   // unknown: raw bytes as grey
                    memcpy(&v, src + i * bpp, bpp < 4 ? bpp : 4);
                    o[0] = o[1] = o[2] = (uint8_t)v;
                }
                }
            }
            char path[96];
            snprintf(path, sizeof path, "gpu_%llx_%ux%u_f%d_@%llx.png", (unsigned long long)m->obj & 0xfffffff, m->w,
                     m->h, (int)m->fmt, (unsigned long long)(uintptr_t)m->storage);
            nx2_write_png(path, rgba, m->w, rows, m->w >= 2560 ? 3 : 1);
            if (m->fmt == VK_FORMAT_R16G16B16A16_SFLOAT || m->fmt == VK_FORMAT_R32G32B32A32_SFLOAT ||
                m->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || m->fmt == VK_FORMAT_R32_SFLOAT) {
                char rp[112];   // float images: the raw texels too (gpu_*.f16 / .f32 / .r11 / .r32, width x rows)
                snprintf(rp, sizeof rp, "%.*s_%ux%u.%s", (int)(strlen(path) - 4), path, m->w, rows,
                         m->fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? "f16"
                         : m->fmt == VK_FORMAT_R32G32B32A32_SFLOAT ? "f32"
                         : m->fmt == VK_FORMAT_R32_SFLOAT ? "r32" : "r11");
                FILE *rf = fopen(rp, "wb");
                if (rf) fwrite(src, 1, (size_t)bytes, rf), fclose(rf);
            }
            free(rgba);
            n++;
        }
    rt_log("vkdraw: %d GPU images written (gpu_*.png)", n);
}

// ---- compute
static uint32_t ctl_word(const Nx2Program *p, int off) {
    return p->ctrl[0] ? *(const uint32_t *)(uintptr_t)(p->ctrl[0] + off) : 0;
}

static Pipe *cpipes[4096];   // compute pipelines by program serial

static Pipe *compute_pipe(const Nx2Program *prog) {
    for (Pipe *p = cpipes[prog->serial % 4096]; p; p = p->next)
        if (p->key.serial == prog->serial) return p->failed ? NULL : p;
    Pipe *pp = calloc(1, sizeof *pp);
    if (!pp) rt_fatal("vkdraw: compute pipeline record");
    pp->key.serial = prog->serial;
    pp->compute = 1;
    pp->next = cpipes[prog->serial % 4096];
    cpipes[prog->serial % 4096] = pp;
    ConstData cdata;
    memset(&cdata, 0, sizeof cdata);
    cdata.data[5] = (const uint8_t *)(uintptr_t)(prog->code[0] + prog->const_off[0]);
    cdata.size[5] = prog->const_size[0];
    Nx2XOptions opt = {0};
    opt.user = &cdata;
    opt.cbuf_value = cb_cbuf_value;
    opt.texture_type = cb_texture_type;
    opt.texture_integer = cb_texture_integer;
    opt.workgroup[0] = ctl_word(prog, 0x58), opt.workgroup[1] = ctl_word(prog, 0x5c);
    opt.workgroup[2] = ctl_word(prog, 0x60);
    opt.float_controls_set = 1;
    opt.denorm_flush16 = props12.shaderDenormFlushToZeroFloat16;
    opt.denorm_flush32 = props12.shaderDenormFlushToZeroFloat32;
    opt.denorm_preserve16 = props12.shaderDenormPreserveFloat16;
    opt.denorm_preserve32 = props12.shaderDenormPreserveFloat32;
    opt.szinf_nan16 = props12.shaderSignedZeroInfNanPreserveFloat16;
    opt.szinf_nan32 = props12.shaderSignedZeroInfNanPreserveFloat32;
    opt.szinf_nan64 = props12.shaderSignedZeroInfNanPreserveFloat64;
    const void *code = (const void *)(uintptr_t)prog->code[0];
    size_t size = prog->code_size[0];
    size_t len = stage_code_len(prog, code, size);
    uint64_t tx = plat_now_ns();
    int xr = nx2_xcache_translate(1, &code, &size, &len, 1, &opt, pp->st);
    prof_xlate_ns += plat_now_ns() - tx;
    if (xr) {
        rt_log("vkdraw: compute program #%u not translated: %s", prog->serial, pp->st[0].error);
        pp->failed = 1;
        return NULL;
    }
    pp->nst = 1;
    slots_prog = prog;
    assign_slots(pp);
    slots_prog = NULL;
    if (push_region(prog, &pp->push_off, &pp->push_size))
        rt_log("vkdraw: compute program #%u: %u bytes of push constants at c0[%#x]", prog->serial, pp->push_size,
               pp->push_off);
    VkDescriptorSetLayoutBinding b[96];
    int nb = 0;
    for (int i = 0; i < pp->st[0].ndesc && nb < 96; i++) {
        const Nx2XDesc *dd = &pp->st[0].desc[i];
        b[nb++] = (VkDescriptorSetLayoutBinding){dd->binding, desc_type(dd->kind), dd->count, VK_SHADER_STAGE_COMPUTE_BIT,
                                                 NULL};
    }
    pp->ndesc = nb;
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, (uint32_t)nb, b};
    VKCHECK(vkCreateDescriptorSetLayout(D, &dl, NULL, &pp->dsl));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &pp->dsl, 1, &pcr};
    VKCHECK(vkCreatePipelineLayout(D, &pl, NULL, &pp->layout));
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, module(&pp->st[0]), "main", NULL};
    ci.layout = pp->layout;
    VkResult r = vkCreateComputePipelines(D, VK_NULL_HANDLE, 1, &ci, NULL, &pp->pipe);
    vkDestroyShaderModule(D, ci.stage.module, NULL);
    nx2_xlate_free(pp->st, 1);
    if (r != VK_SUCCESS) {
        rt_log("vkdraw: compute pipeline of program #%u failed (%d)", prog->serial, (int)r);
        pp->failed = 1;
        return NULL;
    }
    rt_log("vkdraw: compute pipeline for program #%u: %d descriptors, workgroup %u x %u x %u", prog->serial, nb,
           opt.workgroup[0], opt.workgroup[1], opt.workgroup[2]);
    {   // layout study: control words 0xa0-0xbc and where the shader reads each resource
        char line[512];
        int o = snprintf(line, sizeof line, "  ctrl a0..bc:");
        for (int k = 0xa0; k < 0xc0; k += 4) o += snprintf(line + o, sizeof line - o, " %08x", ctl_word(prog, k));
        for (int i = 0; i < pp->st[0].ndesc && o < (int)sizeof line - 32; i++) {
            const Nx2XDesc *d = &pp->st[0].desc[i];
            static const char *kn[] = {"cb", "ssbo", "tbuf", "ibuf", "tex", "img"};
            if (d->kind == NX2X_CBUF) o += snprintf(line + o, sizeof line - o, " cb%u", d->index);
            else o += snprintf(line + o, sizeof line - o, " %s@c%u[%#x]", kn[d->kind], d->cbuf_index, d->cbuf_offset);
        }
        rt_log("%s", line);
        o = snprintf(line, sizeof line, "  bound:");
        for (int i = 0; i < 8; i++) {
            if (nx2_state.tex[5][i]) o += snprintf(line + o, sizeof line - o, " tex%d", i);
            if (nx2_state.image[5][i]) o += snprintf(line + o, sizeof line - o, " img%d", i);
            if (nx2_state.ssbo[5][i].size) o += snprintf(line + o, sizeof line - o, " ssbo%d", i);
        }
        rt_log("%s", line);
    }
    return pp;
}

static uint64_t g_dispatches;

void nx2_vk_dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!active() || !gx || !gy || !gz) return;
    plat_mutex_lock(&lock);
    dp_t = __rdtsc();
    Nx2Program prog;
    if (!nx2_prog_get(nx2_state.program, &prog)) goto out;
    Pipe *pp = compute_pipe(&prog);
    if (!pp) goto out;
    begin_cb();
    end_rendering();
    ConstData cdata;
    memset(&cdata, 0, sizeof cdata);
    cdata.data[5] = (const uint8_t *)(uintptr_t)(prog.code[0] + prog.const_off[0]);
    cdata.size[5] = prog.const_size[0];
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!bind_resources(pp, &cdata, &set)) goto out;
    end_rendering();
    barrier_all();
    // The game's neural upscaler (72 bytes of push constants, a 0x3000-byte fp16 weight table, one texture in and a
    // larger image out) comes out as a flat colour through the HMMA lowering: scale the scene up with a linear blit
    // instead. NX2_NN_UPSCALE=1 runs the shader.
    static int nn = -1;
    if (nn < 0) nn = getenv("NX2_NN_UPSCALE") && *getenv("NX2_NN_UPSCALE") == '1';
    if (!nn && pp->push_size == 72 && nx2_state.ssbo[5][2].size == 0x3000 && g_bound_tex && g_bound_wimg &&
        g_bound_wimg->w > g_bound_tex->w && g_bound_wimg->h > g_bound_tex->h && !g_bound_tex->f.depth) {
        const int32_t *pc = (const int32_t *)nx2_state.push_cs;   // ints: source origin/size, destination origin/size
        int32_t sx = pc[0], sy = pc[1], sw = pc[2], sh = pc[3], dx = pc[4], dy = pc[5], dw = pc[6], dh = pc[7];
        if (sw <= 0 || sh <= 0 || sx + sw > (int32_t)g_bound_tex->w || sy + sh > (int32_t)g_bound_tex->h)
            sx = sy = 0, sw = (int32_t)g_bound_tex->w, sh = (int32_t)g_bound_tex->h;
        if (dw <= 0 || dh <= 0 || dx + dw > (int32_t)g_bound_wimg->w || dy + dh > (int32_t)g_bound_wimg->h)
            dx = dy = 0, dw = (int32_t)g_bound_wimg->w, dh = (int32_t)g_bound_wimg->h;
        VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{sx, sy, 0}, {sx + sw, sy + sh, 1}},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        // FSR 1 (nx2_fsr.c) where the game's upscaler ran; the linear blit when FSR is off (NX2_UPSCALER=linear)
        int nx2_fsr_upscale(VkDevice, VkCommandBuffer, VkImageView, VkImageLayout, int, int, const int[4], VkImage,
                            VkImageLayout, const int[4]);
        int crop[4] = {sx, sy, sw, sh}, outr[4] = {dx, dy, dw, dh};
        VkImageView sv = img_view(g_bound_tex, NX2X_T_2D);
        int fsr_ok = sv && nx2_fsr_upscale(D, cb, sv, VK_IMAGE_LAYOUT_GENERAL, (int)g_bound_tex->w, (int)g_bound_tex->h,
                                           crop, g_bound_wimg->img, VK_IMAGE_LAYOUT_GENERAL, outr);
        static int nfl;
        if (!fsr_ok && nfl++ < 4)
            rt_log("vkdraw: upscaler %dx%d -> %dx%d (%ux%u -> %ux%u): FSR not used (view %p)", sw, sh, dw, dh, g_bound_tex->w,
                   g_bound_tex->h, g_bound_wimg->w, g_bound_wimg->h, (void *)sv);
        if (!fsr_ok)
            vkCmdBlitImage(cb, g_bound_tex->img, VK_IMAGE_LAYOUT_GENERAL, g_bound_wimg->img, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &b, VK_FILTER_LINEAR);
        // the game's upscaler also turns the sRGB-encoded frame linear (its last pass encodes it again)
        if (g_bound_wimg->fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
            int nx2_srgb_decode(VkDevice, VkCommandBuffer, VkImageView, const int[4]);
            nx2_srgb_decode(D, cb, img_storage_view(g_bound_wimg, NX2X_T_2D), outr);
        } else {
            static int nfmt;
            if (!nfmt++) rt_log("vkdraw: upscaler output format %d: not turned linear", (int)g_bound_wimg->fmt);
        }
        mark_gpu(g_bound_wimg);
        static int logged;
        if (!logged++)
            rt_log("vkdraw: compute program #%u (neural upscaler) replaced by a linear blit %dx%d -> %dx%d", prog.serial,
                   sw, sh, dw, dh);
    } else {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp->pipe);
        if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pp->layout, 0, 1, &set, 0, NULL);
        int gq = gp_begin();
        vkCmdDispatch(cb, gx, gy, gz);
        gp_end(gq, prog.serial, gx, gy, 1);
        ssbo_writeback();
    }
    barrier_all();
    g_dispatches++;
    flush_chunk();
    if (tracing()) {
        rt_log("trace dispatch #%u %ux%ux%u |%s", prog.serial, gx, gy, gz, trace_buf);
        if (pp->push_size) {
            const float *f = (const float *)nx2_state.push_cs;
            rt_log("trace     push: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g", f[0], f[1], f[2], f[3],
                   f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17]);
        }
        for (int i = 0; i < 4; i++)
            if (nx2_state.ssbo[5][i].size) {
                const float *f = (const float *)(uintptr_t)nx2_state.ssbo[5][i].addr;
                const uint32_t *u = (const uint32_t *)f;
                rt_log("trace     ssbo%d (%#llx +%#llx): %g %g %g %g %g %g %g %g (%08x %08x)", i,
                       (unsigned long long)nx2_state.ssbo[5][i].addr, (unsigned long long)nx2_state.ssbo[5][i].size,
                       f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], u[0], u[1]);
                char path[64];   // the whole buffer, as the CPU sees it when the dispatch is recorded
                snprintf(path, sizeof path, "ssbo%d_cs%u.bin", i, prog.serial);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    uint64_t n = nx2_state.ssbo[5][i].size;
                    fwrite(f, 1, n > (1u << 20) ? (1u << 20) : n, fp);
                    fclose(fp);
                }
            }
    }
out:
    plat_mutex_unlock(&lock);
}

void nx2_vk_texture_dirty(uint64_t obj) {
    if (!V) return;
    plat_mutex_lock(&lock);
    nvn_texdesc d;   // the CPU wrote the memory: every image on it
    uint8_t *storage = NULL;
    uint64_t size;
    if (nx2_tex_get(obj, &d, &storage, &size))
        for (Img *m = imgs[img_bucket(storage, obj)]; m; m = m->next)
            if (m->storage == storage && (storage || m->obj == obj)) m->dirty = 1;
    plat_mutex_unlock(&lock);
}

// CopyTextureToTexture: a blit (scaled, LINEAR_FILTER_BIT 1 picks linear filtering; MIRROR_X/Y 4/8), or a plain
// copy for formats that cannot be blitted. Level 0, layer 0 for now (texture views of the copy are not decoded).
// ---- samples-passed counters (ResetCounter / ReportCounter with NVN_COUNTER_TYPE_SAMPLES_PASSED). The game reads
// ~180 a frame, in pairs reset -> draws -> report: what is under and around the player (ink to swim in and climb,
// damage) and visibility. A Vulkan occlusion query runs from the reset to the report (begun and ended outside
// rendering, so it spans render passes); at the report the GPU copies its 64-bit count into the game's memory over
// the replay's estimate. A submission in between (flush) ends the query: that report keeps the estimate.
// NX2_OCCLUSION=0: estimates only (the draws since the reset).
#define OCC_QUERIES 65536
#define OCC_CHUNK 1024   // queries reset together outside rendering at the start of a command buffer
static uint32_t occ_next, occ_base, occ_used = OCC_CHUNK;
static int occ_in_r;
void occ_new_chunk(void) {   // recording, outside rendering
    if (!occ_pool) return;
    occ_base = (occ_next += OCC_CHUNK) % OCC_QUERIES;
    vkCmdResetQueryPool(cb, occ_pool, occ_base, OCC_CHUNK);
    occ_used = 0;
}
static void occ_cut(void) {   // the rendering ends while a query begun inside it runs: that report keeps the estimate
    if (occ_active < 0 || !occ_in_r) return;
    vkCmdEndQuery(cb, occ_pool, (uint32_t)occ_active);
    occ_active = -1;
    occ_broken++;
}
// NX2_OCC_LOG=1 (debug): what each query measured, and what the GPU counted, by program, every 300 presents
static struct OccInfo { uint64_t frame; uint32_t serial, ndraws, w, h; uint8_t dtest, dwrite, dfunc, depth, cull, skipped; } occ_info[OCC_QUERIES];
static int occ_log = -1;
static void occ_note_draw(uint32_t serial, const Nx2Raster *R, int has_depth, uint32_t w, uint32_t h) {
    if (occ_active < 0 || occ_log <= 0) return;
    struct OccInfo *o = &occ_info[occ_active];
    if (!o->ndraws++) o->serial = serial, o->dtest = R->depth_test, o->dwrite = R->depth_write, o->dfunc = R->depth_func,
                     o->depth = (uint8_t)has_depth, o->cull = R->cull, o->w = w, o->h = h;
}
static void occ_note_skip(void) {
    if (occ_active >= 0 && occ_log > 0) occ_info[occ_active].skipped++;
}
static void occ_dump(void) {
    if (occ_log <= 0 || !occ_pool || nx2_vk_frame % 300 || nx2_vk_frame < 600) return;
    static struct { uint32_t serial, n, nonzero; uint64_t sum, max; struct OccInfo i; } g[64];
    int ng = 0;
    for (uint32_t q = 0; q < OCC_QUERIES; q++) {
        struct OccInfo *o = &occ_info[q];
        if (o->frame + 2 != nx2_vk_frame || !o->ndraws) continue;
        uint64_t v[2] = {0, 0};
        if (vkGetQueryPoolResults(D, occ_pool, q, 1, sizeof v, v, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) < 0 || !v[1]) continue;
        int k = 0;
        while (k < ng && g[k].serial != o->serial) k++;
        if (k == ng) {
            if (ng == 64) continue;
            memset(&g[k], 0, sizeof g[k]), g[k].serial = o->serial, g[k].i = *o, ng++;
        }
        g[k].n++, g[k].sum += v[0], g[k].nonzero += v[0] != 0, g[k].i.skipped += o->skipped;
        if (v[0] > g[k].max) g[k].max = v[0];
    }
    for (int k = 0; k < ng; k++)
        rt_log("occ: present %llu program #%u: %u queries (%u draws first), %u nonzero, avg %llu max %llu; depth test %u write %u func %u target %u, cull %u, %ux%u, %u skipped draws",
               (unsigned long long)nx2_vk_frame - 2, g[k].serial, g[k].n, g[k].i.ndraws, g[k].nonzero,
               (unsigned long long)(g[k].sum / g[k].n), (unsigned long long)g[k].max, g[k].i.dtest, g[k].i.dwrite,
               g[k].i.dfunc, g[k].i.depth, g[k].i.cull, g[k].i.w, g[k].i.h, (unsigned)g[k].i.skipped);
}
static int occ_precise = -1, occ_off = -1;
static uint64_t occ_gpu, occ_est;

void nx2_vk_counter_reset(void) {
    if (occ_off < 0) occ_off = getenv("NX2_OCCLUSION") && *getenv("NX2_OCCLUSION") == '0';
    if (occ_off || !active()) return;
    plat_mutex_lock(&lock);
    if (!occ_pool && occ_precise < 0) {
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceFeatures(V->phys, &f);
        occ_precise = f.occlusionQueryPrecise;
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_OCCLUSION, OCC_QUERIES};
        if (vkCreateQueryPool(D, &qi, NULL, &occ_pool) != VK_SUCCESS) occ_pool = VK_NULL_HANDLE;
        rt_log("vkdraw: samples-passed counters as occlusion queries%s", occ_pool ? (occ_precise ? " (precise)" : "") : ": no pool");
    }
    if (occ_pool) {
        begin_cb();
        if (occ_active >= 0) {   // a reset without a report: that query is dropped
            if (!occ_in_r) end_rendering();
            if (occ_active >= 0) vkCmdEndQuery(cb, occ_pool, (uint32_t)occ_active), occ_active = -1;
        }
        if (occ_used >= OCC_CHUNK) {   // the chunk reset at the start of this command buffer is used up
            end_rendering();
            occ_new_chunk();
        }
        uint32_t q = occ_base + occ_used++;
        vkCmdBeginQuery(cb, occ_pool, q, occ_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        occ_active = (int)q;
        occ_in_r = rendering;   // begun inside the open rendering: must end inside it (else outside any)
        if (occ_log < 0) occ_log = getenv("NX2_OCC_LOG") && *getenv("NX2_OCC_LOG") == '1';
        if (occ_log > 0) memset(&occ_info[q], 0, sizeof occ_info[q]), occ_info[q].frame = nx2_vk_frame;
    }
    plat_mutex_unlock(&lock);
}

// 1: the GPU writes the count at addr (8 bytes) after the draws since the reset; 0: the caller's estimate stays.
int nx2_vk_counter_report(uint64_t addr) {
    if (occ_off || !active() || !addr || (addr & 7)) return 0;
    plat_mutex_lock(&lock);
    int ok = 0;
    VkBuffer b;
    VkDeviceSize o;
    if (occ_pool && occ_active >= 0) {
        begin_cb();
        if (!occ_in_r) end_rendering();   // begun outside rendering: ends outside
        vkCmdEndQuery(cb, occ_pool, (uint32_t)occ_active);
        if (buffer_at(addr, 8, 8, &b, &o)) {
            if (nocc_copy == (int)(sizeof occ_copy / sizeof *occ_copy)) occ_copy_all();
            occ_copy[nocc_copy].q = (uint32_t)occ_active, occ_copy[nocc_copy].buf = b, occ_copy[nocc_copy++].off = o;
            ok = 1;
        }
        occ_active = -1;
    }
    if (ok) occ_gpu++;
    else occ_est++;
    uint64_t n = occ_gpu + occ_est;
    if (!(n & (n - 1)) && n >= 64)
        rt_log("vkdraw: samples-passed reports: %llu counted by the GPU, %llu estimated (%llu cut by a submission)",
               (unsigned long long)occ_gpu, (unsigned long long)occ_est, (unsigned long long)occ_broken);
    plat_mutex_unlock(&lock);
    return ok;
}

// DrawElementsIndirect / DrawArraysIndirect: a compute pass before it wrote the numbers (particles, ink shots);
// a full barrier outside the rendering makes them visible to the draw.
void nx2_vk_draw_indirect(const Nx2Draw *d, uint64_t args) {
    if (!active() || !args) return;
    plat_mutex_lock(&lock);
    begin_cb();
    end_rendering();
    barrier_all();
    plat_mutex_unlock(&lock);
    g_indirect = args;
    nx2_vk_draw(d);
    g_indirect = 0;
}

// SignalEvent in a command buffer: the GPU writes the value after everything before it (NVN_EVENT_SIGNAL_MODE_WRITE),
// then the work goes to the GPU at once. The CPU polls the event (nvnEventGetValue) before it reads what the GPU
// computed for it. Adding (mode 1) or memory that cannot be imported: written by the CPU now.
void nx2_vk_signal_event(uint64_t addr, int mode, uint32_t value) {
    if (!addr || (addr & 3)) return;
    static uint64_t on_gpu, on_cpu;
    VkBuffer b;
    VkDeviceSize o;
    if (active() && mode == 0) {
        plat_mutex_lock(&lock);
        if (buffer_at(addr, 4, 4, &b, &o)) {
            begin_cb();
            end_rendering();
            barrier_all();
            vkCmdFillBuffer(cb, b, o, 4, value);
            barrier_all();
            static int now = -1;   // NX2_EVENT_FLUSH=0: the signal waits for the next submission instead
            if (now < 0) now = !getenv("NX2_EVENT_FLUSH") || *getenv("NX2_EVENT_FLUSH") != '0';
            if (now) flush();
            plat_mutex_unlock(&lock);
            on_gpu++;
            if (!(on_gpu & (on_gpu - 1))) rt_log("vkdraw: %llu event signals by the GPU", (unsigned long long)on_gpu);
            return;
        }
        plat_mutex_unlock(&lock);
    }
    uint32_t *p = (uint32_t *)(uintptr_t)addr;
    if (mode == 1) __atomic_add_fetch(p, value, __ATOMIC_ACQ_REL);
    else __atomic_store_n(p, value, __ATOMIC_RELEASE);
    on_cpu++;
    if (!(on_cpu & (on_cpu - 1))) rt_log("vkdraw: %llu event signals by the CPU (mode %d)", (unsigned long long)on_cpu, mode);
}

void nx2_vk_copy_texture(const Nx2Copy *cp) {
    if (!active()) return;
    plat_mutex_lock(&lock);
    Img *src = img_get(cp->src), *dst = img_get(cp->dst);
    if (!src || !dst || src->failed || dst->failed) goto out;
    begin_cb();
    end_rendering();
    img_upload(src);
    img_layout(src, VK_IMAGE_LAYOUT_GENERAL);
    img_layout(dst, VK_IMAGE_LAYOUT_GENERAL);
    barrier_all();
    int32_t sx = cp->sr[0], sy = cp->sr[1], sw = cp->sr[3] > 0 ? cp->sr[3] : (int32_t)src->w;
    int32_t sh = cp->sr[4] > 0 ? cp->sr[4] : (int32_t)src->h;
    int32_t dx = cp->dr[0], dy = cp->dr[1], dw = cp->dr[3] > 0 ? cp->dr[3] : (int32_t)dst->w;
    int32_t dh = cp->dr[4] > 0 ? cp->dr[4] : (int32_t)dst->h;
    VkFormatProperties sp, dp;
    vkGetPhysicalDeviceFormatProperties(V->phys, src->fmt, &sp);
    vkGetPhysicalDeviceFormatProperties(V->phys, dst->fmt, &dp);
    int blit = (sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
               (dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) && !src->f.depth && !dst->f.depth;
    if (blit) {
        VkImageBlit b = {{src->all_aspect, 0, 0, 1}, {{sx, sy, 0}, {sx + sw, sy + sh, 1}},
                         {dst->all_aspect, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        if (cp->flags & 4) { int32_t t = b.dstOffsets[0].x; b.dstOffsets[0].x = b.dstOffsets[1].x; b.dstOffsets[1].x = t; }
        if (cp->flags & 8) { int32_t t = b.dstOffsets[0].y; b.dstOffsets[0].y = b.dstOffsets[1].y; b.dstOffsets[1].y = t; }
        vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       (cp->flags & 1) && !src->f.integer ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else if (sw == dw && sh == dh) {
        VkImageCopy c = {{src->all_aspect, 0, 0, 1}, {sx, sy, 0}, {dst->all_aspect, 0, 0, 1}, {dx, dy, 0},
                         {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {
        static int n;
        if (n++ < 8) rt_log("vkdraw: scaled copy between formats %d -> %d not possible", (int)src->fmt, (int)dst->fmt);
        goto out;
    }
    barrier_all();
    mark_gpu(dst);
    if (tracing()) {
        char a[96], b2[96];
        img_desc(a, sizeof a, "src", src);
        img_desc(b2, sizeof b2, "dst", dst);
        rt_log("trace copy%s %d,%d %dx%d ->%s %d,%d %dx%d flags %#x", a, sx, sy, sw, sh, b2, dx, dy, dw, dh, cp->flags);
    }
out:
    plat_mutex_unlock(&lock);
}
