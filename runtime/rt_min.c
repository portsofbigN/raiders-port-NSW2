// Minimal host for stage 3: maps the prelinked modules, dispatches indirect branches, enters main's entry point and
// stops at the first supervisor call with a register dump. No kernel, services or graphics yet (stage 4 and later).
//
// Usage: splatoon.exe <prelinked dir>   (tools/prelink.py output: <mod>.img per module)
#include "cpu.h"
#include "layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#define DECL(n, N) extern const uint64_t n##_fn_addrs[]; extern const GuestFn n##_fn_ptrs[]; extern const uint64_t n##_fn_count;
MOD_LIST(DECL)

volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }

// ---- dispatch: one GuestFn slot per 4-byte guest instruction over the module range
static GuestFn *g_slot;
static uint64_t g_lo, g_hi;

static void dump(const Cpu *c, const char *why, uint64_t pc) {
    fprintf(stderr, "\n== %s at pc %#llx (lr %#llx, sp %#llx)\n", why, (unsigned long long)pc,
            (unsigned long long)c->x[30], (unsigned long long)c->sp);
    for (int i = 0; i < 31; i++)
        fprintf(stderr, "x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "  ");
    fprintf(stderr, "\nnzcv %08llx  tpidr %llx  tpidrro %llx\n", (unsigned long long)c->nzcv,
            (unsigned long long)c->tpidr, (unsigned long long)c->tpidrro);
    fflush(stderr);
}

static const char *module_of(uint64_t a, uint64_t *off) {
#define FIND(n, N) if (a >= MOD_##N##_BASE && a < MOD_##N##_BASE + MOD_##N##_SIZE) { *off = a - MOD_##N##_BASE; return #n; }
    MOD_LIST(FIND)
#undef FIND
    *off = a;
    return "?";
}

// Host functions the guest can call (the callbacks rtld would pass): fake guest addresses in an unmapped range.
#define HOSTFN_BASE 0x6FFE000000ull
#define HOSTFN_MAX 64
static GuestFn g_hostfn[HOSTFN_MAX];
static const char *g_hostfn_name[HOSTFN_MAX];
static int g_nhostfn;

static uint64_t hostfn_register(const char *name, GuestFn fn) {
    g_hostfn[g_nhostfn] = fn;
    g_hostfn_name[g_nhostfn] = name;
    return HOSTFN_BASE + 16ull * g_nhostfn++;
}

void rt_dispatch(Cpu *c) {
    uint64_t pc = c->pc;
    if (pc >= g_lo && pc < g_hi && !(pc & 3)) {
        GuestFn f = g_slot[(pc - g_lo) >> 2];
        if (f) { f(c); return; }
    }
    if (pc >= HOSTFN_BASE && pc < HOSTFN_BASE + 16ull * g_nhostfn && !(pc & 15)) {
        g_hostfn[(pc - HOSTFN_BASE) >> 4](c);
        return;
    }
    uint64_t off;
    const char *m = module_of(pc, &off);
    char why[96];
    snprintf(why, sizeof why, "dispatch miss (%s+%#llx)", m, (unsigned long long)off);
    dump(c, why, pc);
    exit(3);
}

static void dispatch_init(void) {
    g_lo = MOD_MAIN_BASE;
    g_hi = MOD_SDK_BASE + MOD_SDK_SIZE;
    size_t bytes = (size_t)((g_hi - g_lo) >> 2) * sizeof(GuestFn);
    g_slot = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_slot) { fprintf(stderr, "dispatch table: VirtualAlloc %zu failed\n", bytes); exit(1); }
    uint64_t total = 0;
#define FILL(n, N) for (uint64_t i = 0; i < n##_fn_count; i++) g_slot[(n##_fn_addrs[i] - g_lo) >> 2] = n##_fn_ptrs[i]; total += n##_fn_count;
    MOD_LIST(FILL)
#undef FILL
    fprintf(stderr, "dispatch: %llu functions\n", (unsigned long long)total);
}

// ---- traps and supervisor calls
uint64_t rt_ticks(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    // Whole seconds and the remainder apart: no 128-bit division (the MSVC target has no __divti3).
    uint64_t q = (uint64_t)t.QuadPart, hz = (uint64_t)f.QuadPart;
    return q / hz * 19200000 + q % hz * 19200000 / hz;
}

void rt_spin_gate(Cpu *c, uint64_t head) { (void)c; (void)head; }

static uint64_t g_svc_count;

