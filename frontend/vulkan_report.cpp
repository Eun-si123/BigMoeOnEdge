// See vulkan_report.h. The loader is opened at run time, so a build carries this report whether or not
// it was built with the Vulkan backend, and a machine without a loader gets one line saying so.

#include "vulkan_report.h"
#include "vulkan_report_spirv.h"

#if defined(__has_include)
#if __has_include(<vulkan/vulkan.h>) && !defined(_WIN32)
#define BMOE_VULKAN_REPORT 1
#endif
#endif

#ifdef BMOE_VULKAN_REPORT
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bmoe::frontend {

const char * const vulkan_report_marker = "BMOE_VK";

#ifndef BMOE_VULKAN_REPORT

bool vulkan_report(const std::function<void(const std::string &)> & emit) {
    emit("vulkan: this build carries no Vulkan headers, so it cannot ask the loader");
    return false;
}

#else

namespace {

using Emit = std::function<void(const std::string &)>;

// A Vulkan input struct, zeroed, with its type set.
template <class T> T vk_struct(VkStructureType type) {
    T v{};
    v.sType = type;
    return v;
}

std::string version(uint32_t v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    return b;
}

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08x", v);
    return b;
}

const char * yes(bool b) {
    return b ? "yes" : "no";
}

const char * type_name(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual-gpu";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    default:
        return "other";
    }
}

// The 1.2-core device commands ggml's Vulkan backend calls, with the alias a driver may answer instead
// and the extension that alias belongs to. A KHR name is only valid on a device that enabled its
// extension, which is why it is asked twice below.
struct Command {
    const char * core;
    const char * khr; // nullptr where the command has no KHR alias
    const char * extension;
};
constexpr Command k_commands[] = {
    {"vkGetBufferDeviceAddress", "vkGetBufferDeviceAddressKHR", "VK_KHR_buffer_device_address"},
    {"vkWaitSemaphores", "vkWaitSemaphoresKHR", "VK_KHR_timeline_semaphore"},
    {"vkSignalSemaphore", "vkSignalSemaphoreKHR", "VK_KHR_timeline_semaphore"},
    {"vkGetSemaphoreCounterValue", "vkGetSemaphoreCounterValueKHR", "VK_KHR_timeline_semaphore"},
    {"vkGetMemoryHostPointerPropertiesEXT", nullptr, "VK_EXT_external_memory_host"},
};

// The device extensions the backend's choices turn on; the full list is printed too.
constexpr const char * k_watched[] = {
    "VK_KHR_buffer_device_address",
    "VK_KHR_timeline_semaphore",
    "VK_EXT_external_memory_host",
    "VK_KHR_16bit_storage",
    "VK_KHR_8bit_storage",
    "VK_KHR_shader_float16_int8",
    "VK_KHR_shader_integer_dot_product",
    "VK_KHR_cooperative_matrix",
    "VK_EXT_subgroup_size_control",
    "VK_EXT_memory_budget",
    "VK_KHR_driver_properties",
    "VK_EXT_pipeline_robustness",
};

struct Instance {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    template <class F> F get(const char * name) const { return reinterpret_cast<F>(gipa(instance, name)); }
};

