// NVN2 command buffers on the host. The driver's own recording writes Ampere GPU methods and needs its internal GPU
// state, so recording is host code: every command call is appended to a host list for its command buffer as
// {proc, x0..x7}; EndRecording turns the list into an NVNcommandHandle; QueueSubmitCommands hands the lists to the
// back end (nx2_cmd_submitted). The game's command and control memory is accepted but never written, so it always
// reads as free and the out-of-memory callback never fires.
//
// Arguments that point into guest memory (arrays of buffers, clear colours, ...) are kept as pointers for now; the
// Vulkan back end will copy what each command needs at record time, command by command.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "kernel.h"
#include "nx2_draw.h"
#include "nx2_nvn.h"
#include "platform/platform.h"

typedef struct { uint32_t proc; uint32_t blob_n; uint64_t x[8]; uint64_t aux[4]; float f[4]; uint8_t *blob; } Rec;
// proc: NVN2 slot index; f: s0-s3 (float arguments); aux and blob: data copied at record time from what the
// arguments point to (the game reuses that memory). A blob belongs to the list that holds the record.

typedef struct {
    uint64_t obj;
    int recording;
    Rec *recs;
    uint32_t n, cap;
    uint64_t cmd_size, ctl_size, callback, callback_data;
    uint64_t row_stride, image_stride;
    uint64_t since_begin;   // commands recorded since BeginRecording (kept after EndRecording)
    uint64_t mem_lo, mem_hi;   // the command memory added last (CPU addresses; 0: unknown)
    uint64_t rec_lo, rec_hi;   // the command memory the current recording started in
} Cb;

// An ended list. It stays valid until other commands are recorded over its command memory (NVN handles point into
// that memory): lists recorded once at load and called every frame (CallCommands) live on, per-frame lists go when
// the game records into their memory again. The memory a list takes is the estimate the game is told (16 bytes a
// command from where the recording began), which is what the game's own allocator uses for the next recording.
typedef struct Handle {
    uint64_t id;
    Rec *recs;
    uint32_t n;
    uint64_t lo, hi;                 // its command memory (0, 0: unknown, kept)
    struct Handle *next;             // hash chain
    struct Handle *prev_all, *next_all;
    struct Handle *prev_mem, *next_mem;   // the lists whose memory starts in the same MiB (mem_buckets)
} Handle;

// ---- command buffer table: open addressing on the guest object pointer
#define CB_SLOTS 8192
static Cb *cbs[CB_SLOTS];
static PlatMutex cb_lock;

static uint64_t hash(uint64_t p) { return (p >> 4) * 0x9e3779b97f4a7c15ull >> 51; }   // 13 bits

static Cb *cb_get(uint64_t obj, int create) {
    uint64_t h = hash(obj) & (CB_SLOTS - 1);
    plat_mutex_lock_shared(&cb_lock);
    for (uint64_t i = 0; i < CB_SLOTS; i++) {
        Cb *c = cbs[(h + i) & (CB_SLOTS - 1)];
        if (!c) break;
        if (c->obj == obj) {
            plat_mutex_unlock_shared(&cb_lock);
            return c;
        }
    }
    plat_mutex_unlock_shared(&cb_lock);
    if (!create) rt_fatal("nvn2: command buffer %#llx is not initialized", (unsigned long long)obj);
    Cb *n = calloc(1, sizeof *n);
    if (!n) rt_fatal("nvn2: command buffer allocation");
    n->obj = obj;
    plat_mutex_lock(&cb_lock);
    for (uint64_t i = 0; i < CB_SLOTS; i++) {
        Cb **s = &cbs[(h + i) & (CB_SLOTS - 1)];
        if (*s && (*s)->obj == obj) {   // another thread made it meanwhile
            plat_mutex_unlock(&cb_lock);
            free(n);
            return *s;
        }
        if (!*s) {
            *s = n;
            plat_mutex_unlock(&cb_lock);
            return n;
        }
    }
    plat_mutex_unlock(&cb_lock);
    rt_fatal("nvn2: more than %d command buffers", CB_SLOTS);
}

// ---- handles: by id, kept until their command memory is reused (handles_drop_range)
#define HANDLE_BUCKETS (1 << 16)
static Handle *handles[HANDLE_BUCKETS];
static Handle *all_handles;   // every live handle (newest first)
#define MEM_BUCKETS 4096
static Handle *mem_buckets[MEM_BUCKETS];   // by the MiB its command memory starts in (lists with memory only)
static unsigned mem_bucket(uint64_t a) { return (unsigned)((a >> 20) & (MEM_BUCKETS - 1)); }
static uint64_t next_handle = 1, live_handles, dropped_handles;
static PlatMutex handle_lock;   // the tables only, briefly: a replay reads a list's records without it
static int replays_active;      // replays running (nx2_cmd_submitted): dropped lists wait in the graveyard
static Handle *graveyard;       // dropped while a replay ran, freed when none does (chained by next)

static void free_recs(Rec *recs, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) free(recs[i].blob);
    free(recs);
}

static uint64_t handle_new(Rec *recs, uint32_t n, uint64_t lo, uint64_t hi) {
    Handle *h = calloc(1, sizeof *h);
    if (!h) rt_fatal("nvn2: command handle allocation");
    plat_mutex_lock(&handle_lock);
    uint64_t id = next_handle++;
    Handle **b = &handles[id & (HANDLE_BUCKETS - 1)];
    *h = (Handle){id, recs, n, lo, hi, *b, NULL, all_handles};
    *b = h;
    if (all_handles) all_handles->prev_all = h;
    all_handles = h;
    if (lo < hi) {
        Handle **mb = &mem_buckets[mem_bucket(lo)];
        h->next_mem = *mb;
        if (*mb) (*mb)->prev_mem = h;
        *mb = h;
    }
    live_handles++;
    plat_mutex_unlock(&handle_lock);
    return id;
}

