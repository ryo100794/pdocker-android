/*
 * pdocker_gpu_executor.c
 *
 * APK-owned Android/Bionic GPU command executor probe.
 *
 * This process is intentionally not an LLM engine. Container processes keep
 * model loading, tokenization, graph ownership, sampling, and HTTP serving.
 * The executor validates the Android-side GPU command boundary that later
 * backs a glibc-facing pdocker shim/command queue.
 */
#include <EGL/egl.h>
#include <GLES3/gl31.h>
#include <vulkan/vulkan.h>
#include "pdocker_gpu_abi.h"
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

typedef int32_t ocl_int;
typedef uint32_t ocl_uint;
typedef uint64_t ocl_ulong;
typedef uintptr_t ocl_bitfield;
typedef ocl_bitfield ocl_device_type;
typedef ocl_bitfield ocl_mem_flags;
typedef ocl_uint ocl_bool;
typedef intptr_t ocl_context_properties;
typedef intptr_t ocl_queue_properties;

typedef struct _cl_platform_id *ocl_platform_id;
typedef struct _cl_device_id *ocl_device_id;
typedef struct _cl_context *ocl_context;
typedef struct _cl_command_queue *ocl_command_queue;
typedef struct _cl_mem *ocl_mem;
typedef struct _cl_program *ocl_program;
typedef struct _cl_kernel *ocl_kernel;
typedef struct _cl_event *ocl_event;

#define OCL_SUCCESS 0
#define OCL_TRUE 1
#define OCL_DEVICE_TYPE_GPU (1u << 2)
#define OCL_DEVICE_TYPE_DEFAULT (1u << 0)
#define OCL_MEM_READ_WRITE (1u << 0)
#define OCL_MEM_COPY_HOST_PTR (1u << 5)

typedef struct {
    void *lib;
    ocl_platform_id platform;
    ocl_device_id device;
    ocl_context context;
    ocl_command_queue queue;
    ocl_int (*clGetPlatformIDs)(ocl_uint, ocl_platform_id *, ocl_uint *);
    ocl_int (*clGetDeviceIDs)(ocl_platform_id, ocl_device_type, ocl_uint, ocl_device_id *, ocl_uint *);
    ocl_context (*clCreateContext)(const ocl_context_properties *, ocl_uint, const ocl_device_id *, void (*)(const char *, const void *, size_t, void *), void *, ocl_int *);
    ocl_command_queue (*clCreateCommandQueue)(ocl_context, ocl_device_id, ocl_bitfield, ocl_int *);
    ocl_command_queue (*clCreateCommandQueueWithProperties)(ocl_context, ocl_device_id, const ocl_queue_properties *, ocl_int *);
    ocl_mem (*clCreateBuffer)(ocl_context, ocl_mem_flags, size_t, void *, ocl_int *);
    ocl_int (*clReleaseMemObject)(ocl_mem);
    ocl_program (*clCreateProgramWithSource)(ocl_context, ocl_uint, const char **, const size_t *, ocl_int *);
    ocl_int (*clBuildProgram)(ocl_program, ocl_uint, const ocl_device_id *, const char *, void (*)(ocl_program, void *), void *);
    ocl_int (*clGetProgramBuildInfo)(ocl_program, ocl_device_id, ocl_uint, size_t, void *, size_t *);
    ocl_int (*clReleaseProgram)(ocl_program);
    ocl_kernel (*clCreateKernel)(ocl_program, const char *, ocl_int *);
    ocl_int (*clReleaseKernel)(ocl_kernel);
    ocl_int (*clSetKernelArg)(ocl_kernel, ocl_uint, size_t, const void *);
    ocl_int (*clEnqueueWriteBuffer)(ocl_command_queue, ocl_mem, ocl_bool, size_t, size_t, const void *, ocl_uint, const ocl_event *, ocl_event *);
    ocl_int (*clEnqueueReadBuffer)(ocl_command_queue, ocl_mem, ocl_bool, size_t, size_t, void *, ocl_uint, const ocl_event *, ocl_event *);
    ocl_int (*clEnqueueNDRangeKernel)(ocl_command_queue, ocl_kernel, ocl_uint, const size_t *, const size_t *, const size_t *, ocl_uint, const ocl_event *, ocl_event *);
    ocl_int (*clFinish)(ocl_command_queue);
    ocl_int (*clReleaseCommandQueue)(ocl_command_queue);
    ocl_int (*clReleaseContext)(ocl_context);
} OpenClBackend;

typedef enum {
    GPU_API_AUTO = 0,
    GPU_API_VULKAN = 1,
    GPU_API_OPENCL = 2,
} GpuApiAffinity;

