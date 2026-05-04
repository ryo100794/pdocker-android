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
typedef struct PdockerVkDescriptorSet PdockerVkDescriptorSet;
typedef struct PdockerVkPipeline PdockerVkPipeline;
typedef struct PdockerVkFence PdockerVkFence;

struct PdockerVkMemory {
    size_t size;
    int fd;
    void *map;
};

struct PdockerVkBuffer {
    size_t size;
    PdockerVkMemory *memory;
    VkDeviceSize memory_offset;
};

struct PdockerVkDescriptorSet {
    PdockerVkBuffer *storage_buffers[16];
};

struct PdockerVkPipeline {
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
} PdockerVkCommandBuffer;

typedef struct {
    int unused;
} PdockerHandle;

static PdockerVkPhysicalDevice g_device;
static PdockerVkQueue g_queue;

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
    return socket_path && socket_path[0];
}

static int connect_queue(void) {
    const char *path = getenv("PDOCKER_GPU_QUEUE_SOCKET");
    if (!path || !path[0]) return -ENOENT;
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
    snprintf(cmd, sizeof(cmd), "VECTOR_ADD_3FD %zu\n", n);
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

static uint32_t pdocker_api_version(void) {
    return VK_MAKE_API_VERSION(0, 1, 1, 0);
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
    if (!pProperties) return;
    memset(pProperties, 0, sizeof(*pProperties));
    pProperties->apiVersion = pdocker_api_version();
    pProperties->driverVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    pProperties->vendorID = 0x5044; /* PD */
    pProperties->deviceID = 0x0001;
    pProperties->deviceType = bridge_available() ? VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                                                 : VK_PHYSICAL_DEVICE_TYPE_CPU;
    snprintf(pProperties->deviceName, sizeof(pProperties->deviceName),
             "pdocker GPU bridge (%s)", bridge_available() ? "queue" : "offline");
    pProperties->limits.maxComputeSharedMemorySize = 32768;
    pProperties->limits.maxComputeWorkGroupCount[0] = 65535;
    pProperties->limits.maxComputeWorkGroupCount[1] = 65535;
    pProperties->limits.maxComputeWorkGroupCount[2] = 65535;
    pProperties->limits.maxComputeWorkGroupInvocations = 256;
    pProperties->limits.maxComputeWorkGroupSize[0] = 256;
    pProperties->limits.maxComputeWorkGroupSize[1] = 1;
    pProperties->limits.maxComputeWorkGroupSize[2] = 1;
    pProperties->limits.timestampComputeAndGraphics = VK_FALSE;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties2 *pProperties) {
    if (!pProperties) return;
    vkGetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures *pFeatures) {
    (void)physicalDevice;
    if (!pFeatures) return;
    memset(pFeatures, 0, sizeof(*pFeatures));
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures2 *pFeatures) {
    if (!pFeatures) return;
    memset(&pFeatures->features, 0, sizeof(pFeatures->features));
    (void)physicalDevice;
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
        pQueueFamilyProperties[0].queueCount = 1;
        pQueueFamilyProperties[0].timestampValidBits = 0;
        pQueueFamilyProperties[0].minImageTransferGranularity.width = 1;
        pQueueFamilyProperties[0].minImageTransferGranularity.height = 1;
        pQueueFamilyProperties[0].minImageTransferGranularity.depth = 1;
        *pQueueFamilyPropertyCount = 1;
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    (void)physicalDevice;
    if (!pMemoryProperties) return;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));
    pMemoryProperties->memoryTypeCount = 1;
    pMemoryProperties->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;
    pMemoryProperties->memoryHeapCount = 1;
    pMemoryProperties->memoryHeaps[0].size = 256u * 1024u * 1024u;
    pMemoryProperties->memoryHeaps[0].flags = 0;
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
    pMemoryRequirements->memoryTypeBits = 1;
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements2(
        VkDevice device,
        const VkBufferMemoryRequirementsInfo2 *pInfo,
        VkMemoryRequirements2 *pMemoryRequirements) {
    if (!pInfo || !pMemoryRequirements) return;
    vkGetBufferMemoryRequirements(device, pInfo->buffer, &pMemoryRequirements->memoryRequirements);
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
    *ppData = (char *)m->map + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    (void)device;
    (void)memory;
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
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pProperties) {
        *pPropertyCount = 0;
        return VK_SUCCESS;
    }
    *pPropertyCount = 0;
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
            if (binding < 16) set->storage_buffers[binding] = (PdockerVkBuffer *)w->pBufferInfo[j].buffer;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(
        VkDevice device,
        const VkShaderModuleCreateInfo *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkShaderModule *pShaderModule) {
    (void)device;
    (void)pCreateInfo;
    (void)pAllocator;
    if (!pShaderModule) return VK_ERROR_INITIALIZATION_FAILED;
    *pShaderModule = (VkShaderModule)pdocker_alloc_handle(sizeof(PdockerHandle));
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
    if (cmd) cmd->dispatch_x = groupCountX;
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
            if (!cmd || !cmd->set || !cmd->set->storage_buffers[0] ||
                !cmd->set->storage_buffers[1] || !cmd->set->storage_buffers[2]) {
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            PdockerVkBuffer *a = cmd->set->storage_buffers[0];
            PdockerVkBuffer *b = cmd->set->storage_buffers[1];
            PdockerVkBuffer *out = cmd->set->storage_buffers[2];
            if (!a->memory || !b->memory || !out->memory) return VK_ERROR_MEMORY_MAP_FAILED;
            size_t n = a->size / sizeof(float);
            if (b->size / sizeof(float) < n) n = b->size / sizeof(float);
            if (out->size / sizeof(float) < n) n = out->size / sizeof(float);
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
