// AMD FidelityFX Super Resolution 1 (EASU + RCAS) for the Switch 2 renderer, with AMD's shaders as the TotK base ships
// them (shaders/fsr_*.comp over shaders/ffx/ffx_a.h and ffx_fsr1.h, unmodified, MIT; SPIR-V in fsr_*_spv.h). Adapted
// from vk_fsr.c for a plain Vulkan device (no VMA, no push descriptors).
//
// nx2_fsr_upscale scales a rectangle of a sampled image into a rectangle of another image: EASU writes an RGBA16F
// image of the output size, RCAS (sharpness > 0) a second one, and a 1:1 blit converts the result into the
// destination (any blittable format, e.g. the game's RGB9E5 scene). Used where the game's neural upscaler ran
// (nx2_vkdraw.c: 2560x1440 -> 3840x2160 in docked mode) and on the present when the window is larger than the game's
// image (nx2_vk.c). NX2_UPSCALER=fsr (default) | nis (NVIDIA Image Scaling, nx2_nis.c) | linear,
// NX2_SHARPNESS=0..100 (default 80; 0: no RCAS pass).
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "fsr_easu_spv.h"
#include "fsr_rcas_spv.h"
#include "host.h"
#include "srgb_decode_spv.h"

uint32_t nx2_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want);

#define SETS 64   // descriptor set pairs, used round robin (far more than the uses in flight)
#define CACHE 4   // intermediate image pairs, by size

typedef struct { uint32_t w, h; VkImage img[2]; VkDeviceMemory mem[2]; VkImageView view[2]; uint64_t used; } Inter;

static struct {
    int tried, ok;
    VkDevice dev;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline easu, rcas;
    VkSampler sampler;
    VkDescriptorPool pool;
    VkDescriptorSet sets[SETS][2];
    unsigned next;
    Inter inter[CACHE];
    uint64_t uses;
} F;

int nx2_fsr_enabled(void) {
    static int on = -1;
    if (on < 0) on = !getenv("NX2_UPSCALER") || strcmp(getenv("NX2_UPSCALER"), "linear");
    return on;
}

static int sharpness(void) {
    static int s = -1;
    if (s < 0) {
        s = getenv("NX2_SHARPNESS") ? atoi(getenv("NX2_SHARPNESS")) : 80;
        if (s < 0) s = 0;
        if (s > 100) s = 100;
    }
    return s;
}

static VkPipeline make_pipeline(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(F.dev, &mi, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL};
    ci.layout = F.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(F.dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    vkDestroyShaderModule(F.dev, m, NULL);
    return p;
}

static int init(VkDevice dev) {
    F.tried = 1, F.dev = dev;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SETS * 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS * 2}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, SETS * 2, 2, ps};
    if (vkCreateDescriptorSetLayout(dev, &dci, NULL, &F.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &F.dsl, 1, &pr};
    if (vkCreatePipelineLayout(dev, &li, NULL, &F.layout) != VK_SUCCESS || vkCreateSampler(dev, &si, NULL, &F.sampler) != VK_SUCCESS ||
        vkCreateDescriptorPool(dev, &pci, NULL, &F.pool) != VK_SUCCESS)
        goto fail;
    VkDescriptorSetLayout l2[2] = {F.dsl, F.dsl};
    for (int i = 0; i < SETS; i++) {
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, F.pool, 2, l2};
        if (vkAllocateDescriptorSets(dev, &ai, F.sets[i]) != VK_SUCCESS) goto fail;
    }
    if (!(F.easu = make_pipeline(fsr_easu_spv, sizeof fsr_easu_spv)) || !(F.rcas = make_pipeline(fsr_rcas_spv, sizeof fsr_rcas_spv)))
        goto fail;
    rt_log("fsr: FSR 1 (EASU + RCAS) ready, sharpness %d", sharpness());
    return F.ok = 1;
fail:
    rt_log("fsr: off (Vulkan objects could not be created): linear scaling");
    return 0;
}

