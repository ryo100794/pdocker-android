#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <errno.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <linux/elf.h>

extern char **environ;

static int g_trace_verbose = 0;

#define TRACE_LOG(...) do { if (g_trace_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

static ssize_t pdocker_process_vm_readv(pid_t pid,
                                        const struct iovec *local_iov,
                                        unsigned long liovcnt,
                                        const struct iovec *remote_iov,
                                        unsigned long riovcnt,
                                        unsigned long flags) {
    return (ssize_t)syscall(__NR_process_vm_readv, pid, local_iov, liovcnt,
                            remote_iov, riovcnt, flags);
}

static ssize_t pdocker_process_vm_writev(pid_t pid,
                                         const struct iovec *local_iov,
                                         unsigned long liovcnt,
                                         const struct iovec *remote_iov,
                                         unsigned long riovcnt,
                                         unsigned long flags) {
    return (ssize_t)syscall(__NR_process_vm_writev, pid, local_iov, liovcnt,
                            remote_iov, riovcnt, flags);
}

static void usage(FILE *stream) {
    fprintf(stream,
            "usage: pdocker-direct --pdocker-direct-probe\n"
            "       pdocker-direct run --mode MODE --rootfs PATH --workdir PATH [--env KEY=VAL] [--bind SPEC] -- ARGV...\n");
}

static const char *value_after(int *index, int argc, char **argv, const char *name) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "pdocker-direct-executor: missing value for %s\n", name);
        exit(2);
    }
    *index += 1;
    return argv[*index];
}

static int file_starts_with(const char *path, const char *magic) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[4] = {0};
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return n >= strlen(magic) && memcmp(buf, magic, strlen(magic)) == 0;
}

static const char *syscall_name(long nr) {
    switch (nr) {
        case 17: return "getcwd";
        case 29: return "ioctl";
        case 34: return "mkdirat";
        case 35: return "unlinkat";
        case 48: return "faccessat";
        case 49: return "chdir";
        case 56: return "openat";
        case 57: return "close";
        case 61: return "getdents64";
        case 63: return "read";
        case 64: return "write";
        case 78: return "readlinkat";
        case 79: return "newfstatat";
        case 93: return "exit";
        case 94: return "exit_group";
        case 96: return "set_tid_address";
        case 98: return "futex";
        case 99: return "set_robust_list";
        case 117: return "ptrace";
        case 134: return "rt_sigaction";
        case 135: return "rt_sigprocmask";
        case 160: return "uname";
        case 167: return "prctl";
        case 172: return "getpid";
        case 174: return "getuid";
        case 175: return "geteuid";
        case 176: return "getgid";
        case 177: return "getegid";
        case 178: return "gettid";
        case 179: return "sysinfo";
        case 197: return "socket";
        case 198: return "socketpair";
        case 203: return "connect";
        case 214: return "brk";
        case 215: return "munmap";
        case 220: return "clone";
        case 221: return "execve";
        case 222: return "mmap";
        case 226: return "mprotect";
        case 227: return "msync";
        case 233: return "madvise";
        case 242: return "sched_getaffinity";
        case 260: return "wait4";
        case 261: return "prlimit64";
        case 278: return "getrandom";
        case 280: return "bpf";
        case 281: return "execveat";
        case 291: return "statx";
        case 293: return "rseq";
        case 439: return "faccessat2";
        case 440: return "process_madvise";
        case 441: return "epoll_pwait2";
        case 443: return "quotactl_fd";
        case 446: return "landlock_restrict_self";
        case 448: return "process_mrelease";
        case 449: return "futex_waitv";
        case 450: return "set_mempolicy_home_node";
        default: return "?";
    }
}

