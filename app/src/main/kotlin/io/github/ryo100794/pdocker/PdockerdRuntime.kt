package io.github.ryo100794.pdocker

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import java.io.File

/**
 * Assembles a pdockerd runtime layout under filesDir/pdocker-runtime/
 * that matches what pdockerd expects at import time:
 *
 *   runtime/
 *   ├── bin/pdockerd
 *   ├── docker-bin/
 *   │   ├── crane          (-> nativeLibraryDir/libcrane.so)
 *   │   └── proot          (-> nativeLibraryDir/libproot.so)
 *   ├── etc/resolv.conf    (DNS nameservers; crane reads via proot bind)
 *   └── lib/
 *       ├── libcow.so      (-> nativeLibraryDir/libcow.so)
 *       └── libtalloc.so   (-> nativeLibraryDir/libtalloc.so, proot dep)
 *
 * pdockerd derives _PROJECT_DIR = dirname(dirname(__file__)), so running
 * runtime/bin/pdockerd makes it find everything via the expected relative
 * paths. Symlinks point at real ELFs in nativeLibraryDir — the only
 * location where Android allows execve on API 29+.
 *
 * DNS: crane is pure-Go with CGO disabled, so it reads /etc/resolv.conf
 * directly instead of going through Android's bionic resolver. Android
 * doesn't ship /etc/resolv.conf to apps, so crane falls back to [::1]:53
 * and hits "connection refused". We can't shim with a shell wrapper
 * because SELinux blocks execve of scripts under /data/data/<pkg>/files
 * (exec_no_trans on API 29+). Instead, pdockerd itself honours
 * PDOCKER_CRANE_WRAP: pdockerd_bridge.py sets it to the proot invocation
 * that bind-mounts our resolv.conf onto /etc/resolv.conf before crane
 * runs. proot itself is a symlink into nativeLibraryDir, so its execve
 * is allowed.
 */
object PdockerdRuntime {
    private const val TAG = "pdockerd-runtime"

    private const val FALLBACK_RESOLV_CONF = """nameserver 8.8.8.8
nameserver 1.1.1.1
"""

    fun prepare(ctx: Context): File {
        val root = File(ctx.filesDir, "pdocker-runtime")
        val bin = File(root, "bin").apply { mkdirs() }
        val dockerBin = File(root, "docker-bin").apply { mkdirs() }
        val lib = File(root, "lib").apply { mkdirs() }
        val etc = File(root, "etc").apply { mkdirs() }
        // proot needs a writable temp dir for its "glue rootfs" (fabricated
        // directory stubs for missing bind targets). Android app sandboxes
        // have no writable /tmp, so hand proot one inside the app's data dir
        // via PROOT_TMP_DIR (wired up in pdockerd_bridge.py).
        File(root, "tmp").apply { mkdirs() }

        val nativeDir = File(ctx.applicationInfo.nativeLibraryDir)
        val versionStamp = File(root, ".apk-version")
        val currentVersion = longVersionCode(ctx).toString()

        val versionChanged = versionStamp.readTextOrNull() != currentVersion
        extractAsset(ctx, "pdockerd/pdockerd", File(bin, "pdockerd"), versionChanged)

        linkTo(File(nativeDir, "libcrane.so"),         File(dockerBin, "crane"))
        linkTo(File(nativeDir, "libproot.so"),         File(dockerBin, "proot"))
        linkTo(File(nativeDir, "libproot-loader.so"),  File(dockerBin, "proot-loader"))
        linkTo(File(nativeDir, "libcow.so"),           File(lib, "libcow.so"))
        linkTo(File(nativeDir, "libtalloc.so"),        File(lib, "libtalloc.so"))

        writeIfChanged(File(etc, "resolv.conf"), FALLBACK_RESOLV_CONF)

        if (versionChanged) versionStamp.writeText(currentVersion)

        return root
    }

    private fun writeIfChanged(dst: File, content: String) {
        if (!dst.exists() || dst.readText() != content) {
            dst.writeText(content)
            Log.i(TAG, "wrote ${content.length} bytes to $dst")
        }
    }

    private fun extractAsset(ctx: Context, assetPath: String, dst: File, force: Boolean) {
        if (!force && dst.exists()) return
        ctx.assets.open(assetPath).use { input ->
            dst.outputStream().use { output -> input.copyTo(output) }
        }
        Log.i(TAG, "extracted $assetPath -> $dst (${dst.length()} bytes)")
    }

    private fun longVersionCode(ctx: Context): Long {
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            pi.longVersionCode
        } else {
            @Suppress("DEPRECATION") pi.versionCode.toLong()
        }
    }

    private fun File.readTextOrNull(): String? =
        if (exists()) runCatching { readText() }.getOrNull() else null

    private fun linkTo(target: File, link: File) {
        // Unconditionally delete the existing entry. link.exists() returns
        // false for a dangling symlink (canonicalPath to a now-deleted APK
        // install dir), and if we skip the delete, createSymbolicLink hits
        // EEXIST and the copyTo fallback follows the dead symlink into
        // nowhere with ENOENT. Files.deleteIfExists acts on the link itself,
        // not the target.
        java.nio.file.Files.deleteIfExists(link.toPath())
        try {
            java.nio.file.Files.createSymbolicLink(link.toPath(), target.toPath())
        } catch (e: Exception) {
            // Rare filesystems lack symlink support — fall back to a hard copy.
            target.copyTo(link, overwrite = true)
            link.setExecutable(true, false)
            Log.w(TAG, "symlink failed for $link, copied instead: ${e.message}")
        }
    }
}
