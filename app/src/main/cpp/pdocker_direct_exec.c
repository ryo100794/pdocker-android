#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <errno.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <dirent.h>
#include <linux/elf.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <fcntl.h>
#include <time.h>
#include <sys/prctl.h>

extern char **environ;

static int g_trace_verbose = 0;
static int g_trace_linkat = 0;
static int g_trace_paths = 0;
static int g_trace_exec = 0;
static int g_sync_usec = 0;
static int g_stats = 0;
static int g_selective_trace = 0;
static int g_rootfd_rewrite = 0;
static int g_validate_tracees = 0;
static int g_trace_stat_paths = 1;
static int g_rootfs_fd = -1;
static volatile sig_atomic_t g_trace_child_pgid = -1;
static unsigned long long g_syscall_counts[512];
static unsigned long long g_stop_count = 0;
static struct timespec g_stats_start;
static int write_tracee_data(pid_t pid, unsigned long long addr, const void *value, size_t len);
static const char *syscall_name(long nr);

#define MAX_BIND_MAPS 96

typedef struct {
    char host[PATH_MAX];
    char guest[PATH_MAX];
    int readonly;
} BindMap;

static BindMap g_bind_maps[MAX_BIND_MAPS];
static int g_bind_map_count = 0;

#define TRACE_LOG(...) do { if (g_trace_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

static void tracer_signal_handler(int sig) {
    pid_t pgid = (pid_t)g_trace_child_pgid;
    if (pgid > 0) {
        kill(-pgid, SIGKILL);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_tracer_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tracer_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}

static int env_flag_enabled(const char *name) {
    const char *v = getenv(name);
    return v && v[0] && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0;
}

static double monotonic_seconds_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) +
           (double)(now.tv_nsec - start->tv_nsec) / 1000000000.0;
}

static void record_syscall_stat(long nr) {
    if (!g_stats) return;
    if (nr >= 0 && nr < (long)(sizeof(g_syscall_counts) / sizeof(g_syscall_counts[0]))) {
        g_syscall_counts[nr]++;
    }
}

static void print_syscall_stats(const char *reason, int rc) {
    if (!g_stats) return;
    double seconds = monotonic_seconds_since(&g_stats_start);
    fprintf(stderr,
            "pdocker-direct-stats: reason=%s rc=%d elapsed=%.3fs stops=%llu\n",
            reason, rc, seconds, g_stop_count);
    for (int rank = 0; rank < 12; ++rank) {
        int best = -1;
        unsigned long long best_count = 0;
        for (int i = 0; i < (int)(sizeof(g_syscall_counts) / sizeof(g_syscall_counts[0])); ++i) {
            if (g_syscall_counts[i] > best_count) {
                best = i;
                best_count = g_syscall_counts[i];
            }
        }
        if (best < 0 || best_count == 0) break;
        fprintf(stderr, "pdocker-direct-stats: #%d nr=%d(%s) count=%llu\n",
                rank + 1, best, syscall_name(best), best_count);
        g_syscall_counts[best] = 0;
    }
}

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

static void trim_trailing_slashes(char *path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }
}

static int parse_bind_spec(const char *spec) {
    if (!spec || !spec[0] || g_bind_map_count >= MAX_BIND_MAPS) return 0;
    char tmp[PATH_MAX * 2];
    snprintf(tmp, sizeof(tmp), "%s", spec);
    char *host = tmp;
    char *guest = strchr(tmp, ':');
    if (!guest) return 0;
    *guest++ = '\0';
    char *opts = strchr(guest, ':');
    if (opts) *opts++ = '\0';
    if (!host[0] || !guest[0] || guest[0] != '/') return 0;

    BindMap *m = &g_bind_maps[g_bind_map_count];
    char resolved[PATH_MAX];
    if (realpath(host, resolved)) {
        snprintf(m->host, sizeof(m->host), "%s", resolved);
    } else {
        snprintf(m->host, sizeof(m->host), "%s", host);
    }
    snprintf(m->guest, sizeof(m->guest), "%s", guest);
    trim_trailing_slashes(m->host);
    trim_trailing_slashes(m->guest);
    m->readonly = opts && strstr(opts, "ro");
    g_bind_map_count++;
    return 1;
}

static int bind_guest_match(const BindMap *m, const char *guest, const char **suffix) {
    size_t glen = strlen(m->guest);
    if (strcmp(m->guest, "/") == 0) {
        *suffix = guest;
        return 1;
    }
    if (strncmp(guest, m->guest, glen) != 0) return 0;
    if (guest[glen] != '\0' && guest[glen] != '/') return 0;
    *suffix = guest + glen;
    return 1;
}

static int resolve_bind_path(const char *guest, char *out, size_t out_len) {
    int best = -1;
    size_t best_len = 0;
    const char *best_suffix = NULL;
    for (int i = 0; i < g_bind_map_count; ++i) {
        const char *suffix = NULL;
        if (!bind_guest_match(&g_bind_maps[i], guest, &suffix)) continue;
        size_t len = strlen(g_bind_maps[i].guest);
        if (len >= best_len) {
            best = i;
            best_len = len;
            best_suffix = suffix;
        }
    }
    if (best < 0) return 0;
    const char *host = g_bind_maps[best].host;
    if (!best_suffix || !best_suffix[0]) {
        if (snprintf(out, out_len, "%s", host) >= (int)out_len) return -ENAMETOOLONG;
    } else if (strcmp(host, "/") == 0) {
        if (snprintf(out, out_len, "%s", best_suffix) >= (int)out_len) return -ENAMETOOLONG;
    } else {
        if (snprintf(out, out_len, "%s%s", host, best_suffix) >= (int)out_len) return -ENAMETOOLONG;
    }
    return 1;
}

static int resolve_guest_program(const char *rootfs, const char *program, char *out, size_t out_len) {
    if (!program || !program[0]) return -1;
    if (program[0] == '/') {
        if (snprintf(out, out_len, "%s%s", rootfs, program) >= (int)out_len) return -1;
        return access(out, X_OK) == 0 ? 0 : -1;
    }
    const char *path = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    char *copy = strdup(path);
    if (!copy) return -1;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        if (snprintf(out, out_len, "%s/%s/%s", rootfs, dir[0] == '/' ? dir + 1 : dir, program) >= (int)out_len) {
            continue;
        }
        if (access(out, X_OK) == 0) {
            free(copy);
            return 0;
        }
    }
    free(copy);
    return -1;
}

static int parse_shebang(const char *path, char *program, size_t program_len,
                         char *arg, size_t arg_len) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[PATH_MAX];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (strncmp(line, "#!", 2) != 0) return -1;
    char *p = line + 2;
    while (*p == ' ' || *p == '\t') p++;
    char *prog = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    if (*p) *p++ = '\0';
    while (*p == ' ' || *p == '\t') p++;
    char *first_arg = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    if (*p) *p = '\0';
    if (strcmp(prog, "/usr/bin/env") == 0 && first_arg[0]) {
        prog = first_arg;
        first_arg = "";
    }
    snprintf(program, program_len, "%s", prog);
    snprintf(arg, arg_len, "%s", first_arg);
    return program[0] ? 0 : -1;
}

static const char *syscall_name(long nr) {
    switch (nr) {
        case 17: return "getcwd";
        case 5: return "setxattr";
        case 6: return "lsetxattr";
        case 8: return "getxattr";
        case 9: return "lgetxattr";
        case 11: return "listxattr";
        case 12: return "llistxattr";
        case 14: return "removexattr";
        case 15: return "lremovexattr";
        case 29: return "ioctl";
        case 33: return "mknodat";
        case 34: return "mkdirat";
        case 35: return "unlinkat";
        case 36: return "symlinkat";
        case 37: return "linkat";
        case 38: return "renameat";
        case 43: return "statfs";
        case 44: return "fstatfs";
        case 48: return "faccessat";
        case 49: return "chdir";
        case 53: return "fchmodat";
        case 54: return "fchownat";
        case 55: return "fchown";
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
        case 143: return "setregid";
        case 144: return "setgid";
        case 145: return "setreuid";
        case 146: return "setuid";
        case 147: return "setresuid";
        case 148: return "getresuid";
        case 149: return "setresgid";
        case 150: return "getresgid";
        case 151: return "setfsuid";
        case 152: return "setfsgid";
        case 159: return "setgroups";
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
        case 235: return "mbind";
        case 236: return "get_mempolicy";
        case 237: return "set_mempolicy";
        case 238: return "migrate_pages";
        case 239: return "move_pages";
        case 242: return "sched_getaffinity";
        case 260: return "wait4";
        case 261: return "prlimit64";
        case 276: return "renameat2";
        case 278: return "getrandom";
        case 88: return "utimensat";
        case 280: return "bpf";
        case 281: return "execveat";
        case 291: return "statx";
        case 293: return "rseq";
        case 437: return "openat2";
        case 439: return "faccessat2";
        case 425: return "io_uring_setup";
        case 426: return "io_uring_enter";
        case 427: return "io_uring_register";
        case 435: return "clone3";
        case 436: return "close_range";
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
           nr == 174 ||  /* getuid: default container user is root in the current executor. */
           nr == 175 ||  /* geteuid */
           nr == 176 ||  /* getgid */
           nr == 177 ||  /* getegid */
           nr == 54 ||   /* fchownat: keep app-owned files, report container-root success. */
           nr == 55 ||   /* fchown */
           nr == 143 ||  /* setregid */
           nr == 144 ||  /* setgid: keep Android credentials, report container-root success. */
           nr == 145 ||  /* setreuid */
           nr == 146 ||  /* setuid */
           nr == 147 ||  /* setresuid */
           nr == 148 ||  /* getresuid */
           nr == 149 ||  /* setresgid */
           nr == 150 ||  /* getresgid */
           nr == 151 ||  /* setfsuid */
           nr == 152 ||  /* setfsgid */
           nr == 159 ||  /* setgroups */
           nr == 293;    /* rseq: glibc can continue when registration appears unavailable. */
}

static int syscall_emulate_errno(long nr, int *err) {
    if ((nr >= 235 && nr <= 239) || nr == 450) {
        /* Android app seccomp commonly blocks NUMA policy syscalls.  Container
         * workloads such as OpenBLAS/llama.cpp should treat unavailable NUMA
         * policy support like a kernel without NUMA support and continue. */
        if (err) *err = ENOSYS;
        return 1;
    }
    return 0;
}

static int syscall_completed_in_userland(long nr) {
    int ignored = 0;
    return nr == 17 ||   /* getcwd */
           nr == 36 ||   /* symlinkat */
           nr == 37 ||   /* linkat */
           nr == 48 ||   /* faccessat */
           nr == 425 ||  /* io_uring_setup: report unavailable to libc/node. */
           nr == 426 ||  /* io_uring_enter */
           nr == 427 ||  /* io_uring_register */
           nr == 439 ||  /* faccessat2 */
           syscall_emulate_errno(nr, &ignored) ||
           syscall_emulate_success(nr);
}

#define ADD_STMT(code_, k_) do { \
    filter[n++] = (struct sock_filter)BPF_STMT((code_), (k_)); \
} while (0)

#define ADD_JUMP(code_, k_, jt_, jf_) do { \
    filter[n++] = (struct sock_filter)BPF_JUMP((code_), (k_), (jt_), (jf_)); \
} while (0)

#define ADD_TRACE_SYSCALL(nr) do { \
    ADD_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1); \
    ADD_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRACE); \
} while (0)

#define ADD_ERRNO_SYSCALL(nr, err) do { \
    ADD_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1); \
    ADD_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ((err) & SECCOMP_RET_DATA)); \
} while (0)

