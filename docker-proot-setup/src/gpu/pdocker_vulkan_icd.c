/*
 * Minimal glibc-facing pdocker Vulkan ICD.
 *
 * This is the standard Vulkan-loader entry point that containers should see.
 * It deliberately does not dlopen Android/Bionic vendor Vulkan libraries from
 * glibc. Real execution is added below this ICD by lowering Vulkan calls into
 * the pdocker GPU command bridge.
 */
#include "pdocker_gpu_abi.h"

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

typedef struct {
    VK_LOADER_DATA loader;
} PdockerVkInstance;

typedef struct {
    VK_LOADER_DATA loader;
} PdockerVkPhysicalDevice;

typedef struct {
    VK_LOADER_DATA loader;
} PdockerVkDevice;

typedef struct {
    VK_LOADER_DATA loader;
} PdockerVkQueue;

typedef struct PdockerVkMemory PdockerVkMemory;
typedef struct PdockerVkBuffer PdockerVkBuffer;
typedef struct PdockerVkDescriptorBinding PdockerVkDescriptorBinding;
typedef struct PdockerVkDescriptorSet PdockerVkDescriptorSet;
typedef struct PdockerVkShaderModule PdockerVkShaderModule;
typedef struct PdockerVkPipeline PdockerVkPipeline;
typedef struct PdockerVkFence PdockerVkFence;

struct PdockerVkMemory {
    size_t size;
    uint32_t memory_type_index;
    VkMemoryPropertyFlags property_flags;
    int fd;
    void *map;
};

struct PdockerVkBuffer {
    size_t size;
    PdockerVkMemory *memory;
    VkDeviceSize memory_offset;
};

struct PdockerVkDescriptorBinding {
    PdockerVkBuffer *buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
};

struct PdockerVkDescriptorSet {
    PdockerVkDescriptorBinding storage_buffers[16];
};

struct PdockerVkShaderModule {
    size_t code_size;
    uint32_t first_word;
};

struct PdockerVkPipeline {
    PdockerVkShaderModule *shader;
    uint32_t local_size_x;
};

struct PdockerVkFence {
    bool signaled;
};

typedef struct {
    VK_LOADER_DATA loader;
    PdockerVkPipeline *pipeline;
    PdockerVkDescriptorSet *set;
    uint32_t dispatch_x;
    bool has_dispatch;
} PdockerVkCommandBuffer;

typedef struct {
    int unused;
} PdockerHandle;

static PdockerVkPhysicalDevice g_device;
static PdockerVkQueue g_queue;

static VkDeviceSize pdocker_vulkan_heap_size(void) {
    const char *env = getenv("PDOCKER_VULKAN_HEAP_BYTES");
    if (env && env[0]) {
        char *end = NULL;
        unsigned long long value = strtoull(env, &end, 10);
        if (end && *end == '\0' && value >= 256ull * 1024ull * 1024ull) {
            return (VkDeviceSize)value;
        }
    }
    return (VkDeviceSize)(8ull * 1024ull * 1024ull * 1024ull);
}

static VkDeviceSize pdocker_vulkan_host_heap_size(void) {
    VkDeviceSize heap = pdocker_vulkan_heap_size();
    VkDeviceSize host_heap = heap / 2;
    const VkDeviceSize min_heap = (VkDeviceSize)(512ull * 1024ull * 1024ull);
    if (host_heap < min_heap) host_heap = min_heap;
    return host_heap;
}

static bool trace_allocations(void) {
    return getenv("PDOCKER_VULKAN_ICD_TRACE_ALLOC") != NULL;
}

static void trace_pnext_chain(const char *prefix, const void *pNext) {
    if (!trace_allocations()) return;
    const VkBaseInStructure *base = (const VkBaseInStructure *)pNext;
    while (base) {
        fprintf(stderr,
                "pdocker-vulkan-icd: %s pnext sType=%d\n",
                prefix,
                (int)base->sType);
        base = base->pNext;
    }
}

static void *pdocker_alloc_handle(size_t size) {
    return calloc(1, size ? size : sizeof(PdockerHandle));
}

static int create_shared_fd(size_t bytes) {
#ifdef __NR_memfd_create
    int memfd = (int)syscall(__NR_memfd_create, "pdocker-vulkan-memory", MFD_CLOEXEC);
    if (memfd >= 0) {
        if (ftruncate(memfd, (off_t)bytes) == 0) return memfd;
        int err = errno;
        close(memfd);
        errno = err;
        return -1;
    }
#endif
    const char *dir = getenv("PDOCKER_GPU_SHARED_DIR");
    if (!dir || !dir[0]) dir = "/tmp";
    char path[512];
    snprintf(path, sizeof(path), "%s/pdocker-vulkan-memory-XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0) return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)bytes) != 0) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

static bool bridge_available(void) {
    const char *socket_path = getenv("PDOCKER_GPU_QUEUE_SOCKET");
    if (socket_path && socket_path[0]) return true;
    return access("/run/pdocker-gpu/pdocker-gpu.sock", F_OK) == 0;
}

static int connect_queue(void) {
    const char *path = getenv("PDOCKER_GPU_QUEUE_SOCKET");
    if (!path || !path[0]) path = "/run/pdocker-gpu/pdocker-gpu.sock";
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -ENAMETOOLONG;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -errno;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int err = errno;
        close(fd);
        return -err;
    }
    return fd;
}

static int send_vector_add_3fd(size_t n, int fd_a, int fd_b, int fd_out) {
    int socket_fd = connect_queue();
    if (socket_fd < 0) return socket_fd;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "VULKAN_VECTOR_ADD_3FD %zu\n", n);
    int fds[3] = { fd_a, fd_b, fd_out };
    char control[CMSG_SPACE(sizeof(fds))];
    struct iovec iov;
    struct msghdr msg;
    memset(control, 0, sizeof(control));
    memset(&iov, 0, sizeof(iov));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = cmd;
    iov.iov_len = strlen(cmd);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fds));
    memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
    msg.msg_controllen = sizeof(control);
    int rc = 0;
    if (sendmsg(socket_fd, &msg, 0) < 0) {
        rc = -errno;
    } else {
        char line[4096];
        size_t off = 0;
        while (off + 1 < sizeof(line)) {
            char ch;
            ssize_t r = read(socket_fd, &ch, 1);
            if (r <= 0) break;
            line[off++] = ch;
            if (ch == '\n') break;
        }
        line[off] = '\0';
        if (getenv("PDOCKER_VULKAN_ICD_DEBUG")) {
            fprintf(stderr, "pdocker-vulkan-icd: bridge response: %s", line);
            if (off == 0 || line[off - 1] != '\n') fprintf(stderr, "\n");
        }
        if (strstr(line, "\"valid\":true") == NULL) rc = -EIO;
    }
    close(socket_fd);
    return rc;
}

static size_t buffer_available(const PdockerVkBuffer *buffer, VkDeviceSize offset) {
    if (!buffer || !buffer->memory) return 0;
    VkDeviceSize absolute = buffer->memory_offset + offset;
    if (absolute > buffer->memory->size) return 0;
    return buffer->memory->size - (size_t)absolute;
}

