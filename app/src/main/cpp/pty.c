/*
 * pty.c — JNI bridge between Kotlin and a pseudo-terminal child.
 *
 * Android NDK ships posix_openpt/grantpt/unlockpt/ptsname, so the
 * standard Unix98 pty dance works. fork()+execve() runs the child
 * inside the pty; parent gets the master fd and uses it as
 * bidirectional stdio + ioctl(TIOCSWINSZ) for resize.
 */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <android/log.h>

#define LOG_TAG "pdockerpty"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Use child-pid | 0x80000000 as the "fd"? No — callers expect a real
 * fd for read/write. Keep a process table so waitpid() can find the
 * pid later. Small and bounded (pty count is tiny).
 */
#define MAX_PTYS 32
static struct { int fd; pid_t pid; } g_ptys[MAX_PTYS];

static int remember(int fd, pid_t pid) {
    for (int i = 0; i < MAX_PTYS; i++) {
        if (g_ptys[i].fd == 0) {
            g_ptys[i].fd = fd;
            g_ptys[i].pid = pid;
            return 0;
        }
    }
    return -1;
}

static pid_t lookup(int fd) {
    for (int i = 0; i < MAX_PTYS; i++) {
        if (g_ptys[i].fd == fd) return g_ptys[i].pid;
    }
    return -1;
}

static void forget(int fd) {
    for (int i = 0; i < MAX_PTYS; i++) {
        if (g_ptys[i].fd == fd) { g_ptys[i].fd = 0; g_ptys[i].pid = 0; return; }
    }
}

static char **jstrarr_to_cstrarr(JNIEnv *env, jobjectArray arr) {
    jsize n = (*env)->GetArrayLength(env, arr);
    char **out = calloc(n + 1, sizeof(char *));
    if (!out) return NULL;
    for (jsize i = 0; i < n; i++) {
        jstring s = (jstring)(*env)->GetObjectArrayElement(env, arr, i);
        const char *c = (*env)->GetStringUTFChars(env, s, NULL);
        out[i] = strdup(c);
        (*env)->ReleaseStringUTFChars(env, s, c);
        (*env)->DeleteLocalRef(env, s);
    }
    return out;
}

static void free_cstrarr(char **a) {
    if (!a) return;
    for (char **p = a; *p; p++) free(*p);
    free(a);
}

JNIEXPORT jint JNICALL
Java_io_github_ryo100794_pdocker_PtyNative_open(JNIEnv *env, jobject this,
                                      jstring j_cmd, jobjectArray j_argv,
                                      jobjectArray j_env)
{
    const char *cmd = (*env)->GetStringUTFChars(env, j_cmd, NULL);
    char **argv = jstrarr_to_cstrarr(env, j_argv);
    char **envp = jstrarr_to_cstrarr(env, j_env);

    int master;
    pid_t pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) {
        LOGE("forkpty: %s", strerror(errno));
        (*env)->ReleaseStringUTFChars(env, j_cmd, cmd);
        free_cstrarr(argv); free_cstrarr(envp);
        return -1;
    }
    if (pid == 0) {
        /* child */
        execve(cmd, argv, envp);
        fprintf(stderr, "execve %s: %s\n", cmd, strerror(errno));
        _exit(127);
    }
    /* parent */
    (*env)->ReleaseStringUTFChars(env, j_cmd, cmd);
    free_cstrarr(argv); free_cstrarr(envp);

    int flags = fcntl(master, F_GETFL, 0);
    fcntl(master, F_SETFL, flags | O_NONBLOCK);

    if (remember(master, pid) < 0) {
        close(master);
        kill(pid, SIGTERM);
        return -1;
    }
    return master;
}

JNIEXPORT jint JNICALL
Java_io_github_ryo100794_pdocker_PtyNative_resize(JNIEnv *env, jobject this,
                                        jint fd, jint rows, jint cols)
{
    struct winsize ws = { .ws_row = rows, .ws_col = cols };
    return ioctl(fd, TIOCSWINSZ, &ws);
}

JNIEXPORT jint JNICALL
Java_io_github_ryo100794_pdocker_PtyNative_write(JNIEnv *env, jobject this,
                                       jint fd, jbyteArray j_data)
{
    jsize n = (*env)->GetArrayLength(env, j_data);
    jbyte *buf = (*env)->GetByteArrayElements(env, j_data, NULL);
    ssize_t w = write(fd, buf, n);
    (*env)->ReleaseByteArrayElements(env, j_data, buf, JNI_ABORT);
    return (jint)w;
}

JNIEXPORT jint JNICALL
Java_io_github_ryo100794_pdocker_PtyNative_read(JNIEnv *env, jobject this,
                                      jint fd, jbyteArray j_buf)
{
    jsize cap = (*env)->GetArrayLength(env, j_buf);
    jbyte *buf = (*env)->GetByteArrayElements(env, j_buf, NULL);
    ssize_t n;
    do {
        n = read(fd, buf, cap);
    } while (n < 0 && errno == EINTR);
    /* Block on EAGAIN — caller wants blocking semantics */
    if (n < 0 && errno == EAGAIN) {
        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        if (select(fd + 1, &rf, NULL, NULL, NULL) > 0) {
            n = read(fd, buf, cap);
        }
    }
    (*env)->ReleaseByteArrayElements(env, j_buf, buf, n > 0 ? 0 : JNI_ABORT);
    return (jint)n;
}

JNIEXPORT jint JNICALL
Java_io_github_ryo100794_pdocker_PtyNative_close(JNIEnv *env, jobject this, jint fd)
{
    pid_t pid = lookup(fd);
    if (pid > 0) kill(pid, SIGHUP);
    int r = close(fd);
    forget(fd);
    return r;
}

JNIEXPORT jint JNICALL
Java_io_github_ryo100794_pdocker_PtyNative_waitpid(JNIEnv *env, jobject this, jint fd)
{
    pid_t pid = lookup(fd);
    if (pid <= 0) return -1;
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
}