static const Handle *handle_find(uint64_t id) {   // under handle_lock
    for (const Handle *h = handles[id & (HANDLE_BUCKETS - 1)]; h; h = h->next)
        if (h->id == id) return h;
    return NULL;
}

// NX2_LOG_LISTS=<frame>: command memory, recordings, calls and submissions around that present (debug)
static int lists_log(void) {
    static int64_t want = -2;
    extern uint64_t nx2_vk_frame;
    if (want == -2) want = getenv("NX2_LOG_LISTS") ? atoll(getenv("NX2_LOG_LISTS")) : -1;
    return want >= 0 && (int64_t)nx2_vk_frame + 1 >= want && (int64_t)nx2_vk_frame <= want;
}

// Commands are about to be recorded over [lo, hi): the lists there are gone.
static void handles_drop_range(uint64_t lo, uint64_t hi) {
    if (lo >= hi) return;
    Handle *dead = NULL;
    plat_mutex_lock(&handle_lock);
    // lists starting from a MiB before lo (no list is that long) to the MiB of hi - 1; buckets wrap at 4 GiB
    uint64_t first = (lo >> 20) ? (lo >> 20) - 1 : 0, last = (hi - 1) >> 20;
    if (last - first >= MEM_BUCKETS) last = first + MEM_BUCKETS - 1;
    for (uint64_t mbi = first; mbi <= last; mbi++)
    for (Handle *h = mem_buckets[mbi & (MEM_BUCKETS - 1)], *nx; h; h = nx) {
        nx = h->next_mem;
        if (h->lo >= h->hi || h->lo >= hi || lo >= h->hi) continue;
        if (h->prev_mem) h->prev_mem->next_mem = h->next_mem;
        else mem_buckets[mbi & (MEM_BUCKETS - 1)] = h->next_mem;
        if (h->next_mem) h->next_mem->prev_mem = h->prev_mem;
        for (Handle **pp = &handles[h->id & (HANDLE_BUCKETS - 1)]; *pp; pp = &(*pp)->next)
            if (*pp == h) {
                *pp = h->next;
                break;
            }
        if (h->prev_all) h->prev_all->next_all = h->next_all;
        else all_handles = h->next_all;
        if (h->next_all) h->next_all->prev_all = h->prev_all;
        h->next = dead;   // freed below, or after the running replays (they may be reading it)
        dead = h;
        live_handles--, dropped_handles++;
    }
    if (replays_active && dead) {
        Handle *t = dead;
        while (t->next) t = t->next;
        t->next = graveyard, graveyard = dead, dead = NULL;
    }
    plat_mutex_unlock(&handle_lock);
    while (dead) {
        Handle *nx = dead->next;
        free_recs(dead->recs, dead->n);
        free(dead);
        dead = nx;
    }
}