static int get_regs(pid_t pid, struct user_pt_regs *regs) {
    struct iovec iov;
    memset(regs, 0, sizeof(*regs));
    iov.iov_base = regs;
    iov.iov_len = sizeof(*regs);
    return ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

static int set_regs(pid_t pid, struct user_pt_regs *regs) {
    struct iovec iov;
    iov.iov_base = regs;
    iov.iov_len = sizeof(*regs);
    return ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
}

static int syscall_emulate_success(long nr) {
    return nr == 99 ||   /* set_robust_list: Android app seccomp blocks glibc robust futex setup. */
           nr == 293 ||  /* rseq: glibc can continue when registration appears unavailable. */
           nr == 439;    /* faccessat2: provide a permissive compatibility answer until errno mapping lands. */
}

static long syscall_remap_number(long nr) {
    if (nr == 439) return 48;  /* faccessat2 -> faccessat; Android app seccomp blocks the newer form. */
    return nr;
}

typedef struct TraceeState {
    pid_t pid;
    int active;
    int in_syscall;
    long last_nr;
    long emulated_nr;
    long last_emulated_nr;
    unsigned long long last_args[6];
} TraceeState;

#define MAX_TRACEES 128

static TraceeState *find_tracee(TraceeState *tracees, pid_t pid) {
    for (int i = 0; i < MAX_TRACEES; ++i) {
        if (tracees[i].active && tracees[i].pid == pid) return &tracees[i];
    }
    return NULL;
}

static TraceeState *add_tracee(TraceeState *tracees, pid_t pid) {
    TraceeState *existing = find_tracee(tracees, pid);
    if (existing) return existing;
    for (int i = 0; i < MAX_TRACEES; ++i) {
        if (!tracees[i].active) {
            memset(&tracees[i], 0, sizeof(tracees[i]));
            tracees[i].pid = pid;
            tracees[i].active = 1;
            tracees[i].last_nr = -1;
            tracees[i].emulated_nr = -1;
            tracees[i].last_emulated_nr = -1;
            return &tracees[i];
        }
    }
    return NULL;
}

static void remove_tracee(TraceeState *tracees, pid_t pid) {
    TraceeState *state = find_tracee(tracees, pid);
    if (state) memset(state, 0, sizeof(*state));
}

static int tracee_count(TraceeState *tracees) {
    int count = 0;
    for (int i = 0; i < MAX_TRACEES; ++i) {
        if (tracees[i].active) count++;
    }
    return count;
}

static int set_trace_options(pid_t pid) {
    long opts = PTRACE_O_TRACESYSGOOD |
                PTRACE_O_TRACEEXEC |
                PTRACE_O_TRACESECCOMP |
                PTRACE_O_TRACEEXIT |
                PTRACE_O_TRACEFORK |
                PTRACE_O_TRACEVFORK |
                PTRACE_O_TRACECLONE;
    if (ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)opts) != 0) {
        TRACE_LOG("pdocker-direct-trace: PTRACE_SETOPTIONS pid=%d failed: %s\n",
                  (int)pid, strerror(errno));
        return -1;
    }
    return 0;
}

static ssize_t read_tracee_string(pid_t pid, unsigned long long addr, char *buf, size_t cap) {
    if (!addr || cap == 0) return -1;
    size_t off = 0;
    while (off + 1 < cap) {
        char chunk[128];
        size_t want = sizeof(chunk);
        if (want > cap - 1 - off) want = cap - 1 - off;
        struct iovec local = {.iov_base = chunk, .iov_len = want};
        struct iovec remote = {.iov_base = (void *)(uintptr_t)(addr + off), .iov_len = want};
        ssize_t n = pdocker_process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (n <= 0) return -1;
        memcpy(buf + off, chunk, (size_t)n);
        for (ssize_t i = 0; i < n; ++i) {
            if (chunk[i] == '\0') {
                buf[off + (size_t)i] = '\0';
                return (ssize_t)(off + (size_t)i);
            }
        }
        off += (size_t)n;
    }
    buf[cap - 1] = '\0';
    return (ssize_t)(cap - 1);
}

static int write_tracee_string(pid_t pid, unsigned long long addr, const char *value) {
    size_t len = strlen(value) + 1;
    struct iovec local = {.iov_base = (void *)value, .iov_len = len};
    struct iovec remote = {.iov_base = (void *)(uintptr_t)addr, .iov_len = len};
    ssize_t n = pdocker_process_vm_writev(pid, &local, 1, &remote, 1, 0);
    return n == (ssize_t)len ? 0 : -1;
}

static int write_tracee_data(pid_t pid, unsigned long long addr, const void *value, size_t len) {
    struct iovec local = {.iov_base = (void *)value, .iov_len = len};
    struct iovec remote = {.iov_base = (void *)(uintptr_t)addr, .iov_len = len};
    ssize_t n = pdocker_process_vm_writev(pid, &local, 1, &remote, 1, 0);
    return n == (ssize_t)len ? 0 : -1;
}

