// Input-side answers until controller input is ported (TotK Recomp's ov_input.c maps keyboard and gamepads into the
// hid shared memory). With no controller in the hid shared memory, the game opens the controller applet
// (nn::hid::ShowControllerSupport), and system applets cannot be launched here: answer it on the host as if the
// player confirmed one controller, No. 1.
#include <string.h>

#include "host.h"

// Result ShowControllerSupport(ControllerSupportResultInfo *out, const ControllerSupportArg &)
// ControllerSupportResultInfo: { int32 playerCount; NpadIdType selectedId; } (NpadId::No1 = 0)
static void ov_controller_support(Cpu *c) {
    int32_t *out = arg_p(c, 0);
    if (out) out[0] = 1, out[1] = 0;
    static int n;
    if (n++ < 2) rt_log("hid: controller applet answered on the host: 1 player, controller No. 1");
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
              ov_controller_support);
