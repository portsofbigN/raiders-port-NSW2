// NVIDIA Image Scaling (NIS) 1.0.3 NVScaler for the Switch 2 renderer: NVIDIA's shader and filter tables used
// unmodified (third_party/nis, MIT; SPIR-V in nis_spv.h and tables in nis_coef.h, made by tools/gen_nis.py), with the
// configuration of NIS_Config.h's NVScalerUpdateConfig ported to C (SDR mode: the game's image is 10-bit UNORM in
// display range when it is upscaled).
//
// nx2_nis_upscale has nx2_fsr_upscale's interface: the source rectangle into an RGBA16F image of the output size,
// then a 1:1 blit into the destination rectangle. NIS scales up to 2x per axis; larger factors return 0 (the caller
// uses FSR). NX2_UPSCALER=nis selects it; NX2_SHARPNESS (0..100) is its sharpness slider.
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "host.h"
#include "nis_coef.h"
#include "nis_spv.h"

uint32_t nx2_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want);

#define RING 64   // descriptor sets and constant blocks, used round robin

typedef struct {   // NIS_Main.glsl const_buffer (std140: 4-byte scalars, 112 bytes)
    float kDetectRatio, kDetectThres, kMinContrastRatio, kRatioNorm;
    float kContrastBoost, kEps, kSharpStartY, kSharpScaleY;
    float kSharpStrengthMin, kSharpStrengthScale, kSharpLimitMin, kSharpLimitScale;
    float kScaleX, kScaleY;
    float kDstNormX, kDstNormY, kSrcNormX, kSrcNormY;
    uint32_t kInputViewportOriginX, kInputViewportOriginY, kInputViewportWidth, kInputViewportHeight;
    uint32_t kOutputViewportOriginX, kOutputViewportOriginY, kOutputViewportWidth, kOutputViewportHeight;
    float reserved0, reserved1;
} NisConfig;

static struct {
    int tried, ok, coef_ready;
    VkDevice dev;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler sampler;
    VkDescriptorPool pool;
    VkDescriptorSet sets[RING];
    VkBuffer ubo;   // RING blocks of 256 bytes, host visible
    VkDeviceMemory ubo_mem;
    uint8_t *ubo_ptr;
    VkImage coef[2];   // 2 x 64 RGBA32F: coef_scale, coef_usm
    VkDeviceMemory coef_mem[2];
    VkImageView coef_view[2];
    VkBuffer coef_stage;
    VkDeviceMemory coef_stage_mem;
    VkImage out;   // RGBA16F output, grown to fit
    VkDeviceMemory out_mem;
    VkImageView out_view;
    uint32_t out_w, out_h;
    unsigned next;
} N;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float maxf(float a, float b) { return a > b ? a : b; }

// NVScalerUpdateConfig (NIS_Config.h), SDR. 0 when the scale is outside NIS's range (0.5 .. 1 per axis).
static int nis_config(NisConfig *c, float sharpness, uint32_t ix, uint32_t iy, uint32_t iw, uint32_t ih, uint32_t itw,
                      uint32_t ith, uint32_t ox, uint32_t oy, uint32_t ow, uint32_t oh, uint32_t otw, uint32_t oth) {
    sharpness = clampf(sharpness, 0.f, 1.f);
    float slider = sharpness - 0.5f;
    float MaxScale = slider >= 0.f ? 1.25f : 1.75f, MinScale = slider >= 0.f ? 1.25f : 1.0f;
    float LimitScale = slider >= 0.f ? 1.25f : 1.0f;
    float kDetectRatio = 2 * 1127.f / 1024.f, kDetectThres = 64.0f / 1024.0f;
    float kMinContrastRatio = 2.0f, kMaxContrastRatio = 10.0f;
    float kSharpStartY = 0.45f, kSharpEndY = 0.9f;
    float kSharpStrengthMin = maxf(0.0f, 0.4f + slider * MinScale * 1.2f);
    float kSharpStrengthMax = 1.6f + slider * MaxScale * 1.8f;
    float kSharpLimitMin = maxf(0.1f, 0.14f + slider * LimitScale * 0.32f);
    float kSharpLimitMax = 0.5f + slider * LimitScale * 0.6f;
    memset(c, 0, sizeof *c);
    c->kInputViewportWidth = iw ? iw : itw, c->kInputViewportHeight = ih ? ih : ith;
    c->kOutputViewportWidth = ow ? ow : otw, c->kOutputViewportHeight = oh ? oh : oth;
    if (!c->kInputViewportWidth || !c->kInputViewportHeight || !c->kOutputViewportWidth || !c->kOutputViewportHeight)
        return 0;
    c->kInputViewportOriginX = ix, c->kInputViewportOriginY = iy;
    c->kOutputViewportOriginX = ox, c->kOutputViewportOriginY = oy;
    c->kSrcNormX = 1.f / (float)itw, c->kSrcNormY = 1.f / (float)ith;
    c->kDstNormX = 1.f / (float)otw, c->kDstNormY = 1.f / (float)oth;
    c->kScaleX = (float)c->kInputViewportWidth / (float)c->kOutputViewportWidth;
    c->kScaleY = (float)c->kInputViewportHeight / (float)c->kOutputViewportHeight;
    c->kDetectRatio = kDetectRatio, c->kDetectThres = kDetectThres;
    c->kMinContrastRatio = kMinContrastRatio, c->kRatioNorm = 1.0f / (kMaxContrastRatio - kMinContrastRatio);
    c->kContrastBoost = 1.0f, c->kEps = 1.0f / 255.0f;
    c->kSharpStartY = kSharpStartY, c->kSharpScaleY = 1.0f / (kSharpEndY - kSharpStartY);
    c->kSharpStrengthMin = kSharpStrengthMin, c->kSharpStrengthScale = kSharpStrengthMax - kSharpStrengthMin;
    c->kSharpLimitMin = kSharpLimitMin, c->kSharpLimitScale = kSharpLimitMax - kSharpLimitMin;
    return !(c->kScaleX < 0.5f || c->kScaleX > 1.f || c->kScaleY < 0.5f || c->kScaleY > 1.f);
}