// ---- lifecycle
static void cb_initialize(Cpu *c) {   // NVNboolean nvnCommandBufferInitialize(NVNcommandBuffer *, NVNdevice *)
    Cb *b = cb_get(c->x[0], 1);
    b->recording = 0;
    b->n = 0;
    c->x[0] = 1;
}
static void cb_finalize(Cpu *c) {
    Cb *b = cb_get(c->x[0], 1);
    free_recs(b->recs, b->n);
    b->recs = NULL;
    b->n = b->cap = 0;
}
static void cb_nop(Cpu *c) { (void)c; }
static void cb_set_callback(Cpu *c) { cb_get(c->x[0], 1)->callback = c->x[1]; }
static void cb_set_callback_data(Cpu *c) { cb_get(c->x[0], 1)->callback_data = c->x[1]; }
static void cb_get_callback(Cpu *c) { c->x[0] = cb_get(c->x[0], 1)->callback; }
static void cb_get_callback_data(Cpu *c) { c->x[0] = cb_get(c->x[0], 1)->callback_data; }
// void AddCommandMemory(NVNcommandBuffer *, const NVNmemoryPool *, ptrdiff_t offset, size_t size)
static void cb_add_command_memory(Cpu *c) {
    static uint64_t n;
    extern uint64_t nx2_vk_frame;
    Cb *b = cb_get(c->x[0], 1);
    uint64_t base = c->x[1] ? nx2_pool_memory(c->x[1]) : 0;
    uint64_t lo = base ? base + c->x[2] : 0, hi = base ? lo + c->x[3] : 0;
    if (lists_log())
        rt_log("lists: AddCommandMemory cb %#llx %#llx..%#llx (recording %d)", (unsigned long long)b->obj,
               (unsigned long long)lo, (unsigned long long)hi, b->recording);
    b->mem_lo = lo, b->mem_hi = hi;
    if (b->recording && !b->rec_hi) b->rec_lo = lo, b->rec_hi = hi;
    if (!(n & 4095)) {   // the live lists, now and then (they must not pile up)
        uint64_t empty = 0, nomem = 0, recs = 0;
        plat_mutex_lock_shared(&handle_lock);
        for (const Handle *h = all_handles; h; h = h->next_all) empty += !h->n, nomem += !h->lo, recs += h->n;
        plat_mutex_unlock_shared(&handle_lock);
        rt_log("nvn2: AddCommandMemory #%llu (frame %llu): %llu command lists live (%llu empty, %llu without memory, "
               "%llu commands), %llu dropped", (unsigned long long)n, (unsigned long long)nx2_vk_frame,
               (unsigned long long)live_handles, (unsigned long long)empty, (unsigned long long)nomem,
               (unsigned long long)recs, (unsigned long long)dropped_handles);
    }
    static int log_cmdmem = -1;
    if (log_cmdmem < 0) log_cmdmem = getenv("NX2_LOG_CMDMEM") != NULL;
    if (log_cmdmem && n < 200)
        rt_log("nvn2: AddCommandMemory #%llu cb %#llx pool %#llx (mem %#llx) +%#llx size %#llx frame %llu",
               (unsigned long long)n, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)base, (unsigned long long)c->x[2], (unsigned long long)c->x[3],
               (unsigned long long)nx2_vk_frame);
    n++;
    b->cmd_size += c->x[3];
}
// void AddControlMemory(NVNcommandBuffer *, void *memory, size_t size)
static void cb_add_control_memory(Cpu *c) { cb_get(c->x[0], 1)->ctl_size += c->x[2]; }
static void cb_cmd_size(Cpu *c) { c->x[0] = cb_get(c->x[0], 1)->cmd_size; }
static void cb_ctl_size(Cpu *c) { c->x[0] = cb_get(c->x[0], 1)->ctl_size; }
// Memory "used": the game reads it after EndRecording and skips lists that used none, so it must grow with the
// commands recorded (an estimate: 16 bytes of command memory and 4 of control memory per command), never past
// what was added.
static uint64_t used(const Cb *b, uint64_t per, uint64_t size) {
    uint64_t u = b->since_begin * per;
    return u > size ? size : u;
}
static void cb_cmd_used(Cpu *c) { Cb *b = cb_get(c->x[0], 1); c->x[0] = used(b, 16, b->cmd_size); }
static void cb_ctl_used(Cpu *c) { Cb *b = cb_get(c->x[0], 1); c->x[0] = used(b, 4, b->ctl_size); }
static void cb_cmd_free(Cpu *c) { Cb *b = cb_get(c->x[0], 1); c->x[0] = b->cmd_size - used(b, 16, b->cmd_size); }
static void cb_ctl_free(Cpu *c) { Cb *b = cb_get(c->x[0], 1); c->x[0] = b->ctl_size - used(b, 4, b->ctl_size); }
static void cb_is_recording(Cpu *c) { c->x[0] = (uint64_t)cb_get(c->x[0], 1)->recording; }
static void cb_begin(Cpu *c) {
    Cb *b = cb_get(c->x[0], 1);
    for (uint32_t i = 0; i < b->n; i++) free(b->recs[i].blob);   // a list never ended
    b->recording = 1;
    b->n = 0;
    b->since_begin = 0;
    b->rec_lo = b->mem_lo, b->rec_hi = b->mem_hi;
}
static void cb_end(Cpu *c) {   // NVNcommandHandle nvnCommandBufferEndRecording(NVNcommandBuffer *)
    Cb *b = cb_get(c->x[0], 1);
    Rec *recs = NULL;
    if (b->n) {
        recs = malloc(b->n * sizeof *recs);
        if (!recs) rt_fatal("nvn2: command list allocation");
        memcpy(recs, b->recs, b->n * sizeof *recs);
    }
    uint64_t lo = b->rec_lo, hi = b->rec_hi;   // the memory this list took: 16 bytes a command, as the game is told
    if (lo && hi > lo) {
        uint64_t take = b->since_begin * 16;
        if (!take) take = 1;   // an empty list still sits somewhere: it goes when that place is recorded over
        hi = take < hi - lo ? lo + take : hi;
        handles_drop_range(lo, hi);   // recorded over: the lists that were there are gone
        if (b->mem_lo == lo && b->mem_hi > hi) b->mem_lo = hi;   // the next recording follows (unless memory is added)
    }
    uint64_t id = handle_new(recs, b->n, lo, hi);
    if (lists_log())
        rt_log("lists: EndRecording cb %#llx -> list %llu (%u commands, memory %#llx..%#llx)", (unsigned long long)b->obj,
               (unsigned long long)id, b->n, (unsigned long long)lo, (unsigned long long)hi);
    static int dump_ended = -1;   // NX2_DUMP_ENDED=<n>: summary of the first n ended lists
    if (dump_ended < 0) {
        const char *e = getenv("NX2_DUMP_ENDED");
        dump_ended = e ? atoi(e) : 0;
    }
    if (dump_ended > 0) {
        dump_ended--;
        char line[1024];
        int o = snprintf(line, sizeof line, "list %llu of cb %#llx from %s: %u commands:", (unsigned long long)id,
                         (unsigned long long)b->obj, ldr_describe(c->x[30], (char[96]){0}, 96), b->n);
        for (uint32_t i = 0; i < b->n && o < (int)sizeof line - 40; i++) {
            const char *nm = nx2_proc_name_at((int)recs[i].proc) + 16;
            if (i && recs[i].proc == recs[i - 1].proc) continue;
            o += snprintf(line + o, sizeof line - o, " %s", nm);
        }
        rt_log("%s", line);
        static int traced;
        if (b->n > 1000 && traced++ < 2) kern_backtrace(c);
    }
    b->recording = 0;
    b->n = 0;
    c->x[0] = id;
}
static void cb_set_row_stride(Cpu *c) { cb_get(c->x[0], 1)->row_stride = c->x[1]; }
static void cb_set_image_stride(Cpu *c) { cb_get(c->x[0], 1)->image_stride = c->x[1]; }
static void cb_get_row_stride(Cpu *c) { c->x[0] = cb_get(c->x[0], 1)->row_stride; }
static void cb_get_image_stride(Cpu *c) { c->x[0] = cb_get(c->x[0], 1)->image_stride; }

