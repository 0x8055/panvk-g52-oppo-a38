/*
 * Bind de memoria AHB importada a una VkImage real: crea la imagen con
 * VkExternalMemoryImageCreateInfo, importa la memoria con
 * VkMemoryDedicatedAllocateInfo (rama de panvk_android_import_ahb_memory()
 * que usa panvk_android_ahb_image_init(), distinta a la que ya probamos),
 * hace vkBindImageMemory, limpia la imagen a azul puro por GPU y copia el
 * resultado a un buffer legible para confirmar el color exacto.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <android/hardware_buffer.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include "vulkan/vulkan.h"

#define CHECK(x, msg) do { VkResult r = (x); if (r != VK_SUCCESS) { printf("FAILED %s: VkResult=%d\n", msg, r); return 1; } printf("%s ok\n", msg); } while (0)

typedef int  (*PFN_AHB_allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
typedef void (*PFN_AHB_release)(AHardwareBuffer*);

int main(void) {
    void *libnw = dlopen("libnativewindow.so", RTLD_NOW);
    if (!libnw) { printf("dlopen libnativewindow.so failed: %s\n", dlerror()); return 1; }
    PFN_AHB_allocate AHB_allocate = (PFN_AHB_allocate)dlsym(libnw, "AHardwareBuffer_allocate");
    PFN_AHB_release  AHB_release  = (PFN_AHB_release)dlsym(libnw, "AHardwareBuffer_release");
    if (!AHB_allocate || !AHB_release) { printf("dlsym AHB funcs failed\n"); return 1; }

    AHardwareBuffer_Desc desc = {0};
    desc.width = 64; desc.height = 64; desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;

    AHardwareBuffer *buf = NULL;
    int ret = AHB_allocate(&desc, &buf);
    if (ret != 0 || !buf) { printf("AHardwareBuffer_allocate failed: %d\n", ret); return 1; }
    printf("AHardwareBuffer_allocate ok, buf=%p\n", (void*)buf);

    void *lib = dlopen("/data/data/com.termux/files/home/funnymdzz-mesa/build-android/src/panfrost/vulkan/libvulkan_panfrost.so", RTLD_NOW);
    if (!lib) { printf("dlopen driver failed: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr icd_gpa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");

    PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)icd_gpa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance instance;
    CHECK(CreateInstance(&ici, NULL, &instance), "vkCreateInstance");

    PFN_vkEnumeratePhysicalDevices EnumDevs = (PFN_vkEnumeratePhysicalDevices)icd_gpa(instance, "vkEnumeratePhysicalDevices");
    uint32_t count = 1; VkPhysicalDevice pdev;
    EnumDevs(instance, &count, &pdev);

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
    printf("allocationSize=%llu memoryTypeBits=0x%x\n", (unsigned long long)props.allocationSize, props.memoryTypeBits);

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
        .extent = { 64, 64, 1 },
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img;
    CHECK(CreateImage(device, &ici2, NULL, &img), "vkCreateImage (respaldada por AHB)");

    uint32_t mem_type_index = 0;
    for (uint32_t i = 0; i < 32; i++) if (props.memoryTypeBits & (1u << i)) { mem_type_index = i; break; }

    VkImportAndroidHardwareBufferInfoANDROID import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .buffer = buf,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = img,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = mem_type_index,
    };
    PFN_vkAllocateMemory AllocateMemory = DPROC(vkAllocateMemory);
    VkDeviceMemory mem;
    CHECK(AllocateMemory(device, &mai, NULL, &mem), "vkAllocateMemory (dedicada, ligada a VkImage)");

    PFN_vkBindImageMemory BindImageMemory = DPROC(vkBindImageMemory);
    CHECK(BindImageMemory(device, img, mem, 0), "vkBindImageMemory");

    PFN_vkCreateBuffer CreateBuffer = DPROC(vkCreateBuffer);
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64 * 64 * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer readback_buf;
    CHECK(CreateBuffer(device, &bci, NULL, &readback_buf), "vkCreateBuffer (readback)");

    PFN_vkGetBufferMemoryRequirements GetBufMemReqs = DPROC(vkGetBufferMemoryRequirements);
    VkMemoryRequirements buf_reqs;
    GetBufMemReqs(device, readback_buf, &buf_reqs);

    PFN_vkGetPhysicalDeviceMemoryProperties GetPDMemProps =
        (PFN_vkGetPhysicalDeviceMemoryProperties)icd_gpa(instance, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties pd_mem_props;
    GetPDMemProps(pdev, &pd_mem_props);
    uint32_t host_visible_index = 0;
    for (uint32_t i = 0; i < pd_mem_props.memoryTypeCount; i++) {
        if ((buf_reqs.memoryTypeBits & (1u << i)) &&
            (pd_mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            host_visible_index = i; break;
        }
    }
    VkMemoryAllocateInfo buf_mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buf_reqs.size, .memoryTypeIndex = host_visible_index };
    VkDeviceMemory readback_mem;
    CHECK(AllocateMemory(device, &buf_mai, NULL, &readback_mem), "vkAllocateMemory (readback)");
    PFN_vkBindBufferMemory BindBufferMemory = DPROC(vkBindBufferMemory);
    CHECK(BindBufferMemory(device, readback_buf, readback_mem, 0), "vkBindBufferMemory (readback)");

    PFN_vkMapMemory MapMemory = DPROC(vkMapMemory);
    void *mapped;
    CHECK(MapMemory(device, readback_mem, 0, buf_reqs.size, 0, &mapped), "vkMapMemory (readback)");
    memset(mapped, 0xAA, buf_reqs.size);
    printf("readback buffer inicializado con 0xAA\n");

    PFN_vkGetDeviceQueue GetDeviceQueue = DPROC(vkGetDeviceQueue);
    VkQueue queue;
    GetDeviceQueue(device, 0, 0, &queue);

    PFN_vkCreateCommandPool CreateCommandPool = DPROC(vkCreateCommandPool);
    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool cpool;
    CHECK(CreateCommandPool(device, &cpi, NULL, &cpool), "vkCreateCommandPool");

    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = DPROC(vkAllocateCommandBuffers);
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmdbuf;
    CHECK(AllocateCommandBuffers(device, &cbai, &cmdbuf), "vkAllocateCommandBuffers");

    PFN_vkBeginCommandBuffer BeginCommandBuffer = DPROC(vkBeginCommandBuffer);
    VkCommandBufferBeginInfo bbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(BeginCommandBuffer(cmdbuf, &bbi), "vkBeginCommandBuffer");

    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = DPROC(vkCmdPipelineBarrier);
    PFN_vkCmdClearColorImage CmdClearColorImage = DPROC(vkCmdClearColorImage);
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = DPROC(vkCmdCopyImageToBuffer);
    PFN_vkEndCommandBuffer EndCommandBuffer = DPROC(vkEndCommandBuffer);
    PFN_vkQueueSubmit QueueSubmit = DPROC(vkQueueSubmit);
    PFN_vkQueueWaitIdle QueueWaitIdle = DPROC(vkQueueWaitIdle);

    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier to_dst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = range, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    CmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);

    VkClearColorValue clear_color = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } };
    CmdClearColorImage(cmdbuf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &range);

    VkImageMemoryBarrier to_src = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = range,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    CmdPipelineBarrier(cmdbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);

    VkBufferImageCopy region = { .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = {0,0,0}, .imageExtent = {64,64,1} };
    CmdCopyImageToBuffer(cmdbuf, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_buf, 1, &region);

    CHECK(EndCommandBuffer(cmdbuf), "vkEndCommandBuffer");

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmdbuf };
    CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    CHECK(QueueWaitIdle(queue), "vkQueueWaitIdle");

    uint8_t *pixels = (uint8_t*)mapped;
    printf("\npixel central despues del clear: R=%u G=%u B=%u A=%u (esperado: 0,0,255,255 = azul)\n",
           pixels[0], pixels[1], pixels[2], pixels[3]);

    AHB_release(buf);
    return 0;
}
