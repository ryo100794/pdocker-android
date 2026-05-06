# pdocker Syscall Use-Case Profile

- Commit: `b0fbe38`
- Dirty tree: `True`
- Timestamp: `2026-05-06T07:47:16.368681+00:00`
- Device: `10.8.135.134:40455`
- Trace mode: `seccomp`
- Small files: 64
- Path micro profile: `1`

## Use Cases

| use case | rc | real s | traced stops | reported syscalls | top syscall counts |
|---|---:|---:|---:|---:|---|
| shell_startup | 0 | 0.057 | 137 | 108 | newfstatat=47, openat=43, faccessat=5, execve=4, geteuid=3, statfs=2, getuid=1, getgid=1 |
| package_metadata | 0 | 0.072 | 1299 | 1236 | newfstatat=1010, openat=180, faccessat=13, statfs=8, execve=8, readlinkat=4, getuid=4, setgid=3 |
| source_scan | 0 | 0.311 | 5095 | 4014 | openat=1621, newfstatat=1457, mkdirat=262, faccessat=202, chdir=195, execve=135, statfs=134, geteuid=3 |
| object_write | 0 | 0.140 | 4351 | 3536 | newfstatat=1707, openat=1547, faccessat=136, execve=134, statfs=4, geteuid=3, chdir=1, getuid=1 |
| overlay_shape | 0 | 0.319 | 4001 | 3059 | openat=1408, newfstatat=1232, faccessat=136, execve=134, unlinkat=65, linkat=64, mkdirat=6, statfs=4 |
| cleanup | 0 | 0.057 | 838 | 475 | unlinkat=273, newfstatat=103, openat=79, faccessat=6, execve=5, geteuid=3, statfs=2, getuid=1 |

## Path Micro Profile

| use case | calls | avg us | read us | relative validate us | resolve us | validate us | write us | validate calls | validation cache hit % | realpath cache hit % | invalidations | realpath full us | parent realpath us |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| shell_startup | 90 | 6.239 | 151.721 | 44.688 | 11.926 | 306.511 | 24.220 | 15 | 0.0 | 14.3 | 0 | 215.779 | 39.428 |
| package_metadata | 1190 | 3.891 | 823.067 | 27.188 | 153.422 | 2937.757 | 499.527 | 972 | 64.0 | 45.6 | 0 | 1462.541 | 199.313 |
| source_scan | 3341 | 2.285 | 4347.725 | 903.742 | 229.747 | 1386.976 | 340.888 | 575 | 77.7 | 41.2 | 1 | 840.364 | 177.137 |
| object_write | 3254 | 2.359 | 2815.150 | 3435.834 | 152.824 | 612.859 | 307.818 | 634 | 62.8 | 37.2 | 0 | 2103.440 | 102.603 |
| overlay_shape | 2711 | 4.451 | 5122.127 | 70.417 | 265.106 | 5426.093 | 652.350 | 360 | 1.7 | 2.8 | 129 | 3083.285 | 1524.479 |
| cleanup | 455 | 7.634 | 383.128 | 2576.984 | 19.420 | 397.136 | 33.333 | 336 | 8.3 | 1.9 | 273 | 399.326 | 1581.982 |

## Aggregate

| syscall | nr | count |
|---|---:|---:|
| newfstatat | 79 | 5556 |
| openat | 56 | 4878 |
| faccessat | 48 | 498 |
| execve | 221 | 420 |
| unlinkat | 35 | 339 |
| mkdirat | 34 | 268 |
| chdir | 49 | 199 |
| statfs | 43 | 154 |
| linkat | 37 | 64 |
| geteuid | 175 | 18 |
| getuid | 174 | 9 |
| faccessat2 | 439 | 6 |
| getegid | 177 | 6 |
| getgid | 176 | 6 |
| readlinkat | 78 | 4 |
| setgid | 144 | 3 |

## Interpretation

- `seccomp` mode shows the production mediation surface: path, credential, exec, memory guard, and compatibility syscalls that pdocker-direct actually intercepts.
- `syscall` mode shows the full syscall frequency picture, including fd-only calls such as `read`, `write`, `close`, `fstat`, and `lseek`.
- Compare both modes before optimizing: high frequency in `syscall` mode is harmless when the syscall is not intercepted in `seccomp` mode.