static int install_selective_seccomp_trace_filter(void) {
    struct sock_filter filter[160];
    size_t n = 0;

    ADD_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, arch));
    ADD_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0);
    ADD_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    ADD_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)offsetof(struct seccomp_data, nr));

    /* Path-bearing filesystem syscalls. */
    ADD_TRACE_SYSCALL(5);    /* setxattr */
    ADD_TRACE_SYSCALL(6);    /* lsetxattr */
    ADD_TRACE_SYSCALL(8);    /* getxattr */
    ADD_TRACE_SYSCALL(9);    /* lgetxattr */
    ADD_TRACE_SYSCALL(11);   /* listxattr */
    ADD_TRACE_SYSCALL(12);   /* llistxattr */
    ADD_TRACE_SYSCALL(14);   /* removexattr */
    ADD_TRACE_SYSCALL(15);   /* lremovexattr */
    ADD_TRACE_SYSCALL(17);   /* getcwd */
    ADD_TRACE_SYSCALL(33);   /* mknodat */
    ADD_TRACE_SYSCALL(34);   /* mkdirat */
    ADD_TRACE_SYSCALL(35);   /* unlinkat */
    ADD_TRACE_SYSCALL(36);   /* symlinkat */
    ADD_TRACE_SYSCALL(37);   /* linkat */
    ADD_TRACE_SYSCALL(38);   /* renameat */
    ADD_TRACE_SYSCALL(43);   /* statfs */
    ADD_TRACE_SYSCALL(48);   /* faccessat */
    ADD_TRACE_SYSCALL(49);   /* chdir */
    ADD_TRACE_SYSCALL(53);   /* fchmodat */
    ADD_TRACE_SYSCALL(54);   /* fchownat */
    ADD_TRACE_SYSCALL(55);   /* fchown */
    ADD_TRACE_SYSCALL(56);   /* openat */
    ADD_TRACE_SYSCALL(78);   /* readlinkat */
    if (g_trace_stat_paths) {
        ADD_TRACE_SYSCALL(79);   /* newfstatat */
        ADD_TRACE_SYSCALL(291);  /* statx */
    }
    ADD_TRACE_SYSCALL(88);   /* utimensat */
    ADD_TRACE_SYSCALL(264);  /* name_to_handle_at */
    ADD_TRACE_SYSCALL(276);  /* renameat2 */
    ADD_TRACE_SYSCALL(281);  /* execveat */
    ADD_TRACE_SYSCALL(437);  /* openat2 */
    ADD_TRACE_SYSCALL(439);  /* faccessat2 */

    /* Process startup, credentials, and Android-blocked compatibility. */
    ADD_ERRNO_SYSCALL(99, ENOSYS);   /* set_robust_list: glibc tolerates unavailable robust futex lists. */
    ADD_TRACE_SYSCALL(143);  /* setregid */
    ADD_TRACE_SYSCALL(144);  /* setgid */
    ADD_TRACE_SYSCALL(145);  /* setreuid */
    ADD_TRACE_SYSCALL(146);  /* setuid */
    ADD_TRACE_SYSCALL(147);  /* setresuid */
    ADD_TRACE_SYSCALL(148);  /* getresuid */
    ADD_TRACE_SYSCALL(149);  /* setresgid */
    ADD_TRACE_SYSCALL(150);  /* getresgid */
    ADD_TRACE_SYSCALL(151);  /* setfsuid */
    ADD_TRACE_SYSCALL(152);  /* setfsgid */
    ADD_TRACE_SYSCALL(159);  /* setgroups */
    ADD_TRACE_SYSCALL(174);  /* getuid */
    ADD_TRACE_SYSCALL(175);  /* geteuid */
    ADD_TRACE_SYSCALL(176);  /* getgid */
    ADD_TRACE_SYSCALL(177);  /* getegid */
    ADD_TRACE_SYSCALL(221);  /* execve */
    ADD_ERRNO_SYSCALL(293, ENOSYS);  /* rseq */
    ADD_ERRNO_SYSCALL(235, ENOSYS);  /* mbind */
    ADD_ERRNO_SYSCALL(236, ENOSYS);  /* get_mempolicy */
    ADD_ERRNO_SYSCALL(237, ENOSYS);  /* set_mempolicy */
    ADD_ERRNO_SYSCALL(238, ENOSYS);  /* migrate_pages */
    ADD_ERRNO_SYSCALL(239, ENOSYS);  /* move_pages */
    ADD_ERRNO_SYSCALL(425, ENOSYS);  /* io_uring_setup */
    ADD_ERRNO_SYSCALL(426, ENOSYS);  /* io_uring_enter */
    ADD_ERRNO_SYSCALL(427, ENOSYS);  /* io_uring_register */
    ADD_ERRNO_SYSCALL(435, ENOSYS);  /* clone3 */
    ADD_ERRNO_SYSCALL(436, ENOSYS);  /* close_range */
    ADD_ERRNO_SYSCALL(450, ENOSYS);  /* set_mempolicy_home_node */

    ADD_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);

    struct sock_fprog prog = {
        .len = (unsigned short)n,
        .filter = filter,
    };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return -1;
    }
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}

#undef ADD_TRACE_SYSCALL
#undef ADD_ERRNO_SYSCALL
#undef ADD_JUMP
#undef ADD_STMT

static long syscall_remap_number(long nr) {
    return nr;
}

typedef struct TraceeState {
    pid_t pid;
    int active;
    int in_syscall;
    long last_nr;
    long emulated_nr;
    long last_emulated_nr;
    unsigned long long emulated_result;
    unsigned long long uid;
    unsigned long long euid;
    unsigned long long suid;
    unsigned long long gid;
    unsigned long long egid;
    unsigned long long sgid;
    unsigned long long last_args[6];
    char exec_guest_path[PATH_MAX];
    char guest_cwd[PATH_MAX];
    char pending_guest_cwd[PATH_MAX];
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
            tracees[i].emulated_result = 0;
            tracees[i].uid = 0;
            tracees[i].euid = 0;
            tracees[i].suid = 0;
            tracees[i].gid = 0;
            tracees[i].egid = 0;
            tracees[i].sgid = 0;
            snprintf(tracees[i].guest_cwd, sizeof(tracees[i].guest_cwd), "/");
            return &tracees[i];
        }
    }
    return NULL;
}

static int is_minus_one_arg(unsigned long long value) {
    return value == 0xffffffffffffffffULL;
}

static void write_tracee_u32(pid_t pid, unsigned long long addr, unsigned long long value) {
    if (!addr) return;
    uint32_t v = (uint32_t)value;
    if (write_tracee_data(pid, addr, &v, sizeof(v)) == 0) return;
    unsigned long long aligned = addr & ~(unsigned long long)(sizeof(long) - 1);
    unsigned long shift = (unsigned long)(addr - aligned) * 8u;
    errno = 0;
    long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)aligned, NULL);
    if (word == -1 && errno) {
        TRACE_LOG("pdocker-direct-trace: pid=%d write u32 peek failed addr=%llx: %s\n",
                  (int)pid, addr, strerror(errno));
        return;
    }
    unsigned long mask = 0xffffffffUL << shift;
    unsigned long patched = ((unsigned long)word & ~mask) | (((unsigned long)v << shift) & mask);
    if (ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)aligned, (void *)patched) != 0) {
        TRACE_LOG("pdocker-direct-trace: pid=%d write u32 poke failed addr=%llx: %s\n",
                  (int)pid, addr, strerror(errno));
    }
}

static unsigned long long prepare_emulated_result(pid_t pid, TraceeState *state, long nr) {
    switch (nr) {
        case 174: return state->uid;
        case 175: return state->euid;
        case 176: return state->gid;
        case 177: return state->egid;
        case 143: /* setregid(rgid, egid) */
            if (!is_minus_one_arg(state->last_args[0])) state->gid = state->last_args[0];
            if (!is_minus_one_arg(state->last_args[1])) state->egid = state->last_args[1];
            state->sgid = state->egid;
            return 0;
        case 144: /* setgid(gid) */
            state->gid = state->last_args[0];
            state->egid = state->last_args[0];
            state->sgid = state->last_args[0];
            return 0;
        case 145: /* setreuid(ruid, euid) */
            if (!is_minus_one_arg(state->last_args[0])) state->uid = state->last_args[0];
            if (!is_minus_one_arg(state->last_args[1])) state->euid = state->last_args[1];
            state->suid = state->euid;
            return 0;
        case 146: /* setuid(uid) */
            state->uid = state->last_args[0];
            state->euid = state->last_args[0];
            state->suid = state->last_args[0];
            return 0;
        case 147: /* setresuid(ruid, euid, suid) */
            if (!is_minus_one_arg(state->last_args[0])) state->uid = state->last_args[0];
            if (!is_minus_one_arg(state->last_args[1])) state->euid = state->last_args[1];
            if (!is_minus_one_arg(state->last_args[2])) state->suid = state->last_args[2];
            else state->suid = state->euid;
            TRACE_LOG("pdocker-direct-trace: pid=%d setresuid args=%llx,%llx,%llx -> %llu,%llu,%llu\n",
                      (int)pid, state->last_args[0], state->last_args[1], state->last_args[2],
                      state->uid, state->euid, state->suid);
            return 0;
        case 148: /* getresuid(ruid, euid, suid) */
            write_tracee_u32(pid, state->last_args[0], state->uid);
            write_tracee_u32(pid, state->last_args[1], state->euid);
            write_tracee_u32(pid, state->last_args[2], state->suid);
            TRACE_LOG("pdocker-direct-trace: pid=%d getresuid -> %llu,%llu,%llu ptr=%llx,%llx,%llx\n",
                      (int)pid, state->uid, state->euid, state->suid,
                      state->last_args[0], state->last_args[1], state->last_args[2]);
            return 0;
        case 149: /* setresgid(rgid, egid, sgid) */
            if (!is_minus_one_arg(state->last_args[0])) state->gid = state->last_args[0];
            if (!is_minus_one_arg(state->last_args[1])) state->egid = state->last_args[1];
            if (!is_minus_one_arg(state->last_args[2])) state->sgid = state->last_args[2];
            else state->sgid = state->egid;
            TRACE_LOG("pdocker-direct-trace: pid=%d setresgid args=%llx,%llx,%llx -> %llu,%llu,%llu\n",
                      (int)pid, state->last_args[0], state->last_args[1], state->last_args[2],
                      state->gid, state->egid, state->sgid);
            return 0;
        case 150: /* getresgid(rgid, egid, sgid) */
            write_tracee_u32(pid, state->last_args[0], state->gid);
            write_tracee_u32(pid, state->last_args[1], state->egid);
            write_tracee_u32(pid, state->last_args[2], state->sgid);
            TRACE_LOG("pdocker-direct-trace: pid=%d getresgid -> %llu,%llu,%llu ptr=%llx,%llx,%llx\n",
                      (int)pid, state->gid, state->egid, state->sgid,
                      state->last_args[0], state->last_args[1], state->last_args[2]);
            return 0;
        default:
            if (nr == 147 || nr == 149) {
                TRACE_LOG("pdocker-direct-trace: pid=%d cred nr=%ld args=%llx,%llx,%llx uid=%llu,%llu,%llu gid=%llu,%llu,%llu\n",
                          (int)pid, nr, state->last_args[0], state->last_args[1], state->last_args[2],
                          state->uid, state->euid, state->suid, state->gid, state->egid, state->sgid);
            }
            return 0;
    }
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

static int tracee_is_still_owned(pid_t tracer, pid_t tracee) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)tracee);
    FILE *fp = fopen(path, "re");
    if (!fp) return 0;
    char line[128];
    int ppid = -1;
    int tracer_pid = -1;
    while (fgets(line, sizeof(line), fp)) {
        sscanf(line, "PPid:\t%d", &ppid);
        sscanf(line, "TracerPid:\t%d", &tracer_pid);
    }
    fclose(fp);
    return ppid == (int)tracer || tracer_pid == (int)tracer;
}

