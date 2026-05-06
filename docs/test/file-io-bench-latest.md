# pdocker File I/O Benchmark

- Commit: `89173db`
- Timestamp: `2026-05-06T04:49:30.489637+00:00`
- Size: 4 MiB sequential, 64 small files
- Trace mode: `seccomp`
- Persistent noop: native app 0.138 ms/run, native rootfs 0.113 ms/run, container rootfs 0.041 ms/run
- Rootfs persistent noop delta: signed -0.071 ms/run, absolute 0.071 ms/run over 200 runs (target absolute delta <= 10 ms)
- Device: `10.8.135.134:39827`
- Host rc: native `0`, native persistent `0`, native rootfs persistent `0`, container `0`, container persistent `0`

| operation | native launch s | native app persistent s | native rootfs persistent s | container launch s | container rootfs persistent s | launch ratio | rootfs persistent ratio | native rootfs MiB/s | container rootfs MiB/s | stops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| noop | 0.007 |  |  | 0.093 |  | 12.64 |  |  |  | 137 |
| seq_write | 0.023 | 0.039 | 0.043 | 0.075 | 0.011 | 3.21 | 0.25 | 192.1 | 1443.3 | 171 |
| seq_read | 0.021 | 0.042 | 0.028 | 0.100 | 0.019 | 4.89 | 0.68 | 712.0 | 365.6 | 171 |
| small_create | 0.705 | 0.953 | 0.913 | 0.084 | 0.009 | 0.12 | 0.01 |  |  | 203 |
| small_stat | 0.008 | 0.037 | 0.039 | 0.067 | 0.010 | 7.88 | 0.26 |  |  | 203 |
| small_read | 0.019 | 0.049 | 0.049 | 0.062 | 0.011 | 3.25 | 0.23 |  |  | 300 |
| compile_prepare | 1.692 | 1.619 | 1.653 | 0.284 | 0.200 | 0.17 | 0.12 |  |  | 2694 |
| compile_scan | 0.856 | 0.898 | 0.743 | 0.259 | 0.211 | 0.30 | 0.28 |  |  | 2174 |
| compile_objects | 2.648 | 3.064 | 2.450 | 0.372 | 0.335 | 0.14 | 0.14 |  |  | 4222 |
| compile_archive | 0.019 | 0.051 | 0.046 | 0.065 | 0.009 | 3.35 | 0.20 |  |  | 301 |
| overlay_prepare | 2.114 | 2.037 | 1.455 | 0.298 | 0.115 | 0.14 | 0.08 |  |  | 2008 |
| overlay_copyup_write | 0.970 | 0.861 | 0.879 | 0.065 | 0.010 | 0.07 | 0.01 |  |  | 204 |
| overlay_truncate | 0.018 | 0.026 | 0.030 | 0.064 | 0.006 | 3.62 | 0.21 |  |  | 204 |
| overlay_unlink | 1.126 | 0.878 | 0.622 | 0.208 | 0.146 | 0.18 | 0.23 |  |  | 1871 |

## Interpretation

- `noop` is the process/direct-executor startup floor; adjusted launch MiB/s subtracts that floor.
- `native app persistent`, `native rootfs persistent`, and `container rootfs persistent` run the same payload inside one long-lived shell, separating steady-state mediation cost from launch cost at the same measurement granularity.
- `native rootfs persistent` writes into the same rootfs backing tree used by the container path, but still uses Android `/system/bin/sh` and Android userland tools.
- Small-file rows emphasize path mediation, metadata syscalls, and shell loop overhead.
- Compile rows emulate build-system traffic without requiring a compiler: source-tree fanout, dependency scanning, object/dep file writes, and archive concatenation.
- Overlay rows target the pdocker layer/COW shape: hardlink-shared lower/upper files, first-write copy-up via `/.libcow.so` when present, truncate, and unlink-style cleanup.
- Sequential rows emphasize bulk read/write throughput through the mediated rootfs.
