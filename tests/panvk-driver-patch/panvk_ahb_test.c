/*
 * Prueba de importacion de AHardwareBuffer contra el driver PanVK real,
 * construido con el fix de Fase 9 (android_wsi_ahb_fixes.patch) mas el
 * fix del target triple para __INTRODUCED_IN. No existe un test dedicado
 * en el repo de LukeValen para esto (solo lo describio en el commit), asi
 * que se escribe desde cero siguiendo esa misma descripcion:
 * AHardwareBuffer_allocate() contra libnativewindow.so real del sistema +
 * vkGetAndroidHardwareBufferPropertiesANDROID() contra este driver.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <android/hardware_buffer.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include "vulkan/vulkan.h"

#define CHECK(x, msg) do { VkResult r = (x); if (r != VK_SUCCESS) { printf("FAILED %s: VkResult=%d\n", msg, r); return 1; } printf("%s ok\n", msg); } while (0)

typedef int  (*PFN_AHB_allocate)(const AHardwareBuffer_Desc*, AHardwareBuffer**);
typedef void (*PFN_AHB_describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*);
typedef void (*PFN_AHB_release)(AHardwareBuffer*);

int main(void) {
    /* 1) libnativewindow.so REAL del sistema, no el stub de build */
    void *libnw = dlopen("libnativewindow.so", RTLD_NOW);
    if (!libnw) { printf("dlopen libnativewindow.so failed: %s\n", dlerror()); return 1; }
    printf("libnativewindow.so (sistema real) cargado ok\n");

    PFN_AHB_allocate AHB_allocate = (PFN_AHB_allocate)dlsym(libnw, "AHardwareBuffer_allocate");
    PFN_AHB_describe AHB_describe = (PFN_AHB_describe)dlsym(libnw, "AHardwareBuffer_describe");
    PFN_AHB_release  AHB_release  = (PFN_AHB_release)dlsym(libnw, "AHardwareBuffer_release");
    if (!AHB_allocate || !AHB_describe || !AHB_release) {
        printf("dlsym de funciones AHardwareBuffer_* fallo\n"); return 1;
    }
    printf("simbolos AHardwareBuffer_* resueltos ok\n");

    /* 2) Asignar un buffer real de 64x64 RGBA8 */
    AHardwareBuffer_Desc desc = {0};
    desc.width = 64; desc.height = 64; desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;

    AHardwareBuffer *buf = NULL;
    int ret = AHB_allocate(&desc, &buf);
    if (ret != 0 || !buf) { printf("AHardwareBuffer_allocate failed: %d\n", ret); return 1; }
    printf("AHardwareBuffer_allocate ok, buf=%p\n", (void*)buf);

    AHardwareBuffer_Desc real_desc;
    AHB_describe(buf, &real_desc);
    printf("descripcion real: %ux%u layers=%u format=%u usage=0x%llx\n\n",
           real_desc.width, real_desc.height, real_desc.layers, real_desc.format,
           (unsigned long long)real_desc.usage);

    /* 3) Cargar nuestro driver y ver que extensiones de device declara */
    void *lib = dlopen("/data/data/com.termux/files/home/funnymdzz-mesa/build-android/src/panfrost/vulkan/libvulkan_panfrost.so", RTLD_NOW);
    if (!lib) { printf("dlopen driver failed: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr icd_gpa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!icd_gpa) { printf("dlsym vk_icdGetInstanceProcAddr failed\n"); return 1; }

    PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)icd_gpa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance instance;
    CHECK(CreateInstance(&ici, NULL, &instance), "vkCreateInstance");

    PFN_vkEnumeratePhysicalDevices EnumDevs = (PFN_vkEnumeratePhysicalDevices)icd_gpa(instance, "vkEnumeratePhysicalDevices");
    uint32_t count = 1; VkPhysicalDevice pdev;
    EnumDevs(instance, &count, &pdev);
    printf("physical device count=%u\n", count);

    PFN_vkEnumerateDeviceExtensionProperties EnumDevExt =
        (PFN_vkEnumerateDeviceExtensionProperties)icd_gpa(instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t ext_count = 0;
    EnumDevExt(pdev, NULL, &ext_count, NULL);
    VkExtensionProperties *exts = malloc(sizeof(VkExtensionProperties) * ext_count);
    EnumDevExt(pdev, NULL, &ext_count, exts);
    int has_ahb_ext = 0;
    printf("\nextensiones de device declaradas (%u):\n", ext_count);
    for (uint32_t i = 0; i < ext_count; i++) {
        printf("  %s\n", exts[i].extensionName);
        if (strstr(exts[i].extensionName, "hardware_buffer")) has_ahb_ext = 1;
    }
    printf("\nVK_ANDROID_external_memory_android_hardware_buffer declarada: %s\n\n",
           has_ahb_ext ? "SI" : "NO");

    if (!has_ahb_ext) {
        printf("El driver no declara la extension -- me detengo aqui, no tiene\n"
               "caso intentar vkCreateDevice pidiendola.\n");
        return 0;
    }

    PFN_vkCreateDevice CreateDevice = (PFN_vkCreateDevice)icd_gpa(instance, "vkCreateDevice");
    const char *dev_exts[] = { "VK_KHR_external_memory", "VK_ANDROID_external_memory_android_hardware_buffer" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                                .enabledExtensionCount = 2, .ppEnabledExtensionNames = dev_exts };
    VkDevice device;
    CHECK(CreateDevice(pdev, &dci, NULL, &device), "vkCreateDevice (con extension AHB)");

    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)icd_gpa(instance, "vkGetDeviceProcAddr");
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID GetAHBProps =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)GetDeviceProcAddr(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!GetAHBProps) { printf("vkGetDeviceProcAddr(vkGetAndroidHardwareBufferPropertiesANDROID) = NULL\n"); return 1; }
    printf("vkGetAndroidHardwareBufferPropertiesANDROID resuelto ok\n");

    VkAndroidHardwareBufferPropertiesANDROID props = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    CHECK(GetAHBProps(device, buf, &props), "vkGetAndroidHardwareBufferPropertiesANDROID");

    printf("\nallocationSize=%llu (esperado: cercano al tamano real del buffer)\n"
           "memoryTypeBits=0x%x (esperado: distinto de 0)\n",
           (unsigned long long)props.allocationSize, props.memoryTypeBits);

    /* 4) vkAllocateMemory real, importando el AHardwareBuffer -- ejercita
       panvk_android_import_ahb_memory(), la funcion que acabamos de
       parchar (fix del indice de fd del dma-buf). */
    uint32_t mem_type_index = 0;
    for (uint32_t i = 0; i < 32; i++) {
        if (props.memoryTypeBits & (1u << i)) { mem_type_index = i; break; }
    }
    printf("\nusando memoryTypeIndex=%u (primer bit de memoryTypeBits)\n", mem_type_index);

    PFN_vkAllocateMemory AllocateMemory =
        (PFN_vkAllocateMemory)GetDeviceProcAddr(device, "vkAllocateMemory");
    PFN_vkFreeMemory FreeMemory =
        (PFN_vkFreeMemory)GetDeviceProcAddr(device, "vkFreeMemory");

    VkImportAndroidHardwareBufferInfoANDROID import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .buffer = buf,
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = mem_type_index,
    };
    VkDeviceMemory mem;
    CHECK(AllocateMemory(device, &mai, NULL, &mem), "vkAllocateMemory (import AHB real)");

    printf("\n*** vkAllocateMemory con import de AHardwareBuffer real: EXITO ***\n");

    FreeMemory(device, mem, NULL);
    AHB_release(buf);
    return 0;
}
