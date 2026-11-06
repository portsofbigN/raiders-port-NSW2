// Stand-in for Eden's common/settings.h in the standalone shader translator: only the fields the translator reads,
// with the host's fixed choices (no shader-side resolution scaling: the Vulkan back end renders at the chosen size).
#pragma once
#include "common/common_types.h"

namespace Settings {
struct ResolutionScalingInfo {
    bool active = false;
    u32 up_scale = 1;
    u32 down_shift = 0;
    f32 up_factor = 1.0f;
    f32 down_factor = 1.0f;
};
struct Values {
    bool disable_shader_loop_safety_checks = false;
    bool renderer_debug = false;
    ResolutionScalingInfo resolution_info;
};
inline Values values;
}   // namespace Settings
