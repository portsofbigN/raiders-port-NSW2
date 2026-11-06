// Window and Vulkan presenter. The game's window texture (RGBA8, converted to linear rows by the caller) is uploaded
// to a Vulkan image every present and blitted, scaled, onto the swapchain of a Win32 window. This is the first piece
// of the Vulkan back end: later the game's draws render into Vulkan images directly and the upload step goes away.
//
// Also NX2_SHOT=<file.png> with NX2_SHOT_FRAME=<n> (default 300): writes present n as a PNG and exits, for tests.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <windows.h>

#include "host.h"
#include "nx2_vk.h"
#include "platform/platform.h"

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) rt_fatal("vulkan: %s failed (%d)", #x, (int)r_); } while (0)

static HWND hwnd;
static volatile LONG window_ready;
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfamily;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkImage swap_images[8];
static uint32_t nswap;
static VkExtent2D swap_extent;
static VkCommandPool cmdpool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkSemaphore sem_acquire, sem_done[8];   // "rendered" semaphore per swapchain image (reused only after that image is presented again)
static VkImage src_image;
static VkDeviceMemory src_mem;
static VkBuffer staging;
static VkDeviceMemory staging_mem;
static void *staging_ptr;
static uint32_t src_w, src_h;
static int ready, failed;
static PlatMutex lock;

// ---- window thread
// NX2_WINDOW_SIZE=<w>x<h> (default 1280x720), NX2_FULLSCREEN=1: borderless over the whole monitor. F11 or Alt+Enter
// switch between the two; F1 cycles the performance overlay (nx2_overlay.c).
static int fullscreen;
static RECT windowed_rect;
static void set_fullscreen(HWND h, int on) {
    if (on == fullscreen) return;
    if (on) {
        GetWindowRect(h, &windowed_rect);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(h, NULL, windowed_rect.left, windowed_rect.top, windowed_rect.right - windowed_rect.left,
                     windowed_rect.bottom - windowed_rect.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    fullscreen = on;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        rt_log("window closed: exiting");
        ExitProcess(0);
    }
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(l & (1 << 30))) {   // first press, not auto-repeat
        if (w == VK_F11 || (w == VK_RETURN && (l & (1 << 29)))) {
            set_fullscreen(h, !fullscreen);
            return 0;
        }
        if (w == VK_F1) {
            void nx2_overlay_cycle(void);
            nx2_overlay_cycle();
            return 0;
        }
    }
    if (m == WM_SYSKEYDOWN && w == VK_RETURN) return 0;   // no beep on Alt+Enter
    if (m == WM_SYSCHAR && w == VK_RETURN) return 0;
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI window_thread(LPVOID arg) {
    (void)arg;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SplatoonRecomp";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    {   // sizes in real pixels: with Windows' display scaling (125%, 150%) the window is not stretched and blurred
        typedef BOOL(WINAPI * SetCtx)(HANDLE);
        HMODULE u = GetModuleHandleA("user32.dll");
        SetCtx set = u ? (SetCtx)(void *)GetProcAddress(u, "SetThreadDpiAwarenessContext") : NULL;
        if (set) set((HANDLE)-4);   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, for this thread's windows only
    }
    int ww = 1280, wh = 720;
    const char *ws = getenv("NX2_WINDOW_SIZE");
    if (ws && sscanf(ws, "%dx%d", &ww, &wh) != 2) ww = 1280, wh = 720;
    if (ww < 320 || wh < 180) ww = 1280, wh = 720;
    const char *fs = getenv("NX2_FULLSCREEN");
    int full = fs && *fs == '1';
    RECT work;   // the desktop without the taskbar
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        work = (RECT){0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    if (!full && ww >= sw && wh >= sh) {   // the monitor's whole size: that is full screen (a window would not fit)
        rt_log("window: %dx%d is the monitor's size; borderless full screen", ww, wh);
        full = 1;
    }
    RECT r = {0, 0, ww, wh};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int fw = (r.right - r.left) - ww, fh = (r.bottom - r.top) - wh;   // the frame and title bar
    int aw = work.right - work.left - fw, ah = work.bottom - work.top - fh;
    if (!full && (ww > aw || wh > ah) && aw > 0 && ah > 0) {   // larger than the desktop: the largest that fits, same shape
        double k = (double)aw / ww < (double)ah / wh ? (double)aw / ww : (double)ah / wh;
        rt_log("window: %dx%d does not fit the desktop; %dx%d", ww, wh, (int)(ww * k), (int)(wh * k));
        ww = (int)(ww * k), wh = (int)(wh * k);
    }
    int x = work.left + (work.right - work.left - (ww + fw)) / 2, y = work.top + (work.bottom - work.top - (wh + fh)) / 2;
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Splatoon Raiders (recomp)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           x > work.left ? x : work.left, y > work.top ? y : work.top, ww + fw, wh + fh, NULL, NULL,
                           wc.hInstance, NULL);
    if (full) set_fullscreen(hwnd, 1);
    // A controller's buttons are not input to Windows: without this the monitor would turn off (and the presents
    // slow to a crawl) after the idle time while someone plays with a controller.
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    InterlockedExchange(&window_ready, 1);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ---- Vulkan set-up
uint32_t nx2_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    rt_fatal("vulkan: no memory type %#x for %#x", (unsigned)want, (unsigned)bits);
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return;   // minimized: keep the old one
    swap_extent = caps.currentExtent;
    if (swap_extent.width == 0xFFFFFFFFu) swap_extent = (VkExtent2D){1280, 720};
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = count;
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = swap_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // Frames are paced to 60 fps by pace_frame; FIFO on a 144 / 165 Hz monitor also waits for the next vblank,
    // which turned 16.7 ms frames into 18. MAILBOX (no tearing, no wait) when the surface has it; NX2_VSYNC=1: FIFO.
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!getenv("NX2_VSYNC")) {
        VkPresentModeKHR modes[16];
        uint32_t nm = 16;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, modes) >= 0)
            for (uint32_t i = 0; i < nm; i++)
                if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    static int logged;
    if (!logged++) rt_log("vk: present mode %s", ci.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo");
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR next;
    VKCHECK(vkCreateSwapchainKHR(dev, &ci, NULL, &next));
    if (swapchain) vkDestroySwapchainKHR(dev, swapchain, NULL);
    swapchain = next;
    nswap = 8;
    VKCHECK(vkGetSwapchainImagesKHR(dev, swapchain, &nswap, swap_images));
}