// A pair of intermediate images at least w x h (the passes use their top-left w x h). The game's dynamic resolution
// changes the output size from frame to frame (3839, 3071, 2559 wide...): the smallest pair that fits is reused, and a
// new pair is rounded up to 256 so a few sizes cover them all (a new pair needs the GPU idle when it replaces one).
static Inter *intermediates(uint32_t w, uint32_t h) {
    Inter *best = NULL, *lru = &F.inter[0];
    for (int i = 0; i < CACHE; i++) {
        Inter *t = &F.inter[i];
        if (t->img[0] && t->w >= w && t->h >= h && (!best || (uint64_t)t->w * t->h < (uint64_t)best->w * best->h)) best = t;
        if (t->used < lru->used) lru = t;
    }
    if (best) {
        best->used = ++F.uses;
        return best;
    }
    w = (w + 255) & ~255u, h = (h + 255) & ~255u;
    for (int i = 0; i < CACHE; i++)
        if (!F.inter[i].img[0]) {
            lru = &F.inter[i];
            break;
        }
    if (lru->img[0]) {   // a size not seen lately: the oldest pair goes (earlier work may still use it)
        void nx2_vk_queue_lock(void), nx2_vk_queue_unlock(void);
        nx2_vk_queue_lock();   // vkDeviceWaitIdle needs the queue externally synchronized
        vkDeviceWaitIdle(F.dev);
        nx2_vk_queue_unlock();
        for (int k = 0; k < 2; k++) {
            vkDestroyImageView(F.dev, lru->view[k], NULL);
            vkDestroyImage(F.dev, lru->img[k], NULL);
            vkFreeMemory(F.dev, lru->mem[k], NULL);
        }
        memset(lru, 0, sizeof *lru);
    }
    for (int k = 0; k < 2; k++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R16G16B16A16_SFLOAT, ci.extent = (VkExtent3D){w, h, 1};
        ci.mipLevels = ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(F.dev, &ci, NULL, &lru->img[k]) != VK_SUCCESS) return NULL;
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(F.dev, lru->img[k], &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                   nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (vkAllocateMemory(F.dev, &ai, NULL, &lru->mem[k]) != VK_SUCCESS) return NULL;
        vkBindImageMemory(F.dev, lru->img[k], lru->mem[k], 0);
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, lru->img[k], VK_IMAGE_VIEW_TYPE_2D,
                                    VK_FORMAT_R16G16B16A16_SFLOAT, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(F.dev, &vi, NULL, &lru->view[k]) != VK_SUCCESS) return NULL;
    }
    lru->w = w, lru->h = h, lru->used = ++F.uses;
    rt_log("fsr: intermediate images %ux%u", w, h);
    return lru;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void to_general(VkCommandBuffer cb, VkImage img) {   // contents not kept: the pass writes all of it
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL,
                         1, &b);
}

static void pass(VkCommandBuffer cb, VkPipeline p, VkDescriptorSet set, VkImageView in, VkImageLayout in_layout,
                 VkImageView out, const float pc[8], uint32_t gx, uint32_t gy) {
    VkDescriptorImageInfo ii = {F.sampler, in, in_layout}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi}};
    vkUpdateDescriptorSets(F.dev, 2, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, F.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc);
    vkCmdDispatch(cb, gx, gy, 1);
}

// sRGB encoding undone in place in rectangle out (x, y, w, h) of an RGB10A2 image (storage_view, GENERAL layout):
// what the game's upscaler did besides scaling (shaders/srgb_decode.comp). 1: recorded. NX2_UPSCALE_DECODE=0: off.
int nx2_srgb_decode(VkDevice dev, VkCommandBuffer cb, VkImageView storage_view, const int out[4]) {
    static int off = -1;
    static VkDescriptorSetLayout dsl;
    static VkPipelineLayout layout;
    static VkPipeline pipe;
    static VkDescriptorPool pool;
    static VkDescriptorSet sets[SETS];
    static unsigned next;
    if (off < 0) {
        off = getenv("NX2_UPSCALE_DECODE") && *getenv("NX2_UPSCALE_DECODE") == '0';
        if (!off) {
            VkDescriptorSetLayoutBinding b = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
            VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
            VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pr};
            VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS};
            VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, SETS, 1, &ps};
            VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof srgb_decode_spv, srgb_decode_spv};
            VkShaderModule m = VK_NULL_HANDLE;
            off = 1;
            if (vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl) == VK_SUCCESS &&
                vkCreatePipelineLayout(dev, &li, NULL, &layout) == VK_SUCCESS &&
                vkCreateDescriptorPool(dev, &pci, NULL, &pool) == VK_SUCCESS &&
                vkCreateShaderModule(dev, &mi, NULL, &m) == VK_SUCCESS) {
                VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                             VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL};
                ci.layout = layout;
                int ok = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe) == VK_SUCCESS;
                for (int i = 0; ok && i < SETS; i++) {
                    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl};
                    ok = vkAllocateDescriptorSets(dev, &ai, &sets[i]) == VK_SUCCESS;
                }
                off = !ok;
            }
            if (m) vkDestroyShaderModule(dev, m, NULL);
            rt_log("upscale: the frame is turned linear after scaling%s", off ? ": FAILED (pipeline)" : "");
        }
    }
    if (off || !storage_view || out[2] <= 0 || out[3] <= 0) return 0;
    VkDescriptorSet set = sets[next++ % SETS];
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, storage_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi};
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    full_barrier(cb);
    float pc[4] = {(float)out[0], (float)out[1], (float)out[2], (float)out[3]};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pc);
    vkCmdDispatch(cb, ((uint32_t)out[2] + 15) / 16, ((uint32_t)out[3] + 15) / 16, 1);
    full_barrier(cb);
    return 1;
}

