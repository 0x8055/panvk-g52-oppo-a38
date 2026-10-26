/*
 * Independent layout check for an AHardwareBuffer-backed VkImage.
 *
 * The earlier test cleared the image and read it back through the same driver,
 * so it could not tell a wrong layout from a right one. This one crosses the
 * driver with Android's own CPU mapping (AHardwareBuffer_lock), using a
 * per-pixel pattern that a wrong layout would scramble:
 *   A) CPU writes pattern P through AHardwareBuffer_lock, the GPU copies the
 *      image to a buffer, we compare the buffer with P   (CPU write -> GPU read)
 *   B) the GPU copies pattern Q into the image, the CPU reads it back through
 *      AHardwareBuffer_lock using the buffer's own stride (GPU write -> CPU read)
 * Both PASS means the driver and the system allocator agree on a linear layout.
 *
 * usage: ahb_layout_check [path to libvulkan_panfrost.so]
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <android/hardware_buffer.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include "vulkan/vulkan.h"

/* 200x100: the allocator pads the rows (stride 208 px), so a wrong row pitch is detectable too. */
#define W 200
#define H 100
#define CHECK(x, msg) do { VkResult r = (x); if (r != VK_SUCCESS) { printf("FAILED %s: VkResult=%d\n", msg, r); return 1; } printf("%s ok\n", msg); } while (0)

typedef int  (*PFN_AHB_allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
typedef void (*PFN_AHB_release)(AHardwareBuffer*);
typedef void (*PFN_AHB_describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
typedef int  (*PFN_AHB_lock)(AHardwareBuffer*, uint64_t, int32_t, const void*, void**);
typedef int  (*PFN_AHB_unlock)(AHardwareBuffer*, int32_t*);

/* Alpha is always 255 so RGBA and RGBX agree. Little-endian: R is the low byte. */
static uint32_t pat_p(int x, int y) {
    return (uint32_t)x | ((uint32_t)y << 8) | ((uint32_t)((x * 3 + y * 7) & 0xff) << 16) | 0xff000000u;
}
static uint32_t pat_q(int x, int y) {
    return (uint32_t)(255 - x) | ((uint32_t)((x * 5 + y * 3) & 0xff) << 8) | ((uint32_t)y << 16) | 0xff000000u;
}

static uint32_t pat_c(int x, int y) { (void)x; (void)y; return 0xffff00ffu; }

static int compare(const char *what, const uint8_t *base, size_t row_bytes, uint32_t (*pat)(int, int)) {
    int bad = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t got, want = pat(x, y);
            memcpy(&got, base + (size_t)y * row_bytes + (size_t)x * 4, 4);
            if (got != want) {
                if (bad < 3) printf("  %s: mismatch at (%d,%d): got 0x%08x want 0x%08x\n", what, x, y, got, want);
                bad++;
            }
        }
    }
    printf("%s: %d of %d pixels differ -> %s\n", what, bad, W * H, bad ? "FAIL" : "PASS");
    return bad;
}