static void create_source(uint32_t w, uint32_t h) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = (VkExtent3D){w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VKCHECK(vkCreateImage(dev, &ii, NULL, &src_image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, src_image, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VKCHECK(vkAllocateMemory(dev, &ai, NULL, &src_mem));
    VKCHECK(vkBindImageMemory(dev, src_image, src_mem, 0));
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)w * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VKCHECK(vkCreateBuffer(dev, &bi, NULL, &staging));
    vkGetBufferMemoryRequirements(dev, staging, &mr);
    ai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(dev, &ai, NULL, &staging_mem));
    VKCHECK(vkBindBufferMemory(dev, staging, staging_mem, 0));
    VKCHECK(vkMapMemory(dev, staging_mem, 0, VK_WHOLE_SIZE, 0, &staging_ptr));
    src_w = w, src_h = h;
}

static Nx2VkCtx ctx;
static int have_budget;
static uint64_t vram_used(uint64_t *budget) {   // device-local heaps: bytes in use (and the budget) for the overlay
    VkPhysicalDeviceMemoryBudgetPropertiesEXT mb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                            have_budget ? &mb : NULL};
    vkGetPhysicalDeviceMemoryProperties2(phys, &mp);
    uint64_t used = 0, total = 0;
    for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; i++)
        if (mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            used += have_budget ? mb.heapUsage[i] : 0;
            total += have_budget ? mb.heapBudget[i] : mp.memoryProperties.memoryHeaps[i].size;
        }
    *budget = total;
    return used;
}
// the overlay bitmap's staging buffer (host visible), copied into the swapchain image after the game image
static VkImageView present_src_view;   // a sampled view of the image being presented (FSR when the window is larger)
void nx2_present_source_view(VkImageView v) { present_src_view = v; }
static VkBuffer ov_buf;
static VkDeviceMemory ov_mem;
static void *ov_ptr;
#define OV_BYTES (2048u * 1024u * 4u)
static VkPhysicalDeviceFeatures2 feat2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
static VkPhysicalDeviceVulkan11Features feat11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
static VkPhysicalDeviceVulkan12Features feat12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
static VkPhysicalDeviceVulkan13Features feat13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};