static const uint32_t kVectorAddSpv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000002c, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x00060010, 0x00000004,
    0x00000011, 0x00000080, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000008, 0x00000069, 0x00080005, 0x0000000b,
    0x475f6c67, 0x61626f6c, 0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044, 0x00030005, 0x00000012,
    0x0000004f, 0x00040006, 0x00000012, 0x00000000, 0x0000006f, 0x00030005, 0x00000014, 0x00000000,
    0x00030005, 0x00000019, 0x00000041, 0x00040006, 0x00000019, 0x00000000, 0x00000061, 0x00030005,
    0x0000001b, 0x00000000, 0x00030005, 0x00000021, 0x00000042, 0x00040006, 0x00000021, 0x00000000,
    0x00000062, 0x00030005, 0x00000023, 0x00000000, 0x00040047, 0x0000000b, 0x0000000b, 0x0000001c,
    0x00040047, 0x00000011, 0x00000006, 0x00000004, 0x00030047, 0x00000012, 0x00000003, 0x00040048,
    0x00000012, 0x00000000, 0x00000019, 0x00050048, 0x00000012, 0x00000000, 0x00000023, 0x00000000,
    0x00030047, 0x00000014, 0x00000019, 0x00040047, 0x00000014, 0x00000021, 0x00000002, 0x00040047,
    0x00000014, 0x00000022, 0x00000000, 0x00040047, 0x00000018, 0x00000006, 0x00000004, 0x00030047,
    0x00000019, 0x00000003, 0x00040048, 0x00000019, 0x00000000, 0x00000018, 0x00050048, 0x00000019,
    0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x0000001b, 0x00000018, 0x00040047, 0x0000001b,
    0x00000021, 0x00000000, 0x00040047, 0x0000001b, 0x00000022, 0x00000000, 0x00040047, 0x00000020,
    0x00000006, 0x00000004, 0x00030047, 0x00000021, 0x00000003, 0x00040048, 0x00000021, 0x00000000,
    0x00000018, 0x00050048, 0x00000021, 0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000023,
    0x00000018, 0x00040047, 0x00000023, 0x00000021, 0x00000001, 0x00040047, 0x00000023, 0x00000022,
    0x00000000, 0x00040047, 0x0000002b, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040020, 0x00000007,
    0x00000007, 0x00000006, 0x00040017, 0x00000009, 0x00000006, 0x00000003, 0x00040020, 0x0000000a,
    0x00000001, 0x00000009, 0x0004003b, 0x0000000a, 0x0000000b, 0x00000001, 0x0004002b, 0x00000006,
    0x0000000c, 0x00000000, 0x00040020, 0x0000000d, 0x00000001, 0x00000006, 0x00030016, 0x00000010,
    0x00000020, 0x0003001d, 0x00000011, 0x00000010, 0x0003001e, 0x00000012, 0x00000011, 0x00040020,
    0x00000013, 0x00000002, 0x00000012, 0x0004003b, 0x00000013, 0x00000014, 0x00000002, 0x00040015,
    0x00000015, 0x00000020, 0x00000001, 0x0004002b, 0x00000015, 0x00000016, 0x00000000, 0x0003001d,
    0x00000018, 0x00000010, 0x0003001e, 0x00000019, 0x00000018, 0x00040020, 0x0000001a, 0x00000002,
    0x00000019, 0x0004003b, 0x0000001a, 0x0000001b, 0x00000002, 0x00040020, 0x0000001d, 0x00000002,
    0x00000010, 0x0003001d, 0x00000020, 0x00000010, 0x0003001e, 0x00000021, 0x00000020, 0x00040020,
    0x00000022, 0x00000002, 0x00000021, 0x0004003b, 0x00000022, 0x00000023, 0x00000002, 0x0004002b,
    0x00000006, 0x00000029, 0x00000080, 0x0004002b, 0x00000006, 0x0000002a, 0x00000001, 0x0006002c,
    0x00000009, 0x0000002b, 0x00000029, 0x0000002a, 0x0000002a, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000007, 0x00000008, 0x00000007,
    0x00050041, 0x0000000d, 0x0000000e, 0x0000000b, 0x0000000c, 0x0004003d, 0x00000006, 0x0000000f,
    0x0000000e, 0x0003003e, 0x00000008, 0x0000000f, 0x0004003d, 0x00000006, 0x00000017, 0x00000008,
    0x0004003d, 0x00000006, 0x0000001c, 0x00000008, 0x00060041, 0x0000001d, 0x0000001e, 0x0000001b,
    0x00000016, 0x0000001c, 0x0004003d, 0x00000010, 0x0000001f, 0x0000001e, 0x0004003d, 0x00000006,
    0x00000024, 0x00000008, 0x00060041, 0x0000001d, 0x00000025, 0x00000023, 0x00000016, 0x00000024,
    0x0004003d, 0x00000010, 0x00000026, 0x00000025, 0x00050081, 0x00000010, 0x00000027, 0x0000001f,
    0x00000026, 0x00060041, 0x0000001d, 0x00000028, 0x00000014, 0x00000016, 0x00000017, 0x0003003e,
    0x00000028, 0x00000027, 0x000100fd, 0x00010038,
};

static FILE *g_json_out = NULL;

static FILE *json_out(void) {
    return g_json_out ? g_json_out : stdout;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void json_fail(const char *stage, const char *message) {
    fprintf(json_out(),
            "{\"executor\":\"pdocker-gpu-executor\",\"api\":\"%s\",\"abi_version\":\"%s\","
            "\"role\":\"%s\",\"llm_engine\":\"%s\",\"device_independent\":true,"
            "\"backend_impl\":\"gles31_compute\","
            "\"backend_affinity\":\"fallback\","
            "\"valid\":false,\"stage\":\"%s\",\"error\":\"%s\"}\n",
            PDOCKER_GPU_COMMAND_API, PDOCKER_GPU_ABI_VERSION,
            PDOCKER_GPU_EXECUTOR_ROLE, PDOCKER_GPU_LLM_ENGINE_LOCATION,
            stage, message ? message : "unknown");
    fflush(json_out());
}

static void fill_inputs(float *a, float *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        a[i] = (float)i * 0.25f;
        b[i] = 1.0f - (float)i * 0.125f;
    }
}

static GLuint compile_shader(const char *src) {
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei len = 0;
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), &len, log);
        fprintf(stderr, "shader compile failed: %.*s\n", (int)len, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint shader) {
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei len = 0;
        glGetProgramInfoLog(program, (GLsizei)sizeof(log), &len, log);
        fprintf(stderr, "program link failed: %.*s\n", (int)len, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static GLuint make_ssbo(GLuint binding, const void *data, size_t bytes, GLenum usage) {
    GLuint id = 0;
    glGenBuffers(1, &id);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, id);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, data, usage);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, id);
    return id;
}

static void *load_symbol(void *lib, const char *name) {
    void *sym = dlsym(lib, name);
    if (!sym) fprintf(stderr, "pdocker-gpu-executor: OpenCL symbol missing: %s\n", name);
    return sym;
}

static int load_opencl_backend(OpenClBackend *cl) {
    memset(cl, 0, sizeof(*cl));
    const char *last_error = "not attempted";
    const char *env_path = getenv("PDOCKER_ANDROID_OPENCL_LIBRARY");
    const char *paths[] = {
        env_path && env_path[0] ? env_path : NULL,
        "libOpenCL.so",
        "/vendor/lib64/libOpenCL.so",
        "/system/vendor/lib64/libOpenCL.so",
        "/system/lib64/libOpenCL.so",
        "/vendor/lib64/egl/libOpenCL.so",
        NULL,
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (!paths[i] || !paths[i][0]) continue;
        cl->lib = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
        if (cl->lib) break;
        last_error = dlerror();
    }
    if (!cl->lib) {
        fprintf(stderr, "pdocker-gpu-executor: Android OpenCL dlopen failed: %s\n", last_error ? last_error : "unknown");
        return -1;
    }

#define LOAD_OCL(name) do { \
        cl->name = (void *)load_symbol(cl->lib, #name); \
        if (!cl->name) return -2; \
    } while (0)
    LOAD_OCL(clGetPlatformIDs);
    LOAD_OCL(clGetDeviceIDs);
    LOAD_OCL(clCreateContext);
    cl->clCreateCommandQueueWithProperties = (void *)dlsym(cl->lib, "clCreateCommandQueueWithProperties");
    LOAD_OCL(clCreateCommandQueue);
    LOAD_OCL(clCreateBuffer);
    LOAD_OCL(clReleaseMemObject);
    LOAD_OCL(clCreateProgramWithSource);
    LOAD_OCL(clBuildProgram);
    cl->clGetProgramBuildInfo = (void *)dlsym(cl->lib, "clGetProgramBuildInfo");
    LOAD_OCL(clReleaseProgram);
    LOAD_OCL(clCreateKernel);
    LOAD_OCL(clReleaseKernel);
    LOAD_OCL(clSetKernelArg);
    LOAD_OCL(clEnqueueWriteBuffer);
    LOAD_OCL(clEnqueueReadBuffer);
    LOAD_OCL(clEnqueueNDRangeKernel);
    LOAD_OCL(clFinish);
    LOAD_OCL(clReleaseCommandQueue);
    LOAD_OCL(clReleaseContext);
#undef LOAD_OCL

    ocl_int err = OCL_SUCCESS;
    if (cl->clGetPlatformIDs(1, &cl->platform, NULL) != OCL_SUCCESS || !cl->platform) return -3;
    if (cl->clGetDeviceIDs(cl->platform, OCL_DEVICE_TYPE_GPU, 1, &cl->device, NULL) != OCL_SUCCESS || !cl->device) {
        if (cl->clGetDeviceIDs(cl->platform, OCL_DEVICE_TYPE_DEFAULT, 1, &cl->device, NULL) != OCL_SUCCESS || !cl->device) return -4;
    }
    cl->context = cl->clCreateContext(NULL, 1, &cl->device, NULL, NULL, &err);
    if (err != OCL_SUCCESS || !cl->context) return -5;
    if (cl->clCreateCommandQueueWithProperties) {
        cl->queue = cl->clCreateCommandQueueWithProperties(cl->context, cl->device, NULL, &err);
    } else {
        cl->queue = cl->clCreateCommandQueue(cl->context, cl->device, 0, &err);
    }
    if (err != OCL_SUCCESS || !cl->queue) return -6;
    return 0;
}