static int alloc_mem(VkMemoryRequirements mr, VkMemoryPropertyFlags props, VkDeviceMemory *m) {
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, nx2_vk_memory_type(mr.memoryTypeBits, props)};
    return vkAllocateMemory(N.dev, &ai, NULL, m) == VK_SUCCESS;
}

static int make_image(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem,
                      VkImageView *view) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = f, ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (vkCreateImage(N.dev, &ci, NULL, img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(N.dev, *img, &mr);
    if (!alloc_mem(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mem)) return 0;
    vkBindImageMemory(N.dev, *img, *mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, *img, VK_IMAGE_VIEW_TYPE_2D, f, {0},
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(N.dev, &vi, NULL, view) == VK_SUCCESS;
}

static int make_buffer(VkDeviceSize n, VkBufferUsageFlags usage, VkBuffer *b, VkDeviceMemory *m, void **ptr) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = n, bi.usage = usage;
    if (vkCreateBuffer(N.dev, &bi, NULL, b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(N.dev, *b, &mr);
    if (!alloc_mem(mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m)) return 0;
    vkBindBufferMemory(N.dev, *b, *m, 0);
    return vkMapMemory(N.dev, *m, 0, VK_WHOLE_SIZE, 0, ptr) == VK_SUCCESS;
}

static int init(VkDevice dev) {
    N.tried = 1, N.dev = dev;
    VkDescriptorSetLayoutBinding b[6] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 6, b};
    if (vkCreateDescriptorSetLayout(dev, &dci, NULL, &N.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &N.dsl, 0, NULL};
    if (vkCreatePipelineLayout(dev, &li, NULL, &N.layout) != VK_SUCCESS) goto fail;
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &si, NULL, &N.sampler) != VK_SUCCESS) goto fail;
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, RING}, {VK_DESCRIPTOR_TYPE_SAMPLER, RING},
                                  {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RING * 3}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, RING}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, RING, 4, ps};
    if (vkCreateDescriptorPool(dev, &pci, NULL, &N.pool) != VK_SUCCESS) goto fail;
    for (int i = 0; i < RING; i++) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, N.pool, 1, &N.dsl};
        if (vkAllocateDescriptorSets(dev, &ai, &N.sets[i]) != VK_SUCCESS) goto fail;
    }
    if (!make_buffer(256 * RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &N.ubo, &N.ubo_mem, (void **)&N.ubo_ptr)) goto fail;
    for (int k = 0; k < 2; k++)
        if (!make_image(2, 64, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        &N.coef[k], &N.coef_mem[k], &N.coef_view[k]))
            goto fail;
    void *sp;
    if (!make_buffer(2 * sizeof nis_coef_scale, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &N.coef_stage, &N.coef_stage_mem, &sp)) goto fail;
    memcpy(sp, nis_coef_scale, sizeof nis_coef_scale);
    memcpy((uint8_t *)sp + sizeof nis_coef_scale, nis_coef_usm, sizeof nis_coef_usm);
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof nis_scaler_spv, nis_scaler_spv};
    VkShaderModule m;
    if (vkCreateShaderModule(dev, &mi, NULL, &m) != VK_SUCCESS) goto fail;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL};
    ci.layout = N.layout;
    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &N.pipe);
    vkDestroyShaderModule(dev, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    rt_log("nis: NVIDIA Image Scaling 1.0.3 ready");
    return N.ok = 1;
fail:
    rt_log("nis: off (Vulkan objects could not be created)");
    return 0;
}

