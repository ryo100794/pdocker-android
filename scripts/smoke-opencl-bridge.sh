#!/usr/bin/env bash
# Verify the glibc-facing OpenCL shim can submit a minimal vector-add workload
# through the pdocker GPU executor command queue.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXECUTOR="$ROOT/app/src/main/jniLibs/arm64-v8a/libpdockergpuexecutor.so"
OPENCL="$ROOT/docker-proot-setup/lib/pdocker-opencl-icd.so"
TMP="$(mktemp -d)"
SOCK="${TMP}/pdocker-gpu.sock"
trap '[[ -n "${PID:-}" ]] && kill "$PID" 2>/dev/null || true; rm -rf "$TMP"' EXIT

cat >"$TMP/pdocker-opencl-smoke.c" <<'C'
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
typedef cl_ulong cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_bitfield cl_mem_flags;
typedef cl_uint cl_bool;
typedef intptr_t cl_context_properties;
typedef intptr_t cl_queue_properties;
typedef struct _cl_platform_id *cl_platform_id;
typedef struct _cl_device_id *cl_device_id;
typedef struct _cl_context *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_mem *cl_mem;
typedef struct _cl_program *cl_program;
typedef struct _cl_kernel *cl_kernel;
typedef struct _cl_event *cl_event;

#define CL_SUCCESS 0
#define CL_DEVICE_TYPE_GPU (1u << 2)
#define CL_MEM_READ_WRITE (1u << 0)
#define CL_MEM_COPY_HOST_PTR (1u << 5)
#define CL_TRUE 1

#define LOAD(name) do { *(void **)(&name) = dlsym(lib, #name); if (!name) { fprintf(stderr, "missing %s\n", #name); return 2; } } while (0)
#define CHECK(expr, msg) do { cl_int rc__ = (expr); if (rc__ != CL_SUCCESS) { fprintf(stderr, "%s: %d\n", msg, rc__); return 3; } } while (0)

static cl_int (*clGetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
static cl_int (*clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *);
static cl_context (*clCreateContext)(const cl_context_properties *, cl_uint, const cl_device_id *, void (*)(const char *, const void *, size_t, void *), void *, cl_int *);
static cl_command_queue (*clCreateCommandQueueWithProperties)(cl_context, cl_device_id, const cl_queue_properties *, cl_int *);
static cl_mem (*clCreateBuffer)(cl_context, cl_mem_flags, size_t, void *, cl_int *);
static cl_program (*clCreateProgramWithSource)(cl_context, cl_uint, const char **, const size_t *, cl_int *);
static cl_int (*clBuildProgram)(cl_program, cl_uint, const cl_device_id *, const char *, void (*)(cl_program, void *), void *);
static cl_kernel (*clCreateKernel)(cl_program, const char *, cl_int *);
static cl_int (*clSetKernelArg)(cl_kernel, cl_uint, size_t, const void *);
static cl_int (*clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, const cl_event *, cl_event *);
static cl_int (*clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, void *, cl_uint, const cl_event *, cl_event *);
static cl_int (*clFinish)(cl_command_queue);

int main(int argc, char **argv) {
    if (argc != 2) return 64;
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    LOAD(clGetPlatformIDs);
    LOAD(clGetDeviceIDs);
    LOAD(clCreateContext);
    LOAD(clCreateCommandQueueWithProperties);
    LOAD(clCreateBuffer);
    LOAD(clCreateProgramWithSource);
    LOAD(clBuildProgram);
    LOAD(clCreateKernel);
    LOAD(clSetKernelArg);
    LOAD(clEnqueueNDRangeKernel);
    LOAD(clEnqueueReadBuffer);
    LOAD(clFinish);

    cl_platform_id platform = 0;
    cl_device_id device = 0;
    cl_uint count = 0;
    CHECK(clGetPlatformIDs(1, &platform, &count), "clGetPlatformIDs");
    CHECK(clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, &count), "clGetDeviceIDs");
    cl_int err = 0;
    cl_context ctx = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
    CHECK(err, "clCreateContext");
    cl_command_queue queue = clCreateCommandQueueWithProperties(ctx, device, NULL, &err);
    CHECK(err, "clCreateCommandQueueWithProperties");

    const size_t n = 1024;
    float *a = malloc(n * sizeof(float));
    float *b = malloc(n * sizeof(float));
    float *out = calloc(n, sizeof(float));
    for (size_t i = 0; i < n; ++i) { a[i] = (float)i * 0.25f; b[i] = 1.0f - (float)i * 0.125f; }
    cl_mem ma = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, n * sizeof(float), a, &err);
    CHECK(err, "clCreateBuffer A");
    cl_mem mb = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, n * sizeof(float), b, &err);
    CHECK(err, "clCreateBuffer B");
    cl_mem mo = clCreateBuffer(ctx, CL_MEM_READ_WRITE, n * sizeof(float), NULL, &err);
    CHECK(err, "clCreateBuffer O");
    const char *src = "__kernel void add(__global float *a, __global float *b, __global float *o) { size_t i = get_global_id(0); o[i] = a[i] + b[i]; }\n";
    cl_program program = clCreateProgramWithSource(ctx, 1, &src, NULL, &err);
    CHECK(err, "clCreateProgramWithSource");
    CHECK(clBuildProgram(program, 1, &device, "", NULL, NULL), "clBuildProgram");
    cl_kernel kernel = clCreateKernel(program, "add", &err);
    CHECK(err, "clCreateKernel");
    CHECK(clSetKernelArg(kernel, 0, sizeof(ma), &ma), "clSetKernelArg 0");
    CHECK(clSetKernelArg(kernel, 1, sizeof(mb), &mb), "clSetKernelArg 1");
    CHECK(clSetKernelArg(kernel, 2, sizeof(mo), &mo), "clSetKernelArg 2");
    size_t global = n;
    CHECK(clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global, NULL, 0, NULL, NULL), "clEnqueueNDRangeKernel");
    CHECK(clFinish(queue), "clFinish");
    CHECK(clEnqueueReadBuffer(queue, mo, CL_TRUE, 0, n * sizeof(float), out, 0, NULL, NULL), "clEnqueueReadBuffer");
    double max_err = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double e = fabs((double)out[i] - (double)(a[i] + b[i]));
        if (e > max_err) max_err = e;
    }
    printf("maxErr=%.8f out0=%.3f outLast=%.3f\n", max_err, out[0], out[n - 1]);
    return max_err <= 0.0001 ? 0 : 11;
}
C

gcc "$TMP/pdocker-opencl-smoke.c" -o "$TMP/pdocker-opencl-smoke" -ldl -lm
"$EXECUTOR" --serve-socket "$SOCK" >"$TMP/executor.log" 2>&1 &
PID=$!
for _ in $(seq 1 100); do
    [[ -S "$SOCK" ]] && break
    sleep 0.05
done
[[ -S "$SOCK" ]] || { cat "$TMP/executor.log" >&2; exit 1; }

PDOCKER_GPU_QUEUE_SOCKET="$SOCK" "$TMP/pdocker-opencl-smoke" "$OPENCL"