static void close_opencl_backend(OpenClBackend *cl) {
    if (!cl) return;
    if (cl->queue && cl->clReleaseCommandQueue) cl->clReleaseCommandQueue(cl->queue);
    if (cl->context && cl->clReleaseContext) cl->clReleaseContext(cl->context);
    if (cl->lib) dlclose(cl->lib);
    memset(cl, 0, sizeof(*cl));
}

static uint32_t find_vulkan_memory_type(VkPhysicalDevice physical_device, uint32_t type_bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    return UINT32_MAX;
}

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *map;
} VulkanVectorBuffer;

static int create_vulkan_vector_buffer(VkPhysicalDevice physical_device, VkDevice device, size_t bytes, const void *initial, VulkanVectorBuffer *out) {
    memset(out, 0, sizeof(*out));
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult rc = vkCreateBuffer(device, &bci, NULL, &out->buffer);
    if (rc != VK_SUCCESS) return -10;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, out->buffer, &req);
    uint32_t memory_type = find_vulkan_memory_type(
        physical_device,
        req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type == UINT32_MAX) return -11;
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = memory_type,
    };
    rc = vkAllocateMemory(device, &mai, NULL, &out->memory);
    if (rc != VK_SUCCESS) return -12;
    rc = vkBindBufferMemory(device, out->buffer, out->memory, 0);
    if (rc != VK_SUCCESS) return -13;
    rc = vkMapMemory(device, out->memory, 0, (VkDeviceSize)bytes, 0, &out->map);
    if (rc != VK_SUCCESS || !out->map) return -14;
    if (initial) memcpy(out->map, initial, bytes);
    return 0;
}

static void destroy_vulkan_vector_buffer(VkDevice device, VulkanVectorBuffer *buf) {
    if (!buf) return;
    if (buf->map) vkUnmapMemory(device, buf->memory);
    if (buf->buffer) vkDestroyBuffer(device, buf->buffer, NULL);
    if (buf->memory) vkFreeMemory(device, buf->memory, NULL);
    memset(buf, 0, sizeof(*buf));
}

static int run_vector_add_arrays_vulkan(const float *a, const float *b, float *out, size_t n, const char *transport) {
    double init_start = now_ms();
    const char *fail_stage = "start";
    VkResult fail_result = VK_SUCCESS;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VulkanVectorBuffer buffers[3];
    memset(buffers, 0, sizeof(buffers));

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "pdocker-gpu-executor",
        .apiVersion = VK_API_VERSION_1_0,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkResult rc = vkCreateInstance(&ici, NULL, &instance);
    if (rc != VK_SUCCESS) {
        fprintf(stderr, "pdocker-gpu-executor: Vulkan stage=create-instance rc=%d\n", rc);
        return -20;
    }

    uint32_t physical_count = 0;
    fail_stage = "enumerate-physical-count";
    rc = vkEnumeratePhysicalDevices(instance, &physical_count, NULL);
    if (rc != VK_SUCCESS || physical_count == 0) { fail_result = rc; goto fail; }
    VkPhysicalDevice physical_devices[8];
    if (physical_count > 8) physical_count = 8;
    fail_stage = "enumerate-physical";
    rc = vkEnumeratePhysicalDevices(instance, &physical_count, physical_devices);
    if (rc != VK_SUCCESS || physical_count == 0) { fail_result = rc; goto fail; }
    physical_device = physical_devices[0];

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, NULL);
    if (family_count == 0) { fail_stage = "queue-family-count"; goto fail; }
    VkQueueFamilyProperties families[16];
    if (family_count > 16) family_count = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families);
    uint32_t queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            queue_family = i;
            break;
        }
    }
    if (queue_family == UINT32_MAX) { fail_stage = "queue-family-compute"; goto fail; }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    fail_stage = "create-device";
    rc = vkCreateDevice(physical_device, &dci, NULL, &device);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    double init_ms = now_ms() - init_start;

    const size_t bytes = n * sizeof(float);
    double upload_start = now_ms();
    fail_stage = "create-buffer-a";
    if (create_vulkan_vector_buffer(physical_device, device, bytes, a, &buffers[0]) != 0) goto fail;
    fail_stage = "create-buffer-b";
    if (create_vulkan_vector_buffer(physical_device, device, bytes, b, &buffers[1]) != 0) goto fail;
    fail_stage = "create-buffer-out";
    if (create_vulkan_vector_buffer(physical_device, device, bytes, NULL, &buffers[2]) != 0) goto fail;
    double upload_ms = now_ms() - upload_start;

    double compile_start = now_ms();
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(kVectorAddSpv),
        .pCode = kVectorAddSpv,
    };
    fail_stage = "create-shader-module";
    rc = vkCreateShaderModule(device, &smci, NULL, &shader);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    VkDescriptorSetLayoutBinding bindings[3];
    memset(bindings, 0, sizeof(bindings));
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = bindings,
    };
    fail_stage = "create-descriptor-set-layout";
    rc = vkCreateDescriptorSetLayout(device, &dslci, NULL, &set_layout);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
    };
    fail_stage = "create-pipeline-layout";
    rc = vkCreatePipelineLayout(device, &plci, NULL, &pipeline_layout);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main",
        },
        .layout = pipeline_layout,
    };
    fail_stage = "create-compute-pipeline";
    rc = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    double compile_ms = now_ms() - compile_start;

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 3,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    fail_stage = "create-descriptor-pool";
    rc = vkCreateDescriptorPool(device, &dpci, NULL, &descriptor_pool);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    };
    fail_stage = "allocate-descriptor-set";
    rc = vkAllocateDescriptorSets(device, &dsai, &descriptor_set);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    VkDescriptorBufferInfo infos[3];
    VkWriteDescriptorSet writes[3];
    memset(writes, 0, sizeof(writes));
    for (uint32_t i = 0; i < 3; ++i) {
        infos[i].buffer = buffers[i].buffer;
        infos[i].offset = 0;
        infos[i].range = (VkDeviceSize)bytes;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptor_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);

    VkCommandPoolCreateInfo cpoci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queue_family,
    };
    fail_stage = "create-command-pool";
    rc = vkCreateCommandPool(device, &cpoci, NULL, &command_pool);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    fail_stage = "allocate-command-buffer";
    rc = vkAllocateCommandBuffers(device, &cbai, &command_buffer);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }

    double dispatch_start = now_ms();
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    fail_stage = "begin-command-buffer";
    rc = vkBeginCommandBuffer(command_buffer, &cbi);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command_buffer, (uint32_t)((n + 127) / 128), 1, 1);
    fail_stage = "end-command-buffer";
    rc = vkEndCommandBuffer(command_buffer);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fail_stage = "create-fence";
    rc = vkCreateFence(device, &fci, NULL, &fence);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
    };
    fail_stage = "queue-submit";
    rc = vkQueueSubmit(queue, 1, &submit, fence);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    fail_stage = "wait-fence";
    rc = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (rc != VK_SUCCESS) { fail_result = rc; goto fail; }
    double dispatch_ms = now_ms() - dispatch_start;

    double download_start = now_ms();
    memcpy(out, buffers[2].map, bytes);
    double download_ms = now_ms() - download_start;

    double max_err = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double e = fabs((double)out[i] - (double)(a[i] + b[i]));
        if (e > max_err) max_err = e;
    }
    const int valid = max_err <= 0.0001;
    fprintf(json_out(),
            "{\"executor\":\"pdocker-gpu-executor\",\"api\":\"%s\",\"abi_version\":\"%s\","
            "\"role\":\"%s\",\"llm_engine\":\"%s\",\"device_independent\":true,"
            "\"backend_impl\":\"android_vulkan\",\"backend_affinity\":\"same-api\",\"transport\":\"%s\","
            "\"kernel\":\"vector_add\",\"problem_size\":\"n=%zu\","
            "\"init_ms\":%.4f,\"compile_ms\":%.4f,\"upload_ms\":%.4f,"
            "\"dispatch_ms\":%.4f,\"download_ms\":%.4f,\"total_ms\":%.4f,"
            "\"max_abs_error\":%.8f,\"valid\":%s}\n",
            PDOCKER_GPU_COMMAND_API, PDOCKER_GPU_ABI_VERSION,
            PDOCKER_GPU_EXECUTOR_ROLE, PDOCKER_GPU_LLM_ENGINE_LOCATION,
            transport ? transport : "vulkan-local-process-buffer",
            n, init_ms, compile_ms, upload_ms, dispatch_ms, download_ms,
            init_ms + compile_ms + upload_ms + dispatch_ms + download_ms, max_err,
            valid ? "true" : "false");
    fflush(json_out());

    if (fence) vkDestroyFence(device, fence, NULL);
    if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (shader) vkDestroyShaderModule(device, shader, NULL);
    destroy_vulkan_vector_buffer(device, &buffers[0]);
    destroy_vulkan_vector_buffer(device, &buffers[1]);
    destroy_vulkan_vector_buffer(device, &buffers[2]);
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    return valid ? 0 : 6;

