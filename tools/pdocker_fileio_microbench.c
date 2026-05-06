#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void die(const char *what) {
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(1);
}

static void ensure_dir(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) die("mkdir");
}

static void write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) die("write");
        p += n;
        len -= (size_t)n;
    }
}

static void read_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) die("read");
        if (n == 0) break;
        p += n;
        len -= (size_t)n;
    }
}

static void make_path(char *out, size_t out_len, const char *root, const char *name) {
    int n = snprintf(out, out_len, "%s/%s", root, name);
    if (n < 0 || (size_t)n >= out_len) {
        fprintf(stderr, "path too long: %s/%s\n", root, name);
        exit(2);
    }
}

static void make_indexed_path(char *out, size_t out_len, const char *root, const char *prefix, int index) {
    int n = snprintf(out, out_len, "%s/%s%05d", root, prefix, index);
    if (n < 0 || (size_t)n >= out_len) {
        fprintf(stderr, "path too long: %s/%s%d\n", root, prefix, index);
        exit(2);
    }
}

static double elapsed_ms(uint64_t start, uint64_t end) {
    return (double)(end - start) / 1000000.0;
}

static void emit(const char *label, uint64_t start, uint64_t end, uint64_t ops, uint64_t bytes) {
    printf("{\"label\":\"%s\",\"elapsed_ms\":%.6f,\"ops\":%" PRIu64 ",\"bytes\":%" PRIu64 "}\n",
           label, elapsed_ms(start, end), ops, bytes);
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : "/tmp/pdocker-fileio-micro";
    int files = argc > 2 ? atoi(argv[2]) : 256;
    int blocks = argc > 3 ? atoi(argv[3]) : 64;
    int block_size = argc > 4 ? atoi(argv[4]) : 4096;
    int do_fsync = argc > 5 ? atoi(argv[5]) : 0;
    int open_close_iters = argc > 6 ? atoi(argv[6]) : 1000;
    if (files <= 0 || blocks <= 0 || block_size <= 0 || block_size > 1048576 ||
        open_close_iters <= 0) {
        fprintf(stderr, "usage: %s ROOT FILES BLOCKS BLOCK_SIZE [FSYNC] [OPEN_CLOSE_ITERS]\n", argv[0]);
        return 2;
    }

    ensure_dir(root);
    char *buf = malloc((size_t)block_size);
    if (!buf) die("malloc");
    memset(buf, 0x5a, (size_t)block_size);

    char path[4096];
    uint64_t start;
    uint64_t end;
    uint64_t total_bytes = (uint64_t)blocks * (uint64_t)block_size;
    int fd;

    make_path(path, sizeof(path), root, "open-close.bin");
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) die("open open-close seed");
    write_all(fd, buf, 32);
    if (close(fd) != 0) die("close open-close seed");

    start = now_ns();
    for (int i = 0; i < open_close_iters; ++i) {
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) die("open open-close");
        if (close(fd) != 0) die("close open-close");
    }
    end = now_ns();
    emit("open_close", start, end, (uint64_t)open_close_iters, 0);

    int *fds = calloc((size_t)open_close_iters, sizeof(int));
    if (!fds) die("calloc fds");

    start = now_ns();
    for (int i = 0; i < open_close_iters; ++i) {
        fds[i] = open(path, O_RDONLY | O_CLOEXEC);
        if (fds[i] < 0) die("openat_only");
    }
    end = now_ns();
    emit("openat_only", start, end, (uint64_t)open_close_iters, 0);
    for (int i = 0; i < open_close_iters; ++i) {
        if (close(fds[i]) != 0) die("close openat cleanup");
    }

    for (int i = 0; i < open_close_iters; ++i) {
        fds[i] = open(path, O_RDONLY | O_CLOEXEC);
        if (fds[i] < 0) die("open close_only setup");
    }
    start = now_ns();
    for (int i = 0; i < open_close_iters; ++i) {
        if (close(fds[i]) != 0) die("close_only");
    }
    end = now_ns();
    emit("close_only", start, end, (uint64_t)open_close_iters, 0);

    struct stat st;
    start = now_ns();
    for (int i = 0; i < open_close_iters; ++i) {
        if (stat(path, &st) != 0) die("stat_same");
    }
    end = now_ns();
    emit("stat_same", start, end, (uint64_t)open_close_iters, 0);

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open fstat setup");
    start = now_ns();
    for (int i = 0; i < open_close_iters; ++i) {
        if (fstat(fd, &st) != 0) die("fstat_only");
    }
    end = now_ns();
    emit("fstat_only", start, end, (uint64_t)open_close_iters, 0);
    if (close(fd) != 0) die("close fstat setup");

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open lseek setup");
    start = now_ns();
    for (int i = 0; i < open_close_iters; ++i) {
        if (lseek(fd, 0, SEEK_SET) < 0) die("lseek_only");
    }
    end = now_ns();
    emit("lseek_only", start, end, (uint64_t)open_close_iters, 0);
    if (close(fd) != 0) die("close lseek setup");

    make_path(path, sizeof(path), root, "seq.bin");
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) die("open seq write");
    start = now_ns();
    for (int i = 0; i < blocks; ++i) write_all(fd, buf, (size_t)block_size);
    end = now_ns();
    emit("write_only", start, end, (uint64_t)blocks, total_bytes);
    start = now_ns();
    if (do_fsync && fsync(fd) != 0) die("fsync seq");
    end = now_ns();
    if (do_fsync) emit("fsync_once", start, end, 1, 0);
    if (close(fd) != 0) die("close seq write");

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open seq read");
    start = now_ns();
    for (int i = 0; i < blocks; ++i) read_all(fd, buf, (size_t)block_size);
    end = now_ns();
    emit("read_only", start, end, (uint64_t)blocks, total_bytes);
    if (close(fd) != 0) die("close seq read");

    make_path(path, sizeof(path), root, "chmod.bin");
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) die("open chmod seed");
    if (close(fd) != 0) die("close chmod seed");
    start = now_ns();
    for (int i = 0; i < files; ++i) {
        if (chmod(path, (i & 1) ? 0600 : 0644) != 0) die("chmod_same");
    }
    end = now_ns();
    emit("chmod_same", start, end, (uint64_t)files, 0);

    char path2[4096];
    make_path(path, sizeof(path), root, "rename-a.bin");
    make_path(path2, sizeof(path2), root, "rename-b.bin");
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) die("open rename seed");
    if (close(fd) != 0) die("close rename seed");
    start = now_ns();
    for (int i = 0; i < files; ++i) {
        if (rename(path, path2) != 0) die("rename a-b");
        if (rename(path2, path) != 0) die("rename b-a");
    }
    end = now_ns();
    emit("rename_pair", start, end, (uint64_t)files * 2u, 0);

    make_path(path, sizeof(path), root, "mkdir-rmdir");
    start = now_ns();
    for (int i = 0; i < files; ++i) {
        if (mkdir(path, 0700) != 0) die("mkdir_only");
        if (rmdir(path) != 0) die("rmdir_only");
    }
    end = now_ns();
    emit("mkdir_rmdir_pair", start, end, (uint64_t)files * 2u, 0);

    make_path(path, sizeof(path), root, "symlink-target.bin");
    make_path(path2, sizeof(path2), root, "symlink-link.bin");
    fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) die("open symlink target");
    if (close(fd) != 0) die("close symlink target");
    start = now_ns();
    for (int i = 0; i < files; ++i) {
        if (symlink("symlink-target.bin", path2) != 0) die("symlink_only");
        if (unlink(path2) != 0) die("unlink symlink");
    }
    end = now_ns();
    emit("symlink_unlink_pair", start, end, (uint64_t)files * 2u, 0);

    if (symlink("symlink-target.bin", path2) != 0) die("symlink readlink setup");
    start = now_ns();
    for (int i = 0; i < files; ++i) {
        char linkbuf[256];
        ssize_t n = readlink(path2, linkbuf, sizeof(linkbuf));
        if (n < 0) die("readlink_only");
    }
    end = now_ns();
    emit("readlink_only", start, end, (uint64_t)files, 0);
    if (unlink(path2) != 0) die("unlink readlink setup");

    start = now_ns();
    for (int i = 0; i < files; ++i) {
        make_indexed_path(path, sizeof(path), root, "small.", i);
        fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0) die("open small write");
        write_all(fd, buf, 32);
        if (close(fd) != 0) die("close small write");
    }
    end = now_ns();
    emit("small_create", start, end, (uint64_t)files, (uint64_t)files * 32u);

    start = now_ns();
    for (int i = 0; i < files; ++i) {
        make_indexed_path(path, sizeof(path), root, "small.", i);
        if (stat(path, &st) != 0) die("stat small");
    }
    end = now_ns();
    emit("small_stat", start, end, (uint64_t)files, 0);

    start = now_ns();
    for (int i = 0; i < files; ++i) {
        make_indexed_path(path, sizeof(path), root, "small.", i);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) die("open small read");
        read_all(fd, buf, 32);
        if (close(fd) != 0) die("close small read");
    }
    end = now_ns();
    emit("small_read", start, end, (uint64_t)files, (uint64_t)files * 32u);

    start = now_ns();
    for (int i = 0; i < files; ++i) {
        make_indexed_path(path, sizeof(path), root, "small.", i);
        if (unlink(path) != 0) die("unlink small");
    }
    if (unlink((snprintf(path, sizeof(path), "%s/seq.bin", root), path)) != 0) die("unlink seq");
    if (unlink((snprintf(path, sizeof(path), "%s/open-close.bin", root), path)) != 0) die("unlink open-close");
    if (unlink((snprintf(path, sizeof(path), "%s/chmod.bin", root), path)) != 0) die("unlink chmod");
    if (unlink((snprintf(path, sizeof(path), "%s/rename-a.bin", root), path)) != 0) die("unlink rename");
    if (unlink((snprintf(path, sizeof(path), "%s/symlink-target.bin", root), path)) != 0) die("unlink symlink target");
    end = now_ns();
    emit("unlink", start, end, (uint64_t)files + 5u, 0);

    free(fds);
    free(buf);
    return 0;
}
