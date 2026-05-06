# pdocker Syscall Use-Case Profile

- Commit: `b0fbe38`
- Dirty tree: `True`
- Timestamp: `2026-05-06T07:36:58.290636+00:00`
- Device: `10.8.135.134:40455`
- Trace mode: `seccomp`
- Small files: 64
- Path micro profile: `1`

## Use Cases

| use case | rc | real s | traced stops | reported syscalls | top syscall counts |
|---|---:|---:|---:|---:|---|
| shell_startup | 0 | 0.030 | 137 | 108 | newfstatat=47, openat=43, faccessat=5, execve=4, geteuid=3, statfs=2, getuid=1, getgid=1 |
| package_metadata | 0 | 0.095 | 1299 | 1236 | newfstatat=1010, openat=180, faccessat=13, statfs=8, execve=8, readlinkat=4, getuid=4, setgid=3 |
| source_scan | 0 | 0.514 | 5094 | 4014 | openat=1621, newfstatat=1457, mkdirat=262, faccessat=202, chdir=195, execve=135, statfs=134, geteuid=3 |
| object_write | 0 | 0.148 | 4351 | 3536 | newfstatat=1707, openat=1547, faccessat=136, execve=134, statfs=4, geteuid=3, chdir=1, getuid=1 |
| overlay_shape | 0 | 0.124 | 3936 | 3059 | openat=1408, newfstatat=1232, faccessat=136, execve=134, unlinkat=65, linkat=64, mkdirat=6, statfs=4 |
| cleanup | 0 | 0.021 | 510 | 475 | unlinkat=273, newfstatat=103, openat=79, faccessat=6, execve=5, geteuid=3, statfs=2, getuid=1 |

## Path Micro Profile

| use case | calls | avg us | read us | relative validate us | resolve us | validate us | write us | validate calls | realpath full us | parent realpath us | parent loops |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| shell_startup | 90 | 22.193 | 87.134 | 25.781 | 7.553 | 1850.730 | 14.116 | 15 | 1808.175 | 35.052 | 0 |
| package_metadata | 1190 | 15.319 | 896.620 | 14.271 | 165.565 | 16358.536 | 597.040 | 972 | 11003.741 | 4871.082 | 1 |
| source_scan | 3341 | 10.844 | 12603.071 | 9018.500 | 857.913 | 11460.461 | 1038.229 | 575 | 11049.692 | 6964.530 | 0 |
| object_write | 3254 | 3.910 | 3037.794 | 4077.441 | 165.888 | 4719.950 | 351.307 | 634 | 5612.543 | 1945.473 | 0 |
| overlay_shape | 2711 | 3.755 | 2667.502 | 55.938 | 181.983 | 6660.575 | 298.277 | 360 | 5596.151 | 956.299 | 0 |
| cleanup | 455 | 7.403 | 349.384 | 2600.103 | 13.699 | 326.612 | 25.470 | 336 | 453.746 | 1669.008 | 0 |

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
