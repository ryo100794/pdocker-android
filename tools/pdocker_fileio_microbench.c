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
    if (files <= 0 || blocks <= 0 || block_size <= 0 || block_size > 1048576) {
        fprintf(stderr, "usage: %s ROOT FILES BLOCKS BLOCK_SIZE [FSYNC]\n", argv[0]);
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

    make_path(path, sizeof(path), root, "seq.bin");
    start = now_ns();
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) die("open seq write");
    for (int i = 0; i < blocks; ++i) write_all(fd, buf, (size_t)block_size);
    if (do_fsync && fsync(fd) != 0) die("fsync seq");
    if (close(fd) != 0) die("close seq write");
    end = now_ns();
    emit("seq_write", start, end, (uint64_t)blocks, total_bytes);

    start = now_ns();
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) die("open seq read");
    for (int i = 0; i < blocks; ++i) read_all(fd, buf, (size_t)block_size);
    if (close(fd) != 0) die("close seq read");
    end = now_ns();
    emit("seq_read", start, end, (uint64_t)blocks, total_bytes);

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

    struct stat st;
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
    end = now_ns();
    emit("unlink", start, end, (uint64_t)files + 1u, 0);

    free(buf);
    return 0;
}
