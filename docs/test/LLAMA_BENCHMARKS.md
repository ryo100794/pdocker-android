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

## 2026-05-04 GPU Bridge Overhead Probe

The first shim-to-executor transport is now measurable with:

```sh
bash scripts/bench-gpu-bridge.sh 50 docs/test/gpu-bridge-bench-repeat50.json
```

This compares:

- direct APK-side executor loop;
- glibc shim bridge with one socket connection per command;
- glibc shim bridge with a persistent socket connection.
- glibc shim bridge passing a shared vector buffer FD with `SCM_RIGHTS`, both
  one-connection-per-command and persistent-connection forms.
- glibc shim bridge registering a shared vector buffer once, then reusing it
  for repeated commands on the same connection.

Important caveat: process-level wall time is not a fair direct-vs-bridge
comparison because the direct path includes Android executable startup while
the bridge path uses a server that is already resident. The useful early signal
is the warm internal `total_ms` reported by the GPU command itself.

Latest local repeat50 result:

- Direct executor warm total mean: 1.3851 ms.
- One-connection-per-command bridge warm total mean: 1.5915 ms, about 1.15x
  direct.
- Persistent bridge warm total mean: 1.2640 ms, within measurement noise of
  direct.
- NOOP wall per run: direct 13.4684 ms, bridge 2.9931 ms, persistent bridge
  2.1316 ms. This is mostly process/socket/stdio measurement overhead, not GPU
  work.

Follow-up repeat50 with explicit NOOP separation:

- Direct executor warm total mean: 1.3851 ms.
- Non-persistent bridge warm total mean: 1.5915 ms, about 1.15x direct.
- Persistent bridge warm total mean: 1.2640 ms, effectively noise-limited for
  this coarse GPU command.
- A later noisy repeat showed direct process wall time larger than bridge wall
  time, which is not a valid acceleration signal. It demonstrates why host
  executable startup and JSON/stdio wall measurements must not be used as the
  primary bridge overhead metric.

Interpretation:

- The direct host benchmark is too coarse if it includes executable startup,
  shader cache warmup, JSON output, or upload/download costs.
- Bridge tuning must therefore track NOOP/control overhead separately from
  upload, dispatch, and download.
- Persistent transport is mandatory; one socket connection per GPU command is
  useful only as a diagnostic worst case.

Conclusion: the current socket bridge is good enough as a tuning scaffold, but
the LLM path must use persistent transport, buffer reuse, and batched commands.
Single-command connection churn is explicitly not acceptable for real ggml
backend work. The next lower-overhead bridge target remains shared memory for
buffer tables plus a small persistent control channel for command submission
and fences.

Follow-up repeat8 after adding the FD-passed shared-buffer vector-add probe:

- Direct executor warm total mean: 2.0198 ms.
- One-connection-per-command bridge warm total mean: 2.7496 ms, about 1.36x
  direct.
- Persistent command bridge warm total mean: 2.3281 ms, about 1.15x direct.
- FD shared-buffer bridge warm total mean: 2.3776 ms, about 1.18x direct.
- Persistent FD shared-buffer bridge warm total mean: 2.2078 ms, about 1.09x
  direct.

This confirms the bridge can pass a container-owned shared buffer into the
Android GPU executor and receive validated output without exposing Android
vendor GPU libraries to the glibc process. It still allocates and maps a fresh
buffer per command, so it is a bridge substrate measurement, not the final
llama.cpp GPU backend.

Follow-up repeat8 after adding a registered shared-buffer probe:

- Direct executor warm total mean: 3.5770 ms.
- Persistent command bridge warm total mean: 1.7593 ms.
- Persistent FD shared-buffer bridge warm total mean: 3.1626 ms.
- Registered shared-buffer bridge warm total mean: 2.6267 ms.
- Registered shared-buffer wall per run: 25.2951 ms.

These short repeat8 values are noisy and the direct process path is still not
a fair wall-clock baseline because it includes process startup. The important
directional improvement is structural: the bridge can now register a buffer
once and submit repeated commands against it. The next optimization step is a
real multi-buffer table plus fences so llama/ggml operations can chain work
without repeated allocation, mapping, or per-operation connection setup.

The container-facing socket path is `/run/pdocker-gpu/pdocker-gpu.sock`.
pdockerd bind-maps the APK runtime GPU directory to `/run/pdocker-gpu`, and the
direct executor rewrites `connect(AF_UNIX)` socket paths through the bind map.
This avoids leaking Android app-data absolute paths into container code and
keeps the shim ABI portable.

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
