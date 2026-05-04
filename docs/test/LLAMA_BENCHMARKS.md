# llama.cpp Runtime Benchmarks

This document records repeatable llama.cpp measurements from pdocker Android
runtime runs. Keep the latest machine-readable result in
`docs/test/llama-bench-latest.json` and copy it to the device bench directory
with `scripts/android-llama-bench.sh`.

## How To Run

1. Start the llama.cpp project from the app or Engine compose path.
2. Wait until `http://127.0.0.1:18081/` returns HTTP 200.
3. Run:

```sh
bash scripts/android-llama-bench.sh --predict 8 --repeat 1
```

Use the same prompt, token count, and model when comparing CPU fallback with
future Vulkan/CUDA-compatible runs.

## 2026-05-03 Vulkan-Requested Result

- Local path: `docs/test/llama-bench-vulkan-requested-repeat3.json`.
- Device path: `files/pdocker/bench/llama-bench-vulkan-requested-repeat3.json`.
- Mode: Vulkan requested through Docker-compatible `gpus: all`.
- Engine state: `PdockerGpu.Modes=["cuda-compat","vulkan"]` during the HTTP
  run; subsequent OpenCL wiring expands this to include `opencl`.
- Container health: `docker ps` reports `Up (healthy)` after the healthcheck
  settles.
- HTTP generation speed: 0.171 tokens/s mean, 0.162 min, 0.185 max.
- Mean wall time: 58.58s for 8 generated tokens.

This is slower than the CPU fallback baseline. The request path and raw
diagnostic exposure are working, but acceleration is not active:
`llama-server` still reports CPU model, KV, and compute buffers. After fixing
the direct runtime COW mis-detection, the Vulkan ICD file is visible in the
container, but `vulkaninfo --summary` fails because Android's
`/system/lib64/libvulkan.so` depends on Android/Bionic libraries such as
`android.hardware.configstore@1.0.so` that are not loadable from the
Ubuntu/glibc process.

Conclusion: directly exposing Android host GPU libraries into the image is a
diagnostic dead end, not the production design. The container-facing side must
remain glibc-compatible and talk to an APK-owned Android/Bionic GPU command
executor through a thin pdocker bridge. The executor is not a host-side
llama.cpp RPC inference service; `llama-server`, model loading, tokenization,
sampling, and HTTP serving stay inside the container. The bridge contract must
be device-independent so benchmarks compare the same LLM workload while the
executor absorbs GLES/Vulkan/OpenCL/vendor differences underneath.

Official `llama-bench` with `-p 16 -n 8 -r 2 -ngl 999 -t 8` is stored at
`docs/test/llama-bench-tool-vulkan-requested-p16-n8-r2.json`:

- Prompt processing: 1.76 tokens/s average.
- Token generation: 0.248 tokens/s average.

## 2026-05-03 OpenCL Probe

OpenCL was probed after the Vulkan check. The device exposes
`/vendor/lib64/libOpenCL.so`, and pdocker now maps an OpenCL request into:

- `PdockerGpu.Modes` containing `opencl`
- `PDOCKER_OPENCL_PASSTHROUGH=1`
- `OCL_ICD_VENDORS=/etc/OpenCL/vendors`
- `/etc/OpenCL/vendors/pdocker-android.icd`

The library is visible inside the container, but `ctypes.CDLL` fails with
`liblog.so: cannot open shared object file`. `readelf -d` shows the Android
OpenCL library depends on Bionic/Android libraries (`liblog.so`,
`libcutils.so`, `libc++.so`, `libc.so`, `libm.so`, `libdl.so`). So the OpenCL
result matches Vulkan: passthrough metadata and file exposure work as
diagnostics, but direct loading from a glibc container is not a working GPU
backend. OpenCL also needs the glibc bridge plus Android/Bionic GPU-executor
model.

## 2026-05-04 GPU Executor Boundary Probe

The APK now includes `pdocker-gpu-executor`, a Bionic-side command executor
probe for the future device-independent `pdocker-gpu-command-v1` ABI. This is
not an LLM engine and does not move llama.cpp out of the container.

