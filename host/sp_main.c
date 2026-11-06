// Splatoon Raiders recomp entry. Logging, configuration, the trap services the generated code calls and the crash
// reporter are adapted from TotK Recomp runtime/main.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh), which adapts
// LAS Recomp (MIT, Copyright (c) 2026 Peiikai). The boot follows TotK Recomp runtime/boot.c: the guest main thread
// runs the SDK's own nn::init::Start with three host callbacks standing in for rtld.
//
//   splatoon.exe [--exefs DIR] [--romfs DIR] [--profile DIR]
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "sha256.h"
void nx2_ring_dump(void);   // nx2_gfx.c: the last NVN2 calls

#define MAX_MODULES 16

// ---- logging, tracing, hashing
unsigned rt_trace;

// Log file. A game run whose stderr is not a file already (started from Explorer, a shortcut or a console) also
// writes its log to logs\totk.log next to totk.exe, so a player has one to attach to a report; run_game.py and
// `2> file` keep their own file. Each start moves the earlier logs to totk.1.log .. totk.4.log. A file stops at 64 MiB
// (TOTK_TRACE can write that much). Lines from before log_open wait in a static buffer: mem_init runs first so that
// nothing else lands in the guest address space, and an earlier open would put the CRT's heap there.
#define LOG_KEEP 5
#define LOG_CAP (64ull << 20)
enum { LOG_EARLY, LOG_FILE, LOG_OFF };
static int g_log_state;
static PlatAppend *g_log_file;
static char g_log_path[PLAT_PATH_MAX * 3];
static char g_log_early[32768];
static size_t g_log_early_n;
static uint64_t g_log_bytes;

void rt_log_raw(const char *p, size_t n) {
    fwrite(p, 1, n, stderr);
    int state = __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE);
    if (state == LOG_EARLY) {
        size_t at = __atomic_fetch_add(&g_log_early_n, n, __ATOMIC_RELAXED);
        if (at + n <= sizeof g_log_early) memcpy(g_log_early + at, p, n);
    } else if (state == LOG_FILE) {
        uint64_t at = __atomic_fetch_add(&g_log_bytes, n, __ATOMIC_RELAXED);
        if (at + n <= LOG_CAP) {
            plat_append_write(g_log_file, p, n);
        } else if (at <= LOG_CAP) {   // the first write past the cap says so, once
            static const char cut[] = "[host] log: the file reached 64 MiB, the rest of this run goes to stderr only\n";
            plat_append_write(g_log_file, cut, sizeof cut - 1);
        }
    }
}

void rt_vlogf(const char *fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) rt_log_raw(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[host] ");
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
    buf[n++] = '\n';
    rt_log_raw(buf, n);
}

const char *rt_log_path(void) {
    return __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE ? g_log_path : NULL;
}

// Opens the log file for a game run (game = 1) and writes the buffered lines to it; anything else drops them. Runs on
// the main thread before the boot starts other threads.
static void log_open(int game) {
    char dir[PLAT_PATH_MAX * 3], from[PLAT_PATH_MAX * 3 + 16], to[PLAT_PATH_MAX * 3 + 16];
    char *cut = NULL;
    if (game && !plat_stderr_path(dir, sizeof dir) && plat_exe_path(dir, sizeof dir)) {
        cut = plat_last_sep(dir);
        char *f = strrchr(dir, '/');
        if (f > cut) cut = f;
    }
    if (cut && (size_t)(cut - dir) + sizeof "/logs" <= sizeof dir) {
        strcpy(cut, "/logs");
        plat_file_mkdir_p(dir);
        for (int i = LOG_KEEP - 1; i > 0; i--) {   // totk.3.log -> totk.4.log, ..., totk.log -> totk.1.log
            if (i > 1) snprintf(from, sizeof from, "%s/splatoon.%d.log", dir, i - 1);
            else snprintf(from, sizeof from, "%s/splatoon.log", dir);
            snprintf(to, sizeof to, "%s/splatoon.%d.log", dir, i);
            if (plat_file_exists(from)) plat_file_replace(from, to);
        }
        snprintf(g_log_path, sizeof g_log_path, "%s/splatoon.log", dir);
        g_log_file = plat_append_open(g_log_path);
    }
    if (!g_log_file) {
        __atomic_store_n(&g_log_state, LOG_OFF, __ATOMIC_RELEASE);
        return;
    }
    size_t n = __atomic_load_n(&g_log_early_n, __ATOMIC_RELAXED);
    if (n > sizeof g_log_early) n = sizeof g_log_early;
    plat_append_write(g_log_file, g_log_early, n);
    g_log_bytes = n;
    __atomic_store_n(&g_log_state, LOG_FILE, __ATOMIC_RELEASE);
    rt_log("log: %s", g_log_path);
}