static void init(void) {
    CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    while (!InterlockedCompareExchange(&window_ready, 1, 1)) Sleep(1);
    const char *iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "Splatoon Raiders recomp", 1, "nx2", 1,
                             VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iext};
    VKCHECK(vkCreateInstance(&ic, NULL, &inst));
    VkWin32SurfaceCreateInfoKHR sc = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, NULL, 0,
                                      GetModuleHandleW(NULL), hwnd};
    VKCHECK(vkCreateWin32SurfaceKHR(inst, &sc, NULL, &surface));
    uint32_t n = 8;
    VkPhysicalDevice pd[8];
    VKCHECK(vkEnumeratePhysicalDevices(inst, &n, pd));
    phys = pd[0];
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = pd[i]; break; }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    uint32_t nq = 16;
    VkQueueFamilyProperties qf[16];
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    qfamily = UINT32_MAX;
    for (uint32_t i = 0; i < nq && qfamily == UINT32_MAX; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) qfamily = i;
    }
    if (qfamily == UINT32_MAX) rt_fatal("vulkan: no graphics queue that can present");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfamily, 1, &prio};
    // VK_EXT_external_memory_host: the game's memory pools are imported as Vulkan buffers (nx2_vkdraw.c)
    const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                          VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
    uint32_t ndext = 2;
    {   // VK_EXT_memory_budget (the overlay's VRAM line) when the driver has it
        static VkExtensionProperties ep[1024];
        uint32_t ne = 1024;
        if (vkEnumerateDeviceExtensionProperties(phys, NULL, &ne, ep) >= 0)
            for (uint32_t i = 0; i < ne; i++)
                if (!strcmp(ep[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) have_budget = 1, ndext = 3;
    }
    // Every core feature the GPU has (translated shaders use many: 8/16-bit storage, float16/int64, scalar block
    // layout, demote to helper, ...); the robustness features cost speed and stay off.
    feat2.pNext = &feat11, feat11.pNext = &feat12, feat12.pNext = &feat13;
    vkGetPhysicalDeviceFeatures2(phys, &feat2);
    feat2.features.robustBufferAccess = VK_FALSE;
    feat13.robustImageAccess = VK_FALSE;
    VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &feat2, 0, 1, &qc, 0, NULL, ndext, dext, NULL};
    VKCHECK(vkCreateDevice(phys, &dc, NULL, &dev));
    vkGetDeviceQueue(dev, qfamily, 0, &queue);
    create_swapchain();
    VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                  VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfamily};
    VKCHECK(vkCreateCommandPool(dev, &pc, NULL, &cmdpool));
    VkCommandBufferAllocateInfo ca = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cmdpool,
                                      VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VKCHECK(vkAllocateCommandBuffers(dev, &ca, &cmd));
    VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    VKCHECK(vkCreateFence(dev, &fc, NULL, &fence));
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(dev, &sci, NULL, &sem_acquire));
    for (int i = 0; i < 8; i++) VKCHECK(vkCreateSemaphore(dev, &sci, NULL, &sem_done[i]));
    rt_log("vulkan: %s, swapchain %ux%u (%u images); scalar block layout %d, std. uniform layout %d, float16 %d, "
           "int8 %d", props.deviceName, swap_extent.width, swap_extent.height, nswap, feat12.scalarBlockLayout,
           feat12.uniformBufferStandardLayout, feat12.shaderFloat16, feat12.shaderInt8);
    ctx = (Nx2VkCtx){inst, phys, dev, queue, qfamily};
    static char gpu[256];
    snprintf(gpu, sizeof gpu, "%s", props.deviceName);
    void nx2_overlay_info(const char *gpu, uint64_t (*vram)(uint64_t *budget));
    nx2_overlay_info(gpu, vram_used);
    ready = 1;
}

static PlatMutex queue_lock;
void nx2_vk_queue_lock(void) { plat_mutex_lock(&queue_lock); }
void nx2_vk_queue_unlock(void) { plat_mutex_unlock(&queue_lock); }

Nx2VkCtx *nx2_vk(void) {
    plat_mutex_lock(&lock);
    if (!ready) init();
    plat_mutex_unlock(&lock);
    return &ctx;
}

static void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src, dst, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}

// ---- PNG (stored deflate blocks: no compression, small code)
static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c ^= 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), data, n);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

