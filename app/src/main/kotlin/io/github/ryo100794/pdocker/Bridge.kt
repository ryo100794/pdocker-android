package io.github.ryo100794.pdocker

import android.util.Base64
import android.webkit.JavascriptInterface
import android.webkit.WebView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Bridge between xterm.js (WebView) and a PTY child.
 *
 * JS side calls:
 *   PdockerBridge.start("docker exec -it CID sh")
 *   PdockerBridge.input(base64_utf8)
 *   PdockerBridge.resize(rows, cols)
 *
 * Kotlin pushes output back via `window.pdockerRecv(base64)` on the UI thread.
 */
class Bridge(
    private val activity: AppCompatActivity,
    private val webView: WebView,
    private val initialCommand: String = "sh",
    private val onOutput: ((ByteArray) -> Unit)? = null,
) {
    private var fd: Int = -1
    private var reader: Thread? = null
    private val alive = AtomicBoolean(false)

    @JavascriptInterface
    fun start(cmdline: String) {
        if (alive.get()) return
        val shell = detectShell()
        // Pass the requested cmdline to `sh -c` so xterm.js doesn't need
        // to tokenize.
        val argv = arrayOf("sh", "-c", cmdline)
        // Stage runtime so docker/crane/proot symlinks exist + sock path
        // is predictable. PdockerdRuntime.prepare is idempotent.
        val runtime = PdockerdRuntime.prepare(activity)
        val sock = File(activity.filesDir, "pdocker/pdockerd.sock")
        val env = arrayOf(
            "TERM=xterm-256color",
            "HOME=${activity.filesDir}",
            // docker CLI lives at runtime/docker-bin/docker (symlink into
            // nativeLibraryDir/libdocker.so). Putting docker-bin first on
            // PATH means the user can just type `docker ps` in xterm.
            "PATH=${runtime.absolutePath}/docker-bin:/system/bin:/system/xbin",
            "DOCKER_HOST=unix://${sock.absolutePath}",
            "DOCKER_BUILDKIT=0",
            "COMPOSE_DOCKER_CLI_BUILD=0",
            "BUILDKIT_PROGRESS=plain",
            "COMPOSE_PROGRESS=plain",
            "COMPOSE_MENU=false",
            // Docker Compose is a CLI plugin; point Docker at the runtime
            // plugin dir so `docker compose ...` works on Android too.
            "DOCKER_CONFIG=${runtime.absolutePath}/docker-bin"
        )
        fd = PtyNative.open(shell, argv, env)
        if (fd < 0) return
        alive.set(true)
        reader = Thread({
            val buf = ByteArray(4096)
            while (alive.get()) {
                val n = PtyNative.read(fd, buf)
                if (n <= 0) break
                onOutput?.invoke(buf.copyOf(n))
                val b64 = Base64.encodeToString(buf, 0, n, Base64.NO_WRAP)
                activity.runOnUiThread {
                    webView.evaluateJavascript("window.pdockerRecv('$b64')", null)
                }
            }
            alive.set(false)
        }, "pty-reader").also { it.start() }
    }

    @JavascriptInterface
    fun initialCommand(): String = initialCommand

    @JavascriptInterface
    fun input(b64: String) {
        if (!alive.get() || fd < 0) return
        val bytes = Base64.decode(b64, Base64.DEFAULT)
        PtyNative.write(fd, bytes)
    }

    @JavascriptInterface
    fun resize(rows: Int, cols: Int) {
        if (!alive.get() || fd < 0) return
        PtyNative.resize(fd, rows, cols)
    }

    fun close() {
        alive.set(false)
        if (fd >= 0) PtyNative.close(fd)
        fd = -1
        reader?.interrupt()
    }

    private fun detectShell(): String {
        // Prefer the bundled proot-run entrypoint once assets are unpacked;
        // fall back to /system/bin/sh for the scaffold phase.
        val bundled = File(activity.applicationInfo.nativeLibraryDir, "libpdocker-sh.so")
        return if (bundled.exists()) bundled.absolutePath else "/system/bin/sh"
    }
}
