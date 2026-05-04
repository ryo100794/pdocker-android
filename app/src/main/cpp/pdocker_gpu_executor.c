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
#include "pdocker_gpu_abi.h"
#include <math.h>
#include <errno.h>
#include <signal.h>
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
            "\"backend_impl\":\"gles31_compute\",\"transport\":\"%s\","
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
    int rc = run_vector_add_arrays(a, b, out, n, "local-process-buffer");
    free(a);
    free(b);
    free(out);
    return rc;
}

static int run_vector_add_fd(int fd, size_t n) {
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
    int rc = run_vector_add_arrays(a, b, out, n, "unix-socket-scm-rights-shared-buffer");
    munmap(map, total);
    close(fd);
    return rc;
}

static void print_capabilities(const char *transport) {
    fprintf(json_out(),
            "{\"executor\":\"pdocker-gpu-executor\",\"api\":\"%s\",\"abi_version\":\"%s\","
            "\"role\":\"%s\",\"llm_engine\":\"%s\",\"device_independent\":true,"
            "\"transport\":\"%s\","
            "\"backend_impls\":[\"gles31_compute\"],"
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

static int recv_command_with_optional_fd(int cfd, char *cmd, size_t cmd_size, int *passed_fd) {
    if (!cmd || cmd_size == 0 || !passed_fd) return -EINVAL;
    *passed_fd = -1;
    char control[CMSG_SPACE(sizeof(int))];
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
            memcpy(passed_fd, CMSG_DATA(cmsg), sizeof(int));
            break;
        }
    }
    return (int)n;
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
        for (;;) {
            int passed_fd = -1;
            int nread = recv_command_with_optional_fd(cfd, cmd, sizeof(cmd), &passed_fd);
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
                (void)run_vector_add_fd(passed_fd, n);
                passed_fd = -1;
            } else {
                if (passed_fd >= 0) close(passed_fd);
                json_fail("command", "unknown command");
            }
            g_json_out = NULL;
        }
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
    if (argc > 1 && strcmp(argv[1], "--bench-noop") == 0) {
        return bench_noop(parse_count(argc > 2 ? argv[2] : NULL, 5));
    }
    return run_gpu_once();
}