// Scales src rectangle crop (x, y, w, h of an sw x sh image, sampled through src_view in src_layout) into the dst
// rectangle out (x, y, w, h) of dst (in dst_layout, which must allow a transfer write). 1: recorded; 0: FSR is off or
// not available (the caller scales another way). The caller has ordered the source's writes before this (a barrier).
int nx2_fsr_upscale(VkDevice dev, VkCommandBuffer cb, VkImageView src_view, VkImageLayout src_layout, int sw, int sh,
                    const int crop[4], VkImage dst, VkImageLayout dst_layout, const int out[4]) {
    if (!nx2_fsr_enabled() || out[2] <= 0 || out[3] <= 0 || crop[2] <= 0 || crop[3] <= 0) return 0;
    static int nis = -1;   // NX2_UPSCALER=nis: NVIDIA Image Scaling (nx2_nis.c) first; FSR above its 2x limit
    if (nis < 0) nis = getenv("NX2_UPSCALER") && !strcmp(getenv("NX2_UPSCALER"), "nis");
    int nx2_nis_upscale(VkDevice, VkCommandBuffer, VkImageView, VkImageLayout, int, int, const int[4], VkImage,
                        VkImageLayout, const int[4], int);
    if (nis && nx2_nis_upscale(dev, cb, src_view, src_layout, sw, sh, crop, dst, dst_layout, out, sharpness())) return 1;
    if (!F.tried) init(dev);
    if (!F.ok) return 0;
    uint32_t dw = (uint32_t)out[2], dh = (uint32_t)out[3];
    Inter *t = intermediates(dw, dh);
    if (!t) return 0;
    VkDescriptorSet *set = F.sets[F.next++ % SETS];
    static int logged;
    if (logged++ < 4) rt_log("fsr: %dx%d -> %ux%u", crop[2], crop[3], dw, dh);
    full_barrier(cb);
    to_general(cb, t->img[0]);
    to_general(cb, t->img[1]);
    uint32_t gx = (dw + 15) / 16, gy = (dh + 15) / 16;
    float easu[8] = {(float)crop[2], (float)crop[3], (float)sw, (float)sh, (float)dw, (float)dh, (float)crop[0], (float)crop[1]};
    pass(cb, F.easu, set[0], src_view, src_layout, t->view[0], easu, gx, gy);
    int last = 0, sharp = sharpness();
    if (sharp > 0) {
        full_barrier(cb);
        float rcas[8] = {(float)dw, (float)dh, (float)(100 - sharp) / 50.0f, 0, 0, 0, 0, 0};
        pass(cb, F.rcas, set[1], t->view[0], VK_IMAGE_LAYOUT_GENERAL, t->view[1], rcas, gx, gy);
        last = 1;
    }
    full_barrier(cb);
    VkImageBlit region = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int)dw, (int)dh, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{out[0], out[1], 0}, {out[0] + (int)dw, out[1] + (int)dh, 1}}};
    vkCmdBlitImage(cb, t->img[last], VK_IMAGE_LAYOUT_GENERAL, dst, dst_layout, 1, &region, VK_FILTER_NEAREST);
    full_barrier(cb);
    return 1;
}
