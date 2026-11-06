// Switch 2 system services the SDK's own start-up asks the service manager for. TotK Recomp replaces most of the SDK
// at the nn:: function level and serves only audren:u and nfp:user over IPC; on Switch 2 the SDK's start-up
// (nninitInitializeSdkModule and nn::oe, nn::gs, nn::fs initialisation) talks to more services before game code runs.
//
// Each service is a table of the commands this game sends. A command either opens a sub-interface (the reply moves a
// new session to the client) or succeeds with fixed output words. A command not in the table succeeds with no output
// and is logged once with its caller, so the log names the next one to describe. Which commands return sessions, and
// what values are needed, comes from Eglantine's NX2_NOTES.md (read for behaviour only; no GPL code is copied).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "ipc.h"
#include "audren.h"   // audren_manager_handler: TotK Recomp's audio renderer service
#include "platform/platform.h"

enum { EV_NONE, EV_COPY, EV_COPY_SIGNALED };
enum { RAW = 1 };   // opens: the new session stays a handle even when the request came through a domain

typedef struct {
    const char *iface;     // interface the command belongs to
    unsigned cmd;
    const char *opens;     // non-NULL: reply moves a new session of this interface
    uint32_t out[4];       // output words for plain commands
    unsigned nout;
    int event;             // EV_COPY*: reply copies the readable handle of a new event
    uint32_t result;       // non-zero: the command fails with this result
    int flags;             // RAW
    uint32_t shm_size;     // non-zero: reply copies a handle to this command's shared memory (made on first use)
} Nx2Cmd;

#define OUT1(a) {a}, 1
#define OUT2(a, b) {a, b}, 2
#define AM_NO_MESSAGE 0x680u   // applet: ReceiveMessage with an empty queue (module 128, description 3)
#define AM_NO_PARAMETER 0x2a880u   // applet: PopLaunchParameter with nothing to pop (module 128, description 0x154)