static void parse_trace(void) {
    const char *e = cfg_get("TOTK_TRACE");   // no default: nothing is traced when unset
    if (!e) return;
    static const struct { const char *name; unsigned bit; } names[] = {
        {"svc", RT_TRACE_SVC}, {"host", RT_TRACE_HOSTCALL}, {"ipc", RT_TRACE_IPC}, {"fs", RT_TRACE_FS},
        {"gfx", RT_TRACE_GFX}, {"perf", RT_TRACE_PERF}, {"sched", RT_TRACE_SCHED}, {"audio", RT_TRACE_AUDIO},
        {"present", RT_TRACE_PRESENT}, {"all", ~0u}};
    for (const char *p = e; *p;) {
        size_t n = strcspn(p, ",");
        int known = 0;
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) rt_trace |= names[i].bit, known = 1;
        if (!known && n) rt_log("TOTK_TRACE: unknown item '%.*s' (svc, host, ipc, fs, gfx, perf, sched, audio, present, all)", (int)n, p);
        p += n + (p[n] == ',');
    }
}

void rt_sha256(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }

// ---- configuration
static RtConfig cfg;
const RtConfig *rt_config(void) { return &cfg; }

// ---- runtime services the generated code calls (cpu.h)
volatile int g_fnprof = 0;   // NX2_FNTRACE=1 turns it on (fnprof_hit below)
// NX2_FNTRACE=1: every guest function entry (FN_ENTER in the generated code) goes into a ring of the last FNRING
// entries, which the crash reporter prints: the path into a crash, without a debugger. Off by default (one predictable
// branch per call).
// A ring per thread: busy worker threads would push the crashing thread's entries out of a shared one. The crash
// reporter runs on the crashing thread, so it prints its own ring.
#define FNRING 256
typedef struct { uint64_t addr, lr, x0; } FnHit;
static _Thread_local FnHit fn_ring[FNRING];
static _Thread_local uint64_t fn_next;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { fn_ring[fn_next++ % FNRING] = (FnHit){addr, lr, x0}; }
static void fn_ring_dump(uint32_t tid) {
    if (!g_fnprof) return;
    uint64_t end = fn_next, start = end > 80 ? end - 80 : 0;
    char a[128], l[128];
    rt_logf("last guest function entries on thread %u (newest last):\n", tid);
    for (uint64_t i = start; i < end; i++) {
        const FnHit *h = &fn_ring[i % FNRING];
        rt_logf("  %s  from %s  x0=%#llx\n", ldr_describe(h->addr, a, sizeof a), ldr_describe(h->lr, l, sizeof l),
                (unsigned long long)h->x0);
    }
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("brk #%#x at %s (lr %s)", imm, ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
}

void rt_udf(Cpu *c, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("undefined instruction at %s (lr %s)", ldr_describe(pc, at, sizeof at),
             ldr_describe(c->x[30], lr, sizeof lr));
}

// CNTPCT_EL0: the Switch counter runs at 19.2 MHz. The scale is set before main, so no thread reads it half written.
static PlatScale tick_scale;

__attribute__((constructor)) static void tick_scale_init(void) { tick_scale = plat_scale(19200000u, plat_counter_hz()); }

// NX2_VIRTUAL_TIME=<present> (tests): from that present on the counter follows the presents instead of the wall clock,
// 1/60 s per frame (320000 ticks) plus one tick per read inside a frame (busy waits still see time pass; at most
// 300000, so the counter never runs backwards). The game moves the same way in every run of an input script, and with
// NX2_FPS=0 faster than real time. Host sleeps and timeouts still use the wall clock.
uint64_t rt_ticks(void) {
    uint64_t real = plat_scaled(tick_scale, plat_counter());
    static int64_t from = -2;
    static volatile uint64_t base, frame_seen, calls;
    if (from == -2) from = getenv("NX2_VIRTUAL_TIME") ? atoll(getenv("NX2_VIRTUAL_TIME")) : -1;
    if (from < 0) return real;
    extern uint64_t nx2_vk_frame;
    uint64_t f = __atomic_load_n(&nx2_vk_frame, __ATOMIC_ACQUIRE);
    if ((int64_t)f < from) return real;
    if (!base) {
        uint64_t b0 = real - f * 320000ull;
        __atomic_compare_exchange_n(&base, &(uint64_t){0}, b0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        rt_log("time: virtual from present %llu (1/60 s per present)", (unsigned long long)f);
    }
    if (__atomic_load_n(&frame_seen, __ATOMIC_ACQUIRE) != f) {
        __atomic_store_n(&calls, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&frame_seen, f, __ATOMIC_RELEASE);
    }
    uint64_t c = __atomic_add_fetch(&calls, 1, __ATOMIC_ACQ_REL);
    return base + f * 320000ull + (c < 300000 ? c : 300000);
}

// ---- crash reporter
typedef struct { uintptr_t host; uint64_t guest; } FnRev;
static FnRev *fn_rev;
static uint64_t fn_rev_n;

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const FnRev *)a)->host, y = ((const FnRev *)b)->host;
    return x < y ? -1 : x > y;
}