static void tracee_status_summary(pid_t tracee, char *buf, size_t cap) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)tracee);
    FILE *fp = fopen(path, "re");
    if (!fp) {
        snprintf(buf, cap, "status=unreadable:%s", strerror(errno));
        return;
    }
    char line[128];
    char state[32] = "?";
    int ppid = -1;
    int tracer_pid = -1;
    while (fgets(line, sizeof(line), fp)) {
        sscanf(line, "State:\t%31[^\n]", state);
        sscanf(line, "PPid:\t%d", &ppid);
        sscanf(line, "TracerPid:\t%d", &tracer_pid);
    }
    fclose(fp);
    snprintf(buf, cap, "state=%s ppid=%d tracer=%d", state, ppid, tracer_pid);
}

static void dump_active_tracees(TraceeState *tracees, pid_t tracer, const char *reason) {
    fprintf(stderr, "pdocker-direct-trace: %s active=%d\n", reason, tracee_count(tracees));
    for (int i = 0; i < MAX_TRACEES; ++i) {
        if (!tracees[i].active) continue;
        char status[160];
        tracee_status_summary(tracees[i].pid, status, sizeof(status));
        fprintf(stderr, "pdocker-direct-trace: active pid=%d owned=%d %s last=%ld(%s) in=%d emu=%ld last_emu=%ld\n",
                (int)tracees[i].pid, tracee_is_still_owned(tracer, tracees[i].pid),
                status, tracees[i].last_nr, syscall_name(tracees[i].last_nr),
                tracees[i].in_syscall, tracees[i].emulated_nr,
                tracees[i].last_emulated_nr);
    }
}

static int prune_dead_tracees(TraceeState *tracees, pid_t tracer) {
    int alive = 0;
    for (int i = 0; i < MAX_TRACEES; ++i) {
        if (!tracees[i].active) continue;
        if (kill(tracees[i].pid, 0) == 0 || errno == EPERM) {
            if (tracee_is_still_owned(tracer, tracees[i].pid)) {
                alive++;
            } else {
                TRACE_LOG("pdocker-direct-trace: prune detached/reused tracee pid=%d last=%ld(%s)\n",
                          (int)tracees[i].pid, tracees[i].last_nr,
                          syscall_name(tracees[i].last_nr));
                memset(&tracees[i], 0, sizeof(tracees[i]));
            }
        } else if (errno == ESRCH) {
            TRACE_LOG("pdocker-direct-trace: prune vanished tracee pid=%d last=%ld(%s)\n",
                      (int)tracees[i].pid, tracees[i].last_nr,
                      syscall_name(tracees[i].last_nr));
            memset(&tracees[i], 0, sizeof(tracees[i]));
        }
    }
    return alive;
}

static int set_trace_options(pid_t pid) {
    long opts = PTRACE_O_TRACESYSGOOD |
                PTRACE_O_TRACEEXEC |
                PTRACE_O_TRACESECCOMP |
                PTRACE_O_TRACEEXIT |
                PTRACE_O_TRACEFORK |
                PTRACE_O_TRACEVFORK |
                PTRACE_O_TRACECLONE;
#ifdef PTRACE_O_EXITKILL
    opts |= PTRACE_O_EXITKILL;
#endif
    if (ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)opts) != 0) {
        TRACE_LOG("pdocker-direct-trace: PTRACE_SETOPTIONS pid=%d failed: %s\n",
                  (int)pid, strerror(errno));
        return -1;
    }
    return 0;
}

static int continue_tracee(pid_t pid, int sig) {
    if (g_sync_usec > 0) usleep((useconds_t)g_sync_usec);
    int request = g_selective_trace ? PTRACE_CONT : PTRACE_SYSCALL;
    return ptrace(request, pid, NULL, (void *)(long)sig);
}

static int continue_tracee_to_syscall_exit(pid_t pid, int sig) {
    if (g_sync_usec > 0) usleep((useconds_t)g_sync_usec);
    return ptrace(PTRACE_SYSCALL, pid, NULL, (void *)(long)sig);
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

static int read_tracee_u32(pid_t pid, unsigned long long addr, uint32_t *out) {
    unsigned long long aligned = addr & ~(unsigned long long)(sizeof(long) - 1);
    unsigned shift = (unsigned)((addr - aligned) * 8u);
    errno = 0;
    long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)aligned, NULL);
    if (word == -1 && errno) return -1;
    *out = (uint32_t)(((unsigned long)word >> shift) & 0xffffffffUL);
    return 0;
}

static void maybe_advance_past_svc(pid_t pid, struct user_pt_regs *regs) {
    uint32_t insn = 0;
    if (read_tracee_u32(pid, regs->pc, &insn) == 0 && insn == 0xd4000001U) {
        regs->pc += 4;
    }
}

static int complete_emulated_syscall(pid_t pid, struct user_pt_regs *regs,
                                     unsigned long long result) {
    regs->regs[0] = result;
    regs->regs[8] = (unsigned long long)-1;
    maybe_advance_past_svc(pid, regs);
    return set_regs(pid, regs);
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

static int resolve_guest_host_path(const char *rootfs, const char *guest,
                                   char *out, size_t out_len, int *is_bind) {
    if (is_bind) *is_bind = 0;
    if (!should_rewrite_path(rootfs, guest)) return 0;
    int bind_rc = resolve_bind_path(guest, out, out_len);
    if (bind_rc < 0) return bind_rc;
    if (bind_rc > 0) {
        if (is_bind) *is_bind = 1;
        return 1;
    }
    if (snprintf(out, out_len, "%s%s", rootfs, guest) >= (int)out_len) {
        return -ENAMETOOLONG;
    }
    return 1;
}

static int path_has_parent_segment(const char *path) {
    if (!path) return 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
            return 1;
        }
        while (*p && *p != '/') p++;
    }
    return 0;
}

static void trace_interesting_path(pid_t pid, const char *context, int arg_index, const char *path) {
    if (!g_trace_paths) return;
    if (!path) return;
    if (strstr(path, "apt-dpkg-install") || strstr(path, "/var/cache/apt/archives/")) {
        fprintf(stderr, "pdocker-direct-path: pid=%d %s arg%d path=%s\n",
                (int)pid, context, arg_index, path);
    }
}