fail:
    fprintf(stderr, "pdocker-gpu-executor: Vulkan vector_add failed stage=%s rc=%d\n", fail_stage, fail_result);
    if (fence) vkDestroyFence(device, fence, NULL);
    if (command_pool) vkDestroyCommandPool(device, command_pool, NULL);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    if (pipeline) vkDestroyPipeline(device, pipeline, NULL);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    if (shader) vkDestroyShaderModule(device, shader, NULL);
    if (device) {
        destroy_vulkan_vector_buffer(device, &buffers[0]);
        destroy_vulkan_vector_buffer(device, &buffers[1]);
        destroy_vulkan_vector_buffer(device, &buffers[2]);
        vkDestroyDevice(device, NULL);
    }
    if (instance) vkDestroyInstance(instance, NULL);
    return -21;
}

static int run_vector_add_arrays_opencl(const float *a, const float *b, float *out, size_t n, const char *transport) {
    OpenClBackend cl;
    double init_start = now_ms();
    int load_rc = load_opencl_backend(&cl);
    if (load_rc != 0) return load_rc;
    double init_ms = now_ms() - init_start;

    const size_t bytes = n * sizeof(float);
    ocl_int err = OCL_SUCCESS;
    ocl_mem buf_a = NULL;
    ocl_mem buf_b = NULL;
    ocl_mem buf_o = NULL;
    ocl_program program = NULL;
    ocl_kernel kernel = NULL;
    double upload_start = now_ms();
    buf_a = cl.clCreateBuffer(cl.context, OCL_MEM_READ_WRITE | OCL_MEM_COPY_HOST_PTR, bytes, (void *)a, &err);
    if (err != OCL_SUCCESS || !buf_a) goto fail;
    buf_b = cl.clCreateBuffer(cl.context, OCL_MEM_READ_WRITE | OCL_MEM_COPY_HOST_PTR, bytes, (void *)b, &err);
    if (err != OCL_SUCCESS || !buf_b) goto fail;
    buf_o = cl.clCreateBuffer(cl.context, OCL_MEM_READ_WRITE, bytes, NULL, &err);
    if (err != OCL_SUCCESS || !buf_o) goto fail;
    double upload_ms = now_ms() - upload_start;

    double compile_start = now_ms();
    const char *src =
        "__kernel void pdocker_vector_add(__global const float *a, __global const float *b, __global float *out, const uint n) {\n"
        "  size_t i = get_global_id(0);\n"
        "  if (i < n) out[i] = a[i] + b[i];\n"
        "}\n";
    program = cl.clCreateProgramWithSource(cl.context, 1, &src, NULL, &err);
    if (err != OCL_SUCCESS || !program) goto fail;
    err = cl.clBuildProgram(program, 1, &cl.device, "", NULL, NULL);
    if (err != OCL_SUCCESS) {
        if (cl.clGetProgramBuildInfo) {
            char log[1024];
            size_t log_size = 0;
            if (cl.clGetProgramBuildInfo(program, cl.device, 0x1183, sizeof(log), log, &log_size) == OCL_SUCCESS) {
                fprintf(stderr, "pdocker-gpu-executor: OpenCL build failed: %.*s\n", (int)(log_size < sizeof(log) ? log_size : sizeof(log)), log);
            }
        }
        goto fail_program;
    }
    kernel = cl.clCreateKernel(program, "pdocker_vector_add", &err);
    if (err != OCL_SUCCESS || !kernel) goto fail_program;
    double compile_ms = now_ms() - compile_start;

    double dispatch_start = now_ms();
    ocl_uint count = (ocl_uint)n;
    cl.clSetKernelArg(kernel, 0, sizeof(buf_a), &buf_a);
    cl.clSetKernelArg(kernel, 1, sizeof(buf_b), &buf_b);
    cl.clSetKernelArg(kernel, 2, sizeof(buf_o), &buf_o);
    cl.clSetKernelArg(kernel, 3, sizeof(count), &count);
    size_t global = n;
    err = cl.clEnqueueNDRangeKernel(cl.queue, kernel, 1, NULL, &global, NULL, 0, NULL, NULL);
    if (err != OCL_SUCCESS) goto fail_kernel;
    cl.clFinish(cl.queue);
    double dispatch_ms = now_ms() - dispatch_start;

    double download_start = now_ms();
    err = cl.clEnqueueReadBuffer(cl.queue, buf_o, OCL_TRUE, 0, bytes, out, 0, NULL, NULL);
    if (err != OCL_SUCCESS) goto fail_kernel;
    cl.clFinish(cl.queue);
    double download_ms = now_ms() - download_start;

    double max_err = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double e = fabs((double)out[i] - (double)(a[i] + b[i]));
        if (e > max_err) max_err = e;
    }
    const int valid = max_err <= 0.0001;
    fprintf(json_out(),
            "{\"executor\":\"pdocker-gpu-executor\",\"api\":\"%s\",\"abi_version\":\"%s\","
            "\"role\":\"%s\",\"llm_engine\":\"%s\",\"device_independent\":true,"
            "\"backend_impl\":\"android_opencl\",\"backend_affinity\":\"same-api\",\"transport\":\"%s\","
            "\"kernel\":\"vector_add\",\"problem_size\":\"n=%zu\","
            "\"init_ms\":%.4f,\"compile_ms\":%.4f,\"upload_ms\":%.4f,"
            "\"dispatch_ms\":%.4f,\"download_ms\":%.4f,\"total_ms\":%.4f,"
            "\"max_abs_error\":%.8f,\"valid\":%s}\n",
            PDOCKER_GPU_COMMAND_API, PDOCKER_GPU_ABI_VERSION,
            PDOCKER_GPU_EXECUTOR_ROLE, PDOCKER_GPU_LLM_ENGINE_LOCATION,
            transport ? transport : "opencl-local-process-buffer",
            n, init_ms, compile_ms, upload_ms, dispatch_ms, download_ms,
            init_ms + compile_ms + upload_ms + dispatch_ms + download_ms, max_err,
            valid ? "true" : "false");
    fflush(json_out());
    cl.clReleaseKernel(kernel);
    cl.clReleaseProgram(program);
    cl.clReleaseMemObject(buf_a);
    cl.clReleaseMemObject(buf_b);
    cl.clReleaseMemObject(buf_o);
    close_opencl_backend(&cl);
    return valid ? 0 : 6;