// The lifted function that contains a host code address, by the nearest function start below it; 0 outside the lifted
// code.
static uint64_t lifted_function(uintptr_t rip) {
    if (!fn_rev) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        fn_rev = malloc((total ? total : 1) * sizeof *fn_rev);
        if (!fn_rev) return 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++)
                fn_rev[fn_rev_n++] = (FnRev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(fn_rev, fn_rev_n, sizeof *fn_rev, by_host);
    }
    // The lifted code is one range with the host code outside it (kernel.c ap_init checks that); an address above the
    // last function start is taken for host code, which costs only the last function's body.
    if (!fn_rev_n || rip > fn_rev[fn_rev_n - 1].host) return 0;
    uint64_t lo = 0, hi = fn_rev_n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (fn_rev[mid].host <= rip) lo = mid + 1;
        else hi = mid;
    }
    return lo ? fn_rev[lo - 1].guest : 0;
}

// For the sampling profiler (nx2_sampler.c).
uint64_t nx2_lifted_function(uintptr_t rip) { return lifted_function(rip); }

static int readable(uint64_t a, uint64_t n) { return plat_mem_readable(a, n); }

static void crash_handler(const PlatCrash *x) {
    static volatile int32_t busy;
    if (plat_xchg32(&busy, 1)) plat_exit(71);
    char d[160];
    uintptr_t rip = x->rip;
    rt_logf("FATAL: %s (%#x) at host %p (splatoon+%#llx)", x->what, x->code, (void *)rip, (unsigned long long)x->offset);
    if (x->access >= 0)
        rt_logf(", %s address %#llx", x->access == 1 ? "writing" : x->access == 8 ? "executing" : "reading",
                (unsigned long long)x->addr);
    rt_log_raw("\n", 1);
    uint64_t f = lifted_function(rip);
    if (f) rt_logf("  in lifted function f_%llx = %s\n", (unsigned long long)f, ldr_describe(f, d, sizeof d));
    for (int i = 0; i < x->nframes; i++) {   // abort and invalid parameter: where the host code called it
        uint64_t g = lifted_function(x->frames[i]);
        rt_logf("  host frame %2d: splatoon+%#llx", i, (unsigned long long)(x->frames[i] - (rip - x->offset)));
        if (g) rt_logf(" in lifted function f_%llx", (unsigned long long)g);
        rt_log_raw("\n", 1);
    }
    Cpu *c = kern_cpu();
    if (c) {
        for (int i = 0; i < 31; i++)
            rt_logf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
        rt_logf("  sp  %016llx\n  pc  %016llx  lr %s\n", (unsigned long long)c->sp, (unsigned long long)c->pc,
                ldr_describe(c->x[30], d, sizeof d));
        if (c->pc >= RT_HOSTFN_BASE && c->pc < RT_HOSTFN_BASE + RT_HOSTFN_SIZE) {   // in a host function
            const char *hn = hostfn_name(c->pc);
            rt_logf("  host function: %s\n", hn ? hn : "?");
        }
        uint64_t fp = c->x[29];
        for (int n = 0; n < 32 && fp && !(fp & 7) && readable(fp, 16); n++) {
            uint64_t next = LD64(fp), lr = LD64(fp + 8);
            rt_logf("  guest frame %2d: %s\n", n, ldr_describe(lr, d, sizeof d));
            if (next <= fp || next - fp > (64u << 20)) break;
            fp = next;
        }
    } else {
        rt_logf("  not on a guest thread\n");
    }
    kern_free_cores_note();
    fn_ring_dump(plat_thread_id());
    nx2_ring_dump();
    fflush(stderr);
    plat_exit(71);
}

