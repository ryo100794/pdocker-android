# pdocker Syscall Use-Case Profile

- Commit: `b0fbe38`
- Dirty tree: `True`
- Timestamp: `2026-05-06T07:21:44.536283+00:00`
- Device: `10.8.135.134:40455`
- Trace mode: `syscall`
- Small files: 16

## Use Cases

| use case | rc | real s | traced stops | reported syscalls | top syscall counts |
|---|---:|---:|---:|---:|---|
| shell_startup | 0 | 0.020 | 671 | 329 | newfstatat=47, mmap=44, openat=43, close=35, mprotect=24, read=19, munmap=19, brk=12 |
| package_metadata | 0 | 0.096 | 4099 | 2035 | newfstatat=1010, openat=180, mmap=162, close=117, read=111, mprotect=88, munmap=81, fcntl=34 |
| source_scan | 0 | 0.153 | 8652 | 4248 | read=724, mmap=565, openat=469, newfstatat=449, close=335, mprotect=303, munmap=279, brk=117 |
| object_write | 0 | 0.558 | 7300 | 3574 | newfstatat=507, openat=443, close=427, read=416, mmap=374, mprotect=198, munmap=176, brk=131 |
| overlay_shape | 0 | 0.534 | 6085 | 2968 | mmap=421, openat=400, newfstatat=368, close=319, mprotect=230, munmap=191, brk=114, read=105 |
| cleanup | 0 | 0.076 | 1407 | 695 | newfstatat=103, unlinkat=81, close=80, openat=79, fcntl=75, mmap=53, getdents64=43, mprotect=29 |

## Aggregate

| syscall | nr | count |
|---|---:|---:|
| newfstatat | 79 | 2484 |
| mmap | 222 | 1619 |
| openat | 56 | 1614 |
| read | 63 | 1396 |
| close | 57 | 1313 |
| mprotect | 226 | 872 |
| munmap | 215 | 769 |
| brk | 214 | 419 |
| fcntl | 25 | 391 |
| rt_sigprocmask | 135 | 279 |
| wait4 | 260 | 251 |
| dup3 | 24 | 221 |
| set_robust_list | 99 | 167 |
| faccessat | 48 | 162 |
| write | 64 | 143 |
| exit_group | 94 | 134 |
| execve | 221 | 132 |
| getrandom | 278 | 132 |
| prlimit64 | 261 | 132 |
| set_tid_address | 96 | 132 |
| clone | 220 | 128 |
| rt_sigreturn | 139 | 125 |
| getdents64 | 61 | 111 |
| unlinkat | 35 | 99 |
| rt_sigaction | 134 | 93 |
| mkdirat | 34 | 76 |
| statfs | 43 | 58 |
| chdir | 49 | 55 |
| lseek | 62 | 51 |
| fadvise64 | 223 | 48 |
| ioctl | 29 | 47 |
| umask | 166 | 39 |
| pipe2 | 59 | 33 |
| geteuid | 175 | 18 |
| linkat | 37 | 16 |
| sigaltstack | 132 | 16 |
| dup | 23 | 12 |
| getuid | 174 | 9 |
| faccessat2 | 439 | 6 |
| getegid | 177 | 6 |

## Interpretation

- `seccomp` mode shows the production mediation surface: path, credential, exec, memory guard, and compatibility syscalls that pdocker-direct actually intercepts.
- `syscall` mode shows the full syscall frequency picture, including fd-only calls such as `read`, `write`, `close`, `fstat`, and `lseek`.
- Compare both modes before optimizing: high frequency in `syscall` mode is harmless when the syscall is not intercepted in `seccomp` mode.
