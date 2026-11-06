// A small background job queue (two worker threads, started on first use): the driver's pipeline compilation for
// NX2_ASYNC_SHADERS (nx2_vkdraw.c). Jobs run in submission order per worker; nothing waits for them.
#include <stdlib.h>
#include <windows.h>

#include "host.h"

typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static CRITICAL_SECTION lock;
static CONDITION_VARIABLE more;
static Job *head, *tail;
static volatile LONG started;
static volatile LONG pending;

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    for (;;) {
        EnterCriticalSection(&lock);
        while (!head) SleepConditionVariableCS(&more, &lock, INFINITE);
        Job *j = head;
        head = j->next;
        if (!head) tail = NULL;
        LeaveCriticalSection(&lock);
        j->fn(j->arg);
        free(j);
        InterlockedDecrement(&pending);
    }
    return 0;
}

void nx2_job_submit(void (*fn)(void *), void *arg) {
    if (!InterlockedCompareExchange(&started, 1, 0)) {
        InitializeCriticalSection(&lock);
        InitializeConditionVariable(&more);
        for (int i = 0; i < 2; i++) {
            HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
            SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);   // below the game's threads
            CloseHandle(h);
        }
        InterlockedExchange(&started, 2);
    }
    while (started != 2) Sleep(0);
    Job *j = malloc(sizeof *j);
    if (!j) {
        fn(arg);   // no memory: run it here
        return;
    }
    j->fn = fn, j->arg = arg, j->next = NULL;
    InterlockedIncrement(&pending);
    EnterCriticalSection(&lock);
    if (tail) tail->next = j;
    else head = j;
    tail = j;
    LeaveCriticalSection(&lock);
    WakeConditionVariable(&more);
}

int nx2_jobs_pending(void) { return (int)pending; }