static int rewrite_path_arg_scratch(pid_t pid, struct user_pt_regs *regs, int arg_index,
                                    const char *rootfs, const char *context,
                                    unsigned long long scratch_offset) {
    char original[PATH_MAX];
    char rewritten[PATH_MAX];
    if (read_tracee_string(pid, regs->regs[arg_index], original, sizeof(original)) < 0) {
        return 0;
    }
    trace_interesting_path(pid, context, arg_index, original);
    int bind_path = 0;
    int resolved = resolve_guest_host_path(rootfs, original, rewritten, sizeof(rewritten), &bind_path);
    if (resolved == 0) return 0;
    if (resolved < 0) {
        fprintf(stderr, "pdocker-direct-trace: pid=%d path too long for %s: %s\n",
                (int)pid, context, original);
        return 0;
    }
    unsigned long long scratch = (regs->sp - scratch_offset) & ~15ULL;
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

static int rewrite_at_path_arg(pid_t pid, struct user_pt_regs *regs, int dirfd_index,
                               int path_index, const char *rootfs, const char *context,
                               unsigned long long scratch_offset) {
    char original[PATH_MAX];
    if (read_tracee_string(pid, regs->regs[path_index], original, sizeof(original)) < 0) {
        return 0;
    }
    trace_interesting_path(pid, context, path_index, original);
    char rewritten[PATH_MAX];
    int bind_path = 0;
    int resolved = resolve_guest_host_path(rootfs, original, rewritten, sizeof(rewritten), &bind_path);
    if (resolved == 0) return 0;
    if (resolved < 0) {
        fprintf(stderr, "pdocker-direct-trace: pid=%d path too long for %s: %s\n",
                (int)pid, context, original);
        return 0;
    }

    if (!bind_path &&
        g_rootfd_rewrite &&
        g_rootfs_fd >= 0 &&
        original[0] == '/' &&
        original[1] != '\0' &&
        original[1] != '/' &&
        !path_has_parent_segment(original)) {
        regs->regs[dirfd_index] = (unsigned long long)g_rootfs_fd;
        regs->regs[path_index] = regs->regs[path_index] + 1;
        TRACE_LOG("pdocker-direct-trace: pid=%d rootfd-rewrite %s %s -> fd=%d %s\n",
                  (int)pid, context, original, g_rootfs_fd, original + 1);
        return 1;
    }

    unsigned long long scratch = (regs->sp - scratch_offset) & ~15ULL;
    if (write_tracee_string(pid, scratch, rewritten) != 0) {
        fprintf(stderr, "pdocker-direct-trace: pid=%d path rewrite failed for %s: %s -> %s (%s)\n",
                (int)pid, context, original, rewritten, strerror(errno));
        return 0;
    }
    regs->regs[path_index] = scratch;
    TRACE_LOG("pdocker-direct-trace: pid=%d rewrite %s %s -> %s\n",
              (int)pid, context, original, rewritten);
    return 1;
}

static int rewrite_path_arg(pid_t pid, struct user_pt_regs *regs, int arg_index,
                            const char *rootfs, const char *context) {
    return rewrite_path_arg_scratch(pid, regs, arg_index, rootfs, context, 8192u);
}

static void normalize_guest_path(const char *base, const char *path, char *out, size_t out_len) {
    char combined[PATH_MAX];
    if (!path || !path[0]) {
        snprintf(out, out_len, "%s", base && base[0] ? base : "/");
        return;
    }
    if (path[0] == '/') {
        snprintf(combined, sizeof(combined), "%s", path);
    } else {
        snprintf(combined, sizeof(combined), "%s/%s", base && base[0] ? base : "/", path);
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", combined);
    char *parts[256];
    int count = 0;
    char *save = NULL;
    for (char *part = strtok_r(tmp, "/", &save); part && count < 256; part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            if (count > 0) count--;
            continue;
        }
        parts[count++] = part;
    }

    size_t pos = 0;
    if (out_len == 0) return;
    out[pos++] = '/';
    for (int i = 0; i < count && pos + 1 < out_len; ++i) {
        if (i > 0 && pos + 1 < out_len) out[pos++] = '/';
        size_t len = strlen(parts[i]);
        if (len > out_len - pos - 1) len = out_len - pos - 1;
        memcpy(out + pos, parts[i], len);
        pos += len;
    }
    out[pos] = '\0';
}

static int rewrite_chdir_arg(pid_t pid, struct user_pt_regs *regs, TraceeState *state,
                             const char *rootfs) {
    char original[PATH_MAX];
    if (read_tracee_string(pid, regs->regs[0], original, sizeof(original)) >= 0 && state) {
        normalize_guest_path(state->guest_cwd, original, state->pending_guest_cwd,
                             sizeof(state->pending_guest_cwd));
    }
    return rewrite_path_arg(pid, regs, 0, rootfs, "chdir");
}

static int rewrite_path_args(pid_t pid, struct user_pt_regs *regs, int arg_a, int arg_b,
                             const char *rootfs, const char *context) {
    int rewrote = 0;
    rewrote |= rewrite_path_arg_scratch(pid, regs, arg_a, rootfs, context, 16384u);
    rewrote |= rewrite_path_arg_scratch(pid, regs, arg_b, rootfs, context, 8192u);
    return rewrote;
}

static int rewrite_at_path_args(pid_t pid, struct user_pt_regs *regs,
                                int dirfd_a, int path_a, int dirfd_b, int path_b,
                                const char *rootfs, const char *context) {
    int rewrote = 0;
    rewrote |= rewrite_at_path_arg(pid, regs, dirfd_a, path_a, rootfs, context, 16384u);
    rewrote |= rewrite_at_path_arg(pid, regs, dirfd_b, path_b, rootfs, context, 8192u);
    return rewrote;
}

static int emulate_getcwd(pid_t pid, struct user_pt_regs *regs, TraceeState *state,
                          const char *rootfs, unsigned long long *result) {
    unsigned long long buf = regs->regs[0];
    unsigned long long size = regs->regs[1];
    if (!buf || size == 0) {
        *result = (unsigned long long)-EINVAL;
        return 1;
    }
    const char *guest = "/";
    if (state && state->guest_cwd[0]) {
        guest = state->guest_cwd;
    } else {
        char proc_cwd[128];
        char host_cwd[PATH_MAX];
        snprintf(proc_cwd, sizeof(proc_cwd), "/proc/%d/cwd", (int)pid);
        ssize_t n = readlink(proc_cwd, host_cwd, sizeof(host_cwd) - 1);
        if (n < 0) {
            *result = (unsigned long long)-errno;
            return 1;
        }
        host_cwd[n] = '\0';
        size_t root_len = strlen(rootfs);
        if (strncmp(host_cwd, rootfs, root_len) == 0 &&
            (host_cwd[root_len] == '\0' || host_cwd[root_len] == '/')) {
            guest = host_cwd + root_len;
            if (!guest[0]) guest = "/";
        }
    }
    size_t need = strlen(guest) + 1;
    if (need > size) {
        *result = (unsigned long long)-ERANGE;
        return 1;
    }
    if (write_tracee_data(pid, buf, guest, need) != 0) {
        *result = (unsigned long long)-errno;
        return 1;
    }
    *result = (unsigned long long)need;
    return 1;
}

static int emulate_proc_self_exe_readlinkat(pid_t pid, struct user_pt_regs *regs,
                                            TraceeState *state,
                                            unsigned long long *result) {
    if (!state || !state->exec_guest_path[0]) return 0;
    char path[PATH_MAX];
    if (read_tracee_string(pid, regs->regs[1], path, sizeof(path)) < 0) return 0;
    int is_proc_pid_exe = 0;
    if (strncmp(path, "/proc/", 6) == 0) {
        const char *rest = path + 6;
        while (*rest >= '0' && *rest <= '9') rest++;
        is_proc_pid_exe = strcmp(rest, "/exe") == 0;
    }
    if (strcmp(path, "/proc/self/exe") != 0 &&
        strcmp(path, "/proc/thread-self/exe") != 0 &&
        !is_proc_pid_exe) {
        return 0;
    }
    unsigned long long buf = regs->regs[2];
    unsigned long long size = regs->regs[3];
    if (!buf || size == 0) {
        *result = (unsigned long long)-EINVAL;
        return 1;
    }
    size_t len = strlen(state->exec_guest_path);
    if (len > size) len = (size_t)size;
    if (write_tracee_data(pid, buf, state->exec_guest_path, len) != 0) {
        *result = (unsigned long long)-errno;
        return 1;
    }
    *result = (unsigned long long)len;
    return 1;
}

static int rewrite_proc_self_exe_readlinkat(pid_t pid, struct user_pt_regs *regs,
                                            TraceeState *state, const char *rootfs) {
    if (!state || !state->exec_guest_path[0]) return 0;
    char path[PATH_MAX];
    if (read_tracee_string(pid, regs->regs[1], path, sizeof(path)) < 0) return 0;
    int is_proc_pid_exe = 0;
    if (strncmp(path, "/proc/", 6) == 0) {
        const char *rest = path + 6;
        while (*rest >= '0' && *rest <= '9') rest++;
        is_proc_pid_exe = strcmp(rest, "/exe") == 0;
    }
    if (strcmp(path, "/proc/self/exe") != 0 &&
        strcmp(path, "/proc/thread-self/exe") != 0 &&
        !is_proc_pid_exe) {
        return 0;
    }
    char host_link[PATH_MAX];
    if (snprintf(host_link, sizeof(host_link), "%s/tmp/.pdocker-proc-exe-%d", rootfs, (int)pid) >= (int)sizeof(host_link)) {
        return 0;
    }
    unlink(host_link);
    if (symlink(state->exec_guest_path, host_link) != 0) return 0;
    unsigned long long scratch = (regs->sp - 8192u) & ~15ULL;
    if (write_tracee_string(pid, scratch, host_link) != 0) return 0;
    regs->regs[1] = scratch;
    return 1;
}

static int copy_file_for_linkat(const char *old_host, const char *new_host, int flags) {
    if (flags & ~AT_SYMLINK_FOLLOW) return -EINVAL;

    struct stat st;
    if (stat(old_host, &st) != 0) return -errno;
    if (!S_ISREG(st.st_mode)) return -EXDEV;

    int src = open(old_host, O_RDONLY | O_CLOEXEC);
    if (src < 0) return -errno;

    int dst = open(new_host, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);
    if (dst < 0 && errno == EEXIST && strstr(new_host, "/var/lib/dpkg/status-old")) {
        unlink(new_host);
        dst = open(new_host, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);
    }
    if (dst < 0) {
        int rc = -errno;
        close(src);
        return rc;
    }

    char buf[65536];
    int rc = 0;
    while (1) {
        ssize_t n = read(src, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            rc = -errno;
            break;
        }
        char *p = buf;
        ssize_t left = n;
        while (left > 0) {
            ssize_t w = write(dst, p, (size_t)left);
            if (w < 0) {
                rc = -errno;
                break;
            }
            p += w;
            left -= w;
        }
        if (rc != 0) break;
    }

    if (rc == 0 && fchmod(dst, st.st_mode & 07777) != 0) rc = -errno;
    if (close(dst) != 0 && rc == 0) rc = -errno;
    close(src);
    if (rc != 0) unlink(new_host);
    return rc;
}

static int host_path_is_under_rootfs(const char *rootfs, const char *path) {
    size_t root_len = strlen(rootfs);
    return strncmp(path, rootfs, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/');
}

static int resolve_tracee_host_path(pid_t pid, int dirfd, unsigned long long path_addr,
                                    const char *rootfs, char *out, size_t out_len,
                                    char *guest_out, size_t guest_len) {
    char guest[PATH_MAX];
    if (read_tracee_string(pid, path_addr, guest, sizeof(guest)) < 0) return 0;
    if (guest_out && guest_len > 0) {
        snprintf(guest_out, guest_len, "%s", guest);
    }
    if (guest[0] == '\0') return -ENOENT;

    if (guest[0] == '/') {
        return resolve_guest_host_path(rootfs, guest, out, out_len, NULL);
    }

    char proc_path[64];
    if (dirfd == AT_FDCWD) {
        snprintf(proc_path, sizeof(proc_path), "/proc/%d/cwd", (int)pid);
    } else {
        snprintf(proc_path, sizeof(proc_path), "/proc/%d/fd/%d", (int)pid, dirfd);
    }

    char base[PATH_MAX];
    ssize_t n = readlink(proc_path, base, sizeof(base) - 1);
    if (n < 0) return 0;
    base[n] = '\0';
    if (!host_path_is_under_rootfs(rootfs, base)) return 0;
    if (snprintf(out, out_len, "%s/%s", base, guest) >= (int)out_len) return -ENAMETOOLONG;
    return 1;
}

static int emulate_linkat_copy(pid_t pid, struct user_pt_regs *regs, const char *rootfs,
                               unsigned long long *result) {
    char old_host[PATH_MAX];
    char new_host[PATH_MAX];
    char old_guest[PATH_MAX];
    char new_guest[PATH_MAX];
    old_guest[0] = '\0';
    new_guest[0] = '\0';

    int old_rc = resolve_tracee_host_path(pid, (int)regs->regs[0], regs->regs[1],
                                          rootfs, old_host, sizeof(old_host),
                                          old_guest, sizeof(old_guest));
    int new_rc = resolve_tracee_host_path(pid, (int)regs->regs[2], regs->regs[3],
                                          rootfs, new_host, sizeof(new_host),
                                          new_guest, sizeof(new_guest));
    if (g_trace_linkat) {
        fprintf(stderr,
                "pdocker-direct-linkat: pid=%d olddir=%d old=%s rc=%d -> %s newdir=%d new=%s rc=%d -> %s flags=%llu\n",
                (int)pid, (int)regs->regs[0], old_guest, old_rc, old_rc > 0 ? old_host : "",
                (int)regs->regs[2], new_guest, new_rc, new_rc > 0 ? new_host : "",
                (unsigned long long)regs->regs[4]);
    }
    if (old_rc == 0 || new_rc == 0) return 0;
    if (old_rc < 0 || new_rc < 0) {
        *result = (unsigned long long)((old_rc < 0) ? old_rc : new_rc);
        return 1;
    }

    int rc = copy_file_for_linkat(old_host, new_host, (int)regs->regs[4]);
    *result = (unsigned long long)rc;
    if (g_trace_linkat) {
        fprintf(stderr, "pdocker-direct-linkat: copy rc=%d %s -> %s\n", rc, old_host, new_host);
    }
    TRACE_LOG("pdocker-direct-trace: pid=%d emulate linkat copy %s -> %s rc=%d\n",
              (int)pid, old_host, new_host, rc);
    return 1;
}

static int emulate_runtime_tmp_symlinkat(pid_t pid, struct user_pt_regs *regs, const char *rootfs,
                                         unsigned long long *result) {
    char target_guest[PATH_MAX];
    char link_guest[PATH_MAX];
    char target_host[PATH_MAX];
    char link_host[PATH_MAX];

    if (read_tracee_string(pid, regs->regs[0], target_guest, sizeof(target_guest)) < 0) return 0;
    if (read_tracee_string(pid, regs->regs[2], link_guest, sizeof(link_guest)) < 0) return 0;
    trace_interesting_path(pid, "symlinkat-target", 0, target_guest);
    trace_interesting_path(pid, "symlinkat-link", 2, link_guest);
    if (target_guest[0] != '/' || !strstr(target_guest, "/var/cache/apt/archives/")) return 0;
    if (!should_rewrite_path(rootfs, target_guest)) return 0;
    if (snprintf(target_host, sizeof(target_host), "%s%s", rootfs, target_guest) >= (int)sizeof(target_host)) {
        *result = (unsigned long long)-ENAMETOOLONG;
        return 1;
    }
    int link_rc = resolve_tracee_host_path(pid, (int)regs->regs[1], regs->regs[2],
                                           rootfs, link_host, sizeof(link_host),
                                           NULL, 0);
    if (link_rc <= 0) {
        if (link_rc < 0) {
            *result = (unsigned long long)link_rc;
        } else {
            *result = (unsigned long long)-ENOENT;
        }
        return 1;
    }
    int rc = copy_file_for_linkat(target_host, link_host, 0);
    *result = (unsigned long long)rc;
    if (g_trace_paths) {
        fprintf(stderr, "pdocker-direct-symlinkat: apt tmp copy %s -> %s result=%lld\n",
                link_host, target_host, (long long)*result);
    }
    return 1;
}

static int emulate_faccessat_path(pid_t pid, struct user_pt_regs *regs, const char *rootfs,
                                  unsigned long long *result) {
    char host[PATH_MAX];
    int rc = resolve_tracee_host_path(pid, (int)regs->regs[0], regs->regs[1],
                                      rootfs, host, sizeof(host), NULL, 0);
    if (rc == 0) {
        char original[PATH_MAX];
        if (read_tracee_string(pid, regs->regs[1], original, sizeof(original)) < 0) {
            *result = (unsigned long long)-EFAULT;
            return 1;
        }
        if (original[0] == '/') {
            snprintf(host, sizeof(host), "%s", original);
        } else {
            *result = (unsigned long long)-ENOENT;
            return 1;
        }
    } else if (rc < 0) {
        *result = (unsigned long long)rc;
        return 1;
    }
    if (access(host, (int)regs->regs[2]) == 0) {
        *result = 0;
    } else {
        *result = (unsigned long long)-errno;
    }
    TRACE_LOG("pdocker-direct-trace: emulate faccess path=%s mode=%lld result=%lld\n",
              host, (long long)regs->regs[2], (long long)*result);
    return 1;
}

static int should_skip_ldconfig(const char *path) {
    if (getenv("PDOCKER_DIRECT_LDCONFIG_REAL")) return 0;
    if (!path) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, "ldconfig") == 0 || strcmp(base, "ldconfig.real") == 0;
}

static int basename_is(const char *path, const char *name) {
    if (!path || !name) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, name) == 0;
}