static void barrier_full(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void to_general(VkCommandBuffer cb, VkImage img, VkImageLayout from) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, VK_IMAGE_LAYOUT_GENERAL,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

int nx2_nis_upscale(VkDevice dev, VkCommandBuffer cb, VkImageView src_view, VkImageLayout src_layout, int sw, int sh,
                    const int crop[4], VkImage dst, VkImageLayout dst_layout, const int out[4], int sharp) {
    if (out[2] <= 0 || out[3] <= 0 || crop[2] <= 0 || crop[3] <= 0) return 0;
    if (!N.tried) init(dev);
    if (!N.ok) return 0;
    uint32_t dw = (uint32_t)out[2], dh = (uint32_t)out[3];
    if (!N.out || N.out_w < dw || N.out_h < dh) {   // the output image, grown to fit (rounded to 256)
        uint32_t w = (dw + 255) & ~255u, h = (dh + 255) & ~255u;
        if (w < N.out_w) w = N.out_w;
        if (h < N.out_h) h = N.out_h;
        if (N.out) {
            void nx2_vk_queue_lock(void), nx2_vk_queue_unlock(void);
            nx2_vk_queue_lock();
            vkDeviceWaitIdle(dev);
            nx2_vk_queue_unlock();
            vkDestroyImageView(dev, N.out_view, NULL);
            vkDestroyImage(dev, N.out, NULL);
            vkFreeMemory(dev, N.out_mem, NULL);
            N.out = VK_NULL_HANDLE;
        }
        if (!make_image(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        &N.out, &N.out_mem, &N.out_view)) {
            N.ok = 0;
            return 0;
        }
        N.out_w = w, N.out_h = h;
        rt_log("nis: output image %ux%u", w, h);
    }
    unsigned slot = N.next++ % RING;
    NisConfig *c = (NisConfig *)(N.ubo_ptr + 256 * slot);
    if (!nis_config(c, (float)sharp / 100.f, (uint32_t)crop[0], (uint32_t)crop[1], (uint32_t)crop[2], (uint32_t)crop[3],
                    (uint32_t)sw, (uint32_t)sh, 0, 0, dw, dh, N.out_w, N.out_h))
        return 0;   // more than 2x: the caller scales another way
    static int logged;
    if (logged++ < 4) rt_log("nis: %dx%d -> %ux%u (sharpness %d)", crop[2], crop[3], dw, dh, sharp);
    if (!N.coef_ready) {   // the coefficient tables, once
        for (int k = 0; k < 2; k++) {
            to_general(cb, N.coef[k], VK_IMAGE_LAYOUT_UNDEFINED);
            VkBufferImageCopy bc = {(VkDeviceSize)k * sizeof nis_coef_scale, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    {0, 0, 0}, {2, 64, 1}};
            vkCmdCopyBufferToImage(cb, N.coef_stage, N.coef[k], VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
        }
        N.coef_ready = 1;
    }
    barrier_full(cb);
    to_general(cb, N.out, VK_IMAGE_LAYOUT_UNDEFINED);
    VkDescriptorSet set = N.sets[slot];
    VkDescriptorBufferInfo bi = {N.ubo, 256 * slot, sizeof(NisConfig)};
    VkDescriptorImageInfo smp = {N.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkDescriptorImageInfo in = {VK_NULL_HANDLE, src_view, src_layout}, o = {VK_NULL_HANDLE, N.out_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo c0 = {VK_NULL_HANDLE, N.coef_view[0], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo c1 = {VK_NULL_HANDLE, N.coef_view[1], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[6] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &smp},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &in},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &o},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &c0},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &c1}};
    vkUpdateDescriptorSets(dev, 6, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, N.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, N.layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, (dw + 31) / 32, (dh + 23) / 24, 1);
    barrier_full(cb);
    VkImageBlit region = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int)dw, (int)dh, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{out[0], out[1], 0}, {out[0] + (int)dw, out[1] + (int)dh, 1}}};
    vkCmdBlitImage(cb, N.out, VK_IMAGE_LAYOUT_GENERAL, dst, dst_layout, 1, &region, VK_FILTER_NEAREST);
    barrier_full(cb);
    return 1;
}