static void *buffer_ptr(PdockerVkBuffer *buffer, VkDeviceSize offset, VkDeviceSize bytes) {
    if (!buffer || !buffer->memory) return NULL;
    size_t available = buffer_available(buffer, offset);
    if ((size_t)bytes > available) return NULL;
    return (char *)buffer->memory->map + buffer->memory_offset + offset;
}

static size_t descriptor_binding_size(const PdockerVkDescriptorBinding *binding) {
    if (!binding || !binding->buffer) return 0;
    size_t available = buffer_available(binding->buffer, binding->offset);
    if (binding->range == VK_WHOLE_SIZE) return available;
    return (size_t)binding->range < available ? (size_t)binding->range : available;
}

static uint32_t pdocker_api_version(void) {
    return VK_API_VERSION_1_2;
}

static void copy_extension_properties(
        const VkExtensionProperties *available,
        uint32_t available_count,
        uint32_t *pPropertyCount,
        VkExtensionProperties *pProperties) {
    if (!pPropertyCount) return;
    if (!pProperties) {
        *pPropertyCount = available_count;
        return;
    }
    uint32_t count = *pPropertyCount < available_count ? *pPropertyCount : available_count;
    for (uint32_t i = 0; i < count; ++i) pProperties[i] = available[i];
    *pPropertyCount = count;
}

static void fill_physical_device_properties(VkPhysicalDeviceProperties *pProperties) {
    if (!pProperties) return;
    memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion = pdocker_api_version();
    pProperties->driverVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    pProperties->vendorID = 0x5044; /* PD */
    pProperties->deviceID = 0x0001;
    pProperties->deviceType = bridge_available() ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                                                 : VK_PHYSICAL_DEVICE_TYPE_CPU;
    snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
             "pdocker Vulkan bridge (%s)", bridge_available() ? "queue" : "offline");
    pProperties->limits.maxComputeSharedMemorySize = 32768;
    pProperties->limits.maxComputeWorkGroupCount[0] = 65535;
    pProperties->limits.maxComputeWorkGroupCount[1] = 65535;
    pProperties->limits.maxComputeWorkGroupCount[2] = 65535;
    pProperties->limits.maxComputeWorkGroupInvocations = 256;
    pProperties->limits.maxComputeWorkGroupSize[0] = 256;
    pProperties->limits.maxComputeWorkGroupSize[1] = 256;
    pProperties->limits.maxComputeWorkGroupSize[2] = 64;
    pProperties->limits.maxPushConstantsSize = 256;
    pProperties->limits.maxStorageBufferRange = 0xffffffffu;
    pProperties->limits.maxMemoryAllocationCount = 4096;
    pProperties->limits.maxBoundDescriptorSets = 8;
    pProperties->limits.maxPerStageDescriptorStorageBuffers = 64;
    pProperties->limits.maxDescriptorSetStorageBuffers = 64;
    pProperties->limits.minStorageBufferOffsetAlignment = 16;
    pProperties->limits.minUniformBufferOffsetAlignment = 16;
    pProperties->limits.nonCoherentAtomSize = 64;
    pProperties->limits.timestampComputeAndGraphics = VK_FALSE;
}

static void fill_pnext_properties(void *pNext) {
    for (VkBaseOutStructure *cur = (VkBaseOutStructure *)pNext; cur; cur = cur->pNext) {
        switch (cur->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES: {
                VkPhysicalDeviceMaintenance3Properties *p = (VkPhysicalDeviceMaintenance3Properties *)cur;
                p->maxPerSetDescriptors = 1024;
                p->maxMemoryAllocationSize = pdocker_vulkan_heap_size();
                break;
            }
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES: {
                VkPhysicalDeviceMaintenance4Properties *p = (VkPhysicalDeviceMaintenance4Properties *)cur;
                p->maxBufferSize = pdocker_vulkan_heap_size();
                break;
            }
#endif
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
                VkPhysicalDeviceSubgroupProperties *p = (VkPhysicalDeviceSubgroupProperties *)cur;
                p->subgroupSize = 32;
                p->supportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
                p->supportedOperations =
                    VK_SUBGROUP_FEATURE_BASIC_BIT |
                    VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                    VK_SUBGROUP_FEATURE_BALLOT_BIT |
                    VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                    VK_SUBGROUP_FEATURE_VOTE_BIT;
                p->quadOperationsInAllStages = VK_FALSE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES: {
                VkPhysicalDeviceDriverProperties *p = (VkPhysicalDeviceDriverProperties *)cur;
                p->driverID = VK_DRIVER_ID_MESA_LLVMPIPE;
                snprintf(p->driverName, sizeof(p->driverName), "pdocker-vulkan-bridge");
                snprintf(p->driverInfo, sizeof(p->driverInfo), "pdocker neutral Vulkan bridge");
                p->conformanceVersion.major = 1;
                p->conformanceVersion.minor = 2;
                p->conformanceVersion.subminor = 0;
                p->conformanceVersion.patch = 0;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
                VkPhysicalDeviceVulkan11Properties *p = (VkPhysicalDeviceVulkan11Properties *)cur;
                p->subgroupSize = 32;
                p->subgroupSupportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
                p->subgroupSupportedOperations =
                    VK_SUBGROUP_FEATURE_BASIC_BIT |
                    VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                    VK_SUBGROUP_FEATURE_BALLOT_BIT |
                    VK_SUBGROUP_FEATURE_SHUFFLE_BIT |
                    VK_SUBGROUP_FEATURE_VOTE_BIT;
                p->subgroupQuadOperationsInAllStages = VK_FALSE;
                p->maxMultiviewViewCount = 1;
                p->maxMultiviewInstanceIndex = 1;
                p->maxPerSetDescriptors = 1024;
                p->maxMemoryAllocationSize = pdocker_vulkan_heap_size();
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
                VkPhysicalDeviceVulkan12Properties *p = (VkPhysicalDeviceVulkan12Properties *)cur;
                p->driverID = VK_DRIVER_ID_MESA_LLVMPIPE;
                snprintf(p->driverName, sizeof(p->driverName), "pdocker-vulkan-bridge");
                snprintf(p->driverInfo, sizeof(p->driverInfo), "pdocker neutral Vulkan bridge");
                p->conformanceVersion.major = 1;
                p->conformanceVersion.minor = 2;
                p->shaderRoundingModeRTEFloat16 = VK_FALSE;
                p->shaderRoundingModeRTZFloat16 = VK_FALSE;
                break;
            }
            default:
                break;
        }
    }
}

static void fill_physical_device_features(VkPhysicalDeviceFeatures *pFeatures) {
    if (!pFeatures) return;
    memset(pFeatures, 0, sizeof(*pFeatures));
    pFeatures->shaderInt64 = VK_TRUE;
}