// ---- record-time copies, per command. Indices are resolved once the game has asked for the procs.
// kind: how much to copy from the pointer argument x[arg]: a fixed size, or count (x[cnt]) * elem bytes.
typedef struct { const char *name; int arg, cnt, elem, fixed; int idx; } CopyRule;
static CopyRule rules[] = {
    {"nvnCommandBufferUpdateUniformBuffer", 5, 4, 1, 0, -2},   // (cb, addr, size, offset, dataSize, data)
    {"nvnCommandBufferBindMultisampleState", 1, 0, 0, 32, -2},
    {"nvnCommandBufferSetBlendColor", 1, 0, 0, 16, -2},
    {"nvnCommandBufferClearColor", 2, 0, 0, 16, -2},          // (cb, index, const float *color, mask)
    {"nvnCommandBufferClearColori", 2, 0, 0, 16, -2},
    {"nvnCommandBufferClearColorui", 2, 0, 0, 16, -2},
    {"nvnCommandBufferSetViewports", 3, 2, 16, 0, -2},        // (cb, first, count, const float *xywh)
    {"nvnCommandBufferSetScissors", 3, 2, 16, 0, -2},         // (cb, first, count, const int *xywh)
    {"nvnCommandBufferSetDepthRanges", 3, 2, 8, 0, -2},       // (cb, first, count, const float *nf)
    {"nvnCommandBufferBindVertexBuffers", 3, 2, 16, 0, -2},   // (cb, first, count, const NVNbufferRange *)
    {"nvnCommandBufferBindUniformBuffers", 4, 3, 16, 0, -2},  // (cb, stage, first, count, const NVNbufferRange *)
    {"nvnCommandBufferBindTextures", 4, 3, 8, 0, -2},         // (cb, stage, first, count, const NVNtextureHandle *)
    {"nvnCommandBufferBindStorageBuffers", 4, 3, 16, 0, -2},
    {"nvnCommandBufferBindImages", 4, 3, 8, 0, -2},
    {"nvnCommandBufferPushGraphicsConstants", 4, 3, 1, 0, -2},   // (cb, ?, offset, size, const void *data)
    {"nvnCommandBufferPushComputeConstants", 4, 3, 1, 0, -2},
    {"nvnCommandBufferCallCommands", 2, 1, 8, 0, -2},         // (cb, count, const NVNcommandHandle *)
    {"nvnCommandBufferCopyCommands", 2, 1, 8, 0, -2},
};
#define NRULES (int)(sizeof rules / sizeof *rules)
static int idx_set_rt = -2, idx_clear_color = -2, idx_attrib = -2, idx_stream = -2, idx_call = -2, idx_copy = -2;
static int idx_copy_tex = -2;
// Again whenever the game has asked for more procs: some are asked for only later (CopyTextureToTexture,
// BindTextures in gameplay), and an index resolved before that stayed -1, so their data was never copied.
static void resolve(void) {
    static int seen = -1;
    int n = nx2_proc_count();
    if (n == seen) return;
    seen = n;
    idx_copy_tex = nx2_proc_index_by_name("nvnCommandBufferCopyTextureToTexture");
    idx_call = nx2_proc_index_by_name("nvnCommandBufferCallCommands");
    idx_copy = nx2_proc_index_by_name("nvnCommandBufferCopyCommands");
    idx_attrib = nx2_proc_index_by_name("nvnCommandBufferBindVertexAttribState");
    idx_stream = nx2_proc_index_by_name("nvnCommandBufferBindVertexStreamState");
    for (int i = 0; i < NRULES; i++) rules[i].idx = nx2_proc_index_by_name(rules[i].name);
    idx_clear_color = nx2_proc_index_by_name("nvnCommandBufferClearColor");
    idx_set_rt = nx2_proc_index_by_name("nvnCommandBufferSetRenderTargets");
}

static void copy_blob(Rec *r, const void *src, uint64_t n) {
    if (!src || !n || n > (1u << 20)) return;
    r->blob = malloc(n);
    if (!r->blob) rt_fatal("nvn2: record copy of %llu bytes", (unsigned long long)n);
    memcpy(r->blob, src, n);
    r->blob_n = (uint32_t)n;
}

// Vertex attribute / stream states are packed by the driver: decoded with its getters, cached by raw value.
typedef struct { uint64_t raw; int used; int v[3]; } DecCache;
#define DEC_SLOTS 1024
static DecCache attrib_cache[DEC_SLOTS], stream_cache[DEC_SLOTS];
static PlatMutex dec_lock;

static void decode_state(Cpu *c, int attrib, const uint8_t *state, int v[3]) {
    uint64_t raw = 0;
    memcpy(&raw, state, attrib ? 4 : 8);
    DecCache *cache = attrib ? attrib_cache : stream_cache;
    uint64_t h = (raw * 0x9e3779b97f4a7c15ull) >> 54;
    plat_mutex_lock(&dec_lock);
    for (int i = 0; i < 16; i++) {
        DecCache *e = &cache[(h + i) % DEC_SLOTS];
        if (e->used && e->raw == raw) {
            memcpy(v, e->v, sizeof e->v);
            plat_mutex_unlock(&dec_lock);
            return;
        }
    }
    plat_mutex_unlock(&dec_lock);
    static _Thread_local uint64_t *out;
    if (!out) out = mem_kernel_alloc(64, 64);
    static uint64_t get_format, get_stream_index, get_stride, get_divisor;
    uint64_t st = (uint64_t)(uintptr_t)state;
    if (attrib) {
        out[0] = out[1] = 0;
        nx2_driver_call(c, &get_format, "nvnVertexAttribStateGetFormat", 3, st, (uint64_t)(uintptr_t)&out[0],
                        (uint64_t)(uintptr_t)&out[1], 0);
        v[0] = (int)out[0], v[1] = (int)out[1];
        v[2] = (int)nx2_driver_call(c, &get_stream_index, "nvnVertexAttribStateGetStreamIndex", 1, st, 0, 0, 0);
    } else {
        v[0] = (int)nx2_driver_call(c, &get_stride, "nvnVertexStreamStateGetStride", 1, st, 0, 0, 0);
        v[1] = (int)nx2_driver_call(c, &get_divisor, "nvnVertexStreamStateGetDivisor", 1, st, 0, 0, 0);
        v[2] = 0;
    }
    plat_mutex_lock(&dec_lock);
    for (int i = 0; i < 16; i++) {
        DecCache *e = &cache[(h + i) % DEC_SLOTS];
        if (!e->used) {
            *e = (DecCache){raw, 1, {v[0], v[1], v[2]}};
            break;
        }
    }
    plat_mutex_unlock(&dec_lock);
}

