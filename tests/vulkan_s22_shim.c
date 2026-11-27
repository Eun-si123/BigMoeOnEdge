// LD_PRELOAD shim: make a conformant Vulkan 1.2+ driver (lavapipe) look like the Galaxy S22 Ultra's
// Adreno 730 as its Vulkan report described it:
//   - physical device apiVersion 1.1.128, reported as an integrated GPU
//   - VkPhysicalDeviceVulkan1{1,2,3,4}{Features,Properties} in a query chain are not filled (a 1.1
//     driver does not know them); in vkCreateDevice they are ignored (dropped from the chain)
//   - vkGetDeviceProcAddr: 1.2/1.3-core names -> NULL, KHR names as the driver answers them
//   - vkGetInstanceProcAddr: 1.2/1.3-core device names -> a stub that jumps to address 0 (the
//     loader's trampoline into an empty dispatch slot), KHR names as the driver answers them
// Knobs: S22_HIDE_EXT="VK_A,VK_B" hides device extensions; S22_NO_INT64=1 clears shaderInt64.
//
// Not built by CMake: it is a manual tool for a machine with a Vulkan driver (docs/s22-compatibility.md).
//   cc -shared -fPIC tests/vulkan_s22_shim.c -o s22shim.so -ldl
//   VK_ICD_FILENAMES=<lavapipe icd json> LD_PRELOAD=$PWD/s22shim.so build/bin/bmoe-cli --vulkan-report
//   ... or in front of llama.cpp's test-backend-ops / llama-simple built with GGML_VULKAN=ON.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const char *promoted[] = {
    "vkGetBufferDeviceAddress", "vkGetBufferOpaqueCaptureAddress", "vkGetDeviceMemoryOpaqueCaptureAddress",
    "vkWaitSemaphores", "vkSignalSemaphore", "vkGetSemaphoreCounterValue", "vkResetQueryPool",
    "vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount", "vkCreateRenderPass2", "vkCmdBeginRenderPass2",
    "vkCmdNextSubpass2", "vkCmdEndRenderPass2",
    "vkCmdPipelineBarrier2", "vkQueueSubmit2", "vkCmdWriteTimestamp2", "vkCmdCopyBuffer2",
    "vkGetDeviceBufferMemoryRequirements", "vkGetDeviceImageMemoryRequirements", "vkCmdBeginRendering",
    "vkCmdEndRendering", "vkCmdSetEvent2", "vkCmdResetEvent2", "vkCmdWaitEvents2", "vkCreatePrivateDataSlot",
    0};
static int is_promoted(const char *n) {
    for (const char **p = promoted; *p; ++p)
        if (!strcmp(*p, n)) return 1;
    return 0;
}
static int listed(const char *env, const char *n) {
    const char *l = getenv(env);
    if (!l) return 0;
    size_t k = strlen(n);
    for (const char *p = l; (p = strstr(p, n)); p += k)
        if ((p == l || p[-1] == ',') && (p[k] == 0 || p[k] == ',')) return 1;
    return 0;
}
static void *real(const char *n) {
    void *f = dlsym(RTLD_NEXT, n);
    if (!f) {
        static void *lib;
        if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
        f = lib ? dlsym(lib, n) : NULL;
    }
    return f;
}

static void jump_to_zero(void) {
    void (*volatile f)(void) = 0;
    f();
}

static int unknown_to_11(VkStructureType t) {
    return t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES ||
           t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
           t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES ||
           t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES ||
           t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES ||
           t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES
           || t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES ||
           t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES
#endif
        ;
}

// Unlink the structs a 1.1 driver does not know; returns them so they can be put back.
#define MAXU 16
typedef struct { VkBaseOutStructure *prev, *self; } Cut;
static int cut(void *head, Cut *out) {
    int n = 0;
    VkBaseOutStructure *prev = (VkBaseOutStructure *) head;
    while (prev->pNext && n < MAXU) {
        VkBaseOutStructure *s = prev->pNext;
        if (unknown_to_11(s->sType)) {
            out[n].prev = prev;
            out[n].self = s;
            ++n;
            prev->pNext = s->pNext;
        } else {
            prev = s;
        }
    }
    return n;
}
static void restore(Cut *c, int n) {
    for (int i = n - 1; i >= 0; --i) c[i].prev->pNext = c[i].self;
}

