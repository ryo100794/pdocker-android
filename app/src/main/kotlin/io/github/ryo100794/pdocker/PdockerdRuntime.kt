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
 *   ├── bin/pdockerd       (extracted from assets/pdockerd/pdockerd)
 *   ├── docker-bin/crane   (-> nativeLibraryDir/libcrane.so)
 *   ├── docker-bin/proot   (-> nativeLibraryDir/libproot.so)
 *   └── lib/libcow.so      (-> nativeLibraryDir/libcow.so)
 *
 * pdockerd derives _PROJECT_DIR = dirname(dirname(__file__)), so running
 * runtime/bin/pdockerd makes it find crane/proot/libcow via the expected
 * relative paths. Symlinks point at the real ELFs in nativeLibraryDir —
 * that's the only location where Android allows execve on API 29+.
 */
object PdockerdRuntime {
    private const val TAG = "pdockerd-runtime"

    fun prepare(ctx: Context): File {
        val root = File(ctx.filesDir, "pdocker-runtime")
        val bin = File(root, "bin").apply { mkdirs() }
        val dockerBin = File(root, "docker-bin").apply { mkdirs() }
        val lib = File(root, "lib").apply { mkdirs() }

        val nativeDir = File(ctx.applicationInfo.nativeLibraryDir)
        val versionStamp = File(root, ".apk-version")
        val currentVersion = longVersionCode(ctx).toString()

        val versionChanged = versionStamp.readTextOrNull() != currentVersion
        extractAsset(ctx, "pdockerd/pdockerd", File(bin, "pdockerd"), versionChanged)
        linkTo(File(nativeDir, "libcrane.so"),  File(dockerBin, "crane"))
        linkTo(File(nativeDir, "libproot.so"),  File(dockerBin, "proot"))
        linkTo(File(nativeDir, "libcow.so"),    File(lib, "libcow.so"))
        // proot dynamically links libtalloc; surface it via runtime/lib/ so
        // LD_LIBRARY_PATH=runtime/lib is enough to resolve all runtime deps
        // from one place. Android's linker doesn't auto-search a binary's
        // own dir on execve, so we can't just rely on nativeLibraryDir.
        linkTo(File(nativeDir, "libtalloc.so"), File(lib, "libtalloc.so"))

        if (versionChanged) versionStamp.writeText(currentVersion)

        return root
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