bool has(const std::vector<VkExtensionProperties> & exts, const char * name) {
    for (const VkExtensionProperties & e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

const char * ok(PFN_vkVoidFunction f) {
    return f ? "ok" : "NULL";
}

// ggml ships every shader as SPIR-V 1.5 (glslc --target-env=vulkan1.2). By the spec a 1.1 device takes
// SPIR-V 1.3 (1.4 with VK_KHR_spirv_1_4); what its driver does with 1.5 is what this asks: a compute
// pipeline from each module, 1.3 and 1.5, a plain one and one with the 8/16-bit storage, Int8,
// Float16 and subgroup-arithmetic capabilities most of ggml's shaders declare.
void try_pipelines(
    PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, const std::string & key, bool types_enabled, const Emit & emit) {
    auto create_module = reinterpret_cast<PFN_vkCreateShaderModule>(gdpa(dev, "vkCreateShaderModule"));
    auto destroy_module = reinterpret_cast<PFN_vkDestroyShaderModule>(gdpa(dev, "vkDestroyShaderModule"));
    auto create_dsl = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(gdpa(dev, "vkCreateDescriptorSetLayout"));
    auto destroy_dsl = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(gdpa(dev, "vkDestroyDescriptorSetLayout"));
    auto create_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(gdpa(dev, "vkCreatePipelineLayout"));
    auto destroy_layout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(gdpa(dev, "vkDestroyPipelineLayout"));
    auto create_pipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(gdpa(dev, "vkCreateComputePipelines"));
    auto destroy_pipeline = reinterpret_cast<PFN_vkDestroyPipeline>(gdpa(dev, "vkDestroyPipeline"));
    if (!create_module || !destroy_module || !create_dsl || !destroy_dsl || !create_layout || !destroy_layout ||
        !create_pipelines || !destroy_pipeline) {
        emit(key + ".spirv: skipped (a pipeline command did not resolve)");
        return;
    }
    struct Module {
        const char * name;
        const uint32_t * words;
        size_t bytes;
        uint32_t bindings;
        bool types;
    };
    namespace sv = spirv;
    const Module modules[] = {
        {"basic-1.3", sv::basic_13, sizeof(sv::basic_13), 1, false},
        {"basic-1.5", sv::basic_15, sizeof(sv::basic_15), 1, false},
        {"types-1.3", sv::types_13, sizeof(sv::types_13), 3, true},
        {"types-1.5", sv::types_15, sizeof(sv::types_15), 3, true},
    };
    for (const Module & m : modules) {
        const std::string row = key + ".spirv." + m.name + ": ";
        if (m.types && !types_enabled) {
            emit(row + "skipped (8/16-bit storage, Int8 or Float16 not enabled on this device)");
            continue;
        }
        emit("step: " + key + " pipeline " + m.name);
        auto mi = vk_struct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        mi.codeSize = m.bytes;
        mi.pCode = m.words;
        VkShaderModule mod = VK_NULL_HANDLE;
        VkResult r = create_module(dev, &mi, nullptr, &mod);
        if (r != VK_SUCCESS) {
            emit(row + "module refused (VkResult " + std::to_string((int) r) + ")");
            continue;
        }
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < m.bindings; ++i) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        auto di = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        di.bindingCount = m.bindings;
        di.pBindings = b;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        r = create_dsl(dev, &di, nullptr, &dsl);
        if (r == VK_SUCCESS) {
            auto li = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
            li.setLayoutCount = 1;
            li.pSetLayouts = &dsl;
            r = create_layout(dev, &li, nullptr, &layout);
        }
        if (r == VK_SUCCESS) {
            auto ci = vk_struct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
            ci.stage = vk_struct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = mod;
            ci.stage.pName = "main";
            ci.layout = layout;
            r = create_pipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
        }
        emit(row + (r == VK_SUCCESS ? std::string("pipeline created")
                                    : "refused (VkResult " + std::to_string((int) r) + ")"));
        if (pipeline) destroy_pipeline(dev, pipeline, nullptr);
        if (layout) destroy_layout(dev, layout, nullptr);
        if (dsl) destroy_dsl(dev, dsl, nullptr);
        destroy_module(dev, mod, nullptr);
    }
}

// One logical device, created with `extensions` and the feature chain `features` (may be null), and
// every command above resolved on it both ways. Then the calls ggml makes, through what resolved.
void try_device(const Instance & in,
                VkPhysicalDevice pd,
                uint32_t queue_family,
                const std::string & key,
                const std::vector<const char *> & extensions,
                void * features,
                bool bda_enabled,
                bool timeline_enabled,
                bool types_enabled,
                bool pre12,
                const Emit & emit) {
    auto create_device = in.get<PFN_vkCreateDevice>("vkCreateDevice");
    auto gdpa = in.get<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    auto destroy_device = in.get<PFN_vkDestroyDevice>("vkDestroyDevice");
    if (!create_device || !gdpa || !destroy_device) {
        emit(key + ".created: no (vkCreateDevice or vkGetDeviceProcAddr did not resolve)");
        return;
    }
    std::string list;
    for (const char * e : extensions)
        list += std::string(list.empty() ? "" : " ") + e;
    emit(key + ".extensions-enabled: " + (list.empty() ? "(none)" : list));

    const float priority = 1.0f;
    auto q = vk_struct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    q.queueFamilyIndex = queue_family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    auto ci = vk_struct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    ci.pNext = features;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = (uint32_t) extensions.size();
    ci.ppEnabledExtensionNames = extensions.data();
    emit("step: " + key + " vkCreateDevice");
    VkDevice dev = VK_NULL_HANDLE;
    const VkResult r = create_device(pd, &ci, nullptr, &dev);
    if (r != VK_SUCCESS) {
        emit(key + ".created: no (VkResult " + std::to_string((int) r) + ")");
        return;
    }
    emit(key + ".created: yes");

    for (const Command & c : k_commands) {
        std::string line = key + ".cmd." + c.core + ": instance=" + ok(in.gipa(in.instance, c.core)) +
                           " device=" + ok(gdpa(dev, c.core));
        if (c.khr)
            line +=
                std::string(" | KHR instance=") + ok(in.gipa(in.instance, c.khr)) + " device=" + ok(gdpa(dev, c.khr));
        emit(line);
    }

    // The calls. ggml's dispatcher takes its device commands from vkGetInstanceProcAddr, so that pointer
    // is the one that matters; the device's own pointer is tried as well, so the two can be told apart.
    auto create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(gdpa(dev, "vkCreateBuffer"));
    auto destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(gdpa(dev, "vkDestroyBuffer"));
    auto mem_reqs = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(gdpa(dev, "vkGetBufferMemoryRequirements"));
    auto allocate = reinterpret_cast<PFN_vkAllocateMemory>(gdpa(dev, "vkAllocateMemory"));
    auto free_memory = reinterpret_cast<PFN_vkFreeMemory>(gdpa(dev, "vkFreeMemory"));
    auto bind = reinterpret_cast<PFN_vkBindBufferMemory>(gdpa(dev, "vkBindBufferMemory"));
    auto mem_props = in.get<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
    if (bda_enabled && create_buffer && destroy_buffer && mem_reqs && allocate && free_memory && bind && mem_props) {
        auto bi = vk_struct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        bi.size = 4096;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (create_buffer(dev, &bi, nullptr, &buf) == VK_SUCCESS) {
            VkMemoryRequirements req{};
            mem_reqs(dev, buf, &req);
            VkPhysicalDeviceMemoryProperties mp{};
            mem_props(pd, &mp);
            uint32_t type = UINT32_MAX;
            for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
                if (req.memoryTypeBits & (1u << i)) type = i;
            auto fi = vk_struct<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
            fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
            auto ai = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
            ai.pNext = &fi;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type;
            if (type != UINT32_MAX && allocate(dev, &ai, nullptr, &mem) == VK_SUCCESS &&
                bind(dev, buf, mem, 0) == VK_SUCCESS) {
                auto info = vk_struct<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
                info.buffer = buf;
                struct Way {
                    const char * how;
                    PFN_vkGetBufferDeviceAddress f;
                };
                // Below 1.2 only the extension's command is valid to call: the core name is the crash
                // this report exists to find, so it is never called there.
                const std::vector<Way> ways =
                    pre12 ? std::vector<Way>{{"device-khr", reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
                                                                gdpa(dev, "vkGetBufferDeviceAddressKHR"))}}
                          : std::vector<Way>{
                                {"instance", reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
                                                 in.gipa(in.instance, "vkGetBufferDeviceAddress"))},
                                {"device",
                                 reinterpret_cast<PFN_vkGetBufferDeviceAddress>(gdpa(dev, "vkGetBufferDeviceAddress"))},
                            };
                if (pre12) emit(key + ".call.buffer-address.core: skipped (device below 1.2: not valid to call)");
                for (const auto & w : ways) {
                    if (!w.f) {
                        emit(key + ".call.buffer-address." + w.how + ": skipped (NULL: ggml would jump to 0 here)");
                        continue;
                    }
                    emit("step: " + key + " vkGetBufferDeviceAddress via " + w.how);
                    emit(key + ".call.buffer-address." + w.how + ": " + yes(w.f(dev, &info) != 0) +
                         " (non-zero address)");
                }
            } else {
                emit(key + ".call.buffer-address: skipped (no memory for a 4 KiB device-address buffer)");
            }
            if (mem) free_memory(dev, mem, nullptr);
            destroy_buffer(dev, buf, nullptr);
        }
    } else {
        emit(key + ".call.buffer-address: skipped (feature not enabled on this device)");
    }

    auto create_sem = reinterpret_cast<PFN_vkCreateSemaphore>(gdpa(dev, "vkCreateSemaphore"));
    auto destroy_sem = reinterpret_cast<PFN_vkDestroySemaphore>(gdpa(dev, "vkDestroySemaphore"));
    // Below 1.2, the extension's commands from the device; otherwise the core ones through the instance,
    // as ggml's dispatcher takes them.
    const char * via = pre12 ? "device-khr" : "instance";
    auto signal = reinterpret_cast<PFN_vkSignalSemaphore>(pre12 ? gdpa(dev, "vkSignalSemaphoreKHR")
                                                                : in.gipa(in.instance, "vkSignalSemaphore"));
    auto value = reinterpret_cast<PFN_vkGetSemaphoreCounterValue>(
        pre12 ? gdpa(dev, "vkGetSemaphoreCounterValueKHR") : in.gipa(in.instance, "vkGetSemaphoreCounterValue"));
    auto wait = reinterpret_cast<PFN_vkWaitSemaphores>(pre12 ? gdpa(dev, "vkWaitSemaphoresKHR")
                                                             : in.gipa(in.instance, "vkWaitSemaphores"));
    if (timeline_enabled && create_sem && destroy_sem) {
        auto ti = vk_struct<VkSemaphoreTypeCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        auto si = vk_struct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        si.pNext = &ti;
        VkSemaphore sem = VK_NULL_HANDLE;
        if (create_sem(dev, &si, nullptr, &sem) == VK_SUCCESS) {
            if (!signal || !value || !wait) {
                emit(key + ".call.timeline." + via + ": skipped (a timeline command is NULL: ggml would jump to 0)");
            } else {
                emit("step: " + key + " timeline semaphore via " + via);
                auto sg = vk_struct<VkSemaphoreSignalInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO);
                sg.semaphore = sem;
                sg.value = 1;
                uint64_t v = 0;
                const uint64_t want = 1;
                auto wi = vk_struct<VkSemaphoreWaitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
                wi.semaphoreCount = 1;
                wi.pSemaphores = &sem;
                wi.pValues = &want;
                const bool good = signal(dev, &sg) == VK_SUCCESS && value(dev, sem, &v) == VK_SUCCESS && v == 1 &&
                                  wait(dev, &wi, 0) == VK_SUCCESS;
                emit(key + ".call.timeline." + via + ": " + yes(good) + " (signal 1, read 1, wait)");
            }
            destroy_sem(dev, sem, nullptr);
        }
    } else {
        emit(key + ".call.timeline: skipped (feature not enabled on this device)");
    }

    try_pipelines(gdpa, dev, key, types_enabled, emit);
    destroy_device(dev, nullptr);
}

