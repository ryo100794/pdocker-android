# GPU Host/Container Comparison

- Date: 20260504T071147Z UTC.
- Container: `pdocker-llama-cpp`.
- Runs: 5.

| Scope | Backend | Valid | Warm total mean ms | Dispatch mean ms | Wall ms/call | Transport |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| Host CPU | cpu_scalar | 5/5 | 0.0965 | 0.0964 | 161.5679 | host-cpu-local-process-buffer |
| Host GPU Vulkan | android_vulkan | 5/5 | 5.9220 | 0.8250 | 156.9952 | direct-vulkan-local-process-buffer |
| Container CPU | cpu_scalar | 5/5 | 0.1702 | 0.1545 | 422.8105 | container-local-process-buffer |
| Bridge NOOP | bridge_roundtrip | 5/5 | 0.1747 | 0.0000 | 349.1574 | unix-socket-command-queue-persistent |
| Container GPU Vulkan bridge | android_vulkan | 5/5 | 7.5369 | 1.9424 | 172.6085 | vulkan-icd-scm-rights-3buffer |

## Ratios

- Container GPU / host GPU warm total: 1.2727x.
- Container CPU / host CPU warm total: 1.7637x.
- Bridge NOOP round trip inside container process: 0.1747 ms/call.
- Direct-executor wall time for the bridge NOOP measurement: 349.1574 ms/call.

The direct-executor wall time includes starting and tracing the benchmark process; use the bridge NOOP round-trip row for the command-queue crossing cost.