// Render state objects: decoded with the driver's getters, cached by their raw bytes (8 at most).
static const struct { const char *bind; int kind, size; } state_binds[] = {
    {"nvnCommandBufferBindBlendState", NX2_ST_BLEND, 8},
    {"nvnCommandBufferBindColorState", NX2_ST_COLOR, 4},
    {"nvnCommandBufferBindChannelMaskState", NX2_ST_CHANNEL, 4},
    {"nvnCommandBufferBindDepthStencilState", NX2_ST_DEPTH, 8},
    {"nvnCommandBufferBindPolygonState", NX2_ST_POLYGON, 4},
};
#define NSTATE (int)(sizeof state_binds / sizeof *state_binds)
static int state_idx[NSTATE] = {-2};
typedef struct { uint64_t raw; int kind; Nx2StateBlob b; } StCache;
static StCache st_cache[4096];

// A driver getter with up to 6 arguments (lookup cached in *cache).
static uint64_t gcall6(Cpu *c, uint64_t *cache, const char *name, int n, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    uint64_t f = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
    if (!f) __atomic_store_n(cache, f = nx2_driver_proc(c, name), __ATOMIC_RELEASE);
    return rt_call(c, f, n, a0, a1, a2, a3, a4, a5);
}
#define gcall(c, cache, name, n, a0, a1, a2, a3) gcall6(c, cache, name, n, a0, a1, a2, a3, 0, 0)

static void decode_render_state(Cpu *c, int kind, int size, const uint8_t *state, Nx2StateBlob *o) {
    uint64_t raw = 0;
    memcpy(&raw, state, (size_t)size);
    uint64_t h = ((raw ^ (uint64_t)kind << 59) * 0x9e3779b97f4a7c15ull) >> 52;
    plat_mutex_lock(&dec_lock);
    for (int i = 0; i < 8; i++) {
        StCache *e = &st_cache[(h + i) % 4096];
        if (e->kind == kind && e->raw == raw) {
            *o = e->b;
            plat_mutex_unlock(&dec_lock);
            return;
        }
    }
    plat_mutex_unlock(&dec_lock);
    static _Thread_local uint32_t *out;
    if (!out) out = mem_kernel_alloc(64, 64);
    uint64_t st = (uint64_t)(uintptr_t)state, o0 = (uint64_t)(uintptr_t)out;
    memset(o, 0, sizeof *o);
    o->kind = kind;
    memset(out, 0, 64);
    switch (kind) {
    case NX2_ST_BLEND: {
        static uint64_t g0, g1, g2;
        o->v[0] = (int)gcall(c, &g0, "nvnBlendStateGetBlendTarget", 1, st, 0, 0, 0);
        gcall6(c, &g1, "nvnBlendStateGetBlendFunc", 5, st, o0, o0 + 4, o0 + 8, o0 + 12, 0);
        o->v[1] = (int)out[0], o->v[2] = (int)out[1], o->v[3] = (int)out[2];
        o->v[4] = (int)out[3];
        gcall(c, &g2, "nvnBlendStateGetBlendEquation", 3, st, o0 + 16, o0 + 20, 0);
        o->v[5] = (int)out[4], o->v[6] = (int)out[5];
        break;
    }
    case NX2_ST_COLOR: {
        static uint64_t g0;
        int mask = 0;
        for (int t = 0; t < 8; t++)
            if (gcall(c, &g0, "nvnColorStateGetBlendEnable", 2, st, (uint64_t)t, 0, 0) & 0xff) mask |= 1 << t;
        o->v[0] = mask;
        break;
    }
    case NX2_ST_CHANNEL: {
        static uint64_t g0;
        for (int t = 0; t < 8; t++) {
            memset(out, 0, 16);
            gcall6(c, &g0, "nvnChannelMaskStateGetChannelMask", 6, st, (uint64_t)t, o0, o0 + 4, o0 + 8, o0 + 12);
            o->v[t] = (out[0] & 0xff ? 1 : 0) | (out[1] & 0xff ? 2 : 0) | (out[2] & 0xff ? 4 : 0) |
                      (out[3] & 0xff ? 8 : 0);
        }
        break;
    }
    case NX2_ST_DEPTH: {
        static uint64_t g0, g1, g2;
        o->v[0] = (int)(gcall(c, &g0, "nvnDepthStencilStateGetDepthTestEnable", 1, st, 0, 0, 0) & 0xff);
        o->v[1] = (int)(gcall(c, &g1, "nvnDepthStencilStateGetDepthWriteEnable", 1, st, 0, 0, 0) & 0xff);
        o->v[2] = (int)gcall(c, &g2, "nvnDepthStencilStateGetDepthFunc", 1, st, 0, 0, 0);
        // stencil: v[3] enable, v[4] / v[5] front / back function, v[6] / v[7] front / back ops (fail | depth fail << 8 |
        // pass << 16); NVNface front 1, back 2
        static uint64_t g3, g4, g5;
        static int32_t ops[3];
        o->v[3] = (int)(gcall(c, &g3, "nvnDepthStencilStateGetStencilTestEnable", 1, st, 0, 0, 0) & 0xff);
        for (int f = 0; f < 2; f++) {
            o->v[4 + f] = (int)gcall(c, &g4, "nvnDepthStencilStateGetStencilFunc", 2, st, (uint64_t)(f + 1), 0, 0);
            ops[0] = ops[1] = ops[2] = 1;
            gcall6(c, &g5, "nvnDepthStencilStateGetStencilOp", 5, st, (uint64_t)(f + 1), (uint64_t)(uintptr_t)&ops[0],
                   (uint64_t)(uintptr_t)&ops[1], (uint64_t)(uintptr_t)&ops[2], 0);
            o->v[6 + f] = (ops[0] & 0xff) | (ops[1] & 0xff) << 8 | (ops[2] & 0xff) << 16;
        }
        break;
    }
    case NX2_ST_POLYGON: {
        static uint64_t g0, g1, g2;
        o->v[0] = (int)gcall(c, &g0, "nvnPolygonStateGetCullFace", 1, st, 0, 0, 0);
        o->v[1] = (int)gcall(c, &g1, "nvnPolygonStateGetFrontFace", 1, st, 0, 0, 0);
        o->v[2] = (int)gcall(c, &g2, "nvnPolygonStateGetPolygonMode", 1, st, 0, 0, 0);
        break;
    }
    }
    static int logged;
    if (logged++ < 24)
        rt_log("nvn2: state kind %d raw %016llx -> %d %d %d %d %d %d %d %d", kind, (unsigned long long)raw, o->v[0],
               o->v[1], o->v[2], o->v[3], o->v[4], o->v[5], o->v[6], o->v[7]);
    plat_mutex_lock(&dec_lock);
    for (int i = 0; i < 8; i++) {
        StCache *e = &st_cache[(h + i) % 4096];
        if (!e->kind) {
            *e = (StCache){raw, kind, *o};
            break;
        }
    }
    plat_mutex_unlock(&dec_lock);
}

