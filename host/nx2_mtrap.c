// Debug (NX2_MIRROR_TRAP=1 with NX2_MIRROR_VERIFY=1): finds who writes game memory without the write-watch seeing
// it. nx2_vkdraw.c arms a pool after its mirror sync when the verifier saw unmarked changes in it: its pages become
// read-only, and the first write to each page faults here. The fault is logged (thread, guest code, host address) and
// the page made writable again, so the write then goes through.
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

#include "host.h"

#define TRAP_MAX 8
static struct { uint64_t base, size; } traps[TRAP_MAX];
static volatile LONG ntraps;
static volatile LONG nlogged;

static LONG CALLBACK trap_veh(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    uint64_t a = (uint64_t)er->ExceptionInformation[1];
    for (LONG i = 0; i < ntraps; i++) {
        if (a - traps[i].base >= traps[i].size) continue;
        DWORD old;
        VirtualProtect((void *)(uintptr_t)(a & ~0xfffull), 0x1000, PAGE_READWRITE, &old);
        if (InterlockedIncrement(&nlogged) <= 60) {
            char g[160] = "host thread";
            Cpu *c = kern_cpu();
            if (c) {
                char pc[96], lr[96];
                snprintf(g, sizeof g, "guest pc %s lr %s", ldr_describe(c->pc, pc, sizeof pc),
                         ldr_describe(c->x[30], lr, sizeof lr));
            }
            uint64_t rip = (uint64_t)ep->ContextRecord->Rip, exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
            rt_log("mtrap: write to %#llx (pool %#llx +%#llx) by thread %lu, %s, host rip exe+%#llx",
                   (unsigned long long)a, (unsigned long long)traps[i].base, (unsigned long long)(a - traps[i].base),
                   GetCurrentThreadId(), g, (unsigned long long)(rip - exe));
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// NX2_READ_TRAP=1 (debug): the CPU storage of images the GPU wrote is made inaccessible; the first access by anyone
// (the game reading GPU results from memory, which nothing copies back) is logged with the image and the guest code,
// and the range made accessible again.
#define RTRAP_MAX 512
static struct { uint64_t base, size; char what[96]; } rtraps[RTRAP_MAX];
static volatile LONG nrtraps, nrlogged;
static LONG CALLBACK rtrap_veh(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t a = (uint64_t)er->ExceptionInformation[1];
    for (LONG i = 0; i < nrtraps; i++) {
        if (a - rtraps[i].base >= rtraps[i].size) continue;
        DWORD old;
        VirtualProtect((void *)(uintptr_t)rtraps[i].base, rtraps[i].size, PAGE_READWRITE, &old);
        if (InterlockedIncrement(&nrlogged) <= 200) {
            char g[200] = "host thread";
            Cpu *c = kern_cpu();
            if (c) {
                char pc[96], lr[96];
                snprintf(g, sizeof g, "guest pc %s lr %s", ldr_describe(c->pc, pc, sizeof pc), ldr_describe(c->x[30], lr, sizeof lr));
            }
            uint64_t rip = (uint64_t)ep->ContextRecord->Rip, exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
            rt_log("rtrap: %s of %s at +%#llx by thread %lu, %s, host rip exe+%#llx", er->ExceptionInformation[0] ? "write" : "read",
                   rtraps[i].what, (unsigned long long)(a - rtraps[i].base), GetCurrentThreadId(), g,
                   (unsigned long long)(rip - exe));
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void nx2_rtrap_arm(uint64_t base, uint64_t size, const char *what) {
    static int installed;
    if (!installed) installed = 1, AddVectoredExceptionHandler(1, rtrap_veh);
    uint64_t b = base & ~0xfffull, e = (base + size + 0xfff) & ~0xfffull;
    LONG i = 0;
    while (i < nrtraps && rtraps[i].base != b) i++;
    if (i == nrtraps) {
        if (nrtraps >= RTRAP_MAX) return;
        rtraps[i].base = b, rtraps[i].size = e - b;
        snprintf(rtraps[i].what, sizeof rtraps[i].what, "%s", what);
        InterlockedIncrement(&nrtraps);
    }
    DWORD old;
    VirtualProtect((void *)(uintptr_t)b, e - b, PAGE_NOACCESS, &old);
}

// Makes [base, base + size) read-only until each page's next write (logged).
void nx2_mtrap_arm(uint64_t base, uint64_t size) {
    static int installed;
    if (!installed) installed = 1, AddVectoredExceptionHandler(1, trap_veh);
    int known = 0;
    for (LONG i = 0; i < ntraps; i++) known |= traps[i].base == base;
    if (!known) {
        if (ntraps >= TRAP_MAX) return;
        traps[ntraps].base = base, traps[ntraps].size = size;
        InterlockedIncrement(&ntraps);
    }
    DWORD old;
    VirtualProtect((void *)(uintptr_t)base, size, PAGE_READONLY, &old);
}

// VirtualQuery of a game address, for logs: allocation base, region type (private / mapped / image), protection.
const char *nx2_mem_describe(uint64_t a, char *buf, int n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) {
        snprintf(buf, (size_t)n, "?");
        return buf;
    }
    snprintf(buf, (size_t)n, "alloc %#llx region %#llx+%#llx %s prot %#lx", (unsigned long long)(uintptr_t)mbi.AllocationBase,
             (unsigned long long)(uintptr_t)mbi.BaseAddress, (unsigned long long)mbi.RegionSize,
             mbi.Type == MEM_PRIVATE ? "private" : mbi.Type == MEM_MAPPED ? "mapped" : mbi.Type == MEM_IMAGE ? "image" : "?",
             mbi.Protect);
    return buf;
}
