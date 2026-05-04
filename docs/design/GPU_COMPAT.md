# pdocker GPU compatibility extensions

Snapshot date: 2026-05-01.

pdocker has an experimental Docker-compatible GPU request surface. It is
designed for Android devices where native Docker GPU runtimes such as
`nvidia-container-runtime` do not exist.

Canonical split:

- Backend request/env/inspect behavior lives in
  [`docker-proot-setup/docs/GPU_COMPAT.md`](../../docker-proot-setup/docs/GPU_COMPAT.md).
- This document owns the Android benchmark philosophy, cuVK direction, UI
  diagnostics, and device-specific measurement notes.

## Design principle

pdocker treats Android GPU support as a Vulkan-first compatibility stack.
Native NVIDIA CUDA is not expected on ordinary Android phones; it is only an
external baseline for Jetson or NVIDIA Linux devices. The Android path is:

```text
Docker --gpus / HostConfig.DeviceRequests
  -> pdocker GPU negotiation
  -> a glibc-facing GPU bridge owned by pdocker
  -> Android-side Vulkan/OpenCL execution behind that bridge
  -> cuVK, a restricted CUDA-like API lowered to the bridge runtime
```

`cuda-compat` therefore means "CUDA-shaped userspace API backed by Android GPU
compute", not PTX execution and not NVIDIA driver passthrough.

The implementation must keep CPU, direct Vulkan, and CUDA-like compatibility
paths comparable. Any benchmark or runtime probe should use the same input
data, validation rules, problem sizes, and output format across:

- `cpu_scalar`
- `cpu_neon`
- `vulkan_spirv`
- `cuvk_transpile`
- optional `native_cuda` on NVIDIA Linux only

Each result must separate:

```text
compile_ms + upload_ms + dispatch_ms + download_ms = total_ms
```

This matters on Android because upload/download and synchronization overhead
can dominate small workloads.

## Current behavior

Current backend behavior is request parsing, environment negotiation, inspect
metadata, health/test reporting, and diagnostic exposure of Android GPU signals.
The direct exposure experiment proved that Android vendor libraries should not
be treated as a working container GPU backend: Android's Vulkan/OpenCL libraries
are Bionic/Android ABI objects, while the bundled Linux images are glibc
userlands. Exposing `/system/lib64/libvulkan.so` or
`/vendor/lib64/libOpenCL.so` into an Ubuntu image naturally drives the glibc
loader into Android-only dependencies such as `android.hardware.*`,
`liblog.so`, and `libcutils.so`.

The production direction is therefore:

```text
glibc container process
  -> pdocker-owned glibc shim library or device ABI
  -> stable shared-memory command queue or narrow ioctl-like control plane
  -> Android/Bionic GPU executor owned by the APK
  -> Vulkan/OpenCL/NNAPI/other Android GPU API
```

The container must see a glibc-compatible ABI. Android GPU libraries may be
used only behind the APK/sidecar boundary, not as direct glibc `dlopen` targets.
The Android side must be a GPU command executor, not a host-side LLM engine.
For llama.cpp, model loading, tokenization, sampling, HTTP serving, scheduling,
and ggml graph ownership stay in the container process. The bridge may execute
GPU kernels, move buffers, and signal fences, but it must not replace
`llama-server` with a host RPC inference service.
The visible contract is a device-independent `pdocker-gpu-command-v1` ABI. GPU
backend choices such as GLES compute, Vulkan, OpenCL, NNAPI, or vendor-specific
driver details belong below that ABI and must be absorbed by the APK-owned
executor layer. Container code should not branch on phone model or vendor GPU
library paths.
The first scaffold now has two binaries:

- `pdocker-gpu-shim`: Linux/glibc container-facing probe, injected as
  `/usr/local/bin/pdocker-gpu-shim` for GPU-requesting containers.
- `pdocker-gpu-executor`: Android/Bionic APK-side executor probe. It owns
  Android GPU APIs and advertises the same neutral command ABI.

The current shim supports capability probing plus a temporary Unix-socket
command path for early measurement. It is not the final transport; the
shared-memory queue, buffer table, and fence protocol are still pending.
The temporary socket transport is allowed only as a measurement scaffold.
Benchmarks must separate NOOP/control overhead from upload/dispatch/download
work, and real LLM integration must use persistent transport plus buffer reuse
so bridge overhead is not paid per tiny ggml operation.
GPU runtime paths are exposed to containers under `/run/pdocker-gpu`. pdockerd
binds the APK runtime GPU directory there and direct execution rewrites
`connect(AF_UNIX)` socket paths, so container code never needs Android app-data
absolute paths.
Until that bridge exists and passes validation, llama.cpp GPU profile selection
must stay on CPU fallback unless a raw diagnostic mode is explicitly requested.

## CUDA-compatible API

`cuda-compat` is not NVIDIA CUDA passthrough. Android normally does not expose
NVIDIA `/dev/nvidia*` devices or the NVIDIA driver ABI. In pdocker, CUDA means
a planned compatibility API layer which can provide a CUDA-shaped userspace ABI
backed primarily by Vulkan Compute.

The first cuVK runtime scope is intentionally small:

- `cuvkInit` / `cuvkShutdown`
- `cuvkMalloc` / `cuvkFree`
- `cuvkMemcpy`
- `cuvkModuleLoadCudaSource`
- `cuvkModuleLoadSpirv`
- `cuvkKernelGet`
- `cuvkLaunchKernel`
- `cuvkDeviceSynchronize`

