// Sampling profiler (NX2_SAMPLE_PROF=1): a background thread interrupts the render thread (the one that calls
// nvnQueueSubmitCommands) about 1000 times a second and counts where it was. Every 5 seconds samples.txt gets the
// counts: host code as offsets in the executable (tools/symbolize_samples.py names them with the PDB), the game's own
// lifted code by guest function. NX2_SAMPLE_FROM=<present>: start counting at that present.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <tlhelp32.h>

#include "host.h"

uint64_t nx2_lifted_function(uintptr_t rip);   // sp_main.c: the guest function of a lifted code address, 0 if none
extern uint64_t nx2_vk_frame;

#define SLOTS 65536
static struct { uint64_t key, count; } table[SLOTS];   // key: host offset, or guest address | 1 << 63
static HANDLE target;
static volatile DWORD last_tid;   // the thread of the last nvnQueueSubmitCommands
static volatile LONG started;
static uint64_t total, lifted_total, idle_total;
#define MODS 32
static struct { HMODULE h; char name[64]; uint64_t count; } mods[MODS];   // samples outside the executable, by DLL
static int nmods;

static void count(uint64_t key) {
    uint64_t h = (key * 0x9e3779b97f4a7c15ull) >> 48;
    for (uint64_t i = 0; i < SLOTS; i++) {
        uint64_t j = (h + i) & (SLOTS - 1);
        if (table[j].key == key) { table[j].count++; return; }
        if (!table[j].key) { table[j].key = key, table[j].count = 1; return; }
    }
}

static int by_count(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[1], y = ((const uint64_t *)b)[1];
    return x < y ? 1 : x > y ? -1 : 0;
}

static void dump(void) {
    static uint64_t sorted[SLOTS][2];
    int n = 0;
    for (int i = 0; i < SLOTS; i++)
        if (table[i].key) sorted[n][0] = table[i].key, sorted[n][1] = table[i].count, n++;
    qsort(sorted, (size_t)n, sizeof sorted[0], by_count);
    FILE *f = fopen("samples.txt.new", "w");
    if (!f) return;
    fprintf(f, "# %llu samples of the render thread, %llu in lifted game code, %llu waiting in the kernel\n",
            (unsigned long long)total, (unsigned long long)lifted_total, (unsigned long long)idle_total);
    for (int k = 0; k < nmods; k++) {
        const char *b = strrchr(mods[k].name, 92);   // the file name after the last backslash
        fprintf(f, "# dll %llu %s\n", (unsigned long long)mods[k].count, b ? b + 1 : mods[k].name);
    }
    for (int i = 0; i < n && i < 4000; i++) {
        if (sorted[i][0] >> 63) {
            char d[160];
            fprintf(f, "%llu guest %s\n", (unsigned long long)sorted[i][1], ldr_describe(sorted[i][0] & ~(1ull << 63), d, sizeof d));
        } else {
            fprintf(f, "%llu host 0x%llx\n", (unsigned long long)sorted[i][1], (unsigned long long)sorted[i][0]);
        }
    }
    fclose(f);
    MoveFileExA("samples.txt.new", "samples.txt", MOVEFILE_REPLACE_EXISTING);
}

// NX2_SAMPLE_PROF=all: every thread of the process (loading work runs on the game's other threads);
// NX2_SAMPLE_UNTIL=<present>: stop counting there.
static HANDLE all_threads[256];
static int nall;
static void refresh_threads(void) {
    for (int i = 0; i < nall; i++) CloseHandle(all_threads[i]);
    nall = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof te};
    DWORD me = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok && nall < 256; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == me && te.th32ThreadID != self) {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (h) all_threads[nall++] = h;
        }
    CloseHandle(snap);
}

static void sample_rip(uint64_t rip, uint64_t exe);

