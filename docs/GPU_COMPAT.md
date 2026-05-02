# pdocker GPU compatibility extensions

Snapshot date: 2026-05-01.

pdocker has an experimental Docker-compatible GPU request surface. It is
designed for Android devices where native Docker GPU runtimes such as
`nvidia-container-runtime` do not exist.

## Design principle

pdocker treats Android GPU support as a Vulkan-first compatibility stack.
Native NVIDIA CUDA is not expected on ordinary Android phones; it is only an
external baseline for Jetson or NVIDIA Linux devices. The Android path is:

```text
Docker --gpus / HostConfig.DeviceRequests
  -> pdocker GPU negotiation
  -> Vulkan passthrough when the device exposes usable Vulkan libraries
  -> cuVK, a restricted CUDA-like API lowered to Vulkan Compute
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

- `docker run --gpus all ...` is accepted by `pdockerd`.
- Docker `HostConfig.DeviceRequests` with `Driver=nvidia` or GPU capabilities
  maps to pdocker GPU modes:
  - `vulkan`
  - `cuda-compat`
- Containers receive environment variables such as:
  - `PDOCKER_GPU=1`
  - `PDOCKER_GPU_MODES=cuda-compat,vulkan`
  - `PDOCKER_VULKAN_PASSTHROUGH=1`
  - `PDOCKER_CUDA_COMPAT=1`
  - `NVIDIA_VISIBLE_DEVICES=all`
  - `NVIDIA_DRIVER_CAPABILITIES=compute,utility,graphics`
- Android Vulkan device/library paths are bind-passed when present:
  - `/dev/kgsl-3d0`
  - `/dev/dri`
  - `/system/lib64/libvulkan.so`
  - `/vendor/lib64/libvulkan.so`
  - `/vendor/lib64/egl`
  - `/vendor/lib64/hw`
  - `/system/etc/vulkan`
  - `/vendor/etc/vulkan`

## Vulkan

Vulkan is a best-effort passthrough. pdockerd writes a minimal ICD file at:

```text
/etc/vulkan/icd.d/pdocker-android.json
```

and points `VK_ICD_FILENAMES` at it.

Whether `vulkaninfo` succeeds depends on vendor driver behavior, Android
device permissions, and whether the Linux userland Vulkan loader can use the
Android driver library from the container process.

## CUDA-compatible API

`cuda-compat` is not NVIDIA CUDA passthrough. Android normally does not expose
NVIDIA `/dev/nvidia*` devices or the NVIDIA driver ABI. In pdocker, CUDA means
a planned compatibility API layer which can provide a CUDA-shaped userspace ABI
backed primarily by Vulkan Compute.

The current implementation handles negotiation and container environment setup.
The actual `libcuda.so`/runtime shim is still pending.

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
which writes CPU scalar reference results as JSON Lines and CSV under
`files/pdocker/bench/` and mirrors them to the app external files `bench/`
directory when Android storage policy allows it. This is the baseline artifact
format for later Vulkan and cuVK runs, not a claim that GPU acceleration is
available yet.

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
4. cuVK runtime: CUDA-shaped allocation/copy/module/kernel launch API backed by
   the Vulkan runtime.
5. CUDA subset transpiler: parse restricted CUDA-like kernels, emit GLSL/SPIR-V
   plus kernel metadata for buffer bindings and push constants.
6. LLM-oriented kernels: RMSNorm, RoPE, softmax, FP16/quantized matmul, with
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
thread/context/GPU-layer choices, memory size, and the CUDA/Vulkan signals that
were visible in the container. This is not a performance validation yet; it is
the visible bridge between Docker-compatible `gpus: all` negotiation and the
future benchmark gate above.
