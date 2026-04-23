package io.github.pdocker

import android.content.Context
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

        extractIfChanged(ctx, "pdockerd/pdockerd", File(bin, "pdockerd"))
        linkTo(File(nativeDir, "libcrane.so"), File(dockerBin, "crane"))
        linkTo(File(nativeDir, "libproot.so"), File(dockerBin, "proot"))
        linkTo(File(nativeDir, "libcow.so"),   File(lib, "libcow.so"))

        return root
    }

    private fun extractIfChanged(ctx: Context, assetPath: String, dst: File) {
        // Re-extract whenever the source asset's size differs from dst.
        // This catches APK upgrades without tracking a separate version file.
        val expected = ctx.assets.open(assetPath).use { it.available().toLong() }
        if (dst.exists() && dst.length() == expected) return
        ctx.assets.open(assetPath).use { input ->
            dst.outputStream().use { output -> input.copyTo(output) }
        }
        Log.i(TAG, "extracted $assetPath -> $dst (${dst.length()} bytes)")
    }

    private fun linkTo(target: File, link: File) {
        if (link.exists()) {
            // Clear stale regular file or mis-pointing symlink before recreating.
            val current = try { link.canonicalPath } catch (_: Exception) { null }
            if (current == target.canonicalPath) return
            link.delete()
        }
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