void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    uint64_t off;
    const char *m = module_of(pc, &off);
    char why[96];
    snprintf(why, sizeof why, "svc %#x #%llu (%s+%#llx)", imm, (unsigned long long)++g_svc_count, m,
             (unsigned long long)off);
    dump(c, why, pc);
    fprintf(stderr, "stage 3 stops at the first svc\n");
    exit(0);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char why[48];
    snprintf(why, sizeof why, "brk %#x", imm);
    dump(c, why, pc);
    exit(4);
}

void rt_udf(Cpu *c, uint64_t pc) {
    uint64_t off;
    const char *m = module_of(pc, &off);
    char why[96];
    snprintf(why, sizeof why, "undefined instruction (%s+%#llx)", m, (unsigned long long)off);
    dump(c, why, pc);
    exit(5);
}

// ---- loading
static void map_module(const char *dir, const char *name, uint64_t base, uint64_t size) {
    void *p = VirtualAlloc((void *)(uintptr_t)base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)base) {
        fprintf(stderr, "%s: cannot map %#llx+%#llx (error %lu)\n", name, (unsigned long long)base,
                (unsigned long long)size, GetLastError());
        exit(1);
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.img", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    size_t n = fread(p, 1, size, f);
    fclose(f);
    fprintf(stderr, "%-8s %#llx  %zu bytes\n", name, (unsigned long long)base, n);
}

static LONG WINAPI on_fault(EXCEPTION_POINTERS *e) {
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    fprintf(stderr, "\n== host exception %#lx at host rip %p", r->ExceptionCode, r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, " (%s %#llx)", r->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)r->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- boot: rtld's hand-off. After linking, rtld calls the SDK's
// nn::init::Start(main thread handle, arguments region, loader_ready, module_inits, module_finis); Start runs the
// callbacks and then nnMain. Same order as TotK Recomp's runtime/boot.c: DT_INIT of main, subsdk0, sdk.
#include "boot.h"

// Call guest code at addr with up to 8 arguments; returns x0. The guest's RET returns from its C function.
static uint64_t rt_call(Cpu *c, uint64_t addr, int n, const uint64_t *args) {
    uint64_t saved_lr = c->x[30], saved_pc = c->pc;
    for (int i = 0; i < n; i++) c->x[i] = args[i];
    c->pc = addr;
    rt_dispatch(c);
    c->x[30] = saved_lr;
    c->pc = saved_pc;
    return c->x[0];
}

static void cb_loader_ready(Cpu *c) { (void)c; fprintf(stderr, "boot: loader_ready callback\n"); }

static void cb_module_inits(Cpu *c) {
    static const struct { const char *name; uint64_t init; } inits[] = {
#define INIT(n, N) {#n, MOD_##N##_INIT},
        MOD_INIT_LIST(INIT)
#undef INIT
    };
    for (size_t i = 0; i < sizeof inits / sizeof *inits; i++) {
        if (!inits[i].init) continue;
        fprintf(stderr, "boot: %s DT_INIT at %#llx\n", inits[i].name, (unsigned long long)inits[i].init);
        rt_call(c, inits[i].init, 0, NULL);
    }
    fprintf(stderr, "boot: module initialisers done\n");
}

static void cb_module_finis(Cpu *c) {
    fprintf(stderr, "boot: module finalisers requested: the game exited\n");
    dump(c, "exit", c->pc);
    exit(0);
}

#define STACK_SIZE (8u << 20)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <prelinked dir>\n", argv[0]); return 2; }
    SetUnhandledExceptionFilter(on_fault);
#define MAP(n, N) map_module(argv[1], #n, MOD_##N##_BASE, MOD_##N##_SIZE);
    MOD_LIST(MAP)
#undef MAP
    dispatch_init();

    static Cpu cpu;
    Cpu *c = &cpu;
    uint8_t *stack = VirtualAlloc(NULL, STACK_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *tls = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);   // thread local region
    c->sp = (uint64_t)(uintptr_t)(stack + STACK_SIZE);
    c->tpidrro = (uint64_t)(uintptr_t)tls;
    uint64_t args[5] = {
        0xFFFF8001,   // main thread handle (pseudo-handle until the kernel exists)
        0,            // arguments region
        hostfn_register("loader_ready", cb_loader_ready),
        hostfn_register("module_inits", cb_module_inits),
        hostfn_register("module_finis", cb_module_finis),
    };
    fprintf(stderr, "boot: nn::init::Start at %#llx\n", (unsigned long long)SDK_INIT_START);
    rt_call(c, SDK_INIT_START, 5, args);
    fprintf(stderr, "boot: nn::init::Start returned (x0 %#llx)\n", (unsigned long long)c->x[0]);
    return 0;
}
