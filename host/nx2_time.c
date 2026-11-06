// The nn::time calls Splatoon imports beyond TotK Recomp's ov_system.c. ov_system.c answers nn::time::Initialize on the
// host, so the SDK never opens its time service sessions; every time call the game makes must then be host code too,
// or the SDK aborts on the missing session (GetDeviceLocationName did, sdk+0x5ceb28).
//
// Time zone: "UTC" (calendar conversion is ov_system.c's, from the host clock). Network clock: unavailable, as on an
// offline console; the availability task finishes at once with that result.
#include <string.h>

#include "host.h"

#define TIME_R_NETWORK_CLOCK_UNAVAILABLE (116u | (0x66u << 9))   // module 116 (time); a failure is all the game needs

// Result GetDeviceLocationName(LocationName *out): char[0x24]
static void ov_location_name(Cpu *c) {
    char *out = arg_p(c, 0);
    if (out) {
        memset(out, 0, 0x24);
        strcpy(out, "UTC");
    }
    ret_x(c, 0);
}
// Result LoadTimeZoneRule(TimeZoneRule *out, const LocationName &): the rule stays opaque; conversions ignore it
static void ov_load_rule(Cpu *c) { ret_x(c, 0); }
// bool StandardNetworkSystemClock::IsAccuracySufficient()
static void ov_accuracy(Cpu *c) { ret_x(c, 0); }

// AsyncEnsureNetworkClockAvailabilityTask: one host system event, signalled when a task starts, shared by all tasks
// (the game runs one at a time).
static uint64_t finish_event;   // guest os::SystemEventType, bound to a kernel event
static void ov_task_ctor(Cpu *c) { (void)c; }
static void ov_task_dtor(Cpu *c) { (void)c; }
static void ov_task_initialize(Cpu *c) {   // Result Initialize(os::EventClearMode)
    if (!finish_event) {
        finish_event = (uint64_t)(uintptr_t)mem_kernel_alloc(0x100, 16);
        kern_bind_system_event(c, finish_event, (int)c->x[1] == 1);
    }
    ret_x(c, 0);
}
static void ov_task_start(Cpu *c) {   // Result StartTask()
    rt_log("time: network clock availability task: offline, finishes at once");
    if (finish_event) kern_signal_system_event(finish_event);
    ret_x(c, 0);
}
static void ov_task_event(Cpu *c) { ret_x(c, finish_event); }   // os::SystemEvent *GetFinishEvent()
static void ov_task_result(Cpu *c) { ret_x(c, TIME_R_NETWORK_CLOCK_UNAVAILABLE); }

HOST_OVERRIDE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", ov_location_name);
HOST_OVERRIDE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", ov_load_rule);
HOST_OVERRIDE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", ov_accuracy);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", ov_task_ctor);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", ov_task_dtor);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", ov_task_initialize);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", ov_task_start);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", ov_task_event);
HOST_OVERRIDE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", ov_task_result);