static int group_exists_in_rootfs(const char *rootfs, const char *group_name) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/etc/group", rootfs) >= (int)sizeof(path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    size_t name_len = strlen(group_name);
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, group_name, name_len) == 0 && line[name_len] == ':') {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void append_group_to_rootfs(const char *rootfs, const char *group_name, const char *gid) {
    if (!group_name || !group_name[0] || group_exists_in_rootfs(rootfs, group_name)) return;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/etc/group", rootfs) < (int)sizeof(path)) {
        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f, "%s:x:%s:\n", group_name, gid && gid[0] ? gid : "999");
            fclose(f);
        }
    }
    if (snprintf(path, sizeof(path), "%s/etc/gshadow", rootfs) < (int)sizeof(path)) {
        FILE *f = fopen(path, "a");
        if (f) {
            fprintf(f, "%s:!::\n", group_name);
            fclose(f);
        }
    }
}

static int emulate_groupadd_from_argv(pid_t pid, const char *rootfs,
                                      unsigned long long *argv_ptrs, int argc) {
    char gid[64] = "999";
    char group_name[256] = "";
    for (int i = 1; i < argc; ++i) {
        char arg[PATH_MAX];
        if (read_tracee_string(pid, argv_ptrs[i], arg, sizeof(arg)) < 0) continue;
        if (strcmp(arg, "-g") == 0 && i + 1 < argc) {
            char next[64];
            if (read_tracee_string(pid, argv_ptrs[i + 1], next, sizeof(next)) >= 0) {
                snprintf(gid, sizeof(gid), "%s", next);
            }
            i++;
            continue;
        }
        if (arg[0] != '-') {
            snprintf(group_name, sizeof(group_name), "%s", arg);
        }
    }
    if (!group_name[0]) return 0;
    append_group_to_rootfs(rootfs, group_name, gid);
    TRACE_LOG("pdocker-direct-trace: emulate groupadd %s gid=%s\n", group_name, gid);
    return 1;
}

static int relative_path_between(const char *from_dir, const char *to_path, char *out, size_t out_len) {
    size_t common = 0;
    size_t last_slash = 0;
    while (from_dir[common] && to_path[common] && from_dir[common] == to_path[common]) {
        if (from_dir[common] == '/') last_slash = common;
        common++;
    }
    if (from_dir[common] == '\0' && (to_path[common] == '/' || to_path[common] == '\0')) {
        last_slash = common;
    }
    if (last_slash == 0) return -1;

    char tmp[PATH_MAX * 2];
    tmp[0] = '\0';
    const char *rest_from = from_dir + last_slash;
    while (*rest_from == '/') rest_from++;
    for (const char *p = rest_from; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        strncat(tmp, "../", sizeof(tmp) - strlen(tmp) - 1);
        while (*p && *p != '/') p++;
    }
    const char *rest_to = to_path + last_slash;
    while (*rest_to == '/') rest_to++;
    if (!tmp[0] && !*rest_to) {
        snprintf(tmp, sizeof(tmp), ".");
    } else {
        strncat(tmp, rest_to, sizeof(tmp) - strlen(tmp) - 1);
    }
    if (strlen(tmp) + 1 > out_len) return -1;
    snprintf(out, out_len, "%s", tmp);
    return 0;
}

static void normalize_absolute_symlinks_recursive(const char *rootfs, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int)sizeof(path)) continue;
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t n = readlink(path, target, sizeof(target) - 1);
            if (n <= 0) continue;
            target[n] = '\0';
            if (target[0] != '/' || !should_rewrite_path(rootfs, target)) continue;

            char host_target[PATH_MAX];
            if (snprintf(host_target, sizeof(host_target), "%s%s", rootfs, target) >= (int)sizeof(host_target)) {
                continue;
            }
            char link_dir[PATH_MAX];
            snprintf(link_dir, sizeof(link_dir), "%s", path);
            char *slash = strrchr(link_dir, '/');
            if (!slash) continue;
            *slash = '\0';

            char rel[PATH_MAX];
            if (relative_path_between(link_dir, host_target, rel, sizeof(rel)) != 0) continue;
            if (unlink(path) == 0) {
                if (symlink(rel, path) != 0) {
                    symlink(target, path);
                } else {
                    TRACE_LOG("pdocker-direct-trace: normalized symlink %s -> %s\n", path, rel);
                }
            }
        } else if (S_ISDIR(st.st_mode)) {
            normalize_absolute_symlinks_recursive(rootfs, path);
        }
    }
    closedir(d);
}