static void capture(Cpu *c, Rec *r) {
    resolve();
    int k = (int)r->proc;
    static int state_seen = -1;   // again when the game has asked for more procs (see resolve)
    if (state_seen != nx2_proc_count()) {
        state_seen = nx2_proc_count();
        for (int i = 0; i < NSTATE; i++) state_idx[i] = nx2_proc_index_by_name(state_binds[i].bind);
    }
    for (int i = 0; i < NSTATE; i++)
        if (k == state_idx[i]) {
            Nx2StateBlob b;
            if (!r->x[1]) return;
            decode_render_state(c, state_binds[i].kind, state_binds[i].size, (const uint8_t *)(uintptr_t)r->x[1], &b);
            copy_blob(r, &b, sizeof b);
            return;
        }
    if (k == idx_attrib || k == idx_stream) {   // (cb, n, const state *): blob is Nx2VertexInputState (one half)
        Nx2VertexInputState vi = {0};
        int n = (int)r->x[1] < 0 ? 0 : (int)r->x[1] > 16 ? 16 : (int)r->x[1];
        const uint8_t *states = (const uint8_t *)(uintptr_t)r->x[2];
        {   // NX2_RAW_VSTATES=1: the raw bytes of the first arrays with several entries (layout checks)
            static int raw = -1, logged;
            static const uint8_t *seen[24];
            if (raw < 0) raw = getenv("NX2_RAW_VSTATES") != NULL;
            int known = 0;
            for (int i = 0; i < logged; i++) known |= seen[i] == states;
            if (raw && states && n >= 3 && logged < 24 && !known) {
                seen[logged++] = states;
                char hex[16 * 16 * 3 + 1];
                int nb = n * 16 > 256 ? 256 : n * 16;
                for (int i = 0; i < nb; i++) snprintf(hex + 3 * i, 4, "%02x ", states[i]);
                rt_log("nvn2: %s n %d at %p: %s", k == idx_attrib ? "attrib states" : "stream states", n,
                       (const void *)states, hex);
            }
        }
        for (int i = 0; i < n && states; i++) {
            int v[3];
            decode_state(c, k == idx_attrib, states + i * (k == idx_attrib ? 4 : 8), v);
            if (k == idx_attrib) vi.a[i] = (Nx2Attrib){v[0], v[1], v[2]};
            else vi.s[i] = (Nx2Stream){v[0], v[1]};
        }
        if (k == idx_attrib) vi.nattrib = n;
        else vi.nstream = n;
        copy_blob(r, &vi, sizeof vi);
        return;
    }
    if (k == idx_copy_tex) {   // (cb, src, srcView, srcRegion *, dst, dstView, dstRegion *, flags)
        Nx2Copy cp = {r->x[1], r->x[4], {0}, {0}, (int)r->x[7], 0};
        if (r->x[3]) memcpy(cp.sr, (const void *)(uintptr_t)r->x[3], sizeof cp.sr);
        if (r->x[6]) memcpy(cp.dr, (const void *)(uintptr_t)r->x[6], sizeof cp.dr);
        copy_blob(r, &cp, sizeof cp);
        return;
    }
    if (k == idx_set_rt) {   // (cb, int numColors, const NVNtexture *const *colors, views, depth, depthView)
        // blob: Nx2RenderTargets
        Nx2RenderTargets t = {0};
        const uint64_t *colors = (const uint64_t *)(uintptr_t)r->x[2];
        t.count = (int)r->x[1] > 0 ? (int)r->x[1] : 0;
        if (t.count > 8) t.count = 8;
        for (int i = 0; i < t.count && colors; i++) t.color[i] = colors[i];
        t.depth = r->x[4];
        const uint64_t *views = (const uint64_t *)(uintptr_t)r->x[3];
        static _Thread_local int32_t *out;
        static uint64_t g_fmt;
        if (!out) out = mem_kernel_alloc(16, 16);
        for (int i = 0; i < t.count && views; i++)
            if (views[i]) {
                out[0] = 0;
                if (nx2_driver_call(c, &g_fmt, "nvnTextureViewGetFormat", 2, views[i], (uint64_t)(uintptr_t)out, 0, 0) & 0xff)
                    t.view_format[i] = out[0];
            }
        static int logged;
        for (int i = 0; i < t.count && logged < 8; i++)
            if (t.view_format[i]) {
                logged++;
                rt_log("nvn2: render target %d through a view of format %#x", i, t.view_format[i]);
            }
        {   // the layer (3D: slice) and mip level each view selects; the depth view's too (shadow map arrays)
            static uint64_t g_ly, g_lv;
            uint64_t o = (uint64_t)(uintptr_t)out;
            for (int i = 0; i <= t.count; i++) {
                uint64_t v = i < t.count ? (views ? views[i] : 0) : r->x[5];
                if (!v) continue;
                int layer = 0, level = 0;
                out[0] = out[1] = 0;
                if (nx2_driver_call(c, &g_ly, "nvnTextureViewGetLayers", 3, v, o, o + 4, 0) & 0xff) layer = out[0];
                out[0] = out[1] = 0;
                if (nx2_driver_call(c, &g_lv, "nvnTextureViewGetLevels", 3, v, o, o + 4, 0) & 0xff) level = out[0];
                if (i < t.count) t.view_layer[i] = layer, t.view_level[i] = level;
                else t.depth_layer = layer, t.depth_level = level;
                static int nl;
                if ((layer || level) && nl++ < 12)
                    rt_log("nvn2: render target %d through a view of layer %d, level %d", i < t.count ? i : -1, layer,
                           level);
            }
        }
        r->aux[0] = t.count && colors ? colors[0] : 0;
        copy_blob(r, &t, sizeof t);
        return;
    }
    for (int i = 0; i < NRULES; i++)
        if (rules[i].idx == k) {
            const CopyRule *q = &rules[i];
            uint64_t n = q->fixed ? (uint64_t)q->fixed : (uint64_t)(uint32_t)r->x[q->cnt] * (uint64_t)q->elem;
            copy_blob(r, (const void *)(uintptr_t)r->x[q->arg], n);
            if (k == idx_clear_color && r->blob) memcpy(r->aux, r->blob, 16);
            return;
        }
}