static const Nx2Cmd cmds[] = {
    // lm: log manager. OpenLogger(pid) -> ILogger; ILogger Log / SetDestination succeed.
    {"lm", 0, "lm:ILogger"},

    // appletOE: OpenApplicationProxy (0; Switch 2 also 10) -> IApplicationProxy (Eglantine row 9)
    {"appletOE", 0, "am:IApplicationProxy"},
    {"appletOE", 10, "am:IApplicationProxy"},
    {"am:IApplicationProxy", 0, "am:ICommonStateGetter"},
    {"am:IApplicationProxy", 1, "am:ISelfController"},
    {"am:IApplicationProxy", 2, "am:IWindowController"},
    {"am:IApplicationProxy", 3, "am:IAudioController"},
    {"am:IApplicationProxy", 4, "am:IDisplayController"},
    {"am:IApplicationProxy", 10, "am:IProcessWindingController"},
    {"am:IApplicationProxy", 11, "am:ILibraryAppletCreator"},
    {"am:IApplicationProxy", 20, "am:IApplicationFunctions"},
    {"am:IApplicationProxy", 40, "am:INx2Getter40"},              // Switch 2 getter (Eglantine row 10)
    {"am:IApplicationProxy", 60, "am:IGraphicsScaleController"},  // Switch 2 (Eglantine row 15)
    {"am:IApplicationProxy", 1000, "am:IDebugFunctions"},
    {"am:IGraphicsScaleController", 10, NULL, OUT1(10)},          // graphics scale: 10, 20 or 30

    // ICommonStateGetter
    {"am:ICommonStateGetter", 0, NULL, {0}, 0, EV_COPY},          // GetEventHandle: message queue event
    {"am:ICommonStateGetter", 1, NULL, {0}, 0, 0, AM_NO_MESSAGE}, // ReceiveMessage: queue empty
    {"am:ICommonStateGetter", 5, NULL, OUT1(1)},                  // GetOperationMode: console (docked)
    {"am:ICommonStateGetter", 6, NULL, OUT1(1)},                  // GetPerformanceMode: boost
    {"am:ICommonStateGetter", 8, NULL, OUT1(0)},                  // GetBootMode: normal
    {"am:ICommonStateGetter", 9, NULL, OUT1(1)},                  // GetCurrentFocusState: in focus

    // ISelfController
    {"am:ISelfController", 40, NULL, OUT2(1, 0)},                 // CreateManagedDisplayLayer: layer id 1
    {"am:ISelfController", 2000, "nx2:gsv", {0}, 0, 0, 0, RAW},   // Switch 2 graphics system session (row 17)

    // IWindowController
    {"am:IWindowController", 1, NULL, OUT2(0x51, 0)},             // GetAppletResourceUserId

    // IApplicationFunctions
    {"am:IApplicationFunctions", 1, NULL, {0}, 0, 0, AM_NO_PARAMETER},   // PopLaunchParameter: none
    {"am:IApplicationFunctions", 20, NULL, OUT2(0, 0)},           // EnsureSaveData: no extra size needed
    {"am:IApplicationFunctions", 21, NULL, OUT2(0x53552d6e, 0x65)},   // GetDesiredLanguage: "en-US"
    {"am:IApplicationFunctions", 40, NULL, OUT1(1)},              // NotifyRunning
    {"am:IApplicationFunctions", 130, NULL, {0}, 0, EV_COPY},     // GetGpuErrorDetectedSystemEvent

    // aoc:u: add-on content. The game has none installed.
    {"aoc:u", 2, NULL, OUT1(0)},                                  // CountAddOnContent
    {"aoc:u", 3, NULL, OUT1(0)},                                  // ListAddOnContent: 0 entries
    {"aoc:u", 8, NULL, {0}, 0, EV_COPY},                          // GetAddOnContentListChangedEvent
    {"aoc:u", 10, NULL, {0}, 0, EV_COPY},                         // GetAddOnContentListChangedEventWithProcessId

    // pctl, pctl:s, pctl:a, pctl:r: CreateService (0) / CreateServiceWithoutInitialize (1) -> IParentalControlService.
    // No restrictions: its commands succeed with no output.
    {"pctl", 0, "pctl:IParentalControlService"},   {"pctl", 1, "pctl:IParentalControlService"},
    {"pctl:s", 0, "pctl:IParentalControlService"}, {"pctl:s", 1, "pctl:IParentalControlService"},
    {"pctl:a", 0, "pctl:IParentalControlService"}, {"pctl:a", 1, "pctl:IParentalControlService"},
    {"pctl:r", 0, "pctl:IParentalControlService"}, {"pctl:r", 1, "pctl:IParentalControlService"},

    // time:u: clocks and the time zone service; 20 GetSharedMemoryNativeHandle (0x1000 bytes of clock contexts).
    {"time:u", 0, "time:ISystemClock"},       // GetStandardUserSystemClock
    {"time:u", 1, "time:ISystemClock"},       // GetStandardNetworkSystemClock
    {"time:u", 2, "time:ISteadyClock"},       // GetStandardSteadyClock
    {"time:u", 3, "time:ITimeZoneService"},   // GetTimeZoneService
    {"time:u", 4, "time:ISystemClock"},       // GetStandardLocalSystemClock
    {"time:u", 5, "time:ISystemClock"},       // GetEphemeralNetworkSystemClock
    {"time:u", 20, NULL, {0}, 0, 0, 0, 0, 0x1000},

    // hid: CreateAppletResource(aruid) -> IAppletResource, whose GetSharedMemoryHandle (0) gives the input shared
    // memory (0x40000 bytes on Switch 1; Switch 2 size taken from the mapping the SDK asks for). Zeroed memory means
    // no controllers connected until input is ported.
    {"hid", 0, "hid:IAppletResource"},
    {"hid:IAppletResource", 0, NULL, {0}, 0, 0, 0, 0, 0x40000},
};

static void nx2_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);

static const Nx2Cmd *find(const char *iface, unsigned cmd) {
    for (unsigned i = 0; i < sizeof cmds / sizeof *cmds; i++)
        if (cmds[i].cmd == cmd && !strcmp(cmds[i].iface, iface)) return &cmds[i];
    return NULL;
}

// Log each (interface, command) once.
#define SEEN_MAX 1024
static struct { const char *iface; unsigned cmd; } seen[SEEN_MAX];
static int nseen;
static PlatMutex seen_lock;   // zero-initialised storage is an unlocked mutex (platform.h)

static int first_time(const char *iface, unsigned cmd) {
    plat_mutex_lock(&seen_lock);
    int found = 0;
    for (int i = 0; i < nseen && !found; i++) found = seen[i].cmd == cmd && !strcmp(seen[i].iface, iface);
    if (!found && nseen < SEEN_MAX) seen[nseen++].iface = iface, seen[nseen - 1].cmd = cmd;
    plat_mutex_unlock(&seen_lock);
    return !found;
}

// ---- applet message queue (ICommonStateGetter 0 GetEventHandle, 1 ReceiveMessage). A starting application finds
// FocusStateChanged (15) queued with the event signalled; nn::oe waits for it and then reads GetCurrentFocusState.
#define AM_MSG_FOCUS_STATE_CHANGED 15u
static PlatMutex am_lock;
static uint32_t am_event;         // writable end, 0 until GetEventHandle
static uint32_t am_queue[16];
static unsigned am_count = 1;
static int am_started;