static int rewrite_execve_arg(pid_t pid, struct user_pt_regs *regs, TraceeState *state,
                              const char *rootfs, const char *loader, const char *libpath) {
    char original[PATH_MAX];
    char target[PATH_MAX];
    if (read_tracee_string(pid, regs->regs[0], original, sizeof(original)) < 0) {
        if (g_trace_exec) {
            fprintf(stderr, "pdocker-direct-exec: pid=%d read exec path failed addr=%llx\n",
                    (int)pid, (unsigned long long)regs->regs[0]);
        }
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
    if (access(target, F_OK) != 0) {
        if (g_trace_exec) {
            fprintf(stderr, "pdocker-direct-exec: pid=%d target missing %s -> %s: %s\n",
                    (int)pid, original, target, strerror(errno));
        }
        return 0;
    }
    if (should_skip_ldconfig(target)) {
        char true_path[PATH_MAX];
        if (resolve_guest_program(rootfs, "true", true_path, sizeof(true_path)) == 0) {
            TRACE_LOG("pdocker-direct-trace: pid=%d replace ldconfig exec %s -> %s\n",
                      (int)pid, target, true_path);
            snprintf(target, sizeof(target), "%s", true_path);
        }
    }
    int is_script = file_starts_with(target, "#!");
    char program[PATH_MAX];
    char script_interp_arg[PATH_MAX];
    int has_script_interp_arg = 0;
    program[0] = '\0';
    script_interp_arg[0] = '\0';
    if (is_script) {
        char interp[PATH_MAX];
        char interp_arg[PATH_MAX];
        if (parse_shebang(target, interp, sizeof(interp), interp_arg, sizeof(interp_arg)) == 0 &&
            resolve_guest_program(rootfs, interp, program, sizeof(program)) == 0) {
            if (interp_arg[0]) {
                snprintf(script_interp_arg, sizeof(script_interp_arg), "%s", interp_arg);
                has_script_interp_arg = 1;
            }
        } else if (snprintf(program, sizeof(program), "%s/bin/bash", rootfs) >= (int)sizeof(program) ||
                   access(program, X_OK) != 0) {
            if (snprintf(program, sizeof(program), "%s/bin/sh", rootfs) >= (int)sizeof(program)) {
                return 0;
            }
        }
    }

    unsigned long long old_argv = regs->regs[1];
    unsigned long long old_arg_ptrs[512];
    int old_argc = 0;
    for (; old_argc < 511; ++old_argc) {
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

    if (basename_is(target, "groupadd") &&
        emulate_groupadd_from_argv(pid, rootfs, old_arg_ptrs, old_argc)) {
        char true_path[PATH_MAX];
        if (resolve_guest_program(rootfs, "true", true_path, sizeof(true_path)) == 0) {
            snprintf(target, sizeof(target), "%s", true_path);
        }
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
    unsigned long long argv0_flag_addr = cursor;
    if (write_tracee_string(pid, argv0_flag_addr, "--argv0") != 0) return 0;
    cursor += strlen("--argv0") + 1;
    unsigned long long argv0_addr = cursor;
    if (write_tracee_string(pid, argv0_addr, is_script ? program : original) != 0) return 0;
    cursor += strlen(is_script ? program : original) + 1;
    unsigned long long target_addr = cursor;
    if (write_tracee_string(pid, target_addr, is_script ? program : target) != 0) return 0;
    cursor += strlen(is_script ? program : target) + 1;
    unsigned long long script_addr = 0;
    if (is_script) {
        script_addr = cursor;
        if (write_tracee_string(pid, script_addr, original) != 0) return 0;
        cursor += strlen(original) + 1;
    }
    unsigned long long script_interp_arg_addr = 0;
    if (has_script_interp_arg) {
        script_interp_arg_addr = cursor;
        if (write_tracee_string(pid, script_interp_arg_addr, script_interp_arg) != 0) return 0;
        cursor += strlen(script_interp_arg) + 1;
    }
    cursor = (cursor + 15u) & ~15ULL;

    unsigned long long new_argv[520];
    int n = 0;
    new_argv[n++] = loader_addr;
    new_argv[n++] = library_path_flag_addr;
    new_argv[n++] = libpath_addr;
    new_argv[n++] = argv0_flag_addr;
    new_argv[n++] = argv0_addr;
    new_argv[n++] = target_addr;
    if (has_script_interp_arg) new_argv[n++] = script_interp_arg_addr;
    if (is_script) new_argv[n++] = script_addr;
    for (int i = 1; i < old_argc && n < 519; ++i) {
        new_argv[n++] = old_arg_ptrs[i];
    }
    new_argv[n++] = 0;
    if (write_tracee_data(pid, cursor, new_argv, (size_t)n * sizeof(unsigned long long)) != 0) {
        return 0;
    }

    regs->regs[0] = loader_addr;
    regs->regs[1] = cursor;
    if (state) {
        const char *guest_exec = original;
        if (strncmp(guest_exec, rootfs, strlen(rootfs)) == 0) {
            guest_exec += strlen(rootfs);
            if (!guest_exec[0]) guest_exec = "/";
        }
        snprintf(state->exec_guest_path, sizeof(state->exec_guest_path), "%s", guest_exec);
    }
    if (g_trace_exec) {
        fprintf(stderr, "pdocker-direct-exec: pid=%d rewrite %s -> %s\n",
                (int)pid, original, target);
    }
    TRACE_LOG("pdocker-direct-trace: pid=%d rewrite execve via loader %s -> %s\n",
              (int)pid, original, target);
    return 1;
}

static int rewrite_syscall_paths(pid_t pid, struct user_pt_regs *regs, TraceeState *state, long nr,
                                 const char *rootfs, const char *loader, const char *libpath) {
    switch (nr) {
        case 5:   /* setxattr(path, name, value, size, flags) */
        case 6:   /* lsetxattr(path, name, value, size, flags) */
        case 8:   /* getxattr(path, name, value, size) */
        case 9:   /* lgetxattr(path, name, value, size) */
        case 11:  /* listxattr(path, list, size) */
        case 12:  /* llistxattr(path, list, size) */
        case 14:  /* removexattr(path, name) */
        case 15:  /* lremovexattr(path, name) */
            return rewrite_path_arg(pid, regs, 0, rootfs, syscall_name(nr));
        case 33:  /* mknodat(dirfd, pathname, mode, dev) */
        case 34:  /* mkdirat(dirfd, pathname, mode) */
        case 35:  /* unlinkat(dirfd, pathname, flags) */
        case 48:  /* faccessat(dirfd, pathname, mode) */
        case 53:  /* fchmodat(dirfd, pathname, mode, flags) */
        case 54:  /* fchownat(dirfd, pathname, owner, group, flags) */
        case 56:  /* openat(dirfd, pathname, flags, mode) */
        case 79:  /* newfstatat(dirfd, pathname, statbuf, flags) */
        case 88:  /* utimensat(dirfd, pathname, times, flags) */
        case 291: /* statx(dirfd, pathname, flags, mask, statxbuf) */
        case 437: /* openat2(dirfd, pathname, how, size) */
        case 439: /* faccessat2(dirfd, pathname, mode, flags) */
            return rewrite_at_path_arg(pid, regs, 0, 1, rootfs, syscall_name(nr), 8192u);
        case 78:  /* readlinkat(dirfd, pathname, buf, bufsiz) */
            if (rewrite_proc_self_exe_readlinkat(pid, regs, state, rootfs)) return 1;
            return rewrite_path_arg(pid, regs, 1, rootfs, syscall_name(nr));
        case 36:  /* symlinkat(target, newdirfd, linkpath) */
            return rewrite_at_path_arg(pid, regs, 1, 2, rootfs, syscall_name(nr), 8192u);
        case 37:  /* linkat(olddirfd, oldpath, newdirfd, newpath, flags) */
            return rewrite_at_path_args(pid, regs, 0, 1, 2, 3, rootfs, syscall_name(nr));
        case 38:  /* renameat(olddirfd, oldpath, newdirfd, newpath) */
        case 276: /* renameat2(olddirfd, oldpath, newdirfd, newpath, flags) */
            return rewrite_at_path_args(pid, regs, 0, 1, 2, 3, rootfs, syscall_name(nr));
        case 43:  /* statfs(pathname, buf) */
            return rewrite_path_arg(pid, regs, 0, rootfs, syscall_name(nr));
        case 49:  /* chdir(pathname) */
            return rewrite_chdir_arg(pid, regs, state, rootfs);
        case 221: /* execve(pathname, argv, envp) */
            return rewrite_execve_arg(pid, regs, state, rootfs, loader, libpath);
        case 281: /* execveat(dirfd, pathname, argv, envp, flags) */
            return rewrite_at_path_arg(pid, regs, 0, 1, rootfs, syscall_name(nr), 8192u);
        default:
            return 0;
    }
}

static int handle_syscall_entry(pid_t pid, struct user_pt_regs *regs, TraceeState *state,
                                const char *rootfs, const char *loader, const char *libpath,
                                int events, int *completed_in_userland) {
    if (completed_in_userland) *completed_in_userland = 0;
    state->last_nr = (long)regs->regs[8];
    record_syscall_stat(state->last_nr);
    for (int i = 0; i < 6; ++i) state->last_args[i] = regs->regs[i];
    int forced_emulation = 0;
    if (state->last_nr == 17 &&
        emulate_getcwd(pid, regs, state, rootfs, &state->emulated_result)) {
        forced_emulation = 1;
        if (complete_emulated_syscall(pid, regs, state->emulated_result) == 0) {
            if (completed_in_userland) *completed_in_userland = 1;
            state->emulated_nr = state->last_nr;
        } else {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs getcwd emulation failed: %s\n",
                    (int)pid, strerror(errno));
        }
    } else if (state->last_nr == -78 &&
               emulate_proc_self_exe_readlinkat(pid, regs, state, &state->emulated_result)) {
        forced_emulation = 1;
        if (complete_emulated_syscall(pid, regs, state->emulated_result) == 0) {
            if (completed_in_userland) *completed_in_userland = 1;
            state->emulated_nr = state->last_nr;
        } else {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs readlinkat emulation failed: %s\n",
                    (int)pid, strerror(errno));
        }
    } else if (state->last_nr >= 425 && state->last_nr <= 427) {
        forced_emulation = 1;
        state->emulated_result = (unsigned long long)-ENOSYS;
        if (complete_emulated_syscall(pid, regs, state->emulated_result) == 0) {
            if (completed_in_userland) *completed_in_userland = 1;
            state->emulated_nr = state->last_nr;
        } else {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs io_uring emulation failed: %s\n",
                    (int)pid, strerror(errno));
        }
    } else if (state->last_nr == 57 && (int)regs->regs[0] == g_rootfs_fd) {
        forced_emulation = 1;
        state->emulated_result = 0;
        if (complete_emulated_syscall(pid, regs, state->emulated_result) == 0) {
            if (completed_in_userland) *completed_in_userland = 1;
            state->emulated_nr = state->last_nr;
        } else {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs close(rootfs_fd) emulation failed: %s\n",
                    (int)pid, strerror(errno));
        }
    } else if (state->last_nr == 37 &&
        emulate_linkat_copy(pid, regs, rootfs, &state->emulated_result)) {
        forced_emulation = 1;
        if (complete_emulated_syscall(pid, regs, state->emulated_result) == 0) {
            if (completed_in_userland) *completed_in_userland = 1;
            state->emulated_nr = state->last_nr;
        } else {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs linkat emulation failed: %s\n",
                    (int)pid, strerror(errno));
        }
    } else if (state->last_nr == 36 &&
               emulate_runtime_tmp_symlinkat(pid, regs, rootfs, &state->emulated_result)) {
        forced_emulation = 1;
        if (complete_emulated_syscall(pid, regs, state->emulated_result) == 0) {
            if (completed_in_userland) *completed_in_userland = 1;
            state->emulated_nr = state->last_nr;
        } else {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs symlinkat emulation failed: %s\n",
                    (int)pid, strerror(errno));
        }
    } else if ((state->last_nr == 48 || state->last_nr == 439) &&
               emulate_faccessat_path(pid, regs, rootfs, &state->emulated_result)) {
        forced_emulation = 1;
        if (complete_emulated_syscall(pid, regs, state->emulated_result) == 0) {
            if (completed_in_userland) *completed_in_userland = 1;
            state->emulated_nr = state->last_nr;
        } else {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs faccess emulation failed: %s\n",
                    (int)pid, strerror(errno));
        }
    }
    int rewrote = 0;
    int remapped = 0;
    if (!forced_emulation) {
        rewrote = rewrite_syscall_paths(pid, regs, state, state->last_nr, rootfs, loader, libpath);
        long remapped_nr = syscall_remap_number(state->last_nr);
        remapped = remapped_nr != state->last_nr;
        if (remapped) {
            TRACE_LOG(
                    "pdocker-direct-trace: pid=%d remap nr=%ld(%s) -> %ld(%s)\n",
                    (int)pid, state->last_nr, syscall_name(state->last_nr),
                    remapped_nr, syscall_name(remapped_nr));
            regs->regs[8] = (unsigned long long)remapped_nr;
        }
    }
    int emulated_errno = 0;
    if (!forced_emulation && syscall_emulate_errno(state->last_nr, &emulated_errno)) {
        state->emulated_result = (unsigned long long)-emulated_errno;
        TRACE_LOG(
                "pdocker-direct-trace: pid=%d emulate-errno nr=%ld(%s) errno=%d via skipped syscall\n",
                (int)pid, state->last_nr, syscall_name(state->last_nr), emulated_errno);
        if (complete_emulated_syscall(pid, regs, state->emulated_result) == 0) {
            if (completed_in_userland) *completed_in_userland = 1;
            state->emulated_nr = state->last_nr;
        } else {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs errno emulation failed: %s\n",
                    (int)pid, strerror(errno));
        }
    } else if (!forced_emulation && syscall_emulate_success(state->last_nr)) {
        state->emulated_result = prepare_emulated_result(pid, state, state->last_nr);
        TRACE_LOG(
                "pdocker-direct-trace: pid=%d emulate-success nr=%ld(%s) result=%llu via skipped syscall\n",
                (int)pid, state->last_nr, syscall_name(state->last_nr),
                state->emulated_result);
        if (complete_emulated_syscall(pid, regs, state->emulated_result) == 0) {
            if (completed_in_userland) *completed_in_userland = 1;
            state->emulated_nr = state->last_nr;
        } else {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs entry failed: %s\n",
                    (int)pid, strerror(errno));
        }
    } else if (rewrote || remapped) {
        if (set_regs(pid, regs) != 0) {
            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs rewrite/remap failed: %s\n",
                    (int)pid, strerror(errno));
        }
    }
    if (events < 80 || state->last_nr == 221 || state->last_nr == 281 ||
        state->last_nr == 293 || state->last_nr == 439 || state->last_nr == 449) {
        TRACE_LOG(
                "pdocker-direct-trace: pid=%d enter #%d nr=%ld(%s) args=%llx,%llx,%llx,%llx,%llx,%llx\n",
                (int)pid, events, state->last_nr, syscall_name(state->last_nr),
                state->last_args[0], state->last_args[1], state->last_args[2],
                state->last_args[3], state->last_args[4], state->last_args[5]);
    }
    return 0;
}