// ---- self-test: layout, dispatch table, host-function registry, without game files
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

// ---- boot: rtld's hand-off (TotK Recomp runtime/boot.c, without its game-version checks)
#define SYM_INIT_START "_ZN2nn4init5StartEmmPFvvES2_S2_"
static uint64_t cb_addr[3];
uint64_t rt_guest_start_ns;

static void cb_loader_ready(Cpu *c) { (void)c; rt_log("boot: rtld callback: modules linked"); }

// DT_INIT of each module registers it with the C library and runs its init_array; main, subsdk0, sdk in that order.
static void cb_module_inits(Cpu *c) {
    static const char *order[] = {"main", "subsdk0", "sdk"};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < ldr_count(); i++) {
            const RtLoaded *m = ldr_module(i);
            if (strcmp(m->name, order[k])) continue;
            if (!m->init) { rt_log("boot: %s has no DT_INIT", m->name); continue; }
            rt_log("boot: ENTER %s DT_INIT (%llu init_array entries)", m->name,
                   (unsigned long long)(m->init_array_size / 8));
            rt_call(c, m->init, 0);
        }
    rt_log("boot: module initialisers done");
}

static void cb_module_finis(Cpu *c) {
    char lr[128];
    rt_fatal("boot: the game asked for module finalizers (from %s): the guest exited",
             ldr_describe(c->x[30] - 4, lr, sizeof lr));
}

// Checkpoints the host code reports (ov_system.c: SetFocusHandlingMode; graphics: the first NVN call). Logged with
// the guest caller; TotK's version also ends a --boot run at "nvn".
void rt_boot_event(const char *what, Cpu *c, const char *detail) {
    char at[160];
    rt_log("boot: %s %s (from %s)", what, detail ? detail : "", c ? ldr_describe(c->x[30], at, sizeof at) : "host");
}

static void guest_main(Cpu *c) {
    uint32_t handle = kern_main_thread(&c);
    uint64_t start = ldr_symbol(SYM_INIT_START);
    if (!start) rt_fatal("boot: %s is not exported", SYM_INIT_START);
    char d[128];
    rt_log("boot: ENTER nn::init::Start at %s, main thread %#x", ldr_describe(start, d, sizeof d), handle);
    rt_call(c, start, 5, (uint64_t)handle, (uint64_t)0, cb_addr[0], cb_addr[1], cb_addr[2]);
    rt_fatal("boot: nn::init::Start returned");
}

int main(int argc, char **argv) {
    mem_init();   // first: claim the guest address space before anything else can land there
    int nx2_settings_start(void);
    if (!nx2_settings_start()) return 0;   // splatoon.ini and the launcher (nx2_settings.c); 0: the player quit
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    plat_crash_hook(crash_handler);
    parse_trace();   // TOTK_TRACE=svc,ipc,host,... (cfg.c reads the TOTK_* names)
    { const char *ft = getenv("NX2_FNTRACE"); if (ft && *ft == '1') g_fnprof = 1; }
    plat_timer_1ms();
    cfg = (RtConfig){.exefs = rt_default_exefs, .romfs = rt_default_romfs, .profile = rt_default_profile,
                     .gpu = "null"};
    int nx2_game_paths(const char **exefs, const char **romfs, const char **profile);
    if (!nx2_game_paths(&cfg.exefs, &cfg.romfs, &cfg.profile)) return 1;   // game folder, saves (nx2_settings.c)
    for (int i = 1; i < argc; i++) {
        const char **path = !strcmp(argv[i], "--exefs") ? &cfg.exefs : !strcmp(argv[i], "--romfs") ? &cfg.romfs
                          : !strcmp(argv[i], "--profile") ? &cfg.profile : NULL;
        if (!path || ++i == argc) {
            fprintf(stderr, "usage: %s [--exefs DIR] [--romfs DIR] [--profile DIR]\n", argv[0]);
            return 2;
        }
        *path = argv[i];
    }
    log_open(1);
    cfg.boot = 1;
    rt_log("splatoon recomp: exefs %s", cfg.exefs);
    ldr_load(cfg.exefs);
    cb_addr[0] = hostfn_register("<rtld loader_ready>", cb_loader_ready);
    cb_addr[1] = hostfn_register("<rtld module_inits>", cb_module_inits);
    cb_addr[2] = hostfn_register("<rtld module_finis>", cb_module_finis);
    hostfn_seal();
    rt_guest_start_ns = kern_now_ns();
    kern_run_on_guest_main(guest_main);
    rt_fatal("boot: the guest main thread ended");
}