fail_kernel:
    if (kernel) cl.clReleaseKernel(kernel);
fail_program:
    if (program) cl.clReleaseProgram(program);
fail:
    if (buf_a) cl.clReleaseMemObject(buf_a);
    if (buf_b) cl.clReleaseMemObject(buf_b);
    if (buf_o) cl.clReleaseMemObject(buf_o);
    close_opencl_backend(&cl);
    return -7;
}

typedef struct {
    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
} GpuContext;

static int init_gpu_context(GpuContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ctx->display == EGL_NO_DISPLAY) {
        json_fail("egl", "eglGetDisplay failed");
        return 10;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(ctx->display, &major, &minor)) {
        json_fail("egl", "eglInitialize failed");
        return 11;
    }
    const EGLint attrs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(ctx->display, attrs, &config, 1, &count) || count <= 0) {
        eglTerminate(ctx->display);
        json_fail("egl", "OpenGL ES 3 config unavailable");
        return 12;
    }
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    ctx->context = eglCreateContext(ctx->display, config, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx->context == EGL_NO_CONTEXT) {
        eglTerminate(ctx->display);
        json_fail("egl", "eglCreateContext failed");
        return 13;
    }
    const EGLint surf_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    ctx->surface = eglCreatePbufferSurface(ctx->display, config, surf_attrs);
    if (ctx->surface == EGL_NO_SURFACE) {
        eglDestroyContext(ctx->display, ctx->context);
        eglTerminate(ctx->display);
        json_fail("egl", "eglCreatePbufferSurface failed");
        return 14;
    }
    if (!eglMakeCurrent(ctx->display, ctx->surface, ctx->surface, ctx->context)) {
        eglDestroySurface(ctx->display, ctx->surface);
        eglDestroyContext(ctx->display, ctx->context);
        eglTerminate(ctx->display);
        json_fail("egl", "eglMakeCurrent failed");
        return 15;
    }
    return 0;
}

static void destroy_gpu_context(GpuContext *ctx) {
    if (!ctx || !ctx->display) return;
    eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (ctx->surface) eglDestroySurface(ctx->display, ctx->surface);
    if (ctx->context) eglDestroyContext(ctx->display, ctx->context);
    eglTerminate(ctx->display);
    memset(ctx, 0, sizeof(*ctx));
}