void report_device(
    const Instance & in, uint32_t instance_version, VkPhysicalDevice pd, uint32_t index, const Emit & emit) {
    const std::string key = "device" + std::to_string(index);
    auto props_fn = in.get<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
    auto props2_fn = in.get<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
    auto features2_fn = in.get<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    auto ext_fn = in.get<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    auto queues_fn = in.get<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    if (!props_fn || !ext_fn || !queues_fn) {
        emit(key + ": the instance did not resolve its physical-device commands");
        return;
    }
    VkPhysicalDeviceProperties p{};
    props_fn(pd, &p);
    // The device's own version: the one Box #109-style reports leave ambiguous with the loader's.
    emit(key + ".name: " + p.deviceName);
    emit(key + ".type: " + type_name(p.deviceType));
    emit(key + ".api-version: " + version(p.apiVersion) + " (" + hex(p.apiVersion) + ")");
    emit(key + ".driver-version: " + hex(p.driverVersion));
    emit(key + ".vendor-device: " + hex(p.vendorID) + " " + hex(p.deviceID));

    uint32_t n = 0;
    ext_fn(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    if (n) ext_fn(pd, nullptr, &n, exts.data());
    exts.resize(n);
    for (const char * w : k_watched)
        emit(key + ".ext." + w + ": " + yes(has(exts, w)));
    std::string all;
    for (const VkExtensionProperties & e : exts)
        all += std::string(all.empty() ? "" : " ") + e.extensionName;
    emit(key + ".extensions-offered: " + std::to_string(n) + " " + all);

    const bool dev12 = p.apiVersion >= VK_API_VERSION_1_2;
    const bool inst11 = instance_version >= VK_API_VERSION_1_1;
    if (props2_fn && inst11 && (dev12 || has(exts, "VK_KHR_driver_properties"))) {
        auto drv = vk_struct<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
        auto p2 = vk_struct<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        p2.pNext = &drv;
        props2_fn(pd, &p2);
        emit(key + ".driver: id=" + std::to_string((int) drv.driverID) + " name=" + drv.driverName +
             " info=" + drv.driverInfo);
        emit(key + ".conformance: " + std::to_string(drv.conformanceVersion.major) + "." +
             std::to_string(drv.conformanceVersion.minor) + "." + std::to_string(drv.conformanceVersion.subminor) +
             "." + std::to_string(drv.conformanceVersion.patch));
    }

    // Features: through the 1.1/1.2 structs only where the device is 1.2 (they are undefined below it),
    // otherwise through each extension's own struct where that extension is offered.
    bool bda = false, timeline = false, sb16 = false, sb8 = false, int8 = false, fp16 = false;
    if (features2_fn && inst11) {
        auto f2 = vk_struct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        auto f11 = vk_struct<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
        auto f12 = vk_struct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        auto fb = vk_struct<VkPhysicalDeviceBufferDeviceAddressFeatures>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
        auto ft = vk_struct<VkPhysicalDeviceTimelineSemaphoreFeatures>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        auto f16 =
            vk_struct<VkPhysicalDevice16BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES);
        auto f8 =
            vk_struct<VkPhysicalDevice8BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES);
        auto fhi = vk_struct<VkPhysicalDeviceShaderFloat16Int8Features>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
        if (dev12) {
            f2.pNext = &f11;
            f11.pNext = &f12;
        } else {
            void ** tail = &f2.pNext;
            *tail = &f16;
            tail = &f16.pNext;
            if (has(exts, "VK_KHR_buffer_device_address")) {
                *tail = &fb;
                tail = &fb.pNext;
            }
            if (has(exts, "VK_KHR_timeline_semaphore")) {
                *tail = &ft;
                tail = &ft.pNext;
            }
            if (has(exts, "VK_KHR_8bit_storage")) {
                *tail = &f8;
                tail = &f8.pNext;
            }
            if (has(exts, "VK_KHR_shader_float16_int8")) *tail = &fhi;
        }
        features2_fn(pd, &f2);
        bda = dev12 ? f12.bufferDeviceAddress : fb.bufferDeviceAddress;
        timeline = dev12 ? f12.timelineSemaphore : ft.timelineSemaphore;
        sb16 = dev12 ? f11.storageBuffer16BitAccess : f16.storageBuffer16BitAccess;
        sb8 = dev12 ? f12.storageBuffer8BitAccess : f8.storageBuffer8BitAccess;
        int8 = dev12 ? f12.shaderInt8 : fhi.shaderInt8;
        fp16 = dev12 ? f12.shaderFloat16 : fhi.shaderFloat16;
        emit(key + ".features: bufferDeviceAddress=" + yes(bda) + " timelineSemaphore=" + yes(timeline) +
             " storageBuffer16BitAccess=" + yes(sb16) + " storageBuffer8BitAccess=" + yes(sb8) + " shaderInt8=" +
             yes(int8) + " shaderFloat16=" + yes(fp16) + " shaderInt64=" + yes(f2.features.shaderInt64) +
             (dev12 ? std::string(" vulkanMemoryModel=") + yes(f12.vulkanMemoryModel)
                    : std::string(" (1.2 features read through the extensions' own structs)")));
    }

    uint32_t qn = 0;
    queues_fn(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    if (qn) queues_fn(pd, &qn, qs.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < qn && family == UINT32_MAX; ++i)
        if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) family = i;
    if (family == UINT32_MAX) {
        emit(key + ".compute-queue: none");
        return;
    }

    // A: the device as ggml creates it - the 1.2 features through the core structs, the promoted
    // extensions not enabled. Only the KHR rows of this pass that say NULL are spec-conformant.
    if (dev12) {
        auto f12 = vk_struct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        f12.bufferDeviceAddress = bda;
        f12.timelineSemaphore = timeline;
        f12.storageBuffer8BitAccess = sb8;
        f12.shaderInt8 = int8;
        f12.shaderFloat16 = fp16;
        auto f11 = vk_struct<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
        f11.pNext = &f12;
        f11.storageBuffer16BitAccess = sb16;
        auto f2 = vk_struct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        f2.pNext = &f11;
        std::vector<const char *> e;
        if (has(exts, "VK_EXT_external_memory_host")) e.push_back("VK_EXT_external_memory_host");
        try_device(in, pd, family, key + ".as-ggml", e, &f2, bda, timeline, sb16 && sb8 && int8 && fp16, false, emit);
    } else {
        emit(key + ".as-ggml: not tried (below 1.2 the core structs do not exist: see with-khr)");
    }

    // B: the promoted extensions enabled by name with their own feature structs, as the backend's
    // pre-1.2 path does. Shows whether the KHR names resolve once the extension is really on - the
    // only case where calling them is valid - and, below 1.2, is the device the backend would use.
    std::vector<const char *> e;
    auto fb = vk_struct<VkPhysicalDeviceBufferDeviceAddressFeatures>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
    auto ft = vk_struct<VkPhysicalDeviceTimelineSemaphoreFeatures>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
    auto f8 = vk_struct<VkPhysicalDevice8BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES);
    auto fhi = vk_struct<VkPhysicalDeviceShaderFloat16Int8Features>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES);
    auto f16 =
        vk_struct<VkPhysicalDevice16BitStorageFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES);
    f16.storageBuffer16BitAccess = sb16;
    void * chain = &f16; // core 1.1
    const bool khr_bda = has(exts, "VK_KHR_buffer_device_address");
    const bool khr_tl = has(exts, "VK_KHR_timeline_semaphore");
    const bool khr_8 = has(exts, "VK_KHR_8bit_storage");
    const bool khr_hi = has(exts, "VK_KHR_shader_float16_int8");
    if (khr_bda) {
        e.push_back("VK_KHR_buffer_device_address");
        fb.bufferDeviceAddress = bda;
        fb.pNext = chain;
        chain = &fb;
    }
    if (khr_tl) {
        e.push_back("VK_KHR_timeline_semaphore");
        ft.timelineSemaphore = timeline;
        ft.pNext = chain;
        chain = &ft;
    }
    if (khr_8) {
        e.push_back("VK_KHR_8bit_storage");
        f8.storageBuffer8BitAccess = sb8;
        f8.pNext = chain;
        chain = &f8;
    }
    if (khr_hi) {
        e.push_back("VK_KHR_shader_float16_int8");
        fhi.shaderInt8 = int8;
        fhi.shaderFloat16 = fp16;
        fhi.pNext = chain;
        chain = &fhi;
    }
    for (const char * x : {"VK_KHR_shader_float_controls", "VK_KHR_spirv_1_4"})
        if (has(exts, x)) e.push_back(x);
    if (!khr_bda && !khr_tl) {
        emit(key + ".with-khr: not tried (the device offers neither KHR extension)");
        return;
    }
    auto f2 = vk_struct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    f2.pNext = chain;
    try_device(in, pd, family, key + ".with-khr", e, inst11 ? &f2 : nullptr, khr_bda && bda, khr_tl && timeline,
               sb16 && khr_8 && sb8 && khr_hi && int8 && fp16, !dev12, emit);
}

} // namespace

