# GPU Host/Container Comparison

- Date: 20260504T073600Z UTC.
- Container: `device-smoke-app-1`.
- Runs: 8.
- Warmup samples discarded per series: 3.

| Scope | Backend | Valid | Steady median ms | Steady mean ms | Steady dispatch mean ms | Wall ms/call | Transport |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| Host CPU | cpu_scalar | 8/8 | 0.0836 | 0.0942 | 0.0942 | 108.7612 | host-cpu-local-process-buffer |
| Host GPU Vulkan transfer | android_vulkan | 8/8 | 5.5497 | 5.3298 | 0.5685 | 111.4332 | direct-vulkan-local-process-buffer |
| Host GPU Vulkan resident | android_vulkan | 8/8 | 0.6522 | 0.8039 | 0.7524 | 97.1247 | direct-vulkan-resident-buffer |
| Host CPU matmul256 | cpu_scalar | 8/8 | 34.9056 | 34.8904 | 34.8904 | 105.6037 | host-cpu-local-process-buffer |
| Host GPU Vulkan matmul256 resident | android_vulkan | 8/8 | 0.9234 | 0.9258 | 0.9031 | 106.2353 | direct-vulkan-resident-buffer |
| Container CPU | cpu_scalar | 8/8 | 0.1042 | 0.1024 | 0.1024 | 108.8227 | container-local-process-buffer |
| Bridge NOOP | bridge_roundtrip | 8/8 | 0.1245 | 0.1263 | 0.0000 | 74.7900 | unix-socket-command-queue-persistent |
| Container GPU Vulkan bridge | android_vulkan | 8/8 | 3.3530 | 3.3319 | 0.4897 | 103.7662 | vulkan-icd-scm-rights-3buffer |

## Ratios

- Container GPU / host GPU steady median total: 0.6042x.
- Container GPU / host resident GPU steady median total: 5.1411x.
- Host resident Vulkan / host transfer Vulkan steady median total: 0.1175x.
- Host GPU resident matmul256 / host CPU matmul256 steady median total: 0.0265x.
- Host CPU matmul256 / host GPU resident matmul256 steady median total: 37.8012x.
- Container CPU / host CPU steady median total: 1.2464x.
- Bridge NOOP round trip inside container process: 0.1245 ms/call.
- Direct-executor wall time for the bridge NOOP measurement: 74.7900 ms/call.

The direct-executor wall time includes starting and tracing the benchmark process; use the bridge NOOP round-trip row for the command-queue crossing cost.
