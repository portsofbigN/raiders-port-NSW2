// NVN format -> Vulkan format (nx2_vkfmt.c).
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkFormat vk;
    uint8_t expand_rgb;   // 3-component format: expand texels to 4 components on upload
    uint8_t alpha_one;    // "X" format: alpha samples as 1
    uint8_t chan[4];      // nonzero chan[0]: component order through the view swizzle (NVN_TEXTURE_SWIZZLE_*)
    uint8_t astc_bw, astc_bh, astc_srgb;   // ASTC: decoded to RGBA8 on upload
    uint8_t depth, stencil, integer, sint;
} Nx2VkFormat;

int nx2_vk_format(int nvn_format, Nx2VkFormat *out);   // 0: no mapping