static int should_rewrite_path(const char *rootfs, const char *path) {
    if (!rootfs || !rootfs[0] || !path || path[0] != '/') return 0;
    size_t root_len = strlen(rootfs);
    if (strncmp(path, rootfs, root_len) == 0 &&
        (path[root_len] == '\0' || path[root_len] == '/')) {
        return 0;
    }
    if (strncmp(path, "/proc/", 6) == 0 || strcmp(path, "/proc") == 0) return 0;
    if (strncmp(path, "/dev/", 5) == 0 || strcmp(path, "/dev") == 0) return 0;
    if (strncmp(path, "/sys/", 5) == 0 || strcmp(path, "/sys") == 0) return 0;
    return 1;
}

static int rewrite_path_arg(pid_t pid, struct user_pt_regs *regs, int arg_index,
                            const char *rootfs, const char *context) {
    char original[PATH_MAX];
    char rewritten[PATH_MAX];
    if (read_tracee_string(pid, regs->regs[arg_index], original, sizeof(original)) < 0) {
        return 0;
    }
    if (!should_rewrite_path(rootfs, original)) return 0;
    if (snprintf(rewritten, sizeof(rewritten), "%s%s", rootfs, original) >= (int)sizeof(rewritten)) {
        fprintf(stderr, "pdocker-direct-trace: pid=%d path too long for %s: %s\n",
                (int)pid, context, original);
        return 0;
    }
    unsigned long long scratch = (regs->sp - 8192u) & ~15ULL;
    if (write_tracee_string(pid, scratch, rewritten) != 0) {
        fprintf(stderr, "pdocker-direct-trace: pid=%d path rewrite failed for %s: %s -> %s (%s)\n",
                (int)pid, context, original, rewritten, strerror(errno));
        return 0;
    }
    regs->regs[arg_index] = scratch;
    TRACE_LOG("pdocker-direct-trace: pid=%d rewrite %s %s -> %s\n",
              (int)pid, context, original, rewritten);
    return 1;
}

static int rewrite_execve_arg(pid_t pid, struct user_pt_regs *regs,
                              const char *rootfs, const char *loader, const char *libpath) {
    char original[PATH_MAX];
    char target[PATH_MAX];
    if (read_tracee_string(pid, regs->regs[0], original, sizeof(original)) < 0) {
        return 0;
    }
    if (strcmp(original, loader) == 0) {
        return 0;
    }
    if (strncmp(original, rootfs, strlen(rootfs)) == 0) {
        snprintf(target, sizeof(target), "%s", original);
    } else if (should_rewrite_path(rootfs, original)) {
        if (snprintf(target, sizeof(target), "%s%s", rootfs, original) >= (int)sizeof(target)) {
            fprintf(stderr, "pdocker-direct-trace: pid=%d execve target too long: %s\n",
                    (int)pid, original);
            return 0;
        }
    } else {
        return 0;
    }

    unsigned long long old_argv = regs->regs[1];
    unsigned long long old_arg_ptrs[64];
    int old_argc = 0;
    for (; old_argc < 63; ++old_argc) {
        struct iovec local = {.iov_base = &old_arg_ptrs[old_argc], .iov_len = sizeof(unsigned long long)};
        struct iovec remote = {
            .iov_base = (void *)(uintptr_t)(old_argv + (unsigned long long)old_argc * sizeof(unsigned long long)),
            .iov_len = sizeof(unsigned long long),
        };
        if (pdocker_process_vm_readv(pid, &local, 1, &remote, 1, 0) != (ssize_t)sizeof(unsigned long long)) {
            break;
        }
        if (old_arg_ptrs[old_argc] == 0) break;
    }

    unsigned long long scratch = (regs->sp - 32768u) & ~15ULL;
    unsigned long long cursor = scratch;
    unsigned long long loader_addr = cursor;
    if (write_tracee_string(pid, loader_addr, loader) != 0) return 0;
    cursor += strlen(loader) + 1;
    unsigned long long library_path_flag_addr = cursor;
    if (write_tracee_string(pid, library_path_flag_addr, "--library-path") != 0) return 0;
    cursor += strlen("--library-path") + 1;
    unsigned long long libpath_addr = cursor;
    if (write_tracee_string(pid, libpath_addr, libpath) != 0) return 0;
    cursor += strlen(libpath) + 1;
    unsigned long long target_addr = cursor;
    if (write_tracee_string(pid, target_addr, target) != 0) return 0;
    cursor += strlen(target) + 1;
    cursor = (cursor + 15u) & ~15ULL;

    unsigned long long new_argv[70];
    int n = 0;
    new_argv[n++] = loader_addr;
    new_argv[n++] = library_path_flag_addr;
    new_argv[n++] = libpath_addr;
    new_argv[n++] = target_addr;
    for (int i = 1; i < old_argc && n < 69; ++i) {
        new_argv[n++] = old_arg_ptrs[i];
    }
    new_argv[n++] = 0;
    if (write_tracee_data(pid, cursor, new_argv, (size_t)n * sizeof(unsigned long long)) != 0) {
        return 0;
    }

    regs->regs[0] = loader_addr;
    regs->regs[1] = cursor;
    TRACE_LOG("pdocker-direct-trace: pid=%d rewrite execve via loader %s -> %s\n",
              (int)pid, original, target);
    return 1;
}

