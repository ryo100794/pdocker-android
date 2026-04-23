package io.github.pdocker

/**
 * JNI bridge to pty.c — fork/exec a child inside a pseudo-terminal.
 *
 * The Kotlin side treats the pty as a read/write fd:
 *   val fd = PtyNative.open("/bin/sh", arrayOf("sh"), arrayOf())
 *   PtyNative.resize(fd, rows, cols)
 *   PtyNative.write(fd, bytes)
 *   val n = PtyNative.read(fd, buf)
 *   PtyNative.close(fd)
 */
object PtyNative {
    init { System.loadLibrary("pdockerpty") }

    external fun open(cmd: String, argv: Array<String>, env: Array<String>): Int
    external fun resize(fd: Int, rows: Int, cols: Int): Int
    external fun write(fd: Int, data: ByteArray): Int
    external fun read(fd: Int, buf: ByteArray): Int
    external fun close(fd: Int): Int
    external fun waitpid(fd: Int): Int
}