void nx2_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t step) {
    uint32_t ow = w / step, oh = h / step;   // downscaled by step, so screenshots stay small
    size_t row = (size_t)ow * 3 + 1, raw_n = row * oh;
    uint8_t *raw = malloc(raw_n);
    for (uint32_t y = 0; y < oh; y++) {
        uint8_t *o = raw + y * row;
        *o++ = 0;
        for (uint32_t x = 0; x < ow; x++) {
            const uint8_t *p = rgba + ((size_t)y * step * w + (size_t)x * step) * 4;
            *o++ = p[0], *o++ = p[1], *o++ = p[2];
        }
    }
    size_t blocks = (raw_n + 65534) / 65535, z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n), *q = z;
    *q++ = 0x78, *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) a = (a + raw[i]) % 65521, b = (b + a) % 65521;
    for (size_t off = 0; off < raw_n; off += 65535) {
        uint32_t n = raw_n - off > 65535 ? 65535 : (uint32_t)(raw_n - off);
        *q++ = off + n == raw_n;
        *q++ = n & 0xFF, *q++ = n >> 8, *q++ = ~n & 0xFF, *q++ = (~n >> 8) & 0xFF;
        memcpy(q, raw + off, n);
        q += n;
    }
    be32(q, (b << 16) | a);
    q += 4;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, ow), be32(ihdr + 4, oh);
    ihdr[8] = 8, ihdr[9] = 2, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

// ---- present: rgba is w x h RGBA8 rows
// Frame pacing: the Switch shows 60 frames a second whatever the monitor's rate; the game's timing (and the input
// scripts, which count frames) expect it. NX2_FPS=<n> sets another rate, 0 no limit.
static void pace_frame(void) {
    static uint64_t next, period = 1;
    if (period == 1) {
        const char *e = getenv("NX2_FPS");
        int fps = e ? atoi(e) : 60;
        period = fps > 0 ? 1000000000ull / (uint64_t)fps : 0;
    }
    if (!period) return;
    extern uint64_t nx2_prof_pace_ns;
    uint64_t now = plat_now_ns();
    if (!next || now > next + 4 * period) next = now;   // first frame, or far behind: start again
    if (now < next) plat_sleep_ns(next - now), nx2_prof_pace_ns += next - now;
    next += period;
    if (getenv("NX2_PACE_LOG")) {   // debug: calls, resets and the interval between returns
        static uint64_t calls, resets, last, sum, mx;
        uint64_t t = plat_now_ns();
        calls++;
        if (last) sum += t - last, mx = t - last > mx ? t - last : mx;
        last = t;
        if (now > next + 3 * period) resets++;
        if (!(calls % 300)) {
            rt_log("pace: %llu calls, avg %.2f ms, max %.2f ms", (unsigned long long)calls, sum / 300.0 * 1e-6, mx * 1e-6);
            sum = mx = 0;
        }
    }
}

void nx2_present_rgba(const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t frame) {
    const char *shot = getenv("NX2_SHOT");
    if (shot) {
        const char *fs = getenv("NX2_SHOT_FRAME");
        uint64_t want = fs ? strtoull(fs, NULL, 10) : 300;
        if (frame == want) {
            nx2_write_png(shot, rgba, w, h, w >= 2560 ? 3 : 1);
            rt_log("screenshot of present #%llu written to %s", (unsigned long long)frame, shot);
            ExitProcess(0);
        }
    }
    if (failed) return;
    nx2_vk();
    plat_mutex_lock(&lock);
    if (!src_image) create_source(w, h);
    if (w != src_w || h != src_h) {
        plat_mutex_unlock(&lock);
        return;
    }
    pace_frame();
    if (IsIconic(hwnd)) {   // minimized: nothing to show (the game goes on)
        plat_mutex_unlock(&lock);
        return;
    }
    VKCHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    uint32_t idx;
    VkResult ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, sem_acquire, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(dev);
        create_swapchain();
        plat_mutex_unlock(&lock);
        return;
    }
    VKCHECK(vkResetFences(dev, 1, &fence));
    memcpy(staging_ptr, rgba, (size_t)w * h * 4);
    VKCHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKCHECK(vkBeginCommandBuffer(cmd, &bi));
    barrier(src_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyBufferToImage(cmd, staging, src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(src_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkImage dst = swap_images[idx];
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int)w, (int)h, 1}},
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        {{0, 0, 0}, {(int)swap_extent.width, (int)swap_extent.height, 1}}};
    vkCmdBlitImage(cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    VKCHECK(vkEndCommandBuffer(cmd));
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &sem_acquire, &wait, 1, &cmd, 1, &sem_done[idx]};
    nx2_vk_queue_lock();
    VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &sem_done[idx], 1, &swapchain, &idx, NULL};
    VkResult pr = vkQueuePresentKHR(queue, &pi);
    nx2_vk_queue_unlock();
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(dev);
        create_swapchain();
    }
    plat_mutex_unlock(&lock);
}

