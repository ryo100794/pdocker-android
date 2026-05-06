# pdocker File I/O Microbenchmark

- Commit: `731919b`
- Timestamp: `2026-05-06T05:17:20.450641+00:00`
- Device: `10.8.135.134:39827`
- Workload: files=64, blocks=64, block_size=4096, fsync=0
- Target: direct executor overhead <= 1 ms per file operation
- Result: PASS; max group overhead 3.912 ms, max per-op overhead 0.060 ms

| operation | native rootfs ms | container rootfs ms | overhead ms | overhead ms/op | ratio | target |
|---|---:|---:|---:|---:|---:|---|
| seq_write | 1.119 | 0.302 | -0.817 | -0.013 | 0.27 | PASS |
| seq_read | 0.135 | 0.081 | -0.054 | -0.001 | 0.60 | PASS |
| small_create | 4.028 | 4.285 | 0.257 | 0.004 | 1.06 | PASS |
| small_stat | 0.192 | 1.481 | 1.289 | 0.020 | 7.72 | PASS |
| small_read | 0.600 | 1.747 | 1.147 | 0.018 | 2.91 | PASS |
| unlink | 2.484 | 6.395 | 3.912 | 0.060 | 2.57 | PASS |

## Method

- Both paths run the same static AArch64 benchmark binary stored in the rootfs.
- `native rootfs` executes that binary directly in the APK domain against the rootfs backing path.
- `container rootfs` executes the same binary through `pdocker-direct`; the same executor behavior is used as normal container execution.
- Timing is measured inside the benchmark process around direct file syscall loops, not around shell command startup.