static DWORD WINAPI sampler(LPVOID arg) {
    (void)arg;
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    int64_t from = getenv("NX2_SAMPLE_FROM") ? atoll(getenv("NX2_SAMPLE_FROM")) : 0;
    int64_t until = getenv("NX2_SAMPLE_UNTIL") ? atoll(getenv("NX2_SAMPLE_UNTIL")) : -1;
    int all = !strcmp(getenv("NX2_SAMPLE_PROF"), "all");
    DWORD last = GetTickCount(), last_list = 0;
    timeBeginPeriod(1);
    for (;;) {
        Sleep(1);   // about 1000 samples a second with the 1 ms timer
        if ((int64_t)nx2_vk_frame < from) continue;
        if (until >= 0 && (int64_t)nx2_vk_frame >= until) {
            static int done;
            if (!done++) dump(), rt_log("sampler: stopped at present %lld", (long long)until);
            continue;
        }
        if (all) {
            if (GetTickCount() - last_list > 500) last_list = GetTickCount(), refresh_threads();
            for (int i = 0; i < nall; i++) {
                if (SuspendThread(all_threads[i]) == (DWORD)-1) continue;
                CONTEXT c = {0};
                c.ContextFlags = CONTEXT_CONTROL;
                BOOL ok = GetThreadContext(all_threads[i], &c);
                ResumeThread(all_threads[i]);
                if (ok) sample_rip(c.Rip, exe);
            }
            if (GetTickCount() - last > 5000) last = GetTickCount(), dump();
            continue;
        }
        if (!target) {   // the thread submitting now (the first submits at boot come from another one)
            target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, last_tid);
            rt_log("sampler: profiling thread %lu from present %llu", last_tid, (unsigned long long)nx2_vk_frame);
            if (!target) break;
        }
        if (SuspendThread(target) == (DWORD)-1) break;
        CONTEXT c = {0};
        c.ContextFlags = CONTEXT_CONTROL;
        BOOL ok = GetThreadContext(target, &c);
        ResumeThread(target);
        if (!ok) continue;
        sample_rip(c.Rip, exe);
        if (GetTickCount() - last > 5000) last = GetTickCount(), dump();
    }
    return 0;
}

static void sample_rip(uint64_t rip, uint64_t exe) {
        total++;
        uint64_t g = nx2_lifted_function((uintptr_t)rip);
        if (g) {
            lifted_total++;
            count(g | 1ull << 63);
        } else if (rip - exe < 0x40000000ull) {
            count(rip - exe);
        } else {
            idle_total++;   // outside the executable: system DLLs (waits, the driver)
            {   // which ntdll system call the thread sits in (its stub is the nearest Nt* export below rip)
                static const char *const fns[] = {"NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtDelayExecution",
                    "NtReadFile", "NtWriteFile", "NtRemoveIoCompletion", "NtWaitForAlertByThreadId", "NtYieldExecution",
                    "NtQueryInformationFile", "NtCreateFile", "NtSignalAndWaitForSingleObject", "NtUserMsgWaitForMultipleObjectsEx",
                    "NtFlushBuffersFile", "NtDeviceIoControlFile", "NtQueryAttributesFile", "NtClose"};
                static uint64_t at[16];
                static int ready;
                if (!ready) {
                    ready = 1;
                    HMODULE nt = GetModuleHandleA("ntdll.dll"), wu = GetModuleHandleA("win32u.dll");
                    for (int i = 0; i < 16; i++) {
                        FARPROC f = nt ? GetProcAddress(nt, fns[i]) : NULL;
                        if (!f && wu) f = GetProcAddress(wu, fns[i]);
                        at[i] = (uint64_t)(uintptr_t)f;
                    }
                }
                int best = -1;
                for (int i = 0; i < 16; i++)
                    if (at[i] && rip >= at[i] && rip - at[i] < 0x20) best = i;
                count(best >= 0 ? 2 + (uint64_t)best : 1);
                if (best >= 0) {
                    static int named[16];
                    if (!named[best]++) rt_log("sampler: key %d = %s", 2 + best, fns[best]);
                }
            }
            HMODULE hm = NULL;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)rip, &hm);
            int k = 0;
            while (k < nmods && mods[k].h != hm) k++;
            if (k == nmods && nmods < MODS) {
                mods[k].h = hm;
                if (!hm || !GetModuleFileNameA(hm, mods[k].name, sizeof mods[k].name)) strcpy(mods[k].name, "?");
                nmods++;
            }
            if (k < nmods) mods[k].count++;
        }
}

// Called on the render thread (nvnQueueSubmitCommands): the first call starts the sampler on it.
void nx2_sampler_here(void) {
    last_tid = GetCurrentThreadId();
    if (started || InterlockedExchange(&started, 1)) return;
    if (getenv("NX2_SAMPLE_PROF") && !strcmp(getenv("NX2_SAMPLE_PROF"), "all")) rt_log("sampler: every thread");
    if (!getenv("NX2_SAMPLE_PROF")) return;
    HANDLE h = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
    rt_log("sampler: on, samples.txt every 5 s");
}