static int trace_and_exec(char *const exec_argv[], const char *rootfs, const char *libpath) {
#define TRACE_RETURN(rc_) do { g_trace_child_pgid = -1; return (rc_); } while (0)
    pid_t child = fork();
    if (child < 0) {
        perror("pdocker-direct-executor: fork tracer");
        return 126;
    }
    if (child == 0) {
        setpgid(0, 0);
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0) {
            perror("pdocker-direct-executor: PTRACE_TRACEME");
            _exit(126);
        }
        raise(SIGSTOP);
        if (g_selective_trace && install_selective_seccomp_trace_filter() != 0) {
            perror("pdocker-direct-executor: seccomp selective trace");
            _exit(126);
        }
        execve(exec_argv[0], exec_argv, environ);
        perror("pdocker-direct-executor: execve loader");
        _exit(126);
    }
    setpgid(child, child);
    g_trace_child_pgid = child;
    install_tracer_signal_handlers();

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        perror("pdocker-direct-executor: wait initial tracee");
        TRACE_RETURN(126);
    }
    if (!WIFSTOPPED(status)) {
        fprintf(stderr, "pdocker-direct-trace: child did not stop before exec status=0x%x\n", status);
        TRACE_RETURN(126);
    }

    TraceeState tracees[MAX_TRACEES];
    memset(tracees, 0, sizeof(tracees));
    TraceeState *child_state = add_tracee(tracees, child);
    if (!child_state) {
        fprintf(stderr, "pdocker-direct-trace: tracee table exhausted before start\n");
        TRACE_RETURN(126);
    }
    const char *initial_guest_cwd = getenv("PDOCKER_GUEST_CWD");
    if (initial_guest_cwd && initial_guest_cwd[0]) {
        normalize_guest_path("/", initial_guest_cwd, child_state->guest_cwd,
                             sizeof(child_state->guest_cwd));
    }
    int root_opts = set_trace_options(child);
    if (g_trace_exec) {
        fprintf(stderr, "pdocker-direct-exec: root setopts pid=%d rc=%d\n", (int)child, root_opts);
    }

    int events = 0;
    int root_done = 0;
    int root_rc = 126;
    if (g_stats) {
        memset(g_syscall_counts, 0, sizeof(g_syscall_counts));
        g_stop_count = 0;
        clock_gettime(CLOCK_MONOTONIC, &g_stats_start);
    }
    if (continue_tracee(child, 0) != 0) {
        perror("pdocker-direct-trace: initial PTRACE_SYSCALL");
        TRACE_RETURN(126);
    }

    while (1) {
        pid_t got = waitpid(-1, &status, __WALL);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD && root_done) {
                print_syscall_stats("echild-root-done", root_rc);
                TRACE_RETURN(root_rc);
            }
            if (errno == ECHILD) {
                int alive = prune_dead_tracees(tracees, getpid());
                fprintf(stderr,
                        "pdocker-direct-trace: no waitable tracees remain before root exit was observed (tracked=%d)\n",
                        alive);
                print_syscall_stats("echild-no-waitable-tracees", 126);
                TRACE_RETURN(126);
            }
            perror("pdocker-direct-trace: waitpid");
            TRACE_RETURN(126);
        }
        if (got == 0) {
            int alive = prune_dead_tracees(tracees, getpid());
            if (root_done && alive == 0) {
                print_syscall_stats("root-done-idle", root_rc);
                TRACE_RETURN(root_rc);
            }
            if (!root_done && alive == 0) {
                fprintf(stderr,
                        "pdocker-direct-trace: no live tracees remain before root exit was observed\n");
                print_syscall_stats("no-live-tracees", 126);
                TRACE_RETURN(126);
            }
            continue;
        }
        TraceeState *state = find_tracee(tracees, got);
        if (!state) {
            state = add_tracee(tracees, got);
            if (!state) {
                fprintf(stderr, "pdocker-direct-trace: tracee table exhausted for pid=%d\n", (int)got);
                continue_tracee(got, SIGKILL);
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
            if (root_done && tracee_count(tracees) == 0) {
                print_syscall_stats("root-exited", root_rc);
                TRACE_RETURN(root_rc);
            }
            continue;
        }
        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            if (got == child || g_trace_verbose) {
                fprintf(stderr,
                        "pdocker-direct-trace: pid=%d signaled sig=%d events=%d last_syscall=%ld(%s) args=%llx,%llx,%llx,%llx,%llx,%llx active=%d\n",
                        (int)got, sig, events, state->last_nr, syscall_name(state->last_nr),
                        state->last_args[0], state->last_args[1], state->last_args[2],
                        state->last_args[3], state->last_args[4], state->last_args[5],
                        tracee_count(tracees) - 1);
            }
            remove_tracee(tracees, got);
            if (got == child) {
                root_done = 1;
                root_rc = 128 + sig;
            }
            if (root_done && tracee_count(tracees) == 0) {
                print_syscall_stats("root-signaled", root_rc);
                TRACE_RETURN(root_rc);
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        if (g_stats) g_stop_count++;

        if (g_validate_tracees && !tracee_is_still_owned(getpid(), got)) {
            char summary[160];
            tracee_status_summary(got, summary, sizeof(summary));
            fprintf(stderr,
                    "pdocker-direct-trace: dropping detached stopped tracee pid=%d status=0x%x %s last=%ld(%s)\n",
                    (int)got, status, summary, state->last_nr,
                    syscall_name(state->last_nr));
            remove_tracee(tracees, got);
            continue;
        }

        int sig = WSTOPSIG(status);
        unsigned int event = (unsigned int)status >> 16;
        events++;

        if (sig == (SIGTRAP | 0x80)) {
            struct user_pt_regs regs;
            if (get_regs(got, &regs) == 0) {
                int completed_in_userland = 0;
                if (!state->in_syscall) {
                    handle_syscall_entry(got, &regs, state, rootfs, exec_argv[0], libpath,
                                         events, &completed_in_userland);
                } else if (state->emulated_nr >= 0) {
                    regs.regs[0] = state->emulated_result;
                    if (set_regs(got, &regs) != 0) {
                        fprintf(stderr, "pdocker-direct-trace: pid=%d setregs exit failed: %s\n",
                                (int)got, strerror(errno));
                    } else {
                        TRACE_LOG(
                                "pdocker-direct-trace: pid=%d emulate-success return nr=%ld(%s) -> %llu\n",
                                (int)got, state->emulated_nr, syscall_name(state->emulated_nr),
                                state->emulated_result);
                    }
                    state->last_emulated_nr = state->emulated_nr;
                    state->emulated_nr = -1;
                } else if (state->last_nr == 49 && (long long)regs.regs[0] == 0 &&
                           state->pending_guest_cwd[0]) {
                    snprintf(state->guest_cwd, sizeof(state->guest_cwd), "%s",
                             state->pending_guest_cwd);
                    state->pending_guest_cwd[0] = '\0';
                }
                    state->in_syscall = completed_in_userland ? 1 : !state->in_syscall;
            }
            if (continue_tracee(got, 0) != 0) break;
            continue;
        }

        if (sig == SIGSYS) {
            struct user_pt_regs regs;
            if (get_regs(got, &regs) == 0) {
                long current_nr = (long)regs.regs[8];
                int completed_current = current_nr == -1 &&
                                        syscall_completed_in_userland(state->last_nr);
                int suppressible = state->emulated_nr >= 0 ||
                                   (state->last_emulated_nr >= 0 &&
                                    syscall_completed_in_userland(state->last_emulated_nr)) ||
                                   completed_current;
                int emulated_errno = 0;
                if (!suppressible && current_nr == 17 &&
                    emulate_getcwd(got, &regs, state, rootfs, &state->emulated_result)) {
                    state->last_nr = current_nr;
                    for (int i = 0; i < 6; ++i) state->last_args[i] = regs.regs[i];
                    if (complete_emulated_syscall(got, &regs, state->emulated_result) == 0) {
                        state->last_emulated_nr = current_nr;
                        state->emulated_nr = -1;
                        state->in_syscall = 0;
                        suppressible = 1;
                    } else {
                        fprintf(stderr, "pdocker-direct-trace: pid=%d setregs direct getcwd SIGSYS emulation failed: %s\n",
                                (int)got, strerror(errno));
                    }
                } else if (!suppressible && (current_nr == 48 || current_nr == 439) &&
                           emulate_faccessat_path(got, &regs, rootfs, &state->emulated_result)) {
                    state->last_nr = current_nr;
                    for (int i = 0; i < 6; ++i) state->last_args[i] = regs.regs[i];
                    if (complete_emulated_syscall(got, &regs, state->emulated_result) == 0) {
                        state->last_emulated_nr = current_nr;
                        state->emulated_nr = -1;
                        state->in_syscall = 0;
                        suppressible = 1;
                    } else {
                        fprintf(stderr, "pdocker-direct-trace: pid=%d setregs direct faccess SIGSYS emulation failed: %s\n",
                                (int)got, strerror(errno));
                    }
                } else if (!suppressible && syscall_emulate_errno(current_nr, &emulated_errno)) {
                    state->last_nr = current_nr;
                    for (int i = 0; i < 6; ++i) state->last_args[i] = regs.regs[i];
                    state->emulated_result = (unsigned long long)-emulated_errno;
                    if (complete_emulated_syscall(got, &regs, state->emulated_result) == 0) {
                        state->last_emulated_nr = current_nr;
                        state->emulated_nr = -1;
                        state->in_syscall = 0;
                        suppressible = 1;
                    } else {
                        fprintf(stderr, "pdocker-direct-trace: pid=%d setregs direct errno SIGSYS emulation failed: %s\n",
                                (int)got, strerror(errno));
                    }
                } else if (!suppressible && syscall_emulate_success(current_nr)) {
                    state->last_nr = current_nr;
                    for (int i = 0; i < 6; ++i) state->last_args[i] = regs.regs[i];
                    state->emulated_result = prepare_emulated_result(got, state, current_nr);
                    if (complete_emulated_syscall(got, &regs, state->emulated_result) == 0) {
                        state->last_emulated_nr = current_nr;
                        state->emulated_nr = -1;
                        state->in_syscall = 0;
                        suppressible = 1;
                    } else {
                        fprintf(stderr, "pdocker-direct-trace: pid=%d setregs direct SIGSYS emulation failed: %s\n",
                                (int)got, strerror(errno));
                    }
                } else if (!suppressible && current_nr >= 425 && current_nr <= 427) {
                    state->last_nr = current_nr;
                    for (int i = 0; i < 6; ++i) state->last_args[i] = regs.regs[i];
                    state->emulated_result = (unsigned long long)-ENOSYS;
                    if (complete_emulated_syscall(got, &regs, state->emulated_result) == 0) {
                        state->last_emulated_nr = current_nr;
                        state->emulated_nr = -1;
                        state->in_syscall = 0;
                        suppressible = 1;
                    } else {
                        fprintf(stderr, "pdocker-direct-trace: pid=%d setregs direct io_uring SIGSYS emulation failed: %s\n",
                                (int)got, strerror(errno));
                    }
                } else if (!suppressible && current_nr == 57 && (int)regs.regs[0] == g_rootfs_fd) {
                    state->last_nr = current_nr;
                    for (int i = 0; i < 6; ++i) state->last_args[i] = regs.regs[i];
                    state->emulated_result = 0;
                    if (complete_emulated_syscall(got, &regs, state->emulated_result) == 0) {
                        state->last_emulated_nr = current_nr;
                        state->emulated_nr = -1;
                        state->in_syscall = 0;
                        suppressible = 1;
                    } else {
                        fprintf(stderr, "pdocker-direct-trace: pid=%d setregs direct close(rootfs_fd) SIGSYS emulation failed: %s\n",
                                (int)got, strerror(errno));
                    }
                }
                if (suppressible) {
                    if (state->emulated_nr >= 0) {
                        regs.regs[0] = state->emulated_result;
                        if (set_regs(got, &regs) != 0) {
                            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs SIGSYS suppression failed: %s\n",
                                    (int)got, strerror(errno));
                        }
                        state->last_emulated_nr = state->emulated_nr;
                        state->emulated_nr = -1;
                        state->in_syscall = 0;
                    } else if (completed_current) {
                        regs.regs[0] = state->emulated_result;
                        if (set_regs(got, &regs) != 0) {
                            fprintf(stderr, "pdocker-direct-trace: pid=%d setregs completed SIGSYS suppression failed: %s\n",
                                    (int)got, strerror(errno));
                        }
                        state->last_emulated_nr = state->last_nr;
                        state->in_syscall = 0;
                    }
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
                if (suppressible) {
                    TRACE_LOG(
                            "pdocker-direct-trace: pid=%d suppress SIGSYS after emulated nr=%ld(%s)\n",
                            (int)got, state->last_emulated_nr, syscall_name(state->last_emulated_nr));
                    state->last_emulated_nr = -1;
                    if (continue_tracee(got, 0) != 0) break;
                    continue;
                }
            } else {
                fprintf(stderr, "pdocker-direct-trace: pid=%d SIGSYS getregs failed: %s last=%ld(%s)\n",
                        (int)got, strerror(errno), state->last_nr, syscall_name(state->last_nr));
            }
            if (continue_tracee(got, SIGSYS) != 0) break;
            continue;
        }

        if (event == PTRACE_EVENT_SECCOMP) {
            struct user_pt_regs regs;
            if (get_regs(got, &regs) == 0) {
                int completed_in_userland = 0;
                handle_syscall_entry(got, &regs, state, rootfs, exec_argv[0], libpath,
                                     events, &completed_in_userland);
                if (completed_in_userland) {
                    state->in_syscall = 1;
                }
            } else {
                fprintf(stderr, "pdocker-direct-trace: pid=%d seccomp getregs failed: %s last=%ld(%s)\n",
                        (int)got, strerror(errno), state->last_nr, syscall_name(state->last_nr));
            }
            if (state->in_syscall && state->emulated_nr >= 0) {
                if (continue_tracee_to_syscall_exit(got, 0) != 0) break;
            } else if (state->last_nr == 49 && state->pending_guest_cwd[0]) {
                state->in_syscall = 1;
                if (continue_tracee_to_syscall_exit(got, 0) != 0) break;
            } else if (continue_tracee(got, 0) != 0) {
                break;
            }
            continue;
        }

        if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK || event == PTRACE_EVENT_CLONE) {
            unsigned long new_pid = 0;
            if (ptrace(PTRACE_GETEVENTMSG, got, NULL, &new_pid) == 0 && new_pid > 0) {
                TraceeState *new_state = add_tracee(tracees, (pid_t)new_pid);
                if (!new_state) {
                    fprintf(stderr, "pdocker-direct-trace: tracee table exhausted for event child=%lu\n", new_pid);
                } else {
                    if (state) {
                        new_state->uid = state->uid;
                        new_state->euid = state->euid;
                        new_state->suid = state->suid;
                        new_state->gid = state->gid;
                        new_state->egid = state->egid;
                        new_state->sgid = state->sgid;
                        snprintf(new_state->exec_guest_path, sizeof(new_state->exec_guest_path),
                                 "%s", state->exec_guest_path);
                        snprintf(new_state->guest_cwd, sizeof(new_state->guest_cwd),
                                 "%s", state->guest_cwd[0] ? state->guest_cwd : "/");
                    }
                    int opt_rc = set_trace_options((pid_t)new_pid);
                    if (g_trace_exec) {
                        fprintf(stderr, "pdocker-direct-exec: event=%u parent=%d new=%lu setopts=%d active=%d\n",
                                event, (int)got, new_pid, opt_rc, tracee_count(tracees));
                    }
                    TRACE_LOG(
                            "pdocker-direct-trace: event=%u parent=%d new_tracee=%lu active=%d\n",
                            event, (int)got, new_pid, tracee_count(tracees));
                }
            } else {
                fprintf(stderr, "pdocker-direct-trace: event=%u parent=%d GETEVENTMSG failed: %s\n",
                        event, (int)got, strerror(errno));
            }
            if (continue_tracee(got, 0) != 0) break;
            continue;
        }

        if (event == PTRACE_EVENT_EXEC || event == PTRACE_EVENT_SECCOMP || event == PTRACE_EVENT_EXIT) {
            TRACE_LOG("pdocker-direct-trace: pid=%d event=%u sig=%d last_syscall=%ld(%s)\n",
                      (int)got, event, sig, state->last_nr, syscall_name(state->last_nr));
            if (continue_tracee(got, 0) != 0) break;
            continue;
        }

        if (sig == SIGSTOP || sig == SIGTRAP) {
            set_trace_options(got);
            if (continue_tracee(got, 0) != 0) break;
            continue;
        }

        if (continue_tracee(got, sig) != 0) break;
    }
    fprintf(stderr, "pdocker-direct-trace: ptrace loop failed: %s\n", strerror(errno));
    print_syscall_stats("ptrace-loop-failed", 126);
    TRACE_RETURN(126);
