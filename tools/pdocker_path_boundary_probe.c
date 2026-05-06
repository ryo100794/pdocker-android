/*
 * Static direct-runtime probe for fd-relative path validation.
 *
 * It intentionally creates an absolute symlink escape, verifies that a
 * follow-final stat is denied by pdocker-direct, then unlinks the symlink and
 * verifies a same-name safe file is visible. This catches stale path-cache
 * entries around mutation syscalls.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int expect_errno(const char *label, int got, int want) {
    if (got == want) return 0;
    fprintf(stderr, "%s: errno=%d (%s), want=%d (%s)\n",
            label, got, strerror(got), want, strerror(want));
    return 1;
}

int main(void) {
    const char *base = "/tmp/pdocker-boundary-cache";
    const char *dir = "/tmp/pdocker-boundary-cache/a";
    int rc = 0;

    unlinkat(AT_FDCWD, "/tmp/pdocker-boundary-cache/a/escape", 0);
    unlinkat(AT_FDCWD, "/tmp/pdocker-boundary-cache/a/safe", 0);
    mkdir(base, 0700);
    mkdir(dir, 0700);

    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        perror("open boundary dir");
        return 2;
    }

    if (symlinkat("/data", dfd, "escape") != 0) {
        perror("symlinkat escape");
        close(dfd);
        return 3;
    }

    struct stat st;
    errno = 0;
    if (fstatat(dfd, "escape", &st, 0) == 0) {
        fprintf(stderr, "relative symlink escape unexpectedly succeeded\n");
        rc = 4;
        goto out;
    }
    rc = expect_errno("relative symlink escape", errno, EXDEV);
    if (rc) goto out;

    if (unlinkat(dfd, "escape", 0) != 0) {
        perror("unlinkat escape");
        rc = 5;
        goto out;
    }

    int fd = openat(dfd, "escape", O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) {
        perror("openat safe replacement");
        rc = 6;
        goto out;
    }
    if (write(fd, "safe\n", 5) != 5) {
        perror("write safe replacement");
        close(fd);
        rc = 7;
        goto out;
    }
    close(fd);

    if (fstatat(dfd, "escape", &st, 0) != 0) {
        perror("fstatat safe replacement");
        rc = 8;
        goto out;
    }

    puts("path_boundary_cache_ok");

out:
    close(dfd);
    return rc;
}
