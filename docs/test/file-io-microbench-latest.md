# pdocker File I/O Microbenchmark

- Commit: `89173db`
- Timestamp: `2026-05-06T05:06:43.413519+00:00`
- Device: `10.8.135.134:39827`
- Workload: files=64, blocks=64, block_size=4096, fsync=0
- Target: direct executor overhead <= 10 ms per operation group
- Result: PASS; max overhead 5.418 ms

| operation | native rootfs ms | container rootfs ms | overhead ms | ratio | target |
|---|---:|---:|---:|---:|---|
| seq_write | 0.438 | 0.268 | -0.170 | 0.61 | PASS |
| seq_read | 0.096 | 0.148 | 0.052 | 1.54 | PASS |
| small_create | 2.785 | 5.821 | 3.035 | 2.09 | PASS |
| small_stat | 0.251 | 5.668 | 5.418 | 22.60 | PASS |
| small_read | 0.404 | 1.338 | 0.934 | 3.31 | PASS |
| unlink | 1.768 | 5.856 | 4.088 | 3.31 | PASS |

## Method

- Both paths run the same static AArch64 benchmark binary stored in the rootfs.
- `native rootfs` executes that binary directly in the APK domain against the rootfs backing path.
- `container rootfs` executes the same binary through `pdocker-direct`, so ptrace/seccomp path mediation remains active.
- Timing is measured inside the benchmark process around direct file syscall loops, not around shell command startup.