static int rewrite_syscall_paths(pid_t pid, struct user_pt_regs *regs, long nr,
                                 const char *rootfs, const char *loader, const char *libpath) {
    switch (nr) {
        case 34:  /* mkdirat(dirfd, pathname, mode) */
        case 35:  /* unlinkat(dirfd, pathname, flags) */
        case 48:  /* faccessat(dirfd, pathname, mode) */
        case 56:  /* openat(dirfd, pathname, flags, mode) */
        case 78:  /* readlinkat(dirfd, pathname, buf, bufsiz) */
        case 79:  /* newfstatat(dirfd, pathname, statbuf, flags) */
        case 291: /* statx(dirfd, pathname, flags, mask, statxbuf) */
        case 439: /* faccessat2(dirfd, pathname, mode, flags) */
            return rewrite_path_arg(pid, regs, 1, rootfs, syscall_name(nr));
        case 49:  /* chdir(pathname) */
            return rewrite_path_arg(pid, regs, 0, rootfs, syscall_name(nr));
        case 221: /* execve(pathname, argv, envp) */
            return rewrite_execve_arg(pid, regs, rootfs, loader, libpath);
        case 281: /* execveat(dirfd, pathname, argv, envp, flags) */
            return rewrite_path_arg(pid, regs, 1, rootfs, syscall_name(nr));
        default:
            return 0;
    }
}