static void lower(VkPhysicalDeviceProperties *p) {
    p->apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 128);
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice d, VkPhysicalDeviceProperties *p) {
    ((PFN_vkGetPhysicalDeviceProperties) real("vkGetPhysicalDeviceProperties"))(d, p);
    lower(p);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *p) {
    Cut c[MAXU];
    int n = cut(p, c);
    ((PFN_vkGetPhysicalDeviceProperties2) real("vkGetPhysicalDeviceProperties2"))(d, p);
    restore(c, n);
    lower(&p->properties);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2 *f) {
    Cut c[MAXU];
    int n = cut(f, c);
    ((PFN_vkGetPhysicalDeviceFeatures2) real("vkGetPhysicalDeviceFeatures2"))(d, f);
    restore(c, n);
    if (getenv("S22_NO_INT64")) f->features.shaderInt64 = VK_FALSE;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice d, VkPhysicalDeviceFeatures *f) {
    ((PFN_vkGetPhysicalDeviceFeatures) real("vkGetPhysicalDeviceFeatures"))(d, f);
    if (getenv("S22_NO_INT64")) f->shaderInt64 = VK_FALSE;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice d, const char *layer,
                                                                    uint32_t *count, VkExtensionProperties *props) {
    PFN_vkEnumerateDeviceExtensionProperties r =
        (PFN_vkEnumerateDeviceExtensionProperties) real("vkEnumerateDeviceExtensionProperties");
    uint32_t n = 0;
    r(d, layer, &n, NULL);
    VkExtensionProperties *all = calloc(n ? n : 1, sizeof *all);
    r(d, layer, &n, all);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (listed("S22_HIDE_EXT", all[i].extensionName)) continue;
        if (props && k < *count) props[k] = all[i];
        ++k;
    }
    free(all);
    if (!props) {
        *count = k;
        return VK_SUCCESS;
    }
    VkResult res = k > *count ? VK_INCOMPLETE : VK_SUCCESS;
    if (k < *count) *count = k;
    return res;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice d, const VkDeviceCreateInfo *ci,
                                              const VkAllocationCallbacks *a, VkDevice *out) {
    VkDeviceCreateInfo copy = *ci;
    Cut c[MAXU];
    int n = cut(&copy, c);
    VkResult r = ((PFN_vkCreateDevice) real("vkCreateDevice"))(d, &copy, a, out);
    restore(c, n);
    return r;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *n) {
    if (is_promoted(n)) return NULL;
    return ((PFN_vkGetDeviceProcAddr) real("vkGetDeviceProcAddr"))(dev, n);
}

#define WRAP(name)                                                                                          \
    if (!strcmp(n, #name)) return (PFN_vkVoidFunction) name;
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i, const char *n) {
    WRAP(vkGetInstanceProcAddr)
    WRAP(vkGetDeviceProcAddr)
    WRAP(vkGetPhysicalDeviceProperties)
    WRAP(vkGetPhysicalDeviceProperties2)
    WRAP(vkGetPhysicalDeviceFeatures)
    WRAP(vkGetPhysicalDeviceFeatures2)
    WRAP(vkEnumerateDeviceExtensionProperties)
    WRAP(vkCreateDevice)
    if (!strcmp(n, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction) vkGetPhysicalDeviceProperties2;
    if (!strcmp(n, "vkGetPhysicalDeviceFeatures2KHR")) return (PFN_vkVoidFunction) vkGetPhysicalDeviceFeatures2;
    if (i && is_promoted(n)) return (PFN_vkVoidFunction) jump_to_zero;
    return ((PFN_vkGetInstanceProcAddr) real("vkGetInstanceProcAddr"))(i, n);
}