static void fill_pnext_features(void *pNext) {
    for (VkBaseOutStructure *cur = (VkBaseOutStructure *)pNext; cur; cur = cur->pNext) {
        switch (cur->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
                VkPhysicalDeviceVulkan11Features *p = (VkPhysicalDeviceVulkan11Features *)cur;
                p->storageBuffer16BitAccess = VK_TRUE;
                p->uniformAndStorageBuffer16BitAccess = VK_TRUE;
                p->storagePushConstant16 = VK_TRUE;
                p->storageInputOutput16 = VK_FALSE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
                VkPhysicalDevice16BitStorageFeatures *p = (VkPhysicalDevice16BitStorageFeatures *)cur;
                p->storageBuffer16BitAccess = VK_TRUE;
                p->uniformAndStorageBuffer16BitAccess = VK_TRUE;
                p->storagePushConstant16 = VK_TRUE;
                p->storageInputOutput16 = VK_FALSE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                VkPhysicalDeviceVulkan12Features *p = (VkPhysicalDeviceVulkan12Features *)cur;
                p->shaderFloat16 = VK_FALSE;
                p->shaderInt8 = VK_FALSE;
                p->bufferDeviceAddress = VK_FALSE;
                p->vulkanMemoryModel = VK_FALSE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
                VkPhysicalDeviceShaderFloat16Int8Features *p = (VkPhysicalDeviceShaderFloat16Int8Features *)cur;
                p->shaderFloat16 = VK_FALSE;
                p->shaderInt8 = VK_FALSE;
                break;
            }
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES: {
                VkPhysicalDeviceMaintenance4Features *p = (VkPhysicalDeviceMaintenance4Features *)cur;
                p->maintenance4 = VK_TRUE;
                break;
            }
#endif
            default:
                break;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    if (!pVersion) return VK_ERROR_INITIALIZATION_FAILED;
    if (*pVersion > 5) *pVersion = 5;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    if (!pApiVersion) return VK_ERROR_INITIALIZATION_FAILED;
    *pApiVersion = pdocker_api_version();
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
        const char *pLayerName,
        uint32_t *pPropertyCount,
        VkExtensionProperties *pProperties) {
    (void)pLayerName;
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProperties) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(
        uint32_t *pPropertyCount,
        VkLayerProperties *pProperties) {
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProperties) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
        const VkInstanceCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkInstance *pInstance) {
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pInstance) return VK_ERROR_INITIALIZATION_FAILED;
    PdockerVkInstance *instance = calloc(1, sizeof(*instance));
    if (!instance) return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic_value(instance);
    *pInstance = (VkInstance)instance;
    set_loader_magic_value(&g_device);
    set_loader_magic_value(&g_queue);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(
        VkInstance instance,
        const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    free((void *)instance);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(
        VkInstance instance,
        uint32_t *pPhysicalDeviceCount,
        VkPhysicalDevice *pPhysicalDevices) {
    (void)instance;
    if (!pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = 1;
        return VK_SUCCESS;
    }
    if (*pPhysicalDeviceCount < 1) return VK_INCOMPLETE;
    pPhysicalDevices[0] = (VkPhysicalDevice)&g_device;
    *pPhysicalDeviceCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties *pProperties) {
    (void)physicalDevice;
    fill_physical_device_properties(pProperties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2 *pProperties) {
    if (!pProperties) return;
    vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
    fill_pnext_properties(pProperties->pNext);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures *pFeatures) {
    (void)physicalDevice;
    fill_physical_device_features(pFeatures);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2 *pFeatures) {
    if (!pFeatures) return;
    (void)physicalDevice;
    fill_physical_device_features(&pFeatures->features);
    fill_pnext_features(pFeatures->pNext);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(
        VkPhysicalDevice physicalDevice,
        VkFormat format,
        VkFormatProperties *pFormatProperties) {
    (void)physicalDevice;
    (void)format;
    if (!pFormatProperties) return;
    memset(pFormatProperties, 0, sizeof(*pFormatProperties));
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
        VkPhysicalDevice physicalDevice,
        VkFormat format,
        VkImageType type,
        VkImageTiling tiling,
        VkImageUsageFlags usage,
        VkImageCreateFlags flags,
        VkImageFormatProperties *pImageFormatProperties) {
    (void)physicalDevice;
    (void)format;
    (void)type;
    (void)tiling;
    (void)usage;
    (void)flags;
    if (!pImageFormatProperties) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(
        VkPhysicalDevice physicalDevice,
        VkFormat format,
        VkImageType type,
        VkSampleCountFlagBits samples,
        VkImageUsageFlags usage,
        VkImageTiling tiling,
        uint32_t *pPropertyCount,
        VkSparseImageFormatProperties *pProperties) {
    (void)physicalDevice;
    (void)format;
    (void)type;
    (void)samples;
    (void)usage;
    (void)tiling;
    if (!pPropertyCount) return;
    if (!pProperties) {
        *pPropertyCount = 0;
        return;
    }
    *pPropertyCount = 0;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(
        VkPhysicalDevice physicalDevice,
        uint32_t *pQueueFamilyPropertyCount,
        VkQueueFamilyProperties *pQueueFamilyProperties) {
    (void)physicalDevice;
    if (!pQueueFamilyPropertyCount) return;
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    if (*pQueueFamilyPropertyCount >= 1) {
        memset(&pQueueFamilyProperties[0], 0, sizeof(pQueueFamilyProperties[0]));
        pQueueFamilyProperties[0].queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        pQueueFamilyProperties[0].queueCount = 2;
        pQueueFamilyProperties[0].timestampValidBits = 0;
        pQueueFamilyProperties[0].minImageTransferGranularity.width = 1;
        pQueueFamilyProperties[0].minImageTransferGranularity.height = 1;
        pQueueFamilyProperties[0].minImageTransferGranularity.depth = 1;
        *pQueueFamilyPropertyCount = 1;
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties2(
        VkPhysicalDevice physicalDevice,
        uint32_t *pQueueFamilyPropertyCount,
        VkQueueFamilyProperties2 *pQueueFamilyProperties) {
    (void)physicalDevice;
    if (!pQueueFamilyPropertyCount) return;
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }
    if (*pQueueFamilyPropertyCount >= 1) {
        memset(&pQueueFamilyProperties[0].queueFamilyProperties, 0, sizeof(pQueueFamilyProperties[0].queueFamilyProperties));
        pQueueFamilyProperties[0].queueFamilyProperties.queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        pQueueFamilyProperties[0].queueFamilyProperties.queueCount = 2;
        pQueueFamilyProperties[0].queueFamilyProperties.minImageTransferGranularity.width = 1;
        pQueueFamilyProperties[0].queueFamilyProperties.minImageTransferGranularity.height = 1;
        pQueueFamilyProperties[0].queueFamilyProperties.minImageTransferGranularity.depth = 1;
        *pQueueFamilyPropertyCount = 1;
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    (void)physicalDevice;
    if (!pMemoryProperties) return;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));
    pMemoryProperties->memoryTypeCount = 2;
    pMemoryProperties->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;
    pMemoryProperties->memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMemoryProperties->memoryTypes[1].heapIndex = 1;
    pMemoryProperties->memoryHeapCount = 2;
    pMemoryProperties->memoryHeaps[0].size = pdocker_vulkan_heap_size();
    pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    pMemoryProperties->memoryHeaps[1].size = pdocker_vulkan_host_heap_size();
    pMemoryProperties->memoryHeaps[1].flags = 0;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    if (!pMemoryProperties) return;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &pMemoryProperties->memoryProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(
        VkDevice device,
        const VkBufferCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkBuffer *pBuffer) {
    (void)device;
    (void)pAllocator;
    if (!pCreateInfo || !pBuffer) return VK_ERROR_INITIALIZATION_FAILED;
    PdockerVkBuffer *buffer = pdocker_alloc_handle(sizeof(*buffer));
    if (!buffer) return VK_ERROR_OUT_OF_HOST_MEMORY;
    buffer->size = (size_t)pCreateInfo->size;
    if (trace_allocations()) {
        fprintf(stderr,
                "pdocker-vulkan-icd: create-buffer size=%zu usage=0x%x sharing=%u\n",
                buffer->size,
                (unsigned)pCreateInfo->usage,
                (unsigned)pCreateInfo->sharingMode);
    }
    *pBuffer = (VkBuffer)buffer;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(
        VkDevice device,
        VkBuffer buffer,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)buffer);
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(
        VkDevice device,
        VkBuffer buffer,
        VkMemoryRequirements *pMemoryRequirements) {
    (void)device;
    if (!pMemoryRequirements) return;
    PdockerVkBuffer *b = (PdockerVkBuffer *)buffer;
    memset(pMemoryRequirements, 0, sizeof(*pMemoryRequirements));
    pMemoryRequirements->size = b ? (VkDeviceSize)b->size : 0;
    pMemoryRequirements->alignment = 16;
    pMemoryRequirements->memoryTypeBits = 0x3;
    if (trace_allocations()) {
        fprintf(stderr,
                "pdocker-vulkan-icd: buffer-requirements size=%llu alignment=%llu typeBits=0x%x\n",
                (unsigned long long)pMemoryRequirements->size,
                (unsigned long long)pMemoryRequirements->alignment,
                (unsigned)pMemoryRequirements->memoryTypeBits);
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(
        VkDevice device,
        const VkBufferMemoryRequirementsInfo2 *pInfo,
        VkMemoryRequirements2 *pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    vkGetBufferMemoryRequirements(device, pInfo->buffer, &pMemoryRequirements->memoryRequirements);
    for (VkBaseOutStructure *base = (VkBaseOutStructure *)pMemoryRequirements->pNext;
         base;
         base = base->pNext) {
        if (base->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            VkMemoryDedicatedRequirements *dedicated = (VkMemoryDedicatedRequirements *)base;
            dedicated->prefersDedicatedAllocation = VK_FALSE;
            dedicated->requiresDedicatedAllocation = VK_FALSE;
            if (trace_allocations()) {
                fprintf(stderr,
                        "pdocker-vulkan-icd: memory-requirements2 dedicated prefers=0 requires=0\n");
            }
        } else if (trace_allocations()) {
            fprintf(stderr,
                    "pdocker-vulkan-icd: memory-requirements2 ignored pnext sType=%d\n",
                    (int)base->sType);
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
        VkDevice device,
        const VkMemoryAllocateInfo *pAllocateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkDeviceMemory *pMemory) {
    (void)device;
    (void)pAllocator;
    if (!pAllocateInfo || !pMemory) return VK_ERROR_INITIALIZATION_FAILED;
    PdockerVkMemory *memory = pdocker_alloc_handle(sizeof(*memory));
    if (!memory) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memory->size = (size_t)pAllocateInfo->allocationSize;
    memory->memory_type_index = pAllocateInfo->memoryTypeIndex;
    memory->property_flags = memory->memory_type_index == 0
        ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    trace_pnext_chain("allocate", pAllocateInfo->pNext);
    if (trace_allocations()) {
        fprintf(stderr,
                "pdocker-vulkan-icd: allocate %zu bytes type=%u flags=0x%x\n",
                memory->size,
                memory->memory_type_index,
                (unsigned)memory->property_flags);
    }
    memory->fd = create_shared_fd(memory->size);
    if (memory->fd < 0) {
        free(memory);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memory->map = mmap(NULL, memory->size, PROT_READ | PROT_WRITE, MAP_SHARED, memory->fd, 0);
    if (memory->map == MAP_FAILED) {
        close(memory->fd);
        free(memory);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    *pMemory = (VkDeviceMemory)memory;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
        VkDevice device,
        VkDeviceMemory memory,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    PdockerVkMemory *m = (PdockerVkMemory *)memory;
    if (!m) return;
    if (m->map && m->map != MAP_FAILED) munmap(m->map, m->size);
    if (m->fd >= 0) close(m->fd);
    free(m);
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(
        VkDevice device,
        VkDeviceMemory memory,
        VkDeviceSize offset,
        VkDeviceSize size,
        VkMemoryMapFlags flags,
        void **ppData) {
    (void)device;
    (void)size;
    (void)flags;
    if (!memory || !ppData) return VK_ERROR_MEMORY_MAP_FAILED;
    PdockerVkMemory *m = (PdockerVkMemory *)memory;
    if ((size_t)offset > m->size) return VK_ERROR_MEMORY_MAP_FAILED;
    if ((m->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        if (trace_allocations()) {
            fprintf(stderr,
                    "pdocker-vulkan-icd: map rejected non-host-visible type=%u allocation=%zu\n",
                    m->memory_type_index,
                    m->size);
        }
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    if (trace_allocations()) {
        fprintf(stderr,
                "pdocker-vulkan-icd: map offset=%llu size=%llu allocation=%zu\n",
                (unsigned long long)offset,
                (unsigned long long)size,
                m->size);
    }
    *ppData = (char *)m->map + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    (void)device;
    (void)memory;
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceMemoryCommitment(
        VkDevice device,
        VkDeviceMemory memory,
        VkDeviceSize *pCommittedMemoryInBytes) {
    (void)device;
    PdockerVkMemory *m = (PdockerVkMemory *)memory;
    if (pCommittedMemoryInBytes) *pCommittedMemoryInBytes = m ? (VkDeviceSize)m->size : 0;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(
        VkDevice device,
        uint32_t memoryRangeCount,
        const VkMappedMemoryRange *pMemoryRanges) {
    (void)device;
    (void)memoryRangeCount;
    (void)pMemoryRanges;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(
        VkDevice device,
        uint32_t memoryRangeCount,
        const VkMappedMemoryRange *pMemoryRanges) {
    (void)device;
    (void)memoryRangeCount;
    (void)pMemoryRanges;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(
        VkDevice device,
        VkBuffer buffer,
        VkDeviceMemory memory,
        VkDeviceSize memoryOffset) {
    (void)device;
    PdockerVkBuffer *b = (PdockerVkBuffer *)buffer;
    if (!b || !memory) return VK_ERROR_INITIALIZATION_FAILED;
    b->memory = (PdockerVkMemory *)memory;
    b->memory_offset = memoryOffset;
    if (trace_allocations()) {
        fprintf(stderr,
                "pdocker-vulkan-icd: bind-buffer buffer_size=%zu memory_size=%zu offset=%llu\n",
                b->size,
                b->memory->size,
                (unsigned long long)memoryOffset);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory2(
        VkDevice device,
        uint32_t bindInfoCount,
        const VkBindBufferMemoryInfo *pBindInfos) {
    for (uint32_t i = 0; i < bindInfoCount; ++i) {
        VkResult rc = vkBindBufferMemory(
            device,
            pBindInfos[i].buffer,
            pBindInfos[i].memory,
            pBindInfos[i].memoryOffset);
        if (rc != VK_SUCCESS) return rc;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice physicalDevice,
        const char *pLayerName,
        uint32_t *pPropertyCount,
        VkExtensionProperties *pProperties) {
    (void)physicalDevice;
    (void)pLayerName;
    const VkExtensionProperties available[] = {
        { VK_KHR_16BIT_STORAGE_EXTENSION_NAME, VK_KHR_16BIT_STORAGE_SPEC_VERSION },
#ifdef VK_KHR_MAINTENANCE_4_EXTENSION_NAME
        { VK_KHR_MAINTENANCE_4_EXTENSION_NAME, VK_KHR_MAINTENANCE_4_SPEC_VERSION },
#endif
    };
    copy_extension_properties(available, (uint32_t)(sizeof(available) / sizeof(available[0])), pPropertyCount, pProperties);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(
        VkPhysicalDevice physicalDevice,
        uint32_t *pPropertyCount,
        VkLayerProperties *pProperties) {
    (void)physicalDevice;
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProperties) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkDevice *pDevice) {
    (void)physicalDevice;
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pDevice) return VK_ERROR_INITIALIZATION_FAILED;
    PdockerVkDevice *device = calloc(1, sizeof(*device));
    if (!device) return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic_value(device);
    *pDevice = (VkDevice)device;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(
        VkDevice device,
        const VkAllocationCallbacks *pAllocator) {
    (void)pAllocator;
    free((void *)device);
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(
        VkDevice device,
        uint32_t queueFamilyIndex,
        uint32_t queueIndex,
        VkQueue *pQueue) {
    (void)device;
    (void)queueFamilyIndex;
    (void)queueIndex;
    if (pQueue) *pQueue = (VkQueue)&g_queue;
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue2(
        VkDevice device,
        const VkDeviceQueueInfo2 *pQueueInfo,
        VkQueue *pQueue) {
    (void)pQueueInfo;
    vkGetDeviceQueue(device, 0, 0, pQueue);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkDescriptorSetLayout *pSetLayout) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pSetLayout) return VK_ERROR_INITIALIZATION_FAILED;
    *pSetLayout = (VkDescriptorSetLayout)pdocker_alloc_handle(sizeof(PdockerHandle));
    return *pSetLayout ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(
        VkDevice device,
        VkDescriptorSetLayout descriptorSetLayout,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)descriptorSetLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(
        VkDevice device,
        const VkPipelineLayoutCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkPipelineLayout *pPipelineLayout) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pPipelineLayout) return VK_ERROR_INITIALIZATION_FAILED;
    *pPipelineLayout = (VkPipelineLayout)pdocker_alloc_handle(sizeof(PdockerHandle));
    return *pPipelineLayout ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(
        VkDevice device,
        VkPipelineLayout pipelineLayout,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)pipelineLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(
        VkDevice device,
        const VkDescriptorPoolCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkDescriptorPool *pDescriptorPool) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pDescriptorPool) return VK_ERROR_INITIALIZATION_FAILED;
    *pDescriptorPool = (VkDescriptorPool)pdocker_alloc_handle(sizeof(PdockerHandle));
    return *pDescriptorPool ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(
        VkDevice device,
        VkDescriptorPool descriptorPool,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)descriptorPool);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(
        VkDevice device,
        VkDescriptorPool descriptorPool,
        VkDescriptorPoolResetFlags flags) {
    (void)device;
    (void)descriptorPool;
    (void)flags;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(
        VkDevice device,
        const VkDescriptorSetAllocateInfo *pAllocateInfo,
        VkDescriptorSet *pDescriptorSets) {
    (void)device;
    if (!pAllocateInfo || !pDescriptorSets) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
        PdockerVkDescriptorSet *set = pdocker_alloc_handle(sizeof(*set));
        if (!set) return VK_ERROR_OUT_OF_HOST_MEMORY;
        pDescriptorSets[i] = (VkDescriptorSet)set;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(
        VkDevice device,
        VkDescriptorPool descriptorPool,
        uint32_t descriptorSetCount,
        const VkDescriptorSet *pDescriptorSets) {
    (void)device;
    (void)descriptorPool;
    for (uint32_t i = 0; i < descriptorSetCount; ++i) {
        free((void *)pDescriptorSets[i]);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(
        VkDevice device,
        uint32_t descriptorWriteCount,
        const VkWriteDescriptorSet *pDescriptorWrites,
        uint32_t descriptorCopyCount,
        const VkCopyDescriptorSet *pDescriptorCopies) {
    (void)device;
    (void)descriptorCopyCount;
    (void)pDescriptorCopies;
    for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
        const VkWriteDescriptorSet *w = &pDescriptorWrites[i];
        PdockerVkDescriptorSet *set = (PdockerVkDescriptorSet *)w->dstSet;
        if (!set || w->descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || !w->pBufferInfo) continue;
        for (uint32_t j = 0; j < w->descriptorCount; ++j) {
            uint32_t binding = w->dstBinding + j;
            if (binding < 16) {
                set->storage_buffers[binding].buffer = (PdockerVkBuffer *)w->pBufferInfo[j].buffer;
                set->storage_buffers[binding].offset = w->pBufferInfo[j].offset;
                set->storage_buffers[binding].range = w->pBufferInfo[j].range;
                if (trace_allocations()) {
                    PdockerVkBuffer *buffer = set->storage_buffers[binding].buffer;
                    fprintf(stderr,
                            "pdocker-vulkan-icd: descriptor storage binding=%u buffer_size=%zu offset=%llu range=%llu effective=%zu\n",
                            binding,
                            buffer ? buffer->size : 0,
                            (unsigned long long)set->storage_buffers[binding].offset,
                            (unsigned long long)set->storage_buffers[binding].range,
                            descriptor_binding_size(&set->storage_buffers[binding]));
                }
            }
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(
        VkDevice device,
        const VkShaderModuleCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkShaderModule *pShaderModule) {
    (void)device;
    (void)pAllocator;
    if (!pCreateInfo || !pShaderModule) return VK_ERROR_INITIALIZATION_FAILED;
    PdockerVkShaderModule *shader = pdocker_alloc_handle(sizeof(*shader));
    if (!shader) return VK_ERROR_OUT_OF_HOST_MEMORY;
    shader->code_size = pCreateInfo->codeSize;
    shader->first_word = (pCreateInfo->pCode && pCreateInfo->codeSize >= sizeof(uint32_t))
        ? pCreateInfo->pCode[0]
        : 0;
    *pShaderModule = (VkShaderModule)shader;
    return *pShaderModule ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(
        VkDevice device,
        VkShaderModule shaderModule,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)shaderModule);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(
        VkDevice device,
        VkPipelineCache pipelineCache,
        uint32_t createInfoCount,
        const VkComputePipelineCreateInfo *pCreateInfos,
        const VkAllocationCallbacks *pAllocator,
        VkPipeline *pPipelines) {
    (void)device;
    (void)pipelineCache;
    (void)pCreateInfos;
    (void)pAllocator;
    if (!pPipelines) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        PdockerVkPipeline *pipeline = pdocker_alloc_handle(sizeof(*pipeline));
        if (!pipeline) return VK_ERROR_OUT_OF_HOST_MEMORY;
        pipeline->shader = (PdockerVkShaderModule *)pCreateInfos[i].stage.module;
        pipeline->local_size_x = 128;
        pPipelines[i] = (VkPipeline)pipeline;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(
        VkDevice device,
        VkPipeline pipeline,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)pipeline);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
        VkDevice device,
        const VkCommandPoolCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkCommandPool *pCommandPool) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    *pCommandPool = (VkCommandPool)pdocker_alloc_handle(sizeof(PdockerHandle));
    return *pCommandPool ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
        VkDevice device,
        VkCommandPool commandPool,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)commandPool);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(
        VkDevice device,
        VkCommandPool commandPool,
        VkCommandPoolResetFlags flags) {
    (void)device;
    (void)commandPool;
    (void)flags;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
        VkDevice device,
        const VkCommandBufferAllocateInfo *pAllocateInfo,
        VkCommandBuffer *pCommandBuffers) {
    (void)device;
    if (!pAllocateInfo || !pCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
        PdockerVkCommandBuffer *cmd = pdocker_alloc_handle(sizeof(*cmd));
        if (!cmd) return VK_ERROR_OUT_OF_HOST_MEMORY;
        set_loader_magic_value(cmd);
        pCommandBuffers[i] = (VkCommandBuffer)cmd;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
        VkDevice device,
        VkCommandPool commandPool,
        uint32_t commandBufferCount,
        const VkCommandBuffer *pCommandBuffers) {
    (void)device;
    (void)commandPool;
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        free((void *)pCommandBuffers[i]);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo *pBeginInfo) {
    (void)pBeginInfo;
    PdockerVkCommandBuffer *cmd = (PdockerVkCommandBuffer *)commandBuffer;
    if (!cmd) return VK_ERROR_INITIALIZATION_FAILED;
    cmd->pipeline = NULL;
    cmd->set = NULL;
    cmd->dispatch_x = 0;
    cmd->has_dispatch = false;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer commandBuffer) {
    return commandBuffer ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags) {
    (void)flags;
    return vkBeginCommandBuffer(commandBuffer, NULL);
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipeline pipeline) {
    (void)pipelineBindPoint;
    PdockerVkCommandBuffer *cmd = (PdockerVkCommandBuffer *)commandBuffer;
    if (cmd) cmd->pipeline = (PdockerVkPipeline *)pipeline;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipelineLayout layout,
        uint32_t firstSet,
        uint32_t descriptorSetCount,
        const VkDescriptorSet *pDescriptorSets,
        uint32_t dynamicOffsetCount,
        const uint32_t *pDynamicOffsets) {
    (void)pipelineBindPoint;
    (void)layout;
    (void)firstSet;
    (void)dynamicOffsetCount;
    (void)pDynamicOffsets;
    PdockerVkCommandBuffer *cmd = (PdockerVkCommandBuffer *)commandBuffer;
    if (cmd && descriptorSetCount > 0 && pDescriptorSets) {
        cmd->set = (PdockerVkDescriptorSet *)pDescriptorSets[0];
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(
        VkCommandBuffer commandBuffer,
        uint32_t groupCountX,
        uint32_t groupCountY,
        uint32_t groupCountZ) {
    (void)groupCountY;
    (void)groupCountZ;
    PdockerVkCommandBuffer *cmd = (PdockerVkCommandBuffer *)commandBuffer;
    if (cmd) {
        cmd->dispatch_x = groupCountX;
        cmd->has_dispatch = true;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(
        VkCommandBuffer commandBuffer,
        VkPipelineLayout layout,
        VkShaderStageFlags stageFlags,
        uint32_t offset,
        uint32_t size,
        const void *pValues) {
    (void)commandBuffer;
    (void)layout;
    (void)stageFlags;
    (void)offset;
    (void)size;
    (void)pValues;
}

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
        VkCommandBuffer commandBuffer,
        VkPipelineStageFlags srcStageMask,
        VkPipelineStageFlags dstStageMask,
        VkDependencyFlags dependencyFlags,
        uint32_t memoryBarrierCount,
        const VkMemoryBarrier *pMemoryBarriers,
        uint32_t bufferMemoryBarrierCount,
        const VkBufferMemoryBarrier *pBufferMemoryBarriers,
        uint32_t imageMemoryBarrierCount,
        const VkImageMemoryBarrier *pImageMemoryBarriers) {
    (void)commandBuffer;
    (void)srcStageMask;
    (void)dstStageMask;
    (void)dependencyFlags;
    (void)memoryBarrierCount;
    (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount;
    (void)pBufferMemoryBarriers;
    (void)imageMemoryBarrierCount;
    (void)pImageMemoryBarriers;
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(
        VkCommandBuffer commandBuffer,
        VkBuffer srcBuffer,
        VkBuffer dstBuffer,
        uint32_t regionCount,
        const VkBufferCopy *pRegions) {
    (void)commandBuffer;
    PdockerVkBuffer *src = (PdockerVkBuffer *)srcBuffer;
    PdockerVkBuffer *dst = (PdockerVkBuffer *)dstBuffer;
    if (!src || !dst || !src->memory || !dst->memory || !pRegions) return;
    for (uint32_t i = 0; i < regionCount; ++i) {
        const VkBufferCopy *r = &pRegions[i];
        void *dst_ptr = buffer_ptr(dst, r->dstOffset, r->size);
        void *src_ptr = buffer_ptr(src, r->srcOffset, r->size);
        if (trace_allocations()) {
            fprintf(stderr,
                    "pdocker-vulkan-icd: copy-buffer src_size=%zu src_mem=%zu src_off=%llu dst_size=%zu dst_mem=%zu dst_off=%llu bytes=%llu ok=%u\n",
                    src->size,
                    src->memory->size,
                    (unsigned long long)r->srcOffset,
                    dst->size,
                    dst->memory->size,
                    (unsigned long long)r->dstOffset,
                    (unsigned long long)r->size,
                    (src_ptr && dst_ptr) ? 1u : 0u);
        }
        if (!src_ptr || !dst_ptr) {
            continue;
        }
        memmove(dst_ptr, src_ptr, (size_t)r->size);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdFillBuffer(
        VkCommandBuffer commandBuffer,
        VkBuffer dstBuffer,
        VkDeviceSize dstOffset,
        VkDeviceSize size,
        uint32_t data) {
    (void)commandBuffer;
    PdockerVkBuffer *dst = (PdockerVkBuffer *)dstBuffer;
    if (!dst || !dst->memory) return;
    size_t available = buffer_available(dst, dstOffset);
    size_t bytes = size == VK_WHOLE_SIZE ? available : (size_t)size;
    if (bytes > available) bytes = available;
    if (trace_allocations()) {
        fprintf(stderr,
                "pdocker-vulkan-icd: fill-buffer dst_size=%zu dst_mem=%zu off=%llu bytes=%zu available=%zu\n",
                dst->size,
                dst->memory->size,
                (unsigned long long)dstOffset,
                bytes,
                available);
    }
    uint32_t *p = (uint32_t *)buffer_ptr(dst, dstOffset, bytes);
    if (!p) return;
    for (size_t i = 0; i < bytes / sizeof(uint32_t); ++i) p[i] = data;
}

VKAPI_ATTR void VKAPI_CALL vkCmdUpdateBuffer(
        VkCommandBuffer commandBuffer,
        VkBuffer dstBuffer,
        VkDeviceSize dstOffset,
        VkDeviceSize dataSize,
        const void *pData) {
    (void)commandBuffer;
    PdockerVkBuffer *dst = (PdockerVkBuffer *)dstBuffer;
    if (!dst || !dst->memory || !pData) return;
    void *dst_ptr = buffer_ptr(dst, dstOffset, dataSize);
    if (trace_allocations()) {
        fprintf(stderr,
                "pdocker-vulkan-icd: update-buffer dst_size=%zu dst_mem=%zu off=%llu bytes=%llu ok=%u\n",
                dst->size,
                dst->memory->size,
                (unsigned long long)dstOffset,
                (unsigned long long)dataSize,
                dst_ptr ? 1u : 0u);
    }
    if (!dst_ptr) return;
    memcpy(dst_ptr, pData, (size_t)dataSize);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
        VkQueue queue,
        uint32_t submitCount,
        const VkSubmitInfo *pSubmits,
        VkFence fence) {
    (void)queue;
    (void)fence;
    for (uint32_t i = 0; i < submitCount; ++i) {
        for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; ++j) {
            PdockerVkCommandBuffer *cmd = (PdockerVkCommandBuffer *)pSubmits[i].pCommandBuffers[j];
            if (!cmd) return VK_ERROR_INITIALIZATION_FAILED;
            if (!cmd->has_dispatch) {
                if (trace_allocations()) {
                    fprintf(stderr, "pdocker-vulkan-icd: queue-submit transfer-only command buffer\n");
                }
                continue;
            }
            if (!cmd || !cmd->set || !cmd->set->storage_buffers[0].buffer ||
                !cmd->set->storage_buffers[1].buffer || !cmd->set->storage_buffers[2].buffer) {
                if (trace_allocations() || getenv("PDOCKER_VULKAN_ICD_DEBUG")) {
                    fprintf(stderr,
                            "pdocker-vulkan-icd: dispatch missing storage buffers set=%p\n",
                            (void *)cmd->set);
                }
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            if (cmd->pipeline && cmd->pipeline->shader && cmd->pipeline->shader->code_size > sizeof(uint32_t)) {
                if (trace_allocations() || getenv("PDOCKER_VULKAN_ICD_DEBUG")) {
                    fprintf(stderr,
                            "pdocker-vulkan-icd: real SPIR-V dispatch is not lowered yet code_size=%zu first_word=0x%08x dispatch_x=%u\n",
                            cmd->pipeline->shader->code_size,
                            cmd->pipeline->shader->first_word,
                            cmd->dispatch_x);
                }
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            PdockerVkBuffer *a = cmd->set->storage_buffers[0].buffer;
            PdockerVkBuffer *b = cmd->set->storage_buffers[1].buffer;
            PdockerVkBuffer *out = cmd->set->storage_buffers[2].buffer;
            if (!a->memory || !b->memory || !out->memory) return VK_ERROR_MEMORY_MAP_FAILED;
            size_t n = descriptor_binding_size(&cmd->set->storage_buffers[0]) / sizeof(float);
            size_t b_n = descriptor_binding_size(&cmd->set->storage_buffers[1]) / sizeof(float);
            size_t out_n = descriptor_binding_size(&cmd->set->storage_buffers[2]) / sizeof(float);
            if (b_n < n) n = b_n;
            if (out_n < n) n = out_n;
            if (cmd->dispatch_x && cmd->pipeline && cmd->pipeline->local_size_x) {
                size_t dispatched = (size_t)cmd->dispatch_x * cmd->pipeline->local_size_x;
                if (dispatched < n) n = dispatched;
            }
            int rc = send_vector_add_3fd(n, a->memory->fd, b->memory->fd, out->memory->fd);
            if (rc != 0) return VK_ERROR_DEVICE_LOST;
        }
    }
    PdockerVkFence *f = (PdockerVkFence *)fence;
    if (f) f->signaled = true;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
    (void)queue;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
    (void)device;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(
        VkDevice device,
        const VkFenceCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkFence *pFence) {
    (void)device;
    (void)pAllocator;
    if (!pFence) return VK_ERROR_INITIALIZATION_FAILED;
    PdockerVkFence *fence = pdocker_alloc_handle(sizeof(*fence));
    if (!fence) return VK_ERROR_OUT_OF_HOST_MEMORY;
    fence->signaled = pCreateInfo && (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT);
    *pFence = (VkFence)fence;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(
        VkDevice device,
        VkFence fence,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(
        VkDevice device,
        uint32_t fenceCount,
        const VkFence *pFences) {
    (void)device;
    for (uint32_t i = 0; i < fenceCount; ++i) {
        PdockerVkFence *fence = (PdockerVkFence *)pFences[i];
        if (fence) fence->signaled = false;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice device, VkFence fence) {
    (void)device;
    PdockerVkFence *f = (PdockerVkFence *)fence;
    return (!f || f->signaled) ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(
        VkDevice device,
        uint32_t fenceCount,
        const VkFence *pFences,
        VkBool32 waitAll,
        uint64_t timeout) {
    (void)device;
    (void)timeout;
    bool any = false;
    for (uint32_t i = 0; i < fenceCount; ++i) {
        PdockerVkFence *fence = (PdockerVkFence *)pFences[i];
        bool signaled = !fence || fence->signaled;
        any = any || signaled;
        if (waitAll && !signaled) return VK_NOT_READY;
    }
    return (!waitAll && fenceCount > 0 && !any) ? VK_NOT_READY : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(
        VkDevice device,
        const VkSemaphoreCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSemaphore *pSemaphore) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pSemaphore) return VK_ERROR_INITIALIZATION_FAILED;
    *pSemaphore = (VkSemaphore)pdocker_alloc_handle(sizeof(PdockerHandle));
    return *pSemaphore ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(
        VkDevice device,
        VkSemaphore semaphore,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)semaphore);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(
        VkDevice device,
        const VkPipelineCacheCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkPipelineCache *pPipelineCache) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pPipelineCache) return VK_ERROR_INITIALIZATION_FAILED;
    *pPipelineCache = (VkPipelineCache)pdocker_alloc_handle(sizeof(PdockerHandle));
    return *pPipelineCache ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(
        VkDevice device,
        VkPipelineCache pipelineCache,
        const VkAllocationCallbacks *pAllocator) {
    (void)device;
    (void)pAllocator;
    free((void *)pipelineCache);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(
        VkDevice device,
        VkPipelineCache pipelineCache,
        size_t *pDataSize,
        void *pData) {
    (void)device;
    (void)pipelineCache;
    if (!pDataSize) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pData) {
        *pDataSize = 0;
        return VK_SUCCESS;
    }
    if (*pDataSize > 0) *pDataSize = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(
        VkDevice device,
        VkPipelineCache dstCache,
        uint32_t srcCacheCount,
        const VkPipelineCache *pSrcCaches) {
    (void)device;
    (void)dstCache;
    (void)srcCacheCount;
    (void)pSrcCaches;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName);

static PFN_vkVoidFunction proc_address(const char *pName) {
    if (!pName) return NULL;
#define MAP_PROC(name) if (strcmp(pName, #name) == 0) return (PFN_vkVoidFunction)name
    MAP_PROC(vkGetInstanceProcAddr);
    MAP_PROC(vkGetDeviceProcAddr);
    MAP_PROC(vkEnumerateInstanceVersion);
    MAP_PROC(vkEnumerateInstanceExtensionProperties);
    MAP_PROC(vkEnumerateInstanceLayerProperties);
    MAP_PROC(vkCreateInstance);
    MAP_PROC(vkDestroyInstance);
    MAP_PROC(vkEnumeratePhysicalDevices);
    MAP_PROC(vkGetPhysicalDeviceProperties);
    MAP_PROC(vkGetPhysicalDeviceProperties2);
    MAP_PROC(vkGetPhysicalDeviceFeatures);
    MAP_PROC(vkGetPhysicalDeviceFeatures2);
    MAP_PROC(vkGetPhysicalDeviceFormatProperties);
    MAP_PROC(vkGetPhysicalDeviceImageFormatProperties);
    MAP_PROC(vkGetPhysicalDeviceSparseImageFormatProperties);
    MAP_PROC(vkGetPhysicalDeviceQueueFamilyProperties);
    MAP_PROC(vkGetPhysicalDeviceQueueFamilyProperties2);
    MAP_PROC(vkGetPhysicalDeviceMemoryProperties);
    MAP_PROC(vkGetPhysicalDeviceMemoryProperties2);
    MAP_PROC(vkEnumerateDeviceExtensionProperties);
    MAP_PROC(vkEnumerateDeviceLayerProperties);
    MAP_PROC(vkCreateDevice);
    MAP_PROC(vkDestroyDevice);
    MAP_PROC(vkGetDeviceQueue);
    MAP_PROC(vkGetDeviceQueue2);
    MAP_PROC(vkCreateBuffer);
    MAP_PROC(vkDestroyBuffer);
    MAP_PROC(vkGetBufferMemoryRequirements);
    MAP_PROC(vkGetBufferMemoryRequirements2);
    MAP_PROC(vkAllocateMemory);
    MAP_PROC(vkFreeMemory);
    MAP_PROC(vkMapMemory);
    MAP_PROC(vkUnmapMemory);
    MAP_PROC(vkGetDeviceMemoryCommitment);
    MAP_PROC(vkFlushMappedMemoryRanges);
    MAP_PROC(vkInvalidateMappedMemoryRanges);
    MAP_PROC(vkBindBufferMemory);
    MAP_PROC(vkBindBufferMemory2);
    MAP_PROC(vkCreateDescriptorSetLayout);
    MAP_PROC(vkDestroyDescriptorSetLayout);
    MAP_PROC(vkCreatePipelineLayout);
    MAP_PROC(vkDestroyPipelineLayout);
    MAP_PROC(vkCreateDescriptorPool);
    MAP_PROC(vkDestroyDescriptorPool);
    MAP_PROC(vkResetDescriptorPool);
    MAP_PROC(vkAllocateDescriptorSets);
    MAP_PROC(vkFreeDescriptorSets);
    MAP_PROC(vkUpdateDescriptorSets);
    MAP_PROC(vkCreateShaderModule);
    MAP_PROC(vkDestroyShaderModule);
    MAP_PROC(vkCreatePipelineCache);
    MAP_PROC(vkDestroyPipelineCache);
    MAP_PROC(vkGetPipelineCacheData);
    MAP_PROC(vkMergePipelineCaches);
    MAP_PROC(vkCreateComputePipelines);
    MAP_PROC(vkDestroyPipeline);
    MAP_PROC(vkCreateCommandPool);
    MAP_PROC(vkDestroyCommandPool);
    MAP_PROC(vkResetCommandPool);
    MAP_PROC(vkAllocateCommandBuffers);
    MAP_PROC(vkFreeCommandBuffers);
    MAP_PROC(vkBeginCommandBuffer);
    MAP_PROC(vkEndCommandBuffer);
    MAP_PROC(vkResetCommandBuffer);
    MAP_PROC(vkCmdBindPipeline);
    MAP_PROC(vkCmdBindDescriptorSets);
    MAP_PROC(vkCmdPushConstants);
    MAP_PROC(vkCmdPipelineBarrier);
    MAP_PROC(vkCmdCopyBuffer);
    MAP_PROC(vkCmdFillBuffer);
    MAP_PROC(vkCmdUpdateBuffer);
    MAP_PROC(vkCmdDispatch);
    MAP_PROC(vkQueueSubmit);
    MAP_PROC(vkQueueWaitIdle);
    MAP_PROC(vkDeviceWaitIdle);
    MAP_PROC(vkCreateFence);
    MAP_PROC(vkDestroyFence);
    MAP_PROC(vkResetFences);
    MAP_PROC(vkGetFenceStatus);
    MAP_PROC(vkWaitForFences);
    MAP_PROC(vkCreateSemaphore);
    MAP_PROC(vkDestroySemaphore);
    MAP_PROC(vk_icdNegotiateLoaderICDInterfaceVersion);
#undef MAP_PROC
    return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    (void)instance;
    return proc_address(pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    (void)device;
    return proc_address(pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    return vkGetInstanceProcAddr(instance, pName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName) {
    (void)instance;
    return proc_address(pName);
}