static int trace_and_exec(char *const exec_argv[], const char *rootfs, const char *libpath) {
    pid_t child = fork();
    if (child < 0) {
        perror("pdocker-direct-executor: fork tracer");
        return 126;
    }
    if (child == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0) {
            perror("pdocker-direct-executor: PTRACE_TRACEME");
            _exit(126);
        }
        raise(SIGSTOP);
        execve(exec_argv[0], exec_argv, environ);
        perror("pdocker-direct-executor: execve loader");
        _exit(126);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        perror("pdocker-direct-executor: wait initial tracee");
        return 126;
    }
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "pdocker-direct-trace: child did not stop before exec status=0x%x\n", status);
        return 126;
    }

    TraceeState tracees[MAX_TRACEES];
    memset(tracees, 0, sizeof(tracees));
    TraceeState *child_state = add_tracee(tracees, child);
    if (!child_state) {
        fprintf(stderr, "pdocker-direct-trace: tracee table exhausted before start\n");
        return 126;
    }
    set_trace_options(child);

    int events = 0;
    int root_done = 0;
    int root_rc = 126;
    if (ptrace(PTRACE_SYSCALL, child, NULL, NULL) != 0) {
        perror("pdocker-direct-trace: initial PTRACE_SYSCALL");
        return 126;
    }

    while (1) {
        pid_t got = waitpid(-1, &status, __WALL);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD && root_done) {
                return root_rc;
            }
            perror("pdocker-direct-trace: waitpid");
            return 126;
        }
        TraceeState *state = find_tracee(tracees, got);
        if (!state) {
            state = add_tracee(tracees, got);
            if (!state) {
                fprintf(stderr, "pdocker-direct-trace: tracee table exhausted for pid=%d\n", (int)got);
                ptrace(PTRACE_SYSCALL, got, NULL, (void *)(long)SIGKILL);
                continue;
            }
        }
        if (WIFEXITED(status)) {
            int rc = WEXITSTATUS(status);
            TRACE_LOG(
                    "pdocker-direct-trace: pid=%d exited rc=%d events=%d last_syscall=%ld(%s) active=%d\n",
                    (int)got, rc, events, state->last_nr, syscall_name(state->last_nr),
                    tracee_count(tracees) - 1);
            remove_tracee(tracees, got);
            if (got == child) {
                root_done = 1;
                root_rc = rc;
            }
            if (root_done && tracee_count(tracees) == 0) return root_rc;
            continue;
        }
        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            fprintf(stderr,
                    "pdocker-direct-trace: pid=%d signaled sig=%d events=%d last_syscall=%ld(%s) args=%llx,%llx,%llx,%llx,%llx,%llx active=%d\n",
                    (int)got, sig, events, state->last_nr, syscall_name(state->last_nr),
                    state->last_args[0], state->last_args[1], state->last_args[2],
                    state->last_args[3], state->last_args[4], state->last_args[5],
                    tracee_count(tracees) - 1);
            remove_tracee(tracees, got);
            if (got == child) {
                root_done = 1;
                root_rc = 128 + sig;
            }
            if (root_done && tracee_count(tracees) == 0) return root_rc;
            continue;
        }
        if (!WIFSTOPPED(status)) continue;

        int sig = WSTOPSIG(status);
        unsigned int event = (unsigned int)status >> 16;
        events++;

        if (sig == (SIGTRAP | 0x80)) {
            struct user_pt_regs regs;
            if (get_regs(got, &regs) == 0) {
                if (!state->in_syscall) {
                    state->last_nr = (long)regs.regs[8];
                    for (int i = 0; i < 6; ++i) state->last_args[i] = regs.regs[i];
                    int rewrote = rewrite_syscall_paths(got, &regs, state->last_nr, rootfs, exec_argv[0], libpath);
                    long remapped_nr = syscall_remap_number(state->last_nr);
                    int remapped = remapped_nr != state->last_nr;
                    if (remapped) {
                        TRACE_LOG(
                                "pdocker-direct-trace: pid=%d remap nr=%ld(%s) -> %ld(%s)\n",
                                (int)got, state->last_nr, syscall_name(state->last_nr),
                                remapped_nr, syscall_name(remapped_nr));
                        regs.regs[8] = (unsigned long long)remapped_nr;
                    }
                    if (syscall_emulate_success(state->last_nr)) {
                        TRACE_LOG(
                                "pdocker-direct-trace: pid=%d emulate-success nr=%ld(%s) via sched_yield placeholder\n",
                                (int)got, state->last_nr, syscall_name(state->last_nr));
                        regs.regs[8] = 124; /* sched_yield: allowed harmless syscall; x0 is overwritten on exit. */
                        if (set_regs(got, &regs) == 0) {
                            state->emulated_nr = state->last_nr;
                        } else {
                            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs entry failed: %s\n",
                                    (int)got, strerror(errno));
                        }
                    } else if (rewrote || remapped) {
                        if (set_regs(got, &regs) != 0) {
                            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs rewrite/remap failed: %s\n",
                                    (int)got, strerror(errno));
                        }
                    }
                    if (events < 80 || state->last_nr == 221 || state->last_nr == 281 ||
                        state->last_nr == 293 || state->last_nr == 439 || state->last_nr == 449) {
                        TRACE_LOG(
                                "pdocker-direct-trace: pid=%d enter #%d nr=%ld(%s) args=%llx,%llx,%llx,%llx,%llx,%llx\n",
                                (int)got, events, state->last_nr, syscall_name(state->last_nr),
                                state->last_args[0], state->last_args[1], state->last_args[2],
                                state->last_args[3], state->last_args[4], state->last_args[5]);
                    }
                } else if (state->emulated_nr >= 0) {
                    regs.regs[0] = 0;
                    if (set_regs(got, &regs) != 0) {
                        fprintf(stderr, "pdocker-direct-trace: pid=%d setregs exit failed: %s\n",
                                (int)got, strerror(errno));
                    } else {
                        TRACE_LOG(
                                "pdocker-direct-trace: pid=%d emulate-success return nr=%ld(%s) -> 0\n",
                                (int)got, state->emulated_nr, syscall_name(state->emulated_nr));
                    }
                    state->last_emulated_nr = state->emulated_nr;
                    state->emulated_nr = -1;
                }
                state->in_syscall = !state->in_syscall;
            }
            if (ptrace(PTRACE_SYSCALL, got, NULL, NULL) != 0) break;
            continue;
        }

        if (sig == SIGSYS) {
            struct user_pt_regs regs;
            int suppressible = state->last_emulated_nr >= 0 && syscall_emulate_success(state->last_emulated_nr);
            if (get_regs(got, &regs) == 0) {
                if (suppressible) {
                    TRACE_LOG(
                            "pdocker-direct-trace: pid=%d SIGSYS nr=%llu(%s) pc=%llx args=%llx,%llx,%llx,%llx,%llx,%llx last=%ld(%s)\n",
                            (int)got,
                            (unsigned long long)regs.regs[8], syscall_name((long)regs.regs[8]),
                            (unsigned long long)regs.pc,
                            (unsigned long long)regs.regs[0], (unsigned long long)regs.regs[1],
                            (unsigned long long)regs.regs[2], (unsigned long long)regs.regs[3],
                            (unsigned long long)regs.regs[4], (unsigned long long)regs.regs[5],
                            state->last_nr, syscall_name(state->last_nr));
                } else {
                    fprintf(stderr,
                            "pdocker-direct-trace: pid=%d SIGSYS nr=%llu(%s) pc=%llx args=%llx,%llx,%llx,%llx,%llx,%llx last=%ld(%s)\n",
                            (int)got,
                            (unsigned long long)regs.regs[8], syscall_name((long)regs.regs[8]),
                            (unsigned long long)regs.pc,
                            (unsigned long long)regs.regs[0], (unsigned long long)regs.regs[1],
                            (unsigned long long)regs.regs[2], (unsigned long long)regs.regs[3],
                            (unsigned long long)regs.regs[4], (unsigned long long)regs.regs[5],
                            state->last_nr, syscall_name(state->last_nr));
                }
            } else {
                fprintf(stderr, "pdocker-direct-trace: pid=%d SIGSYS getregs failed: %s last=%ld(%s)\n",
                        (int)got, strerror(errno), state->last_nr, syscall_name(state->last_nr));
            }
            if (suppressible) {
                TRACE_LOG(
                        "pdocker-direct-trace: pid=%d suppress SIGSYS after emulated nr=%ld(%s)\n",
                        (int)got, state->last_emulated_nr, syscall_name(state->last_emulated_nr));
                state->last_emulated_nr = -1;
                if (ptrace(PTRACE_SYSCALL, got, NULL, NULL) != 0) break;
                continue;
            }
            if (ptrace(PTRACE_SYSCALL, got, NULL, (void *)(long)SIGSYS) != 0) break;
            continue;
        }

        if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
            unsigned long new_pid = 0;
            if (ptrace(PTRACE_GETEVENTMSG, got, NULL, &new_pid) == 0 && new_pid > 0) {
                TraceeState *new_state = add_tracee(tracees, (pid_t)new_pid);
                if (!new_state) {
                    fprintf(stderr, "pdocker-direct-trace: tracee table exhausted for event child=%lu\n", new_pid);
                } else {
                    set_trace_options((pid_t)new_pid);
                    TRACE_LOG(
                            "pdocker-direct-trace: event=%u parent=%d new_tracee=%lu active=%d\n",
                            event, (int)got, new_pid, tracee_count(tracees));
                }
            } else {
                fprintf(stderr, "pdocker-direct-trace: event=%u parent=%d GETEVENTMSG failed: %s\n",
                        event, (int)got, strerror(errno));
            }
            if (ptrace(PTRACE_SYSCALL, got, NULL, NULL) != 0) break;
            continue;
        }

        if (event == PTRACE_EVENT_EXEC || event == PTRACE_EVENT_SECCOMP || event == PTRACE_EVENT_EXIT) {
            TRACE_LOG("pdocker-direct-trace: pid=%d event=%u sig=%d last_syscall=%ld(%s)\n",
                      (int)got, event, sig, state->last_nr, syscall_name(state->last_nr));
            if (ptrace(PTRACE_SYSCALL, got, NULL, NULL) != 0) break;
            continue;
        }

        if (sig == SIGSTOP || sig == SIGTRAP) {
            set_trace_options(got);
            if (ptrace(PTRACE_SYSCALL, got, NULL, NULL) != 0) break;
            continue;
        }

        if (ptrace(PTRACE_SYSCALL, got, NULL, (void *)(long)sig) != 0) break;
    }
    fprintf(stderr, "pdocker-direct-trace: ptrace loop failed: %s\n", strerror(errno));
    return 126;
}

