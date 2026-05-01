package io.github.ryo100794.pdocker

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Typeface
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.io.File
import kotlin.concurrent.thread
import org.json.JSONObject

class MainActivity : AppCompatActivity() {
    private enum class Tab { Overview, Compose, Dockerfiles, Images, Containers, Sessions }

    companion object {
        private const val REQUEST_POST_NOTIFICATIONS = 100
    }

    private val ui = Handler(Looper.getMainLooper())
    private val tabs = linkedMapOf(
        Tab.Overview to "Overview",
        Tab.Compose to "Compose",
        Tab.Dockerfiles to "Dockerfile",
        Tab.Images to "Images",
        Tab.Containers to "Containers",
        Tab.Sessions to "Sessions",
    )
    private val pollTask = object : Runnable {
        override fun run() {
            refreshStatus()
            ui.postDelayed(this, 3000)
        }
    }

    private lateinit var status: TextView
    private lateinit var content: LinearLayout
    private lateinit var tabRow: LinearLayout
    private var currentTab = Tab.Overview

    private val pdockerHome: File by lazy { File(filesDir, "pdocker") }
    private val imageRoot: File by lazy { File(pdockerHome, "images") }
    private val containerRoot: File by lazy { File(pdockerHome, "containers") }
    private val projectRoot: File by lazy { File(pdockerHome, "projects") }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestNotificationPermission()
        seedDefaultProject()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(28, 28, 28, 28)
        }

        status = TextView(this).apply {
            text = "pdockerd: unknown"
            textSize = 14f
            setSingleLine(true)
            ellipsize = TextUtils.TruncateAt.END
        }
        tabRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }

        root.addView(status)
        root.addView(HorizontalScrollView(this).apply { addView(tabRow) })
        root.addView(ScrollView(this).apply { addView(content) }, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0,
            1f,
        ))
        setContentView(root)

        renderTabs()
        renderContent()
    }

    override fun onResume() {
        super.onResume()
        renderContent()
        ui.post(pollTask)
    }

    override fun onPause() {
        super.onPause()
        ui.removeCallbacks(pollTask)
    }

    private fun renderTabs() {
        tabRow.removeAllViews()
        tabs.forEach { (tab, label) ->
            tabRow.addView(Button(this).apply {
                text = label
                isAllCaps = false
                alpha = if (tab == currentTab) 1f else 0.72f
                setOnClickListener {
                    currentTab = tab
                    renderTabs()
                    renderContent()
                }
            })
        }
    }

    private fun renderContent() {
        content.removeAllViews()
        when (currentTab) {
            Tab.Overview -> renderOverview()
            Tab.Compose -> renderCompose()
            Tab.Dockerfiles -> renderDockerfiles()
            Tab.Images -> renderImages()
            Tab.Containers -> renderContainers()
            Tab.Sessions -> renderSessions()
        }
    }

    private fun renderOverview() {
        addSection("Runtime")
        addAction("Start pdockerd", "Launch foreground daemon") { startDaemon() }
        addAction("Stop pdockerd", "Stop foreground service") {
            startService(Intent(this, PdockerdService::class.java).setAction(PdockerdService.ACTION_STOP))
            status.text = "pdockerd: stopped"
        }
        addAction("Keep resident", "Open battery optimization exemption") {
            requestBatteryOptimizationBypass()
        }
        addAction("Docker console", "Interactive shell with DOCKER_HOST set") {
            openTerminal("Docker console", "sh")
        }
        addAction("Default dev workspace", "VS Code Server + Continue + Codex") {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }

        addSection("Inventory")
        addWidget("Images", imageDirs().size.toString(), "Pulled rootfs trees available to browse")
        addWidget("Containers", containerDirs().size.toString(), "Created container states and logs")
        addWidget("Compose projects", composeFiles().size.toString(), projectRoot.absolutePath)
        addWidget("Dockerfiles", dockerfiles().size.toString(), "Build definitions under projects")
    }

    private fun renderCompose() {
        addSection("Compose")
        addAction("New compose.yaml", "Create or edit default Compose project") {
            openEditor(File(projectRoot, "default/compose.yaml"))
        }
        addAction("Default dev compose", "code-server, Continue, Codex CLI") {
            openEditor(File(projectRoot, "default/compose.yaml"))
        }
        addAction("Compose shell", "Open at ${projectRoot.absolutePath}") {
            projectRoot.mkdirs()
            openTerminal("Compose", "cd ${shellQuote(projectRoot.absolutePath)} && sh")
        }
        val files = composeFiles()
        if (files.isEmpty()) {
            addMessage("No compose files under ${projectRoot.absolutePath}.")
            return
        }
        files.forEach { file ->
            addWidget(file.name, "Compose file", file.parentFile?.absolutePath.orEmpty()) {
                openEditor(file)
            }
            addAction("Up ${file.parentFile?.name ?: file.name}", "docker compose up") {
                val dir = file.parentFile ?: projectRoot
                openTerminal("compose up", "cd ${shellQuote(dir.absolutePath)} && docker compose up")
            }
        }
    }

    private fun renderDockerfiles() {
        addSection("Dockerfile")
        addAction("New Dockerfile", "Create or edit default build file") {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }
        addAction("Default dev image", "code-server + extensions + @openai/codex") {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }
        addAction("Build shell", "Open project directory for docker build") {
            projectRoot.mkdirs()
            openTerminal("Docker build", "cd ${shellQuote(projectRoot.absolutePath)} && sh")
        }
        val files = dockerfiles()
        if (files.isEmpty()) {
            addMessage("No Dockerfile found under ${projectRoot.absolutePath}.")
            return
        }
        files.forEach { file ->
            addWidget(file.parentFile?.name ?: file.name, "Dockerfile", file.absolutePath) {
                openEditor(file)
            }
            addAction("Build ${file.parentFile?.name ?: file.name}", file.absolutePath) {
                val dir = file.parentFile ?: projectRoot
                openTerminal("docker build", "cd ${shellQuote(dir.absolutePath)} && docker build -t local/${dir.name}:latest .")
            }
        }
    }

    private fun renderImages() {
        addSection("Images")
        addAction("Pull image", "Open docker pull shell") {
            openTerminal("Pull image", "docker pull ubuntu:22.04")
        }
        addAction("Browse image files", "Inspect pulled rootfs trees") {
            startActivity(Intent(this, ImageFilesActivity::class.java))
        }
        val images = imageDirs()
        if (images.isEmpty()) {
            addMessage("No pulled images yet.")
            return
        }
        images.forEach { image ->
            addWidget(image.name, "Image rootfs", summarizeRootfs(File(image, "rootfs"))) {
                startActivity(Intent(this, ImageFilesActivity::class.java))
            }
        }
    }

    private fun renderContainers() {
        addSection("Containers")
        addAction("docker ps", "Show current containers") {
            openTerminal("docker ps", "docker ps -a")
        }
        val containers = containerDirs()
        if (containers.isEmpty()) {
            addMessage("No containers yet.")
            return
        }
        containers.forEach { dir ->
            val state = readState(dir)
            val name = state?.optString("Name")?.trim('/')?.ifBlank { dir.name } ?: dir.name
            val image = state?.optString("Image")?.ifBlank { "unknown image" } ?: "unknown image"
            val statusText = state?.optString("Status")?.ifBlank { "unknown status" } ?: "unknown status"
            addWidget(name, statusText, "$image\n${containerLogPreview(dir)}") {
                openTerminal("container $name", "docker logs --tail 80 ${dir.name}; printf '\\n# attach shell\\n'; docker exec -it ${dir.name} sh")
            }
        }
    }

    private fun renderSessions() {
        addSection("Sessions")
        addAction("Shell", "Plain app shell") {
            openTerminal("Shell", "sh")
        }
        addAction("Docker -it equivalent", "PTY-backed docker exec/run console") {
            openTerminal("Docker interactive", "docker ps -a; printf '\\nUse: docker exec -it <container> sh\\n'")
        }
        addAction("Compose session", "Run compose commands in projects directory") {
            projectRoot.mkdirs()
            openTerminal("Compose session", "cd ${shellQuote(projectRoot.absolutePath)} && docker compose ps; sh")
        }
        addAction("Text editor", "Edit default Dockerfile") {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }
    }

    private fun startDaemon() {
        val intent = Intent(this, PdockerdService::class.java)
            .setAction(PdockerdService.ACTION_START)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            startService(intent)
        }
        status.text = "pdockerd: starting..."
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
            == PackageManager.PERMISSION_GRANTED) return
        ActivityCompat.requestPermissions(
            this,
            arrayOf(Manifest.permission.POST_NOTIFICATIONS),
            REQUEST_POST_NOTIFICATIONS,
        )
    }

    private fun requestBatteryOptimizationBypass() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return
        val powerManager = getSystemService(PowerManager::class.java)
        if (powerManager.isIgnoringBatteryOptimizations(packageName)) {
            status.text = "pdocker: already excluded from battery optimization"
            return
        }
        startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
            data = Uri.parse("package:$packageName")
        })
    }

    private fun openTerminal(title: String, command: String) {
        startActivity(Intent(this, TerminalActivity::class.java).apply {
            addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT)
            addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
            putExtra(TerminalActivity.EXTRA_TITLE, title)
            putExtra(TerminalActivity.EXTRA_COMMAND, command)
        })
    }

    private fun openEditor(file: File) {
        startActivity(Intent(this, TextEditorActivity::class.java).apply {
            putExtra(TextEditorActivity.EXTRA_PATH, file.absolutePath)
        })
    }

    private fun refreshStatus() {
        val sock = File(pdockerHome, "pdockerd.sock")
        if (!sock.exists()) {
            status.text = "pdockerd: socket absent"
            return
        }
        thread(isDaemon = true, name = "pdockerd-ping") {
            val msg = runCatching {
                LocalSocket().use { ls ->
                    ls.connect(LocalSocketAddress(sock.absolutePath,
                        LocalSocketAddress.Namespace.FILESYSTEM))
                    ls.soTimeout = 500
                    ls.outputStream.write(
                        "GET /_ping HTTP/1.0\r\nHost: pdocker\r\n\r\n".toByteArray()
                    )
                    ls.outputStream.flush()
                    val resp = ls.inputStream.readBytes().toString(Charsets.US_ASCII)
                    if ("200 OK" in resp && resp.trimEnd().endsWith("OK")) "running"
                    else "socket up, unexpected response"
                }
            }.getOrElse { "socket up, ping failed: ${it.message}" }
            ui.post { status.text = "pdockerd: $msg" }
        }
    }

    private fun addSection(text: String) {
        content.addView(TextView(this).apply {
            this.text = text
            textSize = 18f
            typeface = Typeface.DEFAULT_BOLD
            setPadding(0, 22, 0, 8)
        })
    }

    private fun addAction(label: String, detail: String, onClick: () -> Unit) {
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            isClickable = true
            setPadding(0, 16, 0, 16)
            setOnClickListener { onClick() }
            addView(TextView(this@MainActivity).apply {
                text = label
                textSize = 16f
                setSingleLine(true)
                ellipsize = TextUtils.TruncateAt.END
            })
            addView(TextView(this@MainActivity).apply {
                text = detail
                textSize = 12f
                alpha = 0.72f
                setSingleLine(true)
                ellipsize = TextUtils.TruncateAt.MIDDLE
            })
            content.addView(this)
            addDivider()
        }
    }

    private fun addMetric(label: String, value: String) {
        addAction(label, value) {}
    }

    private fun addWidget(title: String, value: String, detail: String, onClick: (() -> Unit)? = null) {
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(18, 18, 18, 18)
            if (onClick != null) {
                isClickable = true
                setOnClickListener { onClick() }
            }
            addView(TextView(this@MainActivity).apply {
                text = title
                textSize = 13f
                alpha = 0.72f
                setSingleLine(true)
                ellipsize = TextUtils.TruncateAt.END
            })
            addView(TextView(this@MainActivity).apply {
                text = value
                textSize = 20f
                typeface = Typeface.DEFAULT_BOLD
                setSingleLine(true)
                ellipsize = TextUtils.TruncateAt.END
            })
            addView(TextView(this@MainActivity).apply {
                text = detail
                textSize = 12f
                alpha = 0.78f
                maxLines = 3
                ellipsize = TextUtils.TruncateAt.END
            })
            content.addView(this)
            addDivider()
        }
    }

    private fun addMessage(text: String) {
        content.addView(TextView(this).apply {
            this.text = text
            textSize = 14f
            setPadding(0, 16, 0, 16)
        })
    }

    private fun addDivider() {
        content.addView(View(this).apply {
            alpha = 0.18f
            setBackgroundColor(0xff888888.toInt())
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1))
    }

    private fun imageDirs(): List<File> =
        imageRoot.listFiles()
            ?.filter { File(it, "rootfs").isDirectory }
            ?.sortedBy { it.name }
            .orEmpty()

    private fun containerDirs(): List<File> =
        containerRoot.listFiles()
            ?.filter { it.isDirectory }
            ?.sortedByDescending { it.lastModified() }
            .orEmpty()

    private fun composeFiles(): List<File> =
        projectRoot.walkSafe()
            .filter { it.isFile && it.name in setOf("compose.yaml", "compose.yml", "docker-compose.yaml", "docker-compose.yml") }
            .sortedBy { it.absolutePath }

    private fun dockerfiles(): List<File> =
        projectRoot.walkSafe()
            .filter { it.isFile && it.name == "Dockerfile" }
            .sortedBy { it.absolutePath }

    private fun File.walkSafe(): List<File> =
        if (!exists()) emptyList() else walkTopDown().onEnter { it.name !in setOf(".git", "node_modules") }.toList()

    private fun readState(dir: File): JSONObject? =
        runCatching { JSONObject(File(dir, "state.json").readText()) }.getOrNull()

    private fun summarizeRootfs(rootfs: File): String {
        val count = rootfs.list()?.size ?: 0
        return "$count top-level entries"
    }

    private fun containerLogPreview(dir: File): String {
        val candidates = listOf(
            File(pdockerHome, "logs/${dir.name}.log"),
            File(dir, "log"),
            File(dir, "logs.txt"),
        )
        val log = candidates.firstOrNull { it.isFile } ?: return "No log preview"
        return runCatching {
            log.readLines().takeLast(3).joinToString("\n").ifBlank { "Log is empty" }
        }.getOrDefault("Log unavailable")
    }

    private fun shellQuote(s: String): String =
        "'" + s.replace("'", "'\"'\"'") + "'"

    private fun seedDefaultProject() {
        val stamp = File(projectRoot, "default/.pdocker-template-version")
        if (stamp.exists()) return
        copyAssetTree("default-project", File(projectRoot, "default"))
        stamp.parentFile?.mkdirs()
        stamp.writeText("1\n")
    }

    private fun copyAssetTree(assetPath: String, dest: File) {
        val children = assets.list(assetPath).orEmpty()
        if (children.isEmpty()) {
            if (!dest.exists()) {
                dest.parentFile?.mkdirs()
                assets.open(assetPath).use { input ->
                    dest.outputStream().use { output -> input.copyTo(output) }
                }
            }
            return
        }
        dest.mkdirs()
        children.forEach { child ->
            copyAssetTree("$assetPath/$child", File(dest, child))
        }
    }
}