static int applet_messages(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    if (strcmp(s->name, "am:ICommonStateGetter") || rq->cmd > 1) return 0;
    plat_mutex_lock(&am_lock);
    if (!am_started) am_queue[0] = AM_MSG_FOCUS_STATE_CHANGED, am_started = 1;
    if (rq->cmd == 0) {   // GetEventHandle
        if (!am_event) {
            am_event = kern_event_new();
            if (am_count) kern_event_signal(am_event);
        }
        rs->copy[rs->ncopy++] = kern_event_readable(am_event);
    } else if (!am_count) {   // ReceiveMessage, queue empty
        rs->result = AM_NO_MESSAGE;
    } else {
        uint32_t msg = am_queue[0];
        memmove(am_queue, am_queue + 1, --am_count * sizeof *am_queue);
        ipc_out(rs, &msg, 4);
        rt_log("nx2: applet message %u delivered", msg);
    }
    plat_mutex_unlock(&am_lock);
    return 1;
}

static void nx2_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    // aud:u (Switch 2 audio entry, nn::audio::CreateUserClientSessionByHipc): command 2 opens the
    // IAudioRendererManager, served by TotK Recomp's audren (Eglantine NX2_NOTES row 38).
    if (!strcmp(s->name, "aud:u") && rq->cmd == 2) {
        rs->move[rs->nmove++] = ipc_session_new("audren:IAudioRendererManager", audren_manager_handler, NULL, NULL);
        if (first_time(s->name, rq->cmd)) rt_log("nx2: aud:u cmd 2 -> audio renderer manager (TotK audren)");
        return;
    }
    if (applet_messages(s, rq, rs)) {
        first_time(s->name, rq->cmd);
        return;
    }
    const Nx2Cmd *c = find(s->name, rq->cmd);
    if (c && c->result) {
        rs->result = c->result;
    } else if (c && c->opens) {
        uint32_t h = ipc_session_new(c->opens, nx2_handler, NULL, NULL);
        if (c->flags & RAW) rs->copy[rs->ncopy++] = h;   // copied handles are never turned into domain objects
        else rs->move[rs->nmove++] = h;
    } else if (c && c->shm_size) {
        static RtShmem *made[sizeof cmds / sizeof *cmds];
        static PlatMutex shm_lock;
        plat_mutex_lock(&shm_lock);
        RtShmem **m = &made[c - cmds];
        if (!*m) *m = shmem_new(c->iface, c->shm_size);
        plat_mutex_unlock(&shm_lock);
        rs->copy[rs->ncopy++] = shmem_handle(*m);
    } else if (c) {
        uint32_t out[4];
        memcpy(out, c->out, sizeof out);
        // NX2_OPERATION_MODE=handheld: the console reports handheld (operation and performance mode 0) instead of
        // docked; the game then renders for the 1080p screen (lighter) instead of a 4K television.
        static int handheld = -1;
        if (handheld < 0) handheld = getenv("NX2_OPERATION_MODE") && !strcmp(getenv("NX2_OPERATION_MODE"), "handheld");
        if (handheld && !strcmp(c->iface, "am:ICommonStateGetter") && (c->cmd == 5 || c->cmd == 6)) out[0] = 0;
        ipc_out(rs, out, c->nout * 4);
        if (c->event) {
            uint32_t w = kern_event_new();
            if (c->event == EV_COPY_SIGNALED) kern_event_signal(w);
            rs->copy[rs->ncopy++] = kern_event_readable(w);
        }
    }
    if (first_time(s->name, rq->cmd)) {
        rt_log("nx2: %s cmd %u%s%s (%u bytes in)", s->name, rq->cmd, c ? "" : " not described, empty success",
               c && c->opens ? " -> new session" : "", rq->data_size);
        if (!c) ipc_backtrace();
    }
}

// Services the table above describes. sm: (ipc.c sm_handler) asks here after its own table.
static const char *const services[] = {"lm", "appletOE", "aoc:u", "pctl", "pctl:s", "pctl:a", "pctl:r", "time:u", "hid"};

// A service in neither table is opened anyway as an undescribed session (every command an empty success, each logged
// once with its caller), so one run lists everything the start-up asks for. TOTK_NX2_STRICT=1 makes it fatal again.
IpcHandler nx2_service(const char *name, const char **canonical) {
    for (unsigned i = 0; i < sizeof services / sizeof *services; i++)
        if (!strcmp(services[i], name)) {
            *canonical = services[i];
            return nx2_handler;
        }
    const char *strict = getenv("TOTK_NX2_STRICT");
    if (strict && *strict == '1') return NULL;
    static char names[64][9];
    static int nnames;
    static PlatMutex lock;
    plat_mutex_lock(&lock);
    int i = 0;
    while (i < nnames && strcmp(names[i], name)) i++;
    if (i == nnames && nnames < 64) snprintf(names[nnames++], 9, "%s", name);
    plat_mutex_unlock(&lock);
    if (i == 64) return NULL;
    rt_log("nx2: service \"%s\" is not described: opening it with empty-success commands", name);
    *canonical = names[i];
    return nx2_handler;
}