static int run_vector_add_arrays(const float *a, const float *b, float *out, size_t n, const char *transport) {
    const size_t bytes = n * sizeof(float);

    double compile_start = now_ms();
    const char *src =
        "#version 310 es\n"
        "layout(local_size_x = 128) in;\n"
        "layout(std430, binding = 0) readonly buffer A { float a[]; };\n"
        "layout(std430, binding = 1) readonly buffer B { float b[]; };\n"
        "layout(std430, binding = 2) writeonly buffer O { float o[]; };\n"
        "uniform uint u_count;\n"
        "void main() {\n"
        "  uint i = gl_GlobalInvocationID.x;\n"
        "  if (i < u_count) o[i] = a[i] + b[i];\n"
        "}\n";
    GLuint shader = compile_shader(src);
    if (!shader) {
        json_fail("compile", "compute shader compile failed");
        return 3;
    }
    GLuint program = link_program(shader);
    glDeleteShader(shader);
    if (!program) {
        json_fail("link", "compute program link failed");
        return 4;
    }
    double compile_ms = now_ms() - compile_start;

    double upload_start = now_ms();
    GLuint buf_a = make_ssbo(0, a, bytes, GL_STATIC_DRAW);
    GLuint buf_b = make_ssbo(1, b, bytes, GL_STATIC_DRAW);
    GLuint buf_o = make_ssbo(2, NULL, bytes, GL_DYNAMIC_READ);
    glFinish();
    double upload_ms = now_ms() - upload_start;

    double dispatch_start = now_ms();
    glUseProgram(program);
    GLint loc = glGetUniformLocation(program, "u_count");
    glUniform1ui(loc, (GLuint)n);
    glDispatchCompute((GLuint)((n + 127) / 128), 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glFinish();
    double dispatch_ms = now_ms() - dispatch_start;

    double download_start = now_ms();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf_o);
    void *mapped = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)bytes, GL_MAP_READ_BIT);
    if (!mapped) {
        glDeleteBuffers(1, &buf_a);
        glDeleteBuffers(1, &buf_b);
        glDeleteBuffers(1, &buf_o);
        glDeleteProgram(program);
        json_fail("download", "glMapBufferRange failed");
        return 5;
    }
    memcpy(out, mapped, bytes);
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    double download_ms = now_ms() - download_start;

    double max_err = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double err = fabs((double)out[i] - (double)(a[i] + b[i]));
        if (err > max_err) max_err = err;
    }

    glDeleteBuffers(1, &buf_a);
    glDeleteBuffers(1, &buf_b);
    glDeleteBuffers(1, &buf_o);
    glDeleteProgram(program);

    const int valid = max_err <= 0.0001;
    fprintf(json_out(),
            "{\"executor\":\"pdocker-gpu-executor\",\"api\":\"%s\",\"abi_version\":\"%s\","
            "\"role\":\"%s\",\"llm_engine\":\"%s\",\"device_independent\":true,"
            "\"backend_impl\":\"gles31_compute\",\"backend_affinity\":\"fallback\",\"transport\":\"%s\","
            "\"kernel\":\"vector_add\",\"problem_size\":\"n=%zu\","
            "\"compile_ms\":%.4f,\"upload_ms\":%.4f,\"dispatch_ms\":%.4f,"
            "\"download_ms\":%.4f,\"total_ms\":%.4f,\"max_abs_error\":%.8f,"
            "\"valid\":%s}\n",
            PDOCKER_GPU_COMMAND_API, PDOCKER_GPU_ABI_VERSION,
            PDOCKER_GPU_EXECUTOR_ROLE, PDOCKER_GPU_LLM_ENGINE_LOCATION,
            transport ? transport : "local-process-buffer",
            n, compile_ms, upload_ms, dispatch_ms, download_ms,
            compile_ms + upload_ms + dispatch_ms + download_ms, max_err,
            valid ? "true" : "false");
    fflush(json_out());
    return valid ? 0 : 6;
}

static int run_vector_add_arrays_best(const float *a, const float *b, float *out, size_t n, const char *transport, GpuApiAffinity affinity) {
    if (affinity == GPU_API_VULKAN && strcmp(getenv("PDOCKER_GPU_DISABLE_ANDROID_VULKAN") ? getenv("PDOCKER_GPU_DISABLE_ANDROID_VULKAN") : "0", "1") != 0) {
        int rc = run_vector_add_arrays_vulkan(a, b, out, n, transport ? transport : "vulkan-command-queue");
        if (rc == 0) return 0;
        fprintf(stderr, "pdocker-gpu-executor: Android Vulkan vector_add unavailable rc=%d; falling back to GLES compute (cross-api fallback)\n", rc);
        return run_vector_add_arrays(a, b, out, n, transport ? transport : "vulkan-to-gles31-fallback");
    }
    if ((affinity == GPU_API_OPENCL || affinity == GPU_API_AUTO) &&
        strcmp(getenv("PDOCKER_GPU_DISABLE_ANDROID_OPENCL") ? getenv("PDOCKER_GPU_DISABLE_ANDROID_OPENCL") : "0", "1") != 0) {
        int rc = run_vector_add_arrays_opencl(a, b, out, n, transport ? transport : "opencl-command-queue");
        if (rc == 0) return 0;
        fprintf(stderr, "pdocker-gpu-executor: Android OpenCL vector_add unavailable rc=%d; falling back to GLES compute (cross-api fallback)\n", rc);
    }
    return run_vector_add_arrays(a, b, out, n, transport ? transport : "gles31-fallback");
}

static int run_vector_add(void) {
    const size_t n = PDOCKER_GPU_VECTOR_ADD_DEFAULT_N;
    const size_t bytes = n * sizeof(float);
    float *a = (float *)malloc(bytes);
    float *b = (float *)malloc(bytes);
    float *out = (float *)calloc(n, sizeof(float));
    if (!a || !b || !out) {
        free(a);
        free(b);
        free(out);
        json_fail("alloc", "host allocation failed");
        return 2;
    }
    fill_inputs(a, b, n);
    int rc = run_vector_add_arrays_best(a, b, out, n, "local-process-buffer", GPU_API_AUTO);
    free(a);
    free(b);
    free(out);
    return rc;
}

static int run_vector_add_fd(int fd, size_t n, GpuApiAffinity affinity) {
    if (fd < 0) {
        json_fail("fd", "missing shared buffer fd");
        return 64;
    }
    if (n == 0 || n > PDOCKER_GPU_VECTOR_ADD_MAX_N) {
        json_fail("fd", "invalid vector size");
        return 64;
    }
    const size_t bytes = n * sizeof(float);
    const size_t total = bytes * 3;
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        json_fail("mmap", strerror(errno));
        close(fd);
        return 70;
    }
    float *a = (float *)map;
    float *b = a + n;
    float *out = b + n;
    int rc = run_vector_add_arrays_best(a, b, out, n, "unix-socket-scm-rights-shared-buffer", affinity);
    munmap(map, total);
    close(fd);
    return rc;
}

static int run_vector_add_3fd(int fd_a, int fd_b, int fd_out, size_t n, GpuApiAffinity affinity) {
    if (fd_a < 0 || fd_b < 0 || fd_out < 0) {
        if (fd_a >= 0) close(fd_a);
        if (fd_b >= 0) close(fd_b);
        if (fd_out >= 0) close(fd_out);
        json_fail("fd", "missing vector buffer fd");
        return 64;
    }
    if (n == 0 || n > PDOCKER_GPU_VECTOR_ADD_MAX_N) {
        close(fd_a);
        close(fd_b);
        close(fd_out);
        json_fail("fd", "invalid vector size");
        return 64;
    }
    const size_t bytes = n * sizeof(float);
    void *map_a = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_a, 0);
    void *map_b = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_b, 0);
    void *map_out = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_out, 0);
    close(fd_a);
    close(fd_b);
    close(fd_out);
    if (map_a == MAP_FAILED || map_b == MAP_FAILED || map_out == MAP_FAILED) {
        if (map_a != MAP_FAILED) munmap(map_a, bytes);
        if (map_b != MAP_FAILED) munmap(map_b, bytes);
        if (map_out != MAP_FAILED) munmap(map_out, bytes);
        json_fail("mmap", strerror(errno));
        return 70;
    }
    int rc = run_vector_add_arrays_best((const float *)map_a, (const float *)map_b, (float *)map_out, n,
                                        affinity == GPU_API_VULKAN ? "vulkan-icd-scm-rights-3buffer" : "opencl-icd-scm-rights-3buffer",
                                        affinity);
    munmap(map_a, bytes);
    munmap(map_b, bytes);
    munmap(map_out, bytes);
    return rc;
}