// ---- every other command: appended to the list
static uint64_t g_recorded;
static void cb_record(Cpu *c) {
    Cb *b = cb_get(c->x[0], 1);
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->recs = realloc(b->recs, b->cap * sizeof *b->recs);
        if (!b->recs) rt_fatal("nvn2: command list growth");
    }
    Rec *r = &b->recs[b->n++];
    b->since_begin++;
    r->proc = (uint32_t)nx2_proc_index(c->pc);
    memcpy(r->x, c->x, sizeof r->x);
    memset(r->aux, 0, sizeof r->aux);
    for (int i = 0; i < 4; i++) memcpy(&r->f[i], &c->v[i], 4);
    r->blob = NULL;
    r->blob_n = 0;
    capture(c, r);
    __atomic_add_fetch(&g_recorded, 1, __ATOMIC_RELAXED);
}

static const Nx2HostProc lifecycle[] = {
    {"nvnCommandBufferInitialize", cb_initialize},
    {"nvnCommandBufferFinalize", cb_finalize},
    {"nvnCommandBufferSetDebugLabel", cb_nop},
    {"nvnCommandBufferSetMemoryCallback", cb_set_callback},
    {"nvnCommandBufferSetMemoryCallbackData", cb_set_callback_data},
    {"nvnCommandBufferGetMemoryCallback", cb_get_callback},
    {"nvnCommandBufferGetMemoryCallbackData", cb_get_callback_data},
    {"nvnCommandBufferSetCommandMemoryCallbackEnabled", cb_nop},
    {"nvnCommandBufferSetControlMemoryCallbackEnabled", cb_nop},
    {"nvnCommandBufferAddCommandMemory", cb_add_command_memory},
    {"nvnCommandBufferAddControlMemory", cb_add_control_memory},
    {"nvnCommandBufferGetCommandMemorySize", cb_cmd_size},
    {"nvnCommandBufferGetCommandMemoryUsed", cb_cmd_used},
    {"nvnCommandBufferGetCommandMemoryFree", cb_cmd_free},
    {"nvnCommandBufferGetControlMemorySize", cb_ctl_size},
    {"nvnCommandBufferGetControlMemoryUsed", cb_ctl_used},
    {"nvnCommandBufferGetControlMemoryFree", cb_ctl_free},
    {"nvnCommandBufferSetRecordingMode", cb_nop},
    {"nvnCommandBufferBeginRecording", cb_begin},
    {"nvnCommandBufferEndRecording", cb_end},
    {"nvnCommandBufferIsRecording", cb_is_recording},
    {"nvnCommandBufferSetCopyRowStride", cb_set_row_stride},
    {"nvnCommandBufferSetCopyImageStride", cb_set_image_stride},
    {"nvnCommandBufferGetCopyRowStride", cb_get_row_stride},
    {"nvnCommandBufferGetCopyImageStride", cb_get_image_stride},
};

GuestFn nx2_cmd_proc(const char *name) {
    if (strncmp(name, "nvnCommandBuffer", 16)) return NULL;
    for (size_t i = 0; i < sizeof lifecycle / sizeof *lifecycle; i++)
        if (!strcmp(lifecycle[i].name, name)) return lifecycle[i].fn;
    return cb_record;
}

// ---- replay. For now on the CPU and only what reaches the screen without the GPU: clears of RGBA8 render targets
// fill the target's storage (a uniform colour is the same 4 bytes at every pixel in any layout). The Vulkan replay
// of draws replaces this.
static uint64_t g_cur_rt;   // first colour target of the queue's last SetRenderTargets

