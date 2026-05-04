# GPU Host-Native Baseline

- Date: 20260504T114320Z UTC.
- Runs: 5.
- Scope: Android native executor inside the APK app process domain.
- This is not CPU emulation; Vulkan samples use the Android Vulkan backend.

| Probe | Backend | Valid | Steady median ms | Steady mean ms | Dispatch median ms | Transport |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| Host CPU matmul256 | cpu_scalar | 5/5 | 34.8216 | 37.7788 | 34.8216 | host-cpu-local-process-buffer |
| Host Vulkan matmul256 resident | android_vulkan | 5/5 | 0.6666 | 1.0253 | 0.6558 | direct-vulkan-resident-buffer |
| Host CPU vector-add | cpu_scalar | 5/5 | 0.0667 | 0.0872 | 0.0667 | host-cpu-local-process-buffer |
| Host Vulkan vector-add resident | android_vulkan | 5/5 | 0.5002 | 0.5523 | 0.4501 | direct-vulkan-resident-buffer |

## Ratios

- Host CPU matmul256 / host Vulkan resident matmul256: 52.2337x.
- Host Vulkan resident matmul256 / host CPU matmul256: 0.0191x.
- Host CPU vector-add / host Vulkan resident vector-add: 0.1333x.
- Host Vulkan resident vector-add / host CPU vector-add: 7.5000x.

Interpretation: matmul is the useful LLM-shaped probe. Vector-add is intentionally retained as a transfer/dispatch overhead canary, and CPU may win there.