typedef struct {
    void *map;
    size_t n;
    size_t total;
} RegisteredVectorBuffer;

static void clear_registered_vector_buffer(RegisteredVectorBuffer *buffer) {
    if (!buffer) return;
    if (buffer->map && buffer->map != MAP_FAILED) {
        munmap(buffer->map, buffer->total);
    }
    memset(buffer, 0, sizeof(*buffer));
}

static int register_vector_buffer(RegisteredVectorBuffer *buffer, int fd, size_t n) {
    if (!buffer || fd < 0) {
        json_fail("fd", "missing shared buffer fd");
        return 64;
    }
    if (n == 0 || n > PDOCKER_GPU_VECTOR_ADD_MAX_N) {
        close(fd);
        json_fail("fd", "invalid vector size");
        return 64;
    }
    const size_t bytes = n * sizeof(float);
    const size_t total = bytes * 3;
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        json_fail("mmap", strerror(errno));
        return 70;
    }
    clear_registered_vector_buffer(buffer);
    buffer->map = map;
    buffer->n = n;
    buffer->total = total;
    fprintf(json_out(),
            "{\"executor\":\"pdocker-gpu-executor\",\"api\":\"%s\",\"abi_version\":\"%s\","
            "\"role\":\"%s\",\"llm_engine\":\"%s\",\"device_independent\":true,"
            "\"transport\":\"unix-socket-registered-shared-buffer\","
            "\"kernel\":\"register_vector_buffer\",\"problem_size\":\"n=%zu\","
            "\"valid\":true}\n",
            PDOCKER_GPU_COMMAND_API, PDOCKER_GPU_ABI_VERSION,
            PDOCKER_GPU_EXECUTOR_ROLE, PDOCKER_GPU_LLM_ENGINE_LOCATION, n);
    fflush(json_out());
    return 0;
}

static int run_registered_vector_add(RegisteredVectorBuffer *buffer) {
    if (!buffer || !buffer->map || buffer->n == 0) {
        json_fail("registered-buffer", "no registered vector buffer");
        return 64;
    }
    float *a = (float *)buffer->map;
    float *b = a + buffer->n;
    float *out = b + buffer->n;
    return run_vector_add_arrays_best(a, b, out, buffer->n, "unix-socket-registered-shared-buffer", GPU_API_AUTO);
}

static void print_capabilities(const char *transport) {
    fprintf(json_out(),
            "{\"executor\":\"pdocker-gpu-executor\",\"api\":\"%s\",\"abi_version\":\"%s\","
            "\"role\":\"%s\",\"llm_engine\":\"%s\",\"device_independent\":true,"
            "\"transport\":\"%s\","
            "\"backend_impls\":[\"android_opencl\",\"gles31_compute\"],"
            "\"preferred_backend\":\"android_opencl\","
            "\"fallback_backend\":\"gles31_compute\","
            "\"backend_affinity_policy\":\"same-api-first\","
            "\"container_contract\":\"glibc-shim-command-queue\","
            "\"fd_shared_buffer\":true,"
            "\"process_exec\":true}\n",
            PDOCKER_GPU_COMMAND_API, PDOCKER_GPU_ABI_VERSION,
            PDOCKER_GPU_EXECUTOR_ROLE, PDOCKER_GPU_LLM_ENGINE_LOCATION,
            transport);
    fflush(json_out());
}

static void print_noop(void) {
    fprintf(json_out(),
            "{\"executor\":\"pdocker-gpu-executor\",\"api\":\"%s\",\"abi_version\":\"%s\","
            "\"role\":\"%s\",\"llm_engine\":\"%s\",\"device_independent\":true,"
            "\"kernel\":\"noop\",\"total_ms\":0.0,\"valid\":true}\n",
            PDOCKER_GPU_COMMAND_API, PDOCKER_GPU_ABI_VERSION,
            PDOCKER_GPU_EXECUTOR_ROLE, PDOCKER_GPU_LLM_ENGINE_LOCATION);
    fflush(json_out());
}

static int run_gpu_once(void) {
    GpuContext ctx;
    int rc = init_gpu_context(&ctx);
    if (rc != 0) return rc;
    rc = run_vector_add();
    destroy_gpu_context(&ctx);
    return rc;
}

static int bench_vector_add(int count) {
    if (count <= 0) count = 1;
    GpuContext ctx;
    int rc = init_gpu_context(&ctx);
    if (rc != 0) return rc;
    int last = 0;
    for (int i = 0; i < count; ++i) {
        last = run_vector_add();
    }
    destroy_gpu_context(&ctx);
    return last;
}

static int bench_vulkan_vector_add(int count) {
    if (count <= 0) count = 1;
    const size_t n = PDOCKER_GPU_VECTOR_ADD_DEFAULT_N;
    const size_t bytes = n * sizeof(float);
    float *a = (float *)malloc(bytes);
    float *b = (float *)malloc(bytes);
    float *out = (float *)calloc(n, sizeof(float));
    if (!a || !b || !out) {
        free(a);
        free(b);
        free(out);
        json_fail("alloc", "host allocation failed");
        return 2;
    }
    fill_inputs(a, b, n);
    int last = 0;
    for (int i = 0; i < count; ++i) {
        memset(out, 0, bytes);
        last = run_vector_add_arrays_vulkan(a, b, out, n, "direct-vulkan-local-process-buffer");
    }
    free(a);
    free(b);
    free(out);
    return last;
}

static int bench_opencl_vector_add(int count) {
    if (count <= 0) count = 1;
    const size_t n = PDOCKER_GPU_VECTOR_ADD_DEFAULT_N;
    const size_t bytes = n * sizeof(float);
    float *a = (float *)malloc(bytes);
    float *b = (float *)malloc(bytes);
    float *out = (float *)calloc(n, sizeof(float));
    if (!a || !b || !out) {
        free(a);
        free(b);
        free(out);
        json_fail("alloc", "host allocation failed");
        return 2;
    }
    fill_inputs(a, b, n);
    int last = 0;
    for (int i = 0; i < count; ++i) {
        memset(out, 0, bytes);
        last = run_vector_add_arrays_opencl(a, b, out, n, "direct-opencl-local-process-buffer");
    }
    free(a);
    free(b);
    free(out);
    return last;
}

static int bench_noop(int count) {
    if (count <= 0) count = 1;
    for (int i = 0; i < count; ++i) {
        print_noop();
    }
    return 0;
}

static int parse_count(const char *s, int fallback) {
    if (!s || !s[0]) return fallback;
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (!end || *end || n <= 0 || n > 10000) return fallback;
    return (int)n;
}

