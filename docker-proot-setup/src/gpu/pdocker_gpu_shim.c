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
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : fallback;
}

static void print_capabilities(void) {
    const char *socket_path = getenv("PDOCKER_GPU_QUEUE_SOCKET");
    const int queue_ready = socket_path && socket_path[0];
    printf("{\"shim\":\"pdocker-gpu-shim\","
           "\"api\":\"%s\","
           "\"abi_version\":\"%s\","
           "\"llm_engine\":\"%s\","
           "\"device_independent\":true,"
           "\"container_contract\":\"%s\","
           "\"executor_available\":%s,"
           "\"executor_role\":\"%s\","
           "\"queue_socket_available\":%s,"
           "\"transport\":\"%s\","
           "\"backend_impl_visible_to_container\":false}\n",
           PDOCKER_GPU_COMMAND_API,
           PDOCKER_GPU_ABI_VERSION,
           PDOCKER_GPU_LLM_ENGINE_LOCATION,
           PDOCKER_GPU_CONTAINER_CONTRACT,
           strcmp(env_or("PDOCKER_GPU_EXECUTOR_AVAILABLE", "0"), "1") == 0 ? "true" : "false",
           env_or("PDOCKER_GPU_EXECUTOR_ROLE", "apk-bionic-gpu-command-executor"),
           queue_ready ? "true" : "false",
           queue_ready ? "unix-socket-command-queue" : "command-queue-pending");
}

static void print_env(void) {
    printf("PDOCKER_GPU_COMMAND_API=%s\n", env_or("PDOCKER_GPU_COMMAND_API", PDOCKER_GPU_COMMAND_API));
    printf("PDOCKER_GPU_ABI_VERSION=%s\n", env_or("PDOCKER_GPU_ABI_VERSION", PDOCKER_GPU_ABI_VERSION));
    printf("PDOCKER_GPU_LLM_ENGINE_LOCATION=%s\n", env_or("PDOCKER_GPU_LLM_ENGINE_LOCATION", PDOCKER_GPU_LLM_ENGINE_LOCATION));
    printf("PDOCKER_GPU_EXECUTOR_AVAILABLE=%s\n", env_or("PDOCKER_GPU_EXECUTOR_AVAILABLE", "0"));
    printf("PDOCKER_GPU_QUEUE_SOCKET=%s\n", env_or("PDOCKER_GPU_QUEUE_SOCKET", ""));
    printf("PDOCKER_GPU_MODES=%s\n", env_or("PDOCKER_GPU_MODES", ""));
}

static int send_command(const char *command) {
    const char *path = getenv("PDOCKER_GPU_QUEUE_SOCKET");
    if (!path || !path[0]) {
        printf("{\"shim\":\"pdocker-gpu-shim\","
               "\"api\":\"%s\","
               "\"valid\":false,"
               "\"reason\":\"PDOCKER_GPU_QUEUE_SOCKET is not set\"}\n",
               PDOCKER_GPU_COMMAND_API);
        return 2;
    }
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "pdocker-gpu-shim: queue socket path too long: %s\n", path);
        return 64;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 70;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "pdocker-gpu-shim: connect %s failed: %s\n", path, strerror(errno));
        close(fd);
        return 69;
    }
    dprintf(fd, "%s\n", command);
    shutdown(fd, SHUT_WR);
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) {
            close(fd);
            return 74;
        }
    }
    close(fd);
    return 0;
}

static int connect_queue(void) {
    const char *path = getenv("PDOCKER_GPU_QUEUE_SOCKET");
    if (!path || !path[0]) {
        return -ENOENT;
    }
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return -ENAMETOOLONG;
    }
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

static int bench_vector_add(int count, int persistent) {
    if (count <= 0) count = 1;
    if (persistent) {
        int fd = connect_queue();
        if (fd < 0) {
            fprintf(stderr, "pdocker-gpu-shim: persistent connect failed: %s\n", strerror(-fd));
            return 69;
        }
        FILE *io = fdopen(fd, "r+");
        if (!io) {
            close(fd);
            return 70;
        }
        setvbuf(io, NULL, _IONBF, 0);
        char line[4096];
        for (int i = 0; i < count; ++i) {
            fprintf(io, "VECTOR_ADD\n");
            if (!fgets(line, sizeof(line), io)) {
                fclose(io);
                return 70;
            }
            fputs(line, stdout);
        }
        fclose(io);
        return 0;
    }
    int last = 0;
    for (int i = 0; i < count; ++i) {
        last = send_command("VECTOR_ADD");
    }
    return last;
}

static int bench_noop(int count, int persistent) {
    if (count <= 0) count = 1;
    if (persistent) {
        int fd = connect_queue();
        if (fd < 0) {
            fprintf(stderr, "pdocker-gpu-shim: persistent connect failed: %s\n", strerror(-fd));
            return 69;
        }
        FILE *io = fdopen(fd, "r+");
        if (!io) {
            close(fd);
            return 70;
        }
        setvbuf(io, NULL, _IONBF, 0);
        char line[4096];
        for (int i = 0; i < count; ++i) {
            fprintf(io, "NOOP\n");
            if (!fgets(line, sizeof(line), io)) {
                fclose(io);
                return 70;
            }
            fputs(line, stdout);
        }
        fclose(io);
        return 0;
    }
    int last = 0;
    for (int i = 0; i < count; ++i) {
        last = send_command("NOOP");
    }
    return last;
}

static int parse_count(const char *s, int fallback) {
    if (!s || !s[0]) return fallback;
    char *end = NULL;
    long n = strtol(s, &end, 10);
    if (!end || *end || n <= 0 || n > 10000) return fallback;
    return (int)n;
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
        return send_command("CAPABILITIES");
    }
    if (strcmp(argv[1], "--vector-add") == 0) {
        return send_command("VECTOR_ADD");
    }
    if (strcmp(argv[1], "--bench-vector-add") == 0) {
        return bench_vector_add(parse_count(argc > 2 ? argv[2] : NULL, 5), 0);
    }
    if (strcmp(argv[1], "--bench-vector-add-persistent") == 0) {
        return bench_vector_add(parse_count(argc > 2 ? argv[2] : NULL, 5), 1);
    }
    if (strcmp(argv[1], "--bench-noop") == 0) {
        return bench_noop(parse_count(argc > 2 ? argv[2] : NULL, 5), 0);
    }
    if (strcmp(argv[1], "--bench-noop-persistent") == 0) {
        return bench_noop(parse_count(argc > 2 ? argv[2] : NULL, 5), 1);
    }
    fprintf(stderr, "usage: %s [--capabilities|--env|--queue-probe|--vector-add|--bench-vector-add N|--bench-vector-add-persistent N|--bench-noop N|--bench-noop-persistent N]\n", argv[0]);
    return 64;
}