bool vulkan_report(const Emit & emit) {
    emit("vulkan.report: 1");
    void * lib = nullptr;
    const char * opened = nullptr;
    for (const char * name : {"libvulkan.so.1", "libvulkan.so", "libvulkan.1.dylib", "libMoltenVK.dylib"}) {
        if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)) != nullptr) {
            opened = name;
            break;
        }
    }
    if (!lib) {
        emit("loader: none (no Vulkan loader library could be opened)");
        return false;
    }
    emit(std::string("loader.library: ") + opened);
    Instance in;
    // The process's own resolution first: the loader a backend already uses, or a test's interposer
    // (tests/vulkan_s22_shim.c) - then the library just opened.
    in.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
    if (!in.gipa) in.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!in.gipa) {
        emit("loader.vkGetInstanceProcAddr: NULL");
        return false;
    }
    auto enum_version = in.get<PFN_vkEnumerateInstanceVersion>("vkEnumerateInstanceVersion");
    uint32_t inst_version = VK_API_VERSION_1_0;
    if (enum_version) enum_version(&inst_version);
    emit("loader.version: " + version(inst_version) + (enum_version ? "" : " (no vkEnumerateInstanceVersion: 1.0)"));

    // The instance as ggml makes it: at the loader's version, no layers.
    auto app = vk_struct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "bmoe-vulkan-report";
    app.apiVersion = inst_version;
    auto ici = vk_struct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    ici.pApplicationInfo = &app;
    auto create_instance = in.get<PFN_vkCreateInstance>("vkCreateInstance");
    emit("step: vkCreateInstance");
    if (!create_instance || create_instance(&ici, nullptr, &in.instance) != VK_SUCCESS) {
        emit("instance.created: no");
        return false;
    }
    emit("instance.created: yes (api " + version(inst_version) + ")");
    auto enum_devices = in.get<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    auto destroy_instance = in.get<PFN_vkDestroyInstance>("vkDestroyInstance");
    uint32_t n = 0;
    if (enum_devices) enum_devices(in.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) enum_devices(in.instance, &n, devs.data());
    emit("devices: " + std::to_string(n));
    for (uint32_t i = 0; i < n; ++i)
        report_device(in, inst_version, devs[i], i, emit);
    if (destroy_instance) destroy_instance(in.instance, nullptr);
    emit("vulkan.report: done");
    return true;
}

#endif

} // namespace bmoe::frontend