static int recv_command_with_fds(int cfd, char *cmd, size_t cmd_size, int *passed_fds, size_t max_fds, size_t *fd_count) {
    if (!cmd || cmd_size == 0 || !passed_fds || !fd_count) return -EINVAL;
    *fd_count = 0;
    for (size_t i = 0; i < max_fds; ++i) passed_fds[i] = -1;
    char control[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec iov;
    struct msghdr msg;
    memset(control, 0, sizeof(control));
    memset(&iov, 0, sizeof(iov));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = cmd;
    iov.iov_len = cmd_size - 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(cfd, &msg, 0);
    if (n <= 0) return (int)n;
    cmd[n] = '\0';
    cmd[strcspn(cmd, "\r\n")] = '\0';
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
            size_t count = bytes / sizeof(int);
            if (count > max_fds) count = max_fds;
            memcpy(passed_fds, CMSG_DATA(cmsg), count * sizeof(int));
            *fd_count = count;
            break;
        }
    }
    return (int)n;
}

static int recv_command_with_optional_fd(int cfd, char *cmd, size_t cmd_size, int *passed_fd) {
    int fds[1] = { -1 };
    size_t count = 0;
    int rc = recv_command_with_fds(cfd, cmd, cmd_size, fds, 1, &count);
    *passed_fd = count > 0 ? fds[0] : -1;
    return rc;
}

static int run_vector_command_with_context(void) {
    int rc = run_vector_add();
    return rc;
}

static int serve_socket(const char *path) {
    if (!path || !path[0]) {
        fprintf(stderr, "pdocker-gpu-executor: missing socket path\n");
        return 64;
    }
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "pdocker-gpu-executor: socket path too long: %s\n", path);
        return 64;
    }
    signal(SIGPIPE, SIG_IGN);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        perror("socket");
        return 70;
    }
    unlink(path);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(sfd);
        return 70;
    }
    chmod(path, 0600);
    if (listen(sfd, 8) != 0) {
        perror("listen");
        close(sfd);
        return 70;
    }
    GpuContext ctx;
    int init_rc = init_gpu_context(&ctx);
    if (init_rc != 0) {
        close(sfd);
        unlink(path);
        return init_rc;
    }
    fprintf(stderr, "pdocker-gpu-executor: serving %s api=%s\n", path, PDOCKER_GPU_COMMAND_API);
    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        FILE *out = fdopen(dup(cfd), "w");
        if (!out) {
            close(cfd);
            continue;
        }
        setvbuf(out, NULL, _IONBF, 0);
        char cmd[160];
        RegisteredVectorBuffer registered;
        memset(&registered, 0, sizeof(registered));
        for (;;) {
            int passed_fds[4] = { -1, -1, -1, -1 };
            size_t passed_fd_count = 0;
            int nread = recv_command_with_fds(cfd, cmd, sizeof(cmd), passed_fds, 4, &passed_fd_count);
            if (nread <= 0) break;
            g_json_out = out;
            if (strcmp(cmd, "CAPABILITIES") == 0) {
                print_capabilities("unix-socket-command-queue");
            } else if (strcmp(cmd, "NOOP") == 0) {
                print_noop();
            } else if (strcmp(cmd, "VECTOR_ADD") == 0) {
                (void)run_vector_command_with_context();
            } else if (strncmp(cmd, "VECTOR_ADD_FD ", 14) == 0) {
                size_t n = (size_t)strtoull(cmd + 14, NULL, 10);
                (void)run_vector_add_fd(passed_fds[0], n, GPU_API_AUTO);
                passed_fds[0] = -1;
            } else if (strncmp(cmd, "REGISTER_VECTOR_FD ", 19) == 0) {
                size_t n = (size_t)strtoull(cmd + 19, NULL, 10);
                (void)register_vector_buffer(&registered, passed_fds[0], n);
                passed_fds[0] = -1;
            } else if (strcmp(cmd, "VECTOR_ADD_REGISTERED") == 0) {
                (void)run_registered_vector_add(&registered);
            } else if (strncmp(cmd, "OPENCL_VECTOR_ADD_3FD ", 22) == 0) {
                size_t n = (size_t)strtoull(cmd + 22, NULL, 10);
                if (passed_fd_count < 3) {
                    json_fail("fd", "OPENCL_VECTOR_ADD_3FD requires three fds");
                } else {
                    (void)run_vector_add_3fd(passed_fds[0], passed_fds[1], passed_fds[2], n, GPU_API_OPENCL);
                    passed_fds[0] = passed_fds[1] = passed_fds[2] = -1;
                }
            } else if (strncmp(cmd, "VULKAN_VECTOR_ADD_3FD ", 22) == 0) {
                size_t n = (size_t)strtoull(cmd + 22, NULL, 10);
                if (passed_fd_count < 3) {
                    json_fail("fd", "VULKAN_VECTOR_ADD_3FD requires three fds");
                } else {
                    (void)run_vector_add_3fd(passed_fds[0], passed_fds[1], passed_fds[2], n, GPU_API_VULKAN);
                    passed_fds[0] = passed_fds[1] = passed_fds[2] = -1;
                }
            } else if (strncmp(cmd, "VECTOR_ADD_3FD ", 15) == 0) {
                size_t n = (size_t)strtoull(cmd + 15, NULL, 10);
                if (passed_fd_count < 3) {
                    json_fail("fd", "VECTOR_ADD_3FD requires three fds");
                } else {
                    (void)run_vector_add_3fd(passed_fds[0], passed_fds[1], passed_fds[2], n, GPU_API_AUTO);
                    passed_fds[0] = passed_fds[1] = passed_fds[2] = -1;
                }
            } else {
                json_fail("command", "unknown command");
            }
            for (size_t i = 0; i < passed_fd_count && i < 4; ++i) {
                if (passed_fds[i] >= 0) close(passed_fds[i]);
            }
            g_json_out = NULL;
        }
        clear_registered_vector_buffer(&registered);
        fclose(out);
        close(cfd);
    }
    destroy_gpu_context(&ctx);
    close(sfd);
    unlink(path);
    return 70;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--capabilities") == 0) {
        print_capabilities("self-test-now; unix-socket-command-queue");
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--serve-socket") == 0) {
        return serve_socket(argv[2]);
    }
    if (argc > 1 && strcmp(argv[1], "--bench-vector-add") == 0) {
        return bench_vector_add(parse_count(argc > 2 ? argv[2] : NULL, 5));
    }
    if (argc > 1 && strcmp(argv[1], "--bench-vulkan-vector-add") == 0) {
        return bench_vulkan_vector_add(parse_count(argc > 2 ? argv[2] : NULL, 5));
    }
    if (argc > 1 && strcmp(argv[1], "--bench-opencl-vector-add") == 0) {
        return bench_opencl_vector_add(parse_count(argc > 2 ? argv[2] : NULL, 5));
    }
    if (argc > 1 && strcmp(argv[1], "--bench-noop") == 0) {
        return bench_noop(parse_count(argc > 2 ? argv[2] : NULL, 5));
    }
    return run_gpu_once();
}
