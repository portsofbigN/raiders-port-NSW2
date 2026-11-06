// Shared memory objects that host services give the guest (time, hid, applet and graphics services on Switch 2).
// Guest addresses are host addresses, so the object has no backing of its own once mapped: a staging buffer holds
// what the host wrote before the guest mapped it, the mapping receives those bytes, and from then on the host writes
// straight into the guest range. One mapping at a time, which is how the SDK uses them.
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#define R_OK 0u
#define R_BAD_HANDLE 0xe401u
#define R_INVALID_SIZE 0xca01u
#define R_INVALID_STATE 0xd401u

struct RtShmem {
    const char *name;
    uint64_t size;
    uint8_t *stage;            // host copy while unmapped
    volatile uint64_t guest;   // mapping address, 0 while unmapped
};

RtShmem *shmem_new(const char *name, uint64_t size) {
    RtShmem *m = calloc(1, sizeof *m);
    if (!m || !(m->stage = calloc(1, size))) rt_fatal("shmem: allocation of %s (%#llx)", name, (unsigned long long)size);
    m->name = name;
    m->size = size;
    return m;
}

uint32_t shmem_handle(RtShmem *m) { return kern_handle_new(KERN_SHMEM, m); }

void *shmem_ptr(RtShmem *m) {
    uint64_t g = m->guest;
    return g ? (void *)(uintptr_t)g : m->stage;
}

uint32_t shmem_svc_map(uint32_t h, uint64_t addr, uint64_t size, uint32_t perm) {
    RtShmem *m = kern_handle_get(h, KERN_SHMEM);
    uint32_t r = !m ? R_BAD_HANDLE : size != m->size ? R_INVALID_SIZE : m->guest ? R_INVALID_STATE : mem_map_shared(addr, size);
    if (r) {
        rt_log("shmem: map of handle %#x (%s) at %#llx+%#llx failed %#x%s", h, m ? m->name : "?", (unsigned long long)addr,
               (unsigned long long)size, r, m && m->guest ? " (already mapped)" : "");
        return r;
    }
    memcpy((void *)(uintptr_t)addr, m->stage, size);
    m->guest = addr;
    rt_log("shmem: %s mapped at %#llx+%#llx (perm %u)", m->name, (unsigned long long)addr, (unsigned long long)size,
           perm);
    return R_OK;
}

uint32_t shmem_svc_unmap(uint32_t h, uint64_t addr, uint64_t size) {
    RtShmem *m = kern_handle_get(h, KERN_SHMEM);
    if (!m) return R_BAD_HANDLE;
    if (m->guest != addr || size != m->size) return R_INVALID_STATE;
    memcpy(m->stage, (void *)(uintptr_t)addr, size);
    m->guest = 0;
    return mem_unmap_shared(addr, size);
}
