// Functions the ported TotK Recomp core calls in subsystems that are not ported yet (graphics, audio, NFC, saves,
// profiling, resolution). Each one does nothing, or answers an IPC request with an error and logs it, so a run that
// reaches one says so instead of crashing. They are replaced as the subsystems are ported (ROADMAP.md stages 5-6).
#include "host.h"
#include "ipc.h"

void vk_pipeline_save(void) {}
void prof_write(void) {}
void perf_main_thread(void) {}
uint64_t res_heap_extra_bytes(void) { return 0; }
int64_t frame_step_world_us(void) { return -1; }

#define RESULT_NOT_PORTED 0x1A801u   // module 1 (kernel), description 0xD40: a value the game reports as a failure



void nfp_user_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s; (void)rq;
    rt_log("stub: nfp:user request (amiibo is not ported yet)");
    rs->result = RESULT_NOT_PORTED;
}