#undef TRACE_RETURN
}

static int run_command(int argc, char **argv) {
    const char *mode = "run";
    const char *rootfs = NULL;
    const char *workdir = "/";
    const char **env_items = calloc((size_t)argc + 1, sizeof(char *));
    int env_count = 0;
    int bind_count = 0;
    int command_index = -1;
    int use_syscall_tracer = !env_flag_enabled("PDOCKER_DIRECT_DISABLE_SYSCALL_TRACE");
    int trace_syscall_logs = env_flag_enabled("PDOCKER_DIRECT_TRACE_SYSCALLS");
    g_trace_verbose = env_flag_enabled("PDOCKER_DIRECT_TRACE_VERBOSE") || trace_syscall_logs;
    g_trace_linkat = env_flag_enabled("PDOCKER_DIRECT_TRACE_LINKAT");
    g_trace_paths = env_flag_enabled("PDOCKER_DIRECT_TRACE_PATHS") || trace_syscall_logs;
    g_trace_exec = env_flag_enabled("PDOCKER_DIRECT_TRACE_EXEC");
    g_stats = env_flag_enabled("PDOCKER_DIRECT_STATS");
    g_rootfd_rewrite = env_flag_enabled("PDOCKER_DIRECT_ROOTFD_REWRITE");
    g_validate_tracees = env_flag_enabled("PDOCKER_DIRECT_VALIDATE_TRACEES");
    g_trace_stat_paths = !env_flag_enabled("PDOCKER_DIRECT_UNTRACED_STAT_PATHS");
    const char *trace_mode = getenv("PDOCKER_DIRECT_TRACE_MODE");
    g_selective_trace = !trace_mode || strcmp(trace_mode, "syscall") != 0;
    const char *sync_env = getenv("PDOCKER_DIRECT_SYNC_USEC");
    if (sync_env && sync_env[0]) {
        g_sync_usec = atoi(sync_env);
        if (g_sync_usec < 0) g_sync_usec = 0;
        if (g_sync_usec > 10000) g_sync_usec = 10000;
    }

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
            parse_bind_spec(value_after(&i, argc, argv, "--bind"));
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
    if (workdir[0] == '/') {
        int bind_path = 0;
        int resolved = resolve_guest_host_path(rootfs, workdir, cwd, sizeof(cwd), &bind_path);
        if (resolved < 0) {
            fprintf(stderr, "pdocker-direct-executor: workdir path too long\n");
            free(env_items);
            return 126;
        }
        if (resolved == 0) {
            if (snprintf(cwd, sizeof(cwd), "%s/%s", rootfs, workdir + 1) >= (int)sizeof(cwd)) {
                fprintf(stderr, "pdocker-direct-executor: workdir path too long\n");
                free(env_items);
                return 126;
            }
        }
    } else {
        if (snprintf(cwd, sizeof(cwd), "%s/%s", rootfs, workdir) >= (int)sizeof(cwd)) {
            fprintf(stderr, "pdocker-direct-executor: workdir path too long\n");
            free(env_items);
            return 126;
        }
    }
    if (chdir(cwd) != 0 && chdir(rootfs) != 0) {
        perror("pdocker-direct-executor: chdir rootfs/workdir");
        free(env_items);
        return 126;
    }
    if (g_rootfs_fd >= 0) {
        close(g_rootfs_fd);
        g_rootfs_fd = -1;
    }
    if (g_rootfd_rewrite) {
        int rootfd = open(rootfs, O_RDONLY | O_DIRECTORY);
        if (rootfd < 0) {
            perror("pdocker-direct-executor: open rootfs fd");
            free(env_items);
            return 126;
        }
        int high_rootfd = fcntl(rootfd, F_DUPFD, 1000);
        if (high_rootfd >= 0) {
            close(rootfd);
            g_rootfs_fd = high_rootfd;
        } else {
            g_rootfs_fd = rootfd;
        }
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

    if (!getenv("PDOCKER_DIRECT_PRESERVE_ABSOLUTE_SYMLINKS")) {
        normalize_absolute_symlinks_recursive(rootfs, rootfs);
    }

    char target[PATH_MAX];
    const char *cmd0 = argv[command_index];
    if (strchr(cmd0, '/') == NULL) {
        if (resolve_guest_program(rootfs, cmd0, target, sizeof(target)) != 0) {
            fprintf(stderr, "pdocker-direct-executor: command not found in rootfs PATH: %s\n", cmd0);
            free(env_items);
            return 127;
        }
    } else if (cmd0[0] == '/') {
        if (snprintf(target, sizeof(target), "%s%s", rootfs, cmd0) >= (int)sizeof(target)) {
            fprintf(stderr, "pdocker-direct-executor: command path too long\n");
            free(env_items);
            return 126;
        }
        if (access(target, X_OK) != 0) {
            fprintf(stderr, "pdocker-direct-executor: command not executable: %s\n", cmd0);
            free(env_items);
            return errno == ENOENT ? 127 : 126;
        }
    } else {
        if (snprintf(target, sizeof(target), "%s/%s", cwd, cmd0) >= (int)sizeof(target)) {
            fprintf(stderr, "pdocker-direct-executor: command path too long\n");
            free(env_items);
            return 126;
        }
        if (access(target, X_OK) != 0) {
            fprintf(stderr, "pdocker-direct-executor: command not executable: %s\n", cmd0);
            free(env_items);
            return errno == ENOENT ? 127 : 126;
        }
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
    setenv("SSL_CERT_FILE", "/etc/ssl/certs/ca-certificates.crt", 0);
    setenv("SSL_CERT_DIR", "/etc/ssl/certs", 0);
    setenv("NODE_EXTRA_CA_CERTS", "/etc/ssl/certs/ca-certificates.crt", 0);
    setenv("PWD", workdir, 1);
    setenv("PDOCKER_GUEST_CWD", workdir, 1);
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
    char shell_arg[PATH_MAX];
    const char *program = target;
    int has_shell_arg = 0;
    shell_arg[0] = '\0';
    if (is_script) {
        char interp[PATH_MAX];
        char interp_arg[PATH_MAX];
        if (parse_shebang(target, interp, sizeof(interp), interp_arg, sizeof(interp_arg)) == 0 &&
            resolve_guest_program(rootfs, interp, shell, sizeof(shell)) == 0) {
            if (interp_arg[0]) {
                snprintf(shell_arg, sizeof(shell_arg), "%s", interp_arg);
                has_shell_arg = 1;
            }
        } else {
            snprintf(shell, sizeof(shell), "%s/bin/bash", rootfs);
            if (access(shell, X_OK) != 0) snprintf(shell, sizeof(shell), "%s/bin/sh", rootfs);
        }
        program = shell;
    }

    int cmd_argc = argc - command_index;
    char **nargv = calloc((size_t)cmd_argc + 10, sizeof(char *));
    if (!nargv) {
        free(env_items);
        return 126;
    }
    int n = 0;
    nargv[n++] = (char *)loader;
    nargv[n++] = "--library-path";
    nargv[n++] = libpath;
    nargv[n++] = "--argv0";
    nargv[n++] = (char *)cmd0;
    if (preload[0]) {
        nargv[n++] = "--preload";
        nargv[n++] = preload;
    }
    nargv[n++] = (char *)program;
    if (is_script && has_shell_arg) nargv[n++] = shell_arg;
    if (is_script) nargv[n++] = target;
    for (int i = command_index + 1; i < argc; ++i) nargv[n++] = argv[i];
    nargv[n] = NULL;

    TRACE_LOG(
            "pdocker-direct-executor: mode=%s rootfs=%s workdir=%s env=%d bind=%d argv0=%s\n",
            mode, rootfs, workdir, env_count, bind_count, cmd0);
    if (use_syscall_tracer) {
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
        puts("cow-bind=0");
        puts("bind-path-rewrite=1");
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
