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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

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

static PdockerVkPhysicalDevice g_device;
static PdockerVkQueue g_queue;

static bool bridge_available(void) {
    const char *socket_path = getenv("PDOCKER_GPU_QUEUE_SOCKET");
    return socket_path && socket_path[0];
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

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
        VkQueue queue,
        uint32_t submitCount,
        const VkSubmitInfo *pSubmits,
        VkFence fence) {
    (void)queue;
    (void)submitCount;
    (void)pSubmits;
    (void)fence;
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
    (void)queue;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device) {
    (void)device;
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
    MAP_PROC(vkEnumerateDeviceExtensionProperties);
    MAP_PROC(vkEnumerateDeviceLayerProperties);
    MAP_PROC(vkCreateDevice);
    MAP_PROC(vkDestroyDevice);
    MAP_PROC(vkGetDeviceQueue);
    MAP_PROC(vkQueueSubmit);
    MAP_PROC(vkQueueWaitIdle);
    MAP_PROC(vkDeviceWaitIdle);
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
