# pdocker File I/O Microbenchmark

- Commit: `b0fbe38`
- Dirty tree: `True`
- Timestamp: `2026-05-06T09:11:36.339407+00:00`
- Device: `10.8.135.134:40455`
- Workload: files=32, blocks=16, block_size=4096, fsync=0
- Target: direct executor overhead <= 1 ms per file operation
- Result: PASS; max per-op overhead 0.387 ms

| operation | ops | native app ms | native rootfs ms | container rootfs ms | rootfs overhead ms/op | container app bind ms | app bind overhead ms/op | native docs ms | container docs bind ms | docs overhead ms/op |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| open_close | 1000 | 5.013 | 2.220 | 34.104 | 0.032 | 28.193 | 0.023 | 3.115 | 45.586 | 0.042 |
| openat_only | 1000 | 4.026 | 1.763 | 58.550 | 0.057 | 38.942 | 0.035 | 2.545 | 29.758 | 0.027 |
| close_only | 1000 | 0.932 | 0.507 | 0.638 | 0.000 | 0.649 | -0.000 | 0.493 | 0.700 | 0.000 |
| stat_same | 1000 | 0.949 | 0.836 | 36.622 | 0.036 | 57.569 | 0.057 | 1.653 | 56.489 | 0.055 |
| fstat_only | 1000 | 0.251 | 0.253 | 0.288 | 0.000 | 0.251 | -0.000 | 0.240 | 0.251 | 0.000 |
| lseek_only | 1000 | 0.181 | 0.180 | 0.191 | 0.000 | 0.191 | 0.000 | 0.168 | 0.192 | 0.000 |
| write_only | 16 | 0.079 | 0.071 | 0.074 | 0.000 | 0.072 | -0.000 | 0.086 | 0.616 | 0.033 |
| read_only | 16 | 0.015 | 0.014 | 0.015 | 0.000 | 0.015 | 0.000 | 0.015 | 0.019 | 0.000 |
| chmod_same | 32 | 0.039 | 0.033 | 0.946 | 0.029 | 5.648 | 0.175 | 0.061 | 12.456 | 0.387 |
| rename_pair | 64 | 0.418 | 0.429 | 8.308 | 0.123 | 11.378 | 0.171 | 0.597 | 15.382 | 0.231 |
| mkdir_rmdir_pair | 64 | 0.804 | 0.909 | 9.920 | 0.141 | 4.187 | 0.053 | 0.899 | 3.046 | 0.034 |
| symlink_unlink_pair | 64 | 0.808 | 0.888 | 11.532 | 0.166 | 4.085 | 0.051 | 0.925 | 14.828 | 0.217 |
| readlink_only | 32 | 0.030 | 0.040 | 1.070 | 0.032 | 0.722 | 0.022 | 0.058 | 0.558 | 0.016 |
| small_create | 32 | 0.854 | 0.843 | 2.967 | 0.066 | 1.742 | 0.028 | 0.934 | 5.033 | 0.128 |
| small_stat | 32 | 0.038 | 0.033 | 8.167 | 0.254 | 0.617 | 0.018 | 0.063 | 2.566 | 0.078 |
| small_read | 32 | 0.096 | 0.102 | 1.341 | 0.039 | 0.732 | 0.020 | 0.127 | 3.202 | 0.096 |
| unlink | 37 | 0.579 | 0.528 | 3.876 | 0.091 | 2.012 | 0.039 | 0.537 | 2.211 | 0.045 |

## Method

- All rows run the same static AArch64 benchmark binary.
- `open_close` opens and closes the same existing file 1000 times by default.
- `*_only` rows time a hot loop around the named syscall with setup and cleanup outside the measured region where practical.
- `*_pair` rows report the combined cost of the named syscall pair because the filesystem object must be returned to its starting state each iteration.
- `native app` uses the app-private pdocker bench folder without the container executor.
- `native rootfs` executes directly in the APK domain against the same rootfs backing path used by the container.
- `container rootfs` executes through `pdocker-direct`; the same executor behavior is used as normal container execution.
- `container app bind` binds an app-private folder into the container and repeats the same workload.
- `native/container documents` run only when a direct writable Documents/SD path is configured or passed with `--documents-host`.
- Timing is measured inside the benchmark process around direct file syscall loops, not around shell command startup.
