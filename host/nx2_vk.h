// Shared Vulkan context of the presenter (nx2_vk.c) and the draw back end (nx2_vkdraw.c).
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfamily;
} Nx2VkCtx;

Nx2VkCtx *nx2_vk(void);   // creates the window and the device on first use
uint32_t nx2_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want);
void nx2_vk_queue_lock(void);   // vkQueueSubmit / vkQueuePresentKHR are made under this lock
void nx2_vk_queue_unlock(void);
// Presents w x h of a colour image; *layout is its current layout (TRANSFER_SRC_OPTIMAL afterwards).
void nx2_present_image(VkImage img, VkImageLayout *layout, uint32_t w, uint32_t h, uint64_t frame);
void nx2_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t step);