// ---- present a GPU image (the window texture as nx2_vkdraw.c rendered it). *layout is the image's current layout;
// TRANSFER_SRC_OPTIMAL afterwards. NX2_SHOT reads the image back.
static void record_begin(void) {
    VKCHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    VKCHECK(vkResetFences(dev, 1, &fence));
    VKCHECK(vkResetCommandBuffer(cmd, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VKCHECK(vkBeginCommandBuffer(cmd, &bi));
}

static void shot_from_image(VkImage img, VkImageLayout *layout, uint32_t w, uint32_t h, uint64_t frame,
                            const char *shot, int exit_after) {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)w * h * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VKCHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
    VKCHECK(vkBindBufferMemory(dev, buf, mem, 0));
    record_begin();
    barrier(img, *layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    *layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
    VKCHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    nx2_vk_queue_lock();
    VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
    nx2_vk_queue_unlock();
    VKCHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    void *p;
    VKCHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p));
    nx2_write_png(shot, p, w, h, getenv("NX2_SHOT_STEP") ? (uint32_t)atoi(getenv("NX2_SHOT_STEP")) : w >= 2560 ? 3 : 1);
    rt_log("screenshot of present #%llu (GPU image) written to %s", (unsigned long long)frame, shot);
    if (exit_after) ExitProcess(0);
    {   // NX2_DUMP_ON_BLACK=1 (debug): a shot whose upper right part is mostly black asks for the GPU images of the
        // next present (nx2_vkdraw.c dumps them and exits)
        static int on = -1;
        if (on < 0) on = getenv("NX2_DUMP_ON_BLACK") != NULL;
        if (on) {
            extern volatile int nx2_dump_request;
            const uint8_t *px = p;
            uint64_t n = 0, black = 0;
            for (uint32_t y = 0; y < h * 45 / 100; y += 4)
                for (uint32_t x = w * 28 / 100; x < w; x += 4, n++) {
                    const uint8_t *q = px + ((uint64_t)y * w + x) * 4;
                    black += q[0] + q[1] + q[2] < 15;
                }
            if (n && black * 5 > n) {
                rt_log("screenshot of present #%llu is %.0f%% black at the top: dumping the next present's images",
                       (unsigned long long)frame, 100.0 * (double)black / (double)n);
                nx2_dump_request = 1;
            }
        }
    }
    vkUnmapMemory(dev, mem);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
}