The supported CUDA-like subset starts with `__global__`, `threadIdx`,
`blockIdx`, fixed `blockDim`, global pointer arguments, scalar push constants,
and restricted `if`/`for`. Full CUDA C++, PTX, cuBLAS, cuDNN, cuFFT, NCCL,
Unified Memory, and complete stream semantics are non-goals for the first
implementation.

## Benchmark gate

GPU support should not be considered "working" just because a device node or
library is visible inside the container. A backend must validate against a CPU
reference implementation before its speedup is accepted.

The Android UI diagnostics now include a first-pass `android-gpu-bench` action
which writes CPU scalar and OpenGL ES 3.1 compute-shader results as JSON Lines
and CSV under `files/pdocker/bench/` and mirrors them to the app external files
`bench/` directory when Android storage policy allows it. GLES compute is an
Android-side GPU smoke backend; the Docker-facing target remains Vulkan/cuVK,
but the same artifact format and validation rules are used so the results are
comparable later.

Initial benchmark kernels:

- `vector_add`
- `saxpy`
- `matmul_fp32`

Follow-up kernels:

- `reduce_sum`
- `conv2d`
- `matmul_fp16`
- `quantized_matmul`
- `rmsnorm`
- `rope`
- `softmax`

Recommended decision thresholds:

- recommend GPU only when warm total time is at least 1.5x faster than
  `cpu_neon`
- recommend chained GPU execution when chained total time is at least 2.0x
  faster than `cpu_neon`
- defer GPU when transfer overhead dominates, validation fails, or thermal
  throttling erases the gain

Latest quick smoke on Sony SOG15, 2026-05-02, using the Android-side
`gles31_compute` backend:

| Kernel | CPU scalar total | GLES 3.1 total | CPU/GLES |
|---|---:|---:|---:|
| `vector_add_cold` n=262144 local_size=256 | 25.41 ms | 12.13 ms | 2.09x |
| `vector_add` stream n=262144 local_size=128 | 25.41 ms | 3.25 ms | 7.82x |
| `saxpy` n=262144 | 29.94 ms | 4.20 ms | 7.12x |
| `matmul_fp32` 64x64 | 33.00 ms | 1.14 ms | 28.91x |

This confirms that the benchmark can observe GPU speedup on this device for
workloads with enough arithmetic intensity. `vector_add_cold` measures the
one-shot path including shader compilation, upload, dispatch, and download.
The tuned `vector_add` stream path reuses the compiled kernel, selects the
fastest tested workgroup size, and still includes upload/dispatch/download
time. On the latest SOG15 smoke run the selected workgroup size is 128 and the
stream path is about 7.82x faster than the CPU scalar baseline.

Benchmark outputs should be JSON Lines and CSV under:

```text
/storage/emulated/0/Android/data/<package>/files/bench/
```

Device records should include model, SoC/GPU name, Android/API version, Vulkan
driver/API version, FP16 support, timestamp query support, battery/charging
state, and thermal state when available.

## Implementation roadmap

1. Minimal runner: CPU scalar/NEON `vector_add` and `saxpy`, shared validator,
   JSONL/CSV writer, device info collector.
2. Vulkan baseline: compute context, buffer upload/download, SPIR-V shader
   loading, CPU wall time and GPU timestamp timing when available.
3. Matrix baseline: CPU reference, simple Vulkan matmul, tiled Vulkan matmul,
   GFLOPS and numerical error reporting.
4. glibc bridge ABI: container-facing shim, shared-memory transport, command
   buffers, fence/error model, and Bionic GPU-executor lifecycle. The executor
   runs GPU commands only; application engines such as llama.cpp remain in the
   container. The container-facing ABI must be device-independent; backend
   differences are handled by executor capability probing and command lowering.
5. cuVK runtime: CUDA-shaped allocation/copy/module/kernel launch API backed by
   the bridge runtime.
6. CUDA subset transpiler: parse restricted CUDA-like kernels, emit GLSL/SPIR-V
   plus kernel metadata for buffer bindings and push constants.
7. LLM-oriented kernels: RMSNorm, RoPE, softmax, FP16/quantized matmul, with
   chained execution comparisons.

## Default dev workspace

The default workspace Compose file sets:

```yaml
gpus: all
```

and the default image includes `vulkan-tools` and `libvulkan1` so Vulkan
passthrough can be tested with commands such as:

```sh
vulkaninfo --summary
env | grep -E 'PDOCKER|CUDA|NVIDIA|VK_'
```

## llama.cpp profile diagnostics

The bundled `llama.cpp GPU workspace` template now records the first practical
diagnostic layer before the benchmark runner exists. Its
`scripts/pdocker-gpu-profile.sh` writes both:

- `profiles/pdocker-gpu.env` for the llama-server startup arguments
- `profiles/pdocker-gpu-diagnostics.json` for UI/log inspection

The JSON diagnostic captures the selected backend, the recommendation reason,
thread/context/GPU-layer choices, memory size, and the CUDA/Vulkan/OpenCL
signals that were visible in the container. Visibility is not performance
validation. The default profile now stays on CPU fallback when only raw Android
library signals are present; raw Vulkan exposure requires an explicit
diagnostic mode such as `PDOCKER_GPU_MODE=vulkan-raw`.
