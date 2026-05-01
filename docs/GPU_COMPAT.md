# pdocker GPU compatibility extensions

Snapshot date: 2026-05-01.

pdocker has an experimental Docker-compatible GPU request surface. It is
designed for Android devices where native Docker GPU runtimes such as
`nvidia-container-runtime` do not exist.

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
backed by Vulkan compute, OpenCL, NNAPI, or another Android GPU path.

The current implementation handles negotiation and container environment setup.
The actual `libcuda.so`/runtime shim is still pending.

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