static void clear_texture(uint64_t tex, const float col[4]) {
    nvn_texdesc d;
    uint8_t *storage;
    uint64_t size;
    if (!nx2_tex_get(tex, &d, &storage, &size)) return;
    if (d.format != 0x25 && d.format != 0x26) {   // RGBA8 unorm / sRGB for now
        static int n;
        if (n++ < 8) rt_log("nvn2 replay: clear of format %#x not done on the CPU", d.format);
        return;
    }
    uint8_t px[4];
    for (int i = 0; i < 4; i++) {
        float v = col[i] < 0 ? 0 : col[i] > 1 ? 1 : col[i];
        px[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
    uint32_t pattern;
    memcpy(&pattern, px, 4);
    static int logged;
    if (logged++ < 12)
        rt_log("nvn2 replay: clear texture %#llx (%ux%u format %#x) to %.3f %.3f %.3f %.3f", (unsigned long long)tex,
               d.width, d.height, d.format, col[0], col[1], col[2], col[3]);
    uint32_t *p = (uint32_t *)storage;
    for (uint64_t i = 0; i < size / 4; i++) p[i] = pattern;
}

static uint64_t g_seen_rt, g_seen_clear, g_seen_clear_no_rt;
static uint64_t g_hist[2048];   // submitted commands per proc

void nx2_cmd_histogram(void) {
    uint64_t top[25] = {0};
    int idx[25];
    for (int k = 0; k < 25; k++) idx[k] = -1;
    for (int p = 0; p < 2048; p++)
        for (int k = 0; k < 25; k++)
            if (g_hist[p] > top[k]) {
                memmove(top + k + 1, top + k, (24 - k) * sizeof *top);
                memmove(idx + k + 1, idx + k, (24 - k) * sizeof *idx);
                top[k] = g_hist[p], idx[k] = p;
                break;
            }
    rt_log("nvn2 replay: most submitted commands so far:");
    for (int k = 0; k < 25 && idx[k] >= 0; k++)
        rt_log("  %10llu  %s", (unsigned long long)top[k], nx2_proc_name_at(idx[k]));
}
static uint64_t g_unknown_handles;
static void replay(const Rec *recs, uint32_t n, int depth);
static void replay_handle(uint64_t id, int depth) {   // during a replay (replays_active)
    plat_mutex_lock(&handle_lock);
    const Handle *h = handle_find(id);
    plat_mutex_unlock(&handle_lock);
    if (lists_log()) rt_log("lists:   call list %llu%s", (unsigned long long)id, h ? "" : " UNKNOWN");
    if (!h) {
        g_unknown_handles++;
        return;
    }
    replay(h->recs, h->n, depth);
}

static void replay(const Rec *recs, uint32_t n, int depth) {
    resolve();
    static int logged;
    if (!logged++) rt_log("nvn2 replay: SetRenderTargets is proc %d, ClearColor proc %d", idx_set_rt, idx_clear_color);
    // NX2_DUMP_SUBMITS=<n>: log every command of the first n submitted lists with its arguments
    // NX2_DUMP_SKIP=<k>: start after k submitted lists
    static int dump_left = -1, dump_skip;
    if (dump_left < 0) {
        const char *e = getenv("NX2_DUMP_SUBMITS"), *k = getenv("NX2_DUMP_SKIP");
        dump_left = e ? atoi(e) : 0;
        dump_skip = k ? atoi(k) : 0;
    }
    static int cur_dump;
    if (!depth) cur_dump = dump_skip > 0 ? (dump_skip--, 0) : dump_left > 0 ? dump_left-- : 0;
    int dump = cur_dump;
    if (dump) rt_log("nvn2 dump: list of %u commands (call depth %d)", n, depth);
    for (uint32_t i = 0; i < n; i++) {
        const Rec *r = &recs[i];
        if (dump)
            rt_log("  %-44s %llx %llx %llx %llx %llx %llx | %llx %llx", nx2_proc_name_at((int)r->proc) + 16,
                   (unsigned long long)r->x[1], (unsigned long long)r->x[2], (unsigned long long)r->x[3],
                   (unsigned long long)r->x[4], (unsigned long long)r->x[5], (unsigned long long)r->x[6],
                   (unsigned long long)r->aux[0], (unsigned long long)r->aux[1]);
        if (dump && r->blob) {
            char hex[3 * 96 + 1] = {0};
            for (uint32_t b = 0; b < r->blob_n && b < 96; b++) snprintf(hex + 3 * b, 4, "%02x ", r->blob[b]);
            rt_log("      blob %u: %s", r->blob_n, hex);
        }
        if (((int)r->proc == idx_call || (int)r->proc == idx_copy) && r->blob && depth < 8) {
            const uint64_t *ids = (const uint64_t *)r->blob;
            for (uint32_t k = 0; k < r->blob_n / 8; k++) replay_handle(ids[k], depth + 1);
            continue;
        }
        nx2_exec(r->proc, r->x, r->f, r->blob, r->blob_n, dump);
        if (r->proc < 2048) __atomic_add_fetch(&g_hist[r->proc], 1, __ATOMIC_RELAXED);
        if ((int)r->proc == idx_set_rt) g_cur_rt = r->aux[0], g_seen_rt++;
        if ((int)r->proc == idx_clear_color) g_seen_clear++, g_seen_clear_no_rt += !g_cur_rt;

    }
}

static uint64_t g_submitted_cmds;
void nx2_cmd_submitted(int count, const uint64_t *ids) {
    extern uint64_t nx2_prof_submit_ns;
    uint64_t t0 = plat_now_ns();
    plat_mutex_lock(&handle_lock);
    replays_active++;
    plat_mutex_unlock(&handle_lock);
    for (int i = 0; i < count; i++) {
        plat_mutex_lock(&handle_lock);
        const Handle *h = handle_find(ids[i]);
        plat_mutex_unlock(&handle_lock);
        if (lists_log()) rt_log("lists: submit list %llu%s", (unsigned long long)ids[i], h ? "" : " UNKNOWN");
        if (h) {
            g_submitted_cmds += h->n;
            replay(h->recs, h->n, 0);
        } else {
            g_unknown_handles++;
        }
    }
    plat_mutex_lock(&handle_lock);
    Handle *free_now = --replays_active ? NULL : graveyard;
    if (!replays_active) graveyard = NULL;
    plat_mutex_unlock(&handle_lock);
    while (free_now) {
        Handle *nx = free_now->next;
        free_recs(free_now->recs, free_now->n);
        free(free_now);
        free_now = nx;
    }
    nx2_vk_submit_end();
    nx2_prof_submit_ns += plat_now_ns() - t0;
    extern uint64_t nx2_stat_submit_ns;   // the overlay's counter (never reset)
    nx2_stat_submit_ns += plat_now_ns() - t0;
}

void nx2_cmd_replay_stats(uint64_t *rts, uint64_t *clears, uint64_t *clears_no_rt) {
    *rts = g_seen_rt, *clears = g_seen_clear, *clears_no_rt = g_seen_clear_no_rt;
}
void nx2_cmd_stats(uint64_t *recorded, uint64_t *submitted, uint64_t *unknown) {
    *recorded = g_recorded;
    *submitted = g_submitted_cmds;
    *unknown = g_unknown_handles;
}
