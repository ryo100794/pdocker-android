/*
 * Container-facing pdocker GPU shim probe.
 *
 * This binary is built for Linux/glibc and bind-mounted into GPU-requesting
 * containers as /usr/local/bin/pdocker-gpu-shim. It is deliberately
 * backend-neutral: Android GLES/Vulkan/OpenCL details stay behind the APK
 * executor. The next implementation step is replacing the current capability
 * probe with a shared-memory command queue.
 */
#include "pdocker_gpu_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : fallback;
}

static void print_capabilities(void) {
    printf("{\"shim\":\"pdocker-gpu-shim\","
           "\"api\":\"%s\","
           "\"abi_version\":\"%s\","
           "\"llm_engine\":\"%s\","
           "\"device_independent\":true,"
           "\"container_contract\":\"%s\","
           "\"executor_available\":%s,"
           "\"executor_role\":\"%s\","
           "\"transport\":\"command-queue-pending\","
           "\"backend_impl_visible_to_container\":false}\n",
           PDOCKER_GPU_COMMAND_API,
           PDOCKER_GPU_ABI_VERSION,
           PDOCKER_GPU_LLM_ENGINE_LOCATION,
           PDOCKER_GPU_CONTAINER_CONTRACT,
           strcmp(env_or("PDOCKER_GPU_EXECUTOR_AVAILABLE", "0"), "1") == 0 ? "true" : "false",
           env_or("PDOCKER_GPU_EXECUTOR_ROLE", "apk-bionic-gpu-command-executor"));
}

static void print_env(void) {
    printf("PDOCKER_GPU_COMMAND_API=%s\n", env_or("PDOCKER_GPU_COMMAND_API", PDOCKER_GPU_COMMAND_API));
    printf("PDOCKER_GPU_ABI_VERSION=%s\n", env_or("PDOCKER_GPU_ABI_VERSION", PDOCKER_GPU_ABI_VERSION));
    printf("PDOCKER_GPU_LLM_ENGINE_LOCATION=%s\n", env_or("PDOCKER_GPU_LLM_ENGINE_LOCATION", PDOCKER_GPU_LLM_ENGINE_LOCATION));
    printf("PDOCKER_GPU_EXECUTOR_AVAILABLE=%s\n", env_or("PDOCKER_GPU_EXECUTOR_AVAILABLE", "0"));
    printf("PDOCKER_GPU_MODES=%s\n", env_or("PDOCKER_GPU_MODES", ""));
}

int main(int argc, char **argv) {
    if (argc <= 1 || strcmp(argv[1], "--capabilities") == 0) {
        print_capabilities();
        return 0;
    }
    if (strcmp(argv[1], "--env") == 0) {
        print_env();
        return 0;
    }
    if (strcmp(argv[1], "--queue-probe") == 0) {
        printf("{\"shim\":\"pdocker-gpu-shim\","
               "\"api\":\"%s\","
               "\"valid\":false,"
               "\"reason\":\"shared-memory command queue is not connected yet\"}\n",
               PDOCKER_GPU_COMMAND_API);
        return 2;
    }
    fprintf(stderr, "usage: %s [--capabilities|--env|--queue-probe]\n", argv[0]);
    return 64;
}