int main(int argc, char **argv) {
    const char *so_path = argc > 1 ? argv[1]
        : "/data/data/com.termux/files/home/funnymdzz-mesa/build-min/src/panfrost/vulkan/libvulkan_panfrost.so";

    void *libnw = dlopen("libnativewindow.so", RTLD_NOW);
    if (!libnw) { printf("dlopen libnativewindow.so failed: %s\n", dlerror()); return 1; }
    PFN_AHB_allocate AHB_allocate = (PFN_AHB_allocate)dlsym(libnw, "AHardwareBuffer_allocate");
    PFN_AHB_release  AHB_release  = (PFN_AHB_release)dlsym(libnw, "AHardwareBuffer_release");
    PFN_AHB_describe AHB_describe = (PFN_AHB_describe)dlsym(libnw, "AHardwareBuffer_describe");
    PFN_AHB_lock     AHB_lock     = (PFN_AHB_lock)dlsym(libnw, "AHardwareBuffer_lock");
    PFN_AHB_unlock   AHB_unlock   = (PFN_AHB_unlock)dlsym(libnw, "AHardwareBuffer_unlock");
    if (!AHB_allocate || !AHB_release || !AHB_describe || !AHB_lock || !AHB_unlock) {
        printf("dlsym AHB funcs failed\n"); return 1;
    }

    AHardwareBuffer_Desc desc = {0};
    desc.width = W; desc.height = H; desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_CPU_READ_RARELY | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;
    AHardwareBuffer *buf = NULL;
    int ret = AHB_allocate(&desc, &buf);
    if (ret != 0 || !buf) { printf("AHardwareBuffer_allocate failed: %d\n", ret); return 1; }
    AHardwareBuffer_Desc real = {0};
    AHB_describe(buf, &real);
    printf("AHardwareBuffer: %ux%u stride=%u pixels format=%u usage=0x%llx\n",
           real.width, real.height, real.stride, real.format, (unsigned long long)real.usage);
    if (real.stride < W) { printf("unexpected stride %u\n", real.stride); return 1; }
    size_t cpu_row = (size_t)real.stride * 4;

    void *lib = dlopen(so_path, RTLD_NOW);
    if (!lib) { printf("dlopen driver failed: %s\n", dlerror()); return 1; }
    printf("driver: %s\n", so_path);
    PFN_vkGetInstanceProcAddr icd_gpa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!icd_gpa) { printf("dlsym vk_icdGetInstanceProcAddr failed\n"); return 1; }

    PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)icd_gpa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance instance;
    CHECK(CreateInstance(&ici, NULL, &instance), "vkCreateInstance");

    PFN_vkEnumeratePhysicalDevices EnumDevs = (PFN_vkEnumeratePhysicalDevices)icd_gpa(instance, "vkEnumeratePhysicalDevices");
    uint32_t count = 1; VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkResult er = EnumDevs(instance, &count, &pdev);
    printf("vkEnumeratePhysicalDevices: result=%d count=%u\n", er, count);
    if (!pdev) { printf("no physical device\n"); return 1; }

    PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)icd_gpa(instance, "vkCreateDevice");
    const char *dev_exts[] = { "VK_KHR_external_memory", "VK_ANDROID_external_memory_android_hardware_buffer" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                                .enabledExtensionCount = 2, .ppEnabledExtensionNames = dev_exts };
    VkDevice device;
    CHECK(CreateDevice(pdev, &dci, NULL, &device), "vkCreateDevice");

    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)icd_gpa(instance, "vkGetDeviceProcAddr");
    #define DPROC(name) (PFN_##name)GetDeviceProcAddr(device, #name)

    PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAHBProps = DPROC(vkGetAndroidHardwareBufferPropertiesANDROID);
    VkAndroidHardwareBufferPropertiesANDROID props = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    CHECK(GetAHBProps(device, buf, &props), "vkGetAndroidHardwareBufferPropertiesANDROID");

    PFN_vkCreateImage CreateImage = DPROC(vkCreateImage);
    VkExternalMemoryImageCreateInfo ext_img_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkImageCreateInfo ici2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_img_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img;
    CHECK(CreateImage(device, &ici2, NULL, &img), "vkCreateImage (AHB-backed)");

    uint32_t mem_type_index = 0;
    for (uint32_t i = 0; i < 32; i++) if (props.memoryTypeBits & (1u << i)) { mem_type_index = i; break; }
    VkImportAndroidHardwareBufferInfoANDROID import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = buf,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &import_info, .image = img,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &dedicated_info,
        .allocationSize = props.allocationSize, .memoryTypeIndex = mem_type_index,
    };
    PFN_vkAllocateMemory AllocateMemory = DPROC(vkAllocateMemory);
    VkDeviceMemory mem;
    CHECK(AllocateMemory(device, &mai, NULL, &mem), "vkAllocateMemory (dedicated import)");
    PFN_vkBindImageMemory BindImageMemory = DPROC(vkBindImageMemory);
    CHECK(BindImageMemory(device, img, mem, 0), "vkBindImageMemory");

    /* Staging buffer: source and destination of the GPU copies, host visible. */
    PFN_vkCreateBuffer CreateBuffer = DPROC(vkCreateBuffer);
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer staging;
    CHECK(CreateBuffer(device, &bci, NULL, &staging), "vkCreateBuffer (staging)");
    PFN_vkGetBufferMemoryRequirements GetBufMemReqs = DPROC(vkGetBufferMemoryRequirements);
    VkMemoryRequirements buf_reqs;
    GetBufMemReqs(device, staging, &buf_reqs);
    PFN_vkGetPhysicalDeviceMemoryProperties GetPDMemProps =
        (PFN_vkGetPhysicalDeviceMemoryProperties)icd_gpa(instance, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties pd_mem_props;
    GetPDMemProps(pdev, &pd_mem_props);
    uint32_t host_visible_index = UINT32_MAX;
    for (uint32_t i = 0; i < pd_mem_props.memoryTypeCount; i++) {
        uint32_t f = pd_mem_props.memoryTypes[i].propertyFlags;
        printf("memory type %u: flags=0x%x%s%s%s%s%s\n", i, f,
               (f & 0x1) ? " DEVICE_LOCAL" : "", (f & 0x2) ? " HOST_VISIBLE" : "",
               (f & 0x4) ? " HOST_COHERENT" : "", (f & 0x8) ? " HOST_CACHED" : "",
               (buf_reqs.memoryTypeBits & (1u << i)) ? " [usable for the buffers]" : "");
    }
    /* Prefer coherent host memory: this test does not flush or invalidate mapped ranges. */
    for (uint32_t i = 0; i < pd_mem_props.memoryTypeCount && host_visible_index == UINT32_MAX; i++)
        if ((buf_reqs.memoryTypeBits & (1u << i)) &&
            (pd_mem_props.memoryTypes[i].propertyFlags & 0x6) == 0x6)
            host_visible_index = i;
    if (host_visible_index == UINT32_MAX) {
        printf("WARNING: no HOST_VISIBLE|HOST_COHERENT type usable for the buffers; falling back to the first "
               "HOST_VISIBLE one, so the results may be unreliable without flush/invalidate\n");
        for (uint32_t i = 0; i < pd_mem_props.memoryTypeCount && host_visible_index == UINT32_MAX; i++)
            if ((buf_reqs.memoryTypeBits & (1u << i)) &&
                (pd_mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                host_visible_index = i;
    }
    if (host_visible_index == UINT32_MAX) { printf("no host visible memory type\n"); return 1; }
    printf("staging buffers use memory type %u\n", host_visible_index);
    VkMemoryAllocateInfo buf_mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buf_reqs.size, .memoryTypeIndex = host_visible_index };
    VkDeviceMemory staging_mem;
    CHECK(AllocateMemory(device, &buf_mai, NULL, &staging_mem), "vkAllocateMemory (staging)");
    PFN_vkBindBufferMemory BindBufferMemory = DPROC(vkBindBufferMemory);
    CHECK(BindBufferMemory(device, staging, staging_mem, 0), "vkBindBufferMemory (staging)");
    PFN_vkMapMemory MapMemory = DPROC(vkMapMemory);
    void *mapped;
    CHECK(MapMemory(device, staging_mem, 0, buf_reqs.size, 0, &mapped), "vkMapMemory (staging)");
    memset(mapped, 0xAA, buf_reqs.size);

    /* Second buffer, only ever written by the GPU: lets phase B check the GPU round trip. */
    VkBuffer rb2;
    CHECK(CreateBuffer(device, &bci, NULL, &rb2), "vkCreateBuffer (readback 2)");
    VkDeviceMemory rb2_mem;
    CHECK(AllocateMemory(device, &buf_mai, NULL, &rb2_mem), "vkAllocateMemory (readback 2)");
    CHECK(BindBufferMemory(device, rb2, rb2_mem, 0), "vkBindBufferMemory (readback 2)");
    void *rb2_map;
    CHECK(MapMemory(device, rb2_mem, 0, buf_reqs.size, 0, &rb2_map), "vkMapMemory (readback 2)");
    memset(rb2_map, 0xAA, buf_reqs.size);

    PFN_vkGetDeviceQueue GetDeviceQueue = DPROC(vkGetDeviceQueue);
    VkQueue queue;
    GetDeviceQueue(device, 0, 0, &queue);
    PFN_vkCreateCommandPool CreateCommandPool = DPROC(vkCreateCommandPool);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool cpool;
    CHECK(CreateCommandPool(device, &cpi, NULL, &cpool), "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = DPROC(vkAllocateCommandBuffers);
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 3 };
    VkCommandBuffer cmd[3];
    CHECK(AllocateCommandBuffers(device, &cbai, cmd), "vkAllocateCommandBuffers");

    PFN_vkBeginCommandBuffer BeginCommandBuffer = DPROC(vkBeginCommandBuffer);
    PFN_vkEndCommandBuffer EndCommandBuffer = DPROC(vkEndCommandBuffer);
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = DPROC(vkCmdPipelineBarrier);
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = DPROC(vkCmdCopyImageToBuffer);
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = DPROC(vkCmdCopyBufferToImage);
    PFN_vkQueueSubmit QueueSubmit = DPROC(vkQueueSubmit);
    PFN_vkQueueWaitIdle QueueWaitIdle = DPROC(vkQueueWaitIdle);
    PFN_vkCmdClearColorImage CmdClearColorImage = DPROC(vkCmdClearColorImage);

    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkBufferImageCopy region = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = {0, 0, 0}, .imageExtent = {W, H, 1} };
    VkCommandBufferBeginInfo bbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    /* Phase A command buffer: image (written by the CPU) -> staging. */
    CHECK(BeginCommandBuffer(cmd[0], &bbi), "vkBeginCommandBuffer A");
    VkImageMemoryBarrier a_bar = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = range, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    CmdPipelineBarrier(cmd[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &a_bar);
    CmdCopyImageToBuffer(cmd[0], img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
    CHECK(EndCommandBuffer(cmd[0]), "vkEndCommandBuffer A");

    /* Phase B command buffer: staging (filled with Q) -> image. */
    CHECK(BeginCommandBuffer(cmd[1], &bbi), "vkBeginCommandBuffer B");
    VkImageMemoryBarrier b_bar = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = range,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    CmdPipelineBarrier(cmd[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b_bar);
    CmdCopyBufferToImage(cmd[1], staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier b2_bar = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = range,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    CmdPipelineBarrier(cmd[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2_bar);
    CmdCopyImageToBuffer(cmd[1], img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb2, 1, &region);
    CHECK(EndCommandBuffer(cmd[1]), "vkEndCommandBuffer B");

    int bad_a = -1, bad_b = -1;

    /* ---- Phase A: CPU write -> GPU read ---- */
    void *p = NULL;
    if (AHB_lock(buf, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, NULL, &p) != 0 || !p) {
        printf("AHardwareBuffer_lock (write) failed\n"); return 1;
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t v = pat_p(x, y);
            memcpy((uint8_t*)p + (size_t)y * cpu_row + (size_t)x * 4, &v, 4);
        }
    AHB_unlock(buf, NULL);
    VkSubmitInfo si_a = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd[0] };
    CHECK(QueueSubmit(queue, 1, &si_a, VK_NULL_HANDLE), "vkQueueSubmit A");
    CHECK(QueueWaitIdle(queue), "vkQueueWaitIdle A");
    bad_a = compare("A (CPU write -> GPU read)", (const uint8_t*)mapped, (size_t)W * 4, pat_p);

    /* ---- Phase B: GPU write -> CPU read ---- */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t v = pat_q(x, y);
            memcpy((uint8_t*)mapped + ((size_t)y * W + x) * 4, &v, 4);
        }
    VkSubmitInfo si_b = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd[1] };
    CHECK(QueueSubmit(queue, 1, &si_b, VK_NULL_HANDLE), "vkQueueSubmit B");
    CHECK(QueueWaitIdle(queue), "vkQueueWaitIdle B");
    int bad_rt = compare("B1 (GPU write -> GPU read)", (const uint8_t*)rb2_map, (size_t)W * 4, pat_q);
    p = NULL;
    if (AHB_lock(buf, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &p) != 0 || !p) {
        printf("AHardwareBuffer_lock (read) failed\n"); return 1;
    }
    bad_b = compare("B2 (GPU write -> CPU read)", (const uint8_t*)p, cpu_row, pat_q);
    int still_p = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t got;
            memcpy(&got, (const uint8_t*)p + (size_t)y * cpu_row + (size_t)x * 4, 4);
            if (got == pat_p(x, y)) still_p++;
        }
    printf("  B2: %d of %d pixels still hold pattern P (what the CPU wrote in phase A)\n", still_p, W * H);
    AHB_unlock(buf, NULL);

    /* ---- Phase C: GPU clear -> CPU read, no staging buffer involved ---- */
    CHECK(BeginCommandBuffer(cmd[2], &bbi), "vkBeginCommandBuffer C");
    VkImageMemoryBarrier c_bar = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = range,
        .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    CmdPipelineBarrier(cmd[2], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &c_bar);
    VkClearColorValue clear_c = { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f } };
    CmdClearColorImage(cmd[2], img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_c, 1, &range);
    CHECK(EndCommandBuffer(cmd[2]), "vkEndCommandBuffer C");
    VkSubmitInfo si_c = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd[2] };
    CHECK(QueueSubmit(queue, 1, &si_c, VK_NULL_HANDLE), "vkQueueSubmit C");
    CHECK(QueueWaitIdle(queue), "vkQueueWaitIdle C");
    p = NULL;
    if (AHB_lock(buf, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &p) != 0 || !p) {
        printf("AHardwareBuffer_lock (read) failed\n"); return 1;
    }
    int bad_c = compare("C (GPU clear -> CPU read)", (const uint8_t*)p, cpu_row, pat_c);
    AHB_unlock(buf, NULL);

    printf("\nSUMMARY\n  A  CPU write -> GPU read : %s\n  B1 GPU write -> GPU read : %s\n  B2 GPU write -> CPU read : %s\n"
           "  C  GPU clear -> CPU read : %s\n",
           bad_a == 0 ? "PASS" : "FAIL", bad_rt == 0 ? "PASS" : "FAIL", bad_b == 0 ? "PASS" : "FAIL",
           bad_c == 0 ? "PASS" : "FAIL");
    AHB_release(buf);
    return (bad_a == 0 && bad_rt == 0 && bad_b == 0 && bad_c == 0) ? 0 : 2;
}