Local executor capability probe:

- API: `pdocker-gpu-command-v1`.
- ABI version: `0.1`.
- Role: `gpu-command-executor`.
- LLM engine location: `container`.
- Current implementation backend: `gles31_compute`.

Local vector-add self-test on 2026-05-04:

- Kernel: `vector_add`, n=262144.
- compile: 35.2674 ms.
- upload: 2.4036 ms.
- dispatch: 6.6698 ms.
- download: 0.9055 ms.
- total: 45.2464 ms.
- max absolute error: 0.0.
- valid: true.

Installed compat APK executor self-test via `run-as` on 2026-05-04:

- Kernel: `vector_add`, n=262144.
- compile: 24.6921 ms.
- upload: 2.4523 ms.
- dispatch: 21.3822 ms.
- download: 1.7867 ms.
- total: 50.3134 ms.
- max absolute error: 0.0.
- valid: true.

This proves the APK-owned executor can run an Android GPU command behind the
neutral ABI. It does not yet prove container llama.cpp acceleration; the next
step is the glibc shim plus shared-memory command queue used by the container
process.

## 2026-05-04 Container Shim Probe

The Linux/glibc container-facing shim is now built as `pdocker-gpu-shim` and
packaged in the APK as `libpdockergpushim.so`. pdockerd bind-mounts it into
GPU-requesting containers at `/usr/local/bin/pdocker-gpu-shim`.

Local capability output:

```json
{"shim":"pdocker-gpu-shim","api":"pdocker-gpu-command-v1","abi_version":"0.1","llm_engine":"container","device_independent":true,"container_contract":"glibc-shim-command-queue","executor_available":false,"executor_role":"apk-bionic-gpu-command-executor","transport":"command-queue-pending","backend_impl_visible_to_container":false}
```

This confirms the container-visible contract remains device-independent. It is
not an acceleration claim until `transport` changes from
`command-queue-pending` to a validated queue implementation.

## Latest HTTP API Result

- Date: 2026-05-03 UTC.
- Local path: `docs/test/llama-bench-cpu-repeat3.json`.
- Latest alias: `docs/test/llama-bench-latest.json`.
- Device path: `files/pdocker/bench/llama-bench-cpu-repeat3.json`.
- Mode: CPU fallback baseline.
- Model: Qwen3 8B GGUF, Q4_K_M, 8.19B parameters.
- Prompt tokens: 6.
- Generated tokens: 8.
- Repetitions: 3.
- Mean wall time: 37.32s.
- Generation speed: 0.260 tokens/s mean, 0.239 min, 0.286 max.

Per-run generation speeds:

- Run 1: 0.255 tokens/s, 39.26s wall.
- Run 2: 0.239 tokens/s, 38.58s wall.
- Run 3: 0.286 tokens/s, 34.11s wall.

## Latest llama-bench Tool Result

The official llama.cpp `llama-bench` tool was built inside the existing
container with:

```sh
cmake --build /opt/llama.cpp/build --target llama-bench --parallel 2
```

The repeatable wrapper is:

```sh
bash scripts/android-llama-tool-bench.sh
```

Latest local path: `docs/test/llama-bench-tool-cpu-p16-n8-r3.json`.

Parameters:

- `-m /models/model.gguf`
- `-p 16`
- `-n 8`
- `-r 3`
- `-ngl 0`
- `-t 8`

Results:

- Prompt processing: 2.40 tokens/s average, samples 2.11, 2.58, 2.51.
- Token generation: 0.228 tokens/s average, samples 0.210, 0.260, 0.215.
- Backend reported by llama-bench: BLAS/OpenBLAS, CPU.

GPU status for this run: CPU fallback. The container diagnostics reported no
Vulkan passthrough, no CUDA-compatible signal, no `VK_ICD_FILENAMES`, and no
usable GPU device inside the container. Future GPU-enabled runs should preserve
this same benchmark shape and record the GPU diagnostic evidence next to the
tokens/s result.