static int run_command(int argc, char **argv) {
    const char *mode = "run";
    const char *rootfs = NULL;
    const char *workdir = "/";
    const char **env_items = calloc((size_t)argc + 1, sizeof(char *));
    int env_count = 0;
    int bind_count = 0;
    int command_index = -1;
    int trace_syscalls = getenv("PDOCKER_DIRECT_TRACE_SYSCALLS") != NULL;
    g_trace_verbose = getenv("PDOCKER_DIRECT_TRACE_VERBOSE") != NULL;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            command_index = i + 1;
            break;
        } else if (strcmp(argv[i], "--mode") == 0) {
            mode = value_after(&i, argc, argv, "--mode");
        } else if (strcmp(argv[i], "--rootfs") == 0) {
            rootfs = value_after(&i, argc, argv, "--rootfs");
        } else if (strcmp(argv[i], "--workdir") == 0) {
            workdir = value_after(&i, argc, argv, "--workdir");
        } else if (strcmp(argv[i], "--env") == 0) {
            env_items[env_count] = value_after(&i, argc, argv, "--env");
            env_count += 1;
        } else if (strcmp(argv[i], "--bind") == 0) {
            (void)value_after(&i, argc, argv, "--bind");
            bind_count += 1;
        } else if (strcmp(argv[i], "--cow-upper") == 0 ||
                   strcmp(argv[i], "--cow-lower") == 0 ||
                   strcmp(argv[i], "--cow-guest") == 0) {
            const char *option = argv[i];
            (void)value_after(&i, argc, argv, option);
        } else {
            fprintf(stderr, "pdocker-direct-executor: unknown option: %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    if (!rootfs || command_index < 0 || command_index >= argc) {
        fprintf(stderr, "pdocker-direct-executor: --rootfs and command argv are required\n");
        usage(stderr);
        free(env_items);
        return 2;
    }

    char rootfs_abs[PATH_MAX];
    if (!realpath(rootfs, rootfs_abs)) {
        perror("pdocker-direct-executor: realpath rootfs");
        free(env_items);
        return 126;
    }
    rootfs = rootfs_abs;

    char cwd[PATH_MAX];
    if (snprintf(cwd, sizeof(cwd), "%s/%s", rootfs, workdir[0] == '/' ? workdir + 1 : workdir) >= (int)sizeof(cwd)) {
        fprintf(stderr, "pdocker-direct-executor: workdir path too long\n");
        free(env_items);
        return 126;
    }
    if (chdir(cwd) != 0 && chdir(rootfs) != 0) {
        perror("pdocker-direct-executor: chdir rootfs/workdir");
        free(env_items);
        return 126;
    }

    char ldpath[PATH_MAX];
    char helper_path[PATH_MAX];
    char helper_dir[PATH_MAX];
    const char *loader = NULL;
    if (realpath(argv[0], helper_path)) {
        strncpy(helper_dir, helper_path, sizeof(helper_dir) - 1);
        helper_dir[sizeof(helper_dir) - 1] = '\0';
        char *slash = strrchr(helper_dir, '/');
        if (slash) {
            *slash = '\0';
            if (((snprintf(ldpath, sizeof(ldpath), "%s/pdocker-ld-linux-aarch64", helper_dir) < (int)sizeof(ldpath) &&
                  access(ldpath, X_OK) == 0) ||
                 (snprintf(ldpath, sizeof(ldpath), "%s/libpdocker-ld-linux-aarch64.so", helper_dir) < (int)sizeof(ldpath) &&
                  access(ldpath, X_OK) == 0))) {
                loader = ldpath;
                goto loader_found;
            }
        }
    }
    const char *ld_candidates[] = {
        "lib/aarch64-linux-gnu/ld-linux-aarch64.so.1",
        "lib/ld-linux-aarch64.so.1",
        "lib64/ld-linux-aarch64.so.1",
        NULL,
    };
    for (int i = 0; ld_candidates[i]; ++i) {
        if (snprintf(ldpath, sizeof(ldpath), "%s/%s", rootfs, ld_candidates[i]) >= (int)sizeof(ldpath)) {
            continue;
        }
        if (access(ldpath, X_OK) == 0) {
            loader = ldpath;
            break;
        }
    }
loader_found:
    if (!loader) {
        fprintf(stderr, "pdocker-direct-executor: rootfs dynamic loader not found under %s\n", rootfs);
        free(env_items);
        return 126;
    }

    char target[PATH_MAX];
    const char *cmd0 = argv[command_index];
    if (cmd0[0] == '/') {
        if (snprintf(target, sizeof(target), "%s%s", rootfs, cmd0) >= (int)sizeof(target)) {
            fprintf(stderr, "pdocker-direct-executor: command path too long\n");
            free(env_items);
            return 126;
        }
    } else {
        if (snprintf(target, sizeof(target), "%s/%s", cwd, cmd0) >= (int)sizeof(target)) {
            fprintf(stderr, "pdocker-direct-executor: command path too long\n");
            free(env_items);
            return 126;
        }
    }
    if (access(target, F_OK) != 0 && strchr(cmd0, '/') == NULL) {
        const char *path = getenv("PATH");
        if (!path || !path[0]) path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        char *copy = strdup(path);
        char *save = NULL;
        for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
            if (snprintf(target, sizeof(target), "%s/%s/%s", rootfs, dir[0] == '/' ? dir + 1 : dir, cmd0) >= (int)sizeof(target)) {
                continue;
            }
            if (access(target, X_OK) == 0) break;
        }
        free(copy);
    }

    char libpath[PATH_MAX * 2];
    snprintf(libpath, sizeof(libpath),
             "%s/lib/aarch64-linux-gnu:%s/usr/lib/aarch64-linux-gnu:%s/lib:%s/usr/lib",
             rootfs, rootfs, rootfs, rootfs);
    char shim_preload[PATH_MAX], preload[PATH_MAX * 2];
    snprintf(shim_preload, sizeof(shim_preload), "%s/.pdocker-rootfs-shim.so", rootfs);
    preload[0] = '\0';

    clearenv();
    setenv("PDOCKER_ROOTFS", rootfs, 1);
    setenv("LD_LIBRARY_PATH", libpath, 1);
    setenv("GLIBC_TUNABLES", "glibc.pthread.rseq=0", 0);
    if (access(shim_preload, R_OK) == 0) {
        snprintf(preload, sizeof(preload), "%s", shim_preload);
    }
    if (preload[0]) {
        setenv("LD_PRELOAD", preload, 1);
    }
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 0);
    setenv("PWD", workdir, 1);
    for (int i = 0; i < env_count; ++i) {
        const char *eq = strchr(env_items[i], '=');
        if (!eq || eq == env_items[i]) continue;
        size_t klen = (size_t)(eq - env_items[i]);
        if (klen == 10 && strncmp(env_items[i], "LD_PRELOAD", 10) == 0) continue;
        if (klen == 15 && strncmp(env_items[i], "LD_LIBRARY_PATH", 15) == 0) continue;
        if (klen == 13 && strncmp(env_items[i], "PDOCKER_ROOTFS", 13) == 0) continue;
        char *key = strndup(env_items[i], klen);
        if (!key) continue;
        setenv(key, eq + 1, 1);
        free(key);
    }

    int is_script = file_starts_with(target, "#!");
    char shell[PATH_MAX];
    const char *program = target;
    if (is_script) {
        snprintf(shell, sizeof(shell), "%s/bin/bash", rootfs);
        if (access(shell, X_OK) != 0) snprintf(shell, sizeof(shell), "%s/bin/sh", rootfs);
        program = shell;
    }

    int cmd_argc = argc - command_index;
    char **nargv = calloc((size_t)cmd_argc + 7, sizeof(char *));
    if (!nargv) {
        free(env_items);
        return 126;
    }
    int n = 0;
    nargv[n++] = (char *)loader;
    nargv[n++] = "--library-path";
    nargv[n++] = libpath;
    if (preload[0]) {
        nargv[n++] = "--preload";
        nargv[n++] = preload;
    }
    nargv[n++] = (char *)program;
    if (is_script) nargv[n++] = target;
    for (int i = command_index + 1; i < argc; ++i) nargv[n++] = argv[i];
    nargv[n] = NULL;

    fprintf(stderr,
            "pdocker-direct-executor: mode=%s rootfs=%s workdir=%s env=%d bind=%d argv0=%s\n",
            mode, rootfs, workdir, env_count, bind_count, cmd0);
    fflush(stderr);
    if (trace_syscalls) {
        int rc = trace_and_exec(nargv, rootfs, libpath);
        free(nargv);
        free(env_items);
        return rc;
    }
    execve(loader, nargv, environ);
    perror("pdocker-direct-executor: execve loader");
    free(nargv);
    free(env_items);
    return 126;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--pdocker-direct-probe") == 0) {
        puts("pdocker-direct-executor:1");
        if (getenv("PDOCKER_DIRECT_EXPERIMENTAL_PROCESS_EXEC")) {
            puts("process-exec=1");
        } else {
            puts("process-exec=0");
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        return run_command(argc, argv);
    }

    usage(stderr);
    return 2;
}
