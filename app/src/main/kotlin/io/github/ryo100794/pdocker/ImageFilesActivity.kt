package io.github.ryo100794.pdocker

import android.os.Bundle
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.nio.file.Files
import java.nio.file.LinkOption
import java.text.DateFormat
import java.util.Date

/**
 * Read-only browser for pulled image and container rootfs trees.
 *
 * This intentionally bypasses the docker CLI. The UI is inside the same app
 * sandbox as pdockerd, so files can be inspected directly without starting a
 * temporary container or round-tripping through docker cp.
 */
class ImageFilesActivity : AppCompatActivity() {
    private lateinit var title: TextView
    private lateinit var path: TextView
    private lateinit var body: LinearLayout

    private val imageRoot: File by lazy { File(filesDir, "pdocker/images") }
    private val containerRoot: File by lazy { File(filesDir, "pdocker/containers") }
    private var currentRoot: File? = null
    private var currentTitle: String? = null
    private var currentDir: File? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }

        val toolbar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }

        val back = Button(this).apply {
            text = getString(R.string.button_back)
            setOnClickListener { navigateBack() }
        }
        val refresh = Button(this).apply {
            text = getString(R.string.button_refresh)
            setOnClickListener { render() }
        }
        toolbar.addView(back)
        toolbar.addView(refresh)

        title = TextView(this).apply {
            text = getString(R.string.title_image_files)
            textSize = 20f
            setSingleLine(true)
            ellipsize = TextUtils.TruncateAt.END
        }

        path = TextView(this).apply {
            textSize = 13f
            setPadding(0, 12, 0, 12)
        }

        body = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        val scroll = ScrollView(this).apply {
            addView(body)
        }

        root.addView(toolbar)
        root.addView(title)
        root.addView(HorizontalScrollView(this).apply { addView(path) })
        root.addView(scroll, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0,
            1f
        ))
        setContentView(root)

        val selectedImage = intent.getStringExtra(EXTRA_IMAGE_NAME)
            ?.takeIf { it.isNotBlank() && it.indexOf(File.separatorChar) < 0 }
        val selectedContainer = intent.getStringExtra(EXTRA_CONTAINER_ID)
            ?.takeIf { it.isNotBlank() && it.indexOf(File.separatorChar) < 0 }
        when {
            selectedImage != null -> {
                val image = File(imageRoot, selectedImage)
                selectRoot(image.name, File(image, "rootfs"))
            }
            selectedContainer != null -> {
                val container = File(containerRoot, selectedContainer)
                val containerFsRoot = listOf(File(container, "rootfs"), File(container, "upper"))
                    .firstOrNull { it.isDirectory }
                if (containerFsRoot != null) {
                    selectRoot(getString(R.string.title_container_files_fmt, container.name.take(12)), containerFsRoot)
                }
            }
        }

        render()
    }

    private fun render() {
        body.removeAllViews()
        val root = currentRoot
        val dir = currentDir
        when {
            root == null -> renderImages()
            dir == null -> {
                currentDir = root
                render()
            }
            else -> renderDirectory(currentTitle ?: getString(R.string.title_image_files), root, dir)
        }
    }

    private fun renderImages() {
        title.text = getString(R.string.title_image_files)
        path.text = imageRoot.absolutePath
        val images = imageRoot.listFiles()
            ?.filter { File(it, "rootfs").isDirectory }
            ?.sortedBy { it.name }
            .orEmpty()

        if (images.isEmpty()) {
            addMessage(getString(R.string.message_no_images_refresh))
            return
        }

        images.forEach { image ->
            addRow(
                label = image.name,
                detail = summarizeDir(File(image, "rootfs")),
                onClick = {
                    selectRoot(image.name, File(image, "rootfs"))
                    render()
                }
            )
        }
    }

    private fun renderDirectory(displayTitle: String, root: File, dir: File) {
        val rootfs = root.canonicalFile
        val safeDir = runCatching { dir.canonicalFile }.getOrNull()
        if (safeDir == null || !safeDir.isInside(rootfs)) {
            addMessage(getString(R.string.message_outside_rootfs))
            currentDir = rootfs
            return
        }

        title.text = displayTitle
        path.text = "/" + rootfs.toPath().relativize(safeDir.toPath()).toString()
            .replace(File.separatorChar, '/')
            .trim('/')

        val parent = safeDir.parentFile
        if (parent != null && parent.isInside(rootfs)) {
            addRow("..", getString(R.string.detail_parent_directory)) {
                currentDir = parent
                render()
            }
        }

        val entries = safeDir.listFiles()?.sortedWith(
            compareBy<File> { !isDirectoryEntry(it) }.thenBy { it.name.lowercase() }
        ).orEmpty()
        if (entries.isEmpty()) {
            addMessage(getString(R.string.message_empty_directory))
            return
        }

        entries.forEach { entry ->
            addRow(entry.name, describe(entry)) {
                when {
                    isDirectoryEntry(entry) -> {
                        currentDir = entry
                        render()
                    }
                    isRegularFileEntry(entry) -> renderPreview(rootfs, entry)
                    else -> addMessage(describe(entry))
                }
            }
        }
    }

    private fun renderPreview(rootfs: File, file: File) {
        body.removeAllViews()
        val safeFile = runCatching { file.canonicalFile }.getOrNull()
        if (safeFile == null || !safeFile.isInside(rootfs)) {
            addMessage(getString(R.string.message_outside_rootfs))
            return
        }
        title.text = file.name
        path.text = "/" + rootfs.toPath().relativize(safeFile.toPath()).toString()
            .replace(File.separatorChar, '/')

        addRow("..", getString(R.string.detail_back_to_directory)) { render() }
        addMessage(describe(file))

        val maxPreview = 64 * 1024
        if (file.length() > maxPreview) {
            addMessage(getString(R.string.message_preview_too_large))
            return
        }

        val bytes = runCatching { file.readBytes() }.getOrElse {
            addMessage(getString(R.string.message_cannot_read_file, it.message.orEmpty()))
            return
        }
        if (bytes.any { it == 0.toByte() }) {
            addMessage(getString(R.string.message_binary_preview_hidden))
            return
        }
        TextView(this).apply {
            text = bytes.toString(Charsets.UTF_8)
            textSize = 13f
            setTextIsSelectable(true)
            setPadding(0, 16, 0, 16)
            body.addView(this)
        }
    }

    private fun navigateBack() {
        val root = currentRoot
        val dir = currentDir
        if (root == null) {
            finish()
            return
        }
        val rootfs = root.canonicalFile
        if (dir == null || dir.canonicalFile == rootfs) {
            currentRoot = null
            currentTitle = null
            currentDir = null
        } else {
            currentDir = dir.parentFile
        }
        render()
    }

    private fun addRow(label: String, detail: String, onClick: () -> Unit) {
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            isClickable = true
            setPadding(0, 18, 0, 18)
            setOnClickListener { onClick() }
            addView(TextView(this@ImageFilesActivity).apply {
                text = label
                textSize = 16f
                setSingleLine(true)
                ellipsize = TextUtils.TruncateAt.MIDDLE
            })
            addView(TextView(this@ImageFilesActivity).apply {
                text = detail
                textSize = 12f
                alpha = 0.72f
                setSingleLine(true)
                ellipsize = TextUtils.TruncateAt.END
            })
            body.addView(this)
            addDivider()
        }
    }

    private fun addMessage(text: String) {
        TextView(this).apply {
            this.text = text
            textSize = 14f
            setPadding(0, 18, 0, 18)
            body.addView(this)
        }
    }

    private fun addDivider() {
        View(this).apply {
            alpha = 0.18f
            setBackgroundColor(0xff888888.toInt())
            body.addView(this, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                1
            ))
        }
    }

    private fun describe(file: File): String {
        if (Files.isSymbolicLink(file.toPath())) {
            val target = runCatching { Files.readSymbolicLink(file.toPath()).toString() }
                .getOrDefault(getString(R.string.detail_unreadable))
            return getString(R.string.detail_symlink_fmt, target)
        }
        val type = when {
            isDirectoryEntry(file) -> getString(R.string.detail_directory)
            isRegularFileEntry(file) -> getString(R.string.detail_file)
            else -> getString(R.string.detail_special)
        }
        val size = if (isRegularFileEntry(file)) getString(R.string.detail_size_bytes_fmt, file.length()) else ""
        val modified = DateFormat.getDateTimeInstance().format(Date(file.lastModified()))
        return getString(R.string.detail_modified_fmt, type, size, modified)
    }

    private fun summarizeDir(dir: File): String {
        val count = dir.list()?.size ?: 0
        return getString(R.string.detail_rootfs_entries_fmt, count)
    }

    private fun selectRoot(label: String, root: File) {
        if (!root.isDirectory) return
        currentRoot = root
        currentTitle = label
        currentDir = root
    }

    private fun File.isInside(root: File): Boolean {
        val base = root.canonicalFile.toPath()
        val child = canonicalFile.toPath()
        return child == base || child.startsWith(base)
    }

    private fun isDirectoryEntry(file: File): Boolean =
        Files.isDirectory(file.toPath(), LinkOption.NOFOLLOW_LINKS)

    private fun isRegularFileEntry(file: File): Boolean =
        Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)

    companion object {
        const val EXTRA_IMAGE_NAME = "io.github.ryo100794.pdocker.extra.IMAGE_NAME"
        const val EXTRA_CONTAINER_ID = "io.github.ryo100794.pdocker.extra.CONTAINER_ID"
    }
}