void nx2_present_image(VkImage img, VkImageLayout *layout, uint32_t w, uint32_t h, uint64_t frame) {
    const char *shot = getenv("NX2_SHOT");
    nx2_vk();
    plat_mutex_lock(&lock);
    if (shot) {
        const char *fs = getenv("NX2_SHOT_FRAME");
        if (frame == (fs ? strtoull(fs, NULL, 10) : 300)) shot_from_image(img, layout, w, h, frame, shot, 1);
    }
    const char *every = getenv("NX2_SHOT_EVERY");   // shots/shot_<frame>.png every n presents, without exiting
    const char *from = getenv("NX2_SHOT_FROM");   // periodic shots from that present on
    if (every && atoi(every) > 0 && frame % (uint64_t)atoi(every) == 0 && (!from || frame >= strtoull(from, NULL, 10))) {
        char path[64];
        CreateDirectoryA("shots", NULL);
        snprintf(path, sizeof path, "shots/shot_%05llu.png", (unsigned long long)frame);
        shot_from_image(img, layout, w, h, frame, path, 0);
    }
    pace_frame();
    if (IsIconic(hwnd)) {   // minimized: nothing to show (the game goes on)
        plat_mutex_unlock(&lock);
        return;
    }
    VKCHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    uint32_t idx;
    VkResult ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, sem_acquire, VK_NULL_HANDLE, &idx);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(dev);
        create_swapchain();
        plat_mutex_unlock(&lock);
        return;
    }
    record_begin();
    // GENERAL: read by the blit or sampled by FSR
    barrier(img, *layout, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    *layout = VK_IMAGE_LAYOUT_GENERAL;
    VkImage dst = swap_images[idx];
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    // the game's shape kept: a window of another shape gets black bars instead of a stretched picture
    int ox = 0, oy = 0, ow2 = (int)swap_extent.width, oh2 = (int)swap_extent.height;
    if ((uint64_t)ow2 * h > (uint64_t)oh2 * w + oh2 / 2) ow2 = (int)(((uint64_t)oh2 * w + h / 2) / h), ox = ((int)swap_extent.width - ow2) / 2;
    else if ((uint64_t)oh2 * w > (uint64_t)ow2 * h + ow2 / 2) oh2 = (int)(((uint64_t)ow2 * h + w / 2) / w), oy = ((int)swap_extent.height - oh2) / 2;
    if (ox || oy || ow2 != (int)swap_extent.width || oh2 != (int)swap_extent.height) {
        static const VkClearColorValue black = {{0, 0, 0, 1}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0,
                             NULL);
    }
    VkImageBlit blit = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int)w, (int)h, 1}},
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        {{ox, oy, 0}, {ox + ow2, oy + oh2, 1}}};
    int upscaled = 0;
    if (present_src_view && ow2 > (int)w && oh2 > (int)h) {   // a window larger than the image: FSR
        int nx2_fsr_upscale(VkDevice, VkCommandBuffer, VkImageView, VkImageLayout, int, int, const int[4], VkImage,
                            VkImageLayout, const int[4]);
        int crop[4] = {0, 0, (int)w, (int)h}, outr[4] = {ox, oy, ow2, oh2};
        upscaled = nx2_fsr_upscale(dev, cmd, present_src_view, VK_IMAGE_LAYOUT_GENERAL, (int)w, (int)h, crop, dst,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, outr);
    }
    if (!upscaled)
        vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    {   // the performance overlay (nx2_overlay.c) over the top-left corner
        const uint32_t *nx2_overlay_present(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, uint32_t *, uint32_t *);
        uint32_t ow, oh, os;
        const uint32_t *px = nx2_overlay_present(swap_extent.width, swap_extent.height, w, h, &ow, &oh, &os);
        if (px && (uint64_t)ow * oh * 4 <= OV_BYTES) {
            if (!ov_buf) {
                VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = OV_BYTES;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VKCHECK(vkCreateBuffer(dev, &bci, NULL, &ov_buf));
                VkMemoryRequirements mr;
                vkGetBufferMemoryRequirements(dev, ov_buf, &mr);
                VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                           nx2_vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
                VKCHECK(vkAllocateMemory(dev, &ai, NULL, &ov_mem));
                VKCHECK(vkBindBufferMemory(dev, ov_buf, ov_mem, 0));
                VKCHECK(vkMapMemory(dev, ov_mem, 0, VK_WHOLE_SIZE, 0, &ov_ptr));
            }
            for (uint32_t yy = 0; yy < oh; yy++) memcpy((uint32_t *)ov_ptr + (size_t)yy * ow, px + (size_t)yy * os, ow * 4);
            uint32_t margin = swap_extent.height / 90 + 4;
            VkBufferImageCopy oc = {0, ow, oh, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {(int)margin, (int)margin, 0},
                                    {ow, oh, 1}};
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_TRANSFER_WRITE_BIT};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL,
                                 0, NULL);
            vkCmdCopyBufferToImage(cmd, ov_buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &oc);
        }
    }
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    VKCHECK(vkEndCommandBuffer(cmd));
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &sem_acquire, &wait, 1, &cmd, 1, &sem_done[idx]};
    nx2_vk_queue_lock();
    VKCHECK(vkQueueSubmit(queue, 1, &si, fence));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &sem_done[idx], 1, &swapchain, &idx, NULL};
    VkResult pr = vkQueuePresentKHR(queue, &pi);
    nx2_vk_queue_unlock();
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(dev);
        create_swapchain();
    }
    plat_mutex_unlock(&lock);
}

HWND nx2_vk_hwnd(void) { return hwnd; }   // the game window (nx2_hid.c reads the keyboard while it has focus)
