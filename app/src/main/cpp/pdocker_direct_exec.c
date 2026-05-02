#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int run_command(int argc, char **argv) {
    const char *mode = "run";
    const char *rootfs = NULL;
    const char *workdir = "/";
    int env_count = 0;
    int bind_count = 0;
    int command_index = -1;

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
            (void)value_after(&i, argc, argv, "--env");
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
        return 2;
    }

    fprintf(stderr,
            "pdocker-direct-executor: helper installed, process-exec capability is disabled\n"
            "pdocker-direct-executor: mode=%s rootfs=%s workdir=%s env=%d bind=%d argv0=%s\n"
            "pdocker-direct-executor: rootfs process execution is not implemented yet; "
            "filesystem and syscall mediation must land before docker run/exec/compose services can start\n",
            mode, rootfs, workdir, env_count, bind_count, argv[command_index]);
    return 126;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--pdocker-direct-probe") == 0) {
        puts("pdocker-direct-executor:1");
        puts("process-exec=0");
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "run") == 0) {
        return run_command(argc, argv);
    }

    usage(stderr);
    return 2;
}
