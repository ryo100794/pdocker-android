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
import org.json.JSONArray
import org.json.JSONObject

class MainActivity : AppCompatActivity() {
    private enum class Tab { Overview, Compose, Dockerfiles, Images, Containers, Sessions }

    companion object {
        private const val REQUEST_POST_NOTIFICATIONS = 100
    }

    private val ui = Handler(Looper.getMainLooper())
    private val tabs = listOf(Tab.Overview, Tab.Compose, Tab.Dockerfiles, Tab.Images, Tab.Containers, Tab.Sessions)
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
            text = getString(R.string.status_unknown)
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
        tabs.forEach { tab ->
            tabRow.addView(Button(this).apply {
                text = tabLabel(tab)
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

    private fun tabLabel(tab: Tab): String = getString(when (tab) {
        Tab.Overview -> R.string.tab_overview
        Tab.Compose -> R.string.tab_compose
        Tab.Dockerfiles -> R.string.tab_dockerfile
        Tab.Images -> R.string.tab_images
        Tab.Containers -> R.string.tab_containers
        Tab.Sessions -> R.string.tab_sessions
    })

    private fun renderOverview() {
        addSection(getString(R.string.section_runtime))
        addAction(getString(R.string.action_start_pdockerd), getString(R.string.detail_start_pdockerd)) { startDaemon() }
        addAction(getString(R.string.action_stop_pdockerd), getString(R.string.detail_stop_pdockerd)) {
            startService(Intent(this, PdockerdService::class.java).setAction(PdockerdService.ACTION_STOP))
            status.text = getString(R.string.status_stopped)
        }
        addAction(getString(R.string.action_keep_resident), getString(R.string.detail_keep_resident)) {
            requestBatteryOptimizationBypass()
        }
        addAction(getString(R.string.action_docker_console), getString(R.string.detail_docker_console)) {
            openTerminal(getString(R.string.action_docker_console), "sh")
        }
        addAction(getString(R.string.action_default_dev_workspace), getString(R.string.detail_default_dev_workspace)) {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }

        addSection(getString(R.string.section_inventory))
        addWidget(getString(R.string.widget_images), imageDirs().size.toString(), getString(R.string.detail_images_inventory))
        addWidget(getString(R.string.widget_containers), containerDirs().size.toString(), getString(R.string.detail_containers_inventory))
        addWidget(getString(R.string.widget_compose_projects), composeFiles().size.toString(), projectRoot.absolutePath)
        addWidget(getString(R.string.widget_dockerfiles), dockerfiles().size.toString(), getString(R.string.detail_dockerfiles_inventory))
    }

    private fun renderCompose() {
        addSection(getString(R.string.section_compose))
        addAction(getString(R.string.action_new_compose), getString(R.string.detail_new_compose)) {
            openEditor(File(projectRoot, "default/compose.yaml"))
        }
        addAction(getString(R.string.action_default_dev_compose), getString(R.string.detail_default_dev_compose)) {
            openEditor(File(projectRoot, "default/compose.yaml"))
        }
        addAction(getString(R.string.action_compose_shell), getString(R.string.detail_open_at_fmt, projectRoot.absolutePath)) {
            projectRoot.mkdirs()
            openTerminal(getString(R.string.section_compose), "cd ${shellQuote(projectRoot.absolutePath)} && sh")
        }
        val files = composeFiles()
        if (files.isEmpty()) {
            addMessage(getString(R.string.message_no_compose_fmt, projectRoot.absolutePath))
            return
        }
        files.forEach { file ->
            addWidget(file.name, getString(R.string.detail_compose_file), file.parentFile?.absolutePath.orEmpty()) {
                openEditor(file)
            }
            addAction(getString(R.string.action_up_fmt, file.parentFile?.name ?: file.name), getString(R.string.detail_compose_up)) {
                val dir = file.parentFile ?: projectRoot
                openTerminal(getString(R.string.terminal_compose_up), "cd ${shellQuote(dir.absolutePath)} && docker compose up")
            }
        }
    }

    private fun renderDockerfiles() {
        addSection(getString(R.string.section_dockerfile))
        addAction(getString(R.string.action_new_dockerfile), getString(R.string.detail_new_dockerfile)) {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }
        addAction(getString(R.string.action_default_dev_image), getString(R.string.detail_default_dev_image)) {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }
        addAction(getString(R.string.action_build_shell), getString(R.string.detail_build_shell)) {
            projectRoot.mkdirs()
            openTerminal(getString(R.string.terminal_docker_build), "cd ${shellQuote(projectRoot.absolutePath)} && sh")
        }
        val files = dockerfiles()
        if (files.isEmpty()) {
            addMessage(getString(R.string.message_no_dockerfile_fmt, projectRoot.absolutePath))
            return
        }
        files.forEach { file ->
            addWidget(file.parentFile?.name ?: file.name, getString(R.string.section_dockerfile), file.absolutePath) {
                openEditor(file)
            }
            addAction(getString(R.string.action_build_fmt, file.parentFile?.name ?: file.name), file.absolutePath) {
                val dir = file.parentFile ?: projectRoot
                openTerminal(getString(R.string.terminal_docker_build), "cd ${shellQuote(dir.absolutePath)} && docker build -t local/${dir.name}:latest .")
            }
        }
    }

    private fun renderImages() {
        addSection(getString(R.string.section_images))
        addAction(getString(R.string.action_pull_image), getString(R.string.detail_pull_image)) {
            openTerminal(getString(R.string.action_pull_image), "docker pull ubuntu:22.04")
        }
        addAction(getString(R.string.action_browse_image_files), getString(R.string.detail_browse_image_files)) {
            startActivity(Intent(this, ImageFilesActivity::class.java))
        }
        val images = imageDirs()
        if (images.isEmpty()) {
            addMessage(getString(R.string.message_no_pulled_images))
            return
        }
        images.forEach { image ->
            addWidget(image.name, getString(R.string.detail_image_rootfs), summarizeRootfs(File(image, "rootfs"))) {
                startActivity(Intent(this, ImageFilesActivity::class.java))
            }
        }
    }

    private fun renderContainers() {
        addSection(getString(R.string.section_containers))
        addAction(getString(R.string.action_docker_ps), getString(R.string.detail_docker_ps)) {
            openTerminal(getString(R.string.terminal_docker_ps), "docker ps -a")
        }
        val containers = containerDirs()
        if (containers.isEmpty()) {
            addMessage(getString(R.string.message_no_containers))
            return
        }
        containers.forEach { dir ->
            val state = readState(dir)
            val name = state?.optString("Name")?.trim('/')?.ifBlank { dir.name } ?: dir.name
            val image = state?.optString("Image")?.ifBlank { getString(R.string.unknown_image) } ?: getString(R.string.unknown_image)
            val statusText = state?.optString("Status")?.ifBlank { getString(R.string.unknown_status) } ?: getString(R.string.unknown_status)
            addWidget(name, statusText, "$image\n${containerNetworkSummary(state)}\n${containerLogPreview(dir)}") {
                openTerminal(getString(R.string.terminal_container_fmt, name), "docker logs --tail 80 ${dir.name}; printf '\\n# attach shell\\n'; docker exec -it ${dir.name} sh")
            }
        }
    }

    private fun renderSessions() {
        addSection(getString(R.string.section_sessions))
        addAction(getString(R.string.action_shell), getString(R.string.detail_shell)) {
            openTerminal(getString(R.string.terminal_shell), "sh")
        }
        addAction(getString(R.string.action_docker_it), getString(R.string.detail_docker_it)) {
            openTerminal(getString(R.string.terminal_docker_interactive), "docker ps -a; printf '\\nUse: docker exec -it <container> sh\\n'")
        }
        addAction(getString(R.string.action_compose_session), getString(R.string.detail_compose_session)) {
            projectRoot.mkdirs()
            openTerminal(getString(R.string.action_compose_session), "cd ${shellQuote(projectRoot.absolutePath)} && docker compose ps; sh")
        }
        addAction(getString(R.string.action_text_editor), getString(R.string.detail_text_editor)) {
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
        status.text = getString(R.string.status_starting)
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
            status.text = getString(R.string.status_battery_ignored)
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
            status.text = getString(R.string.status_socket_absent)
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
                    if ("200 OK" in resp && resp.trimEnd().endsWith("OK")) getString(R.string.status_running)
                    else getString(R.string.status_unexpected_response)
                }
            }.getOrElse { getString(R.string.status_ping_failed, it.message.orEmpty()) }
            ui.post { status.text = getString(R.string.status_pdocker_fmt, msg) }
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

    private fun containerNetworkSummary(state: JSONObject?): String {
        val dockerNetwork = state?.optJSONObject("NetworkSettings")
        val pdockerNetwork = state?.optJSONObject("PdockerNetwork")
        val bridgeNetwork = dockerNetwork
            ?.optJSONObject("Networks")
            ?.optJSONObject("bridge")
        val ip = listOf(
            pdockerNetwork?.optString("IPAddress"),
            dockerNetwork?.optString("IPAddress"),
            bridgeNetwork?.optString("IPAddress"),
        ).firstOrNull { !it.isNullOrBlank() }.orEmpty()
        val ports = pdockerNetwork?.optJSONObject("Ports")
            ?: dockerNetwork?.optJSONObject("Ports")
        val rewriteCount = pdockerNetwork?.optJSONArray("PortRewrite")?.length() ?: 0
        val lines = mutableListOf(
            getString(R.string.container_ip_fmt, ip.ifBlank { "-" }),
            getString(R.string.container_ports_fmt, summarizePorts(ports)),
        )
        if (rewriteCount > 0) {
            lines += getString(R.string.container_hook_plan_fmt, rewriteCount)
        }
        return lines.joinToString("\n")
    }

    private fun summarizePorts(ports: JSONObject?): String {
        if (ports == null || ports.length() == 0) {
            return getString(R.string.container_ports_none)
        }
        val keys = mutableListOf<String>()
        val iter = ports.keys()
        while (iter.hasNext()) keys += iter.next()
        return keys.sorted().flatMap { key ->
            val value = ports.opt(key)
            if (value is JSONArray && value.length() > 0) {
                (0 until value.length()).mapNotNull { i ->
                    value.optJSONObject(i)?.let { binding ->
                        getString(
                            R.string.container_port_binding_fmt,
                            binding.optString("HostIp").ifBlank { "127.0.0.1" },
                            binding.optString("HostPort").ifBlank { "?" },
                            key,
                        )
                    }
                }
            } else {
                listOf(getString(R.string.container_port_exposed_fmt, key))
            }
        }.joinToString(", ")
    }

    private fun summarizeRootfs(rootfs: File): String {
        val count = rootfs.list()?.size ?: 0
        return getString(R.string.summary_rootfs_fmt, count)
    }

    private fun containerLogPreview(dir: File): String {
        val candidates = listOf(
            File(pdockerHome, "logs/${dir.name}.log"),
            File(dir, "log"),
            File(dir, "logs.txt"),
        )
        val log = candidates.firstOrNull { it.isFile } ?: return getString(R.string.log_no_preview)
        return runCatching {
            log.readLines().takeLast(3).joinToString("\n").ifBlank { getString(R.string.log_empty) }
        }.getOrDefault(getString(R.string.log_unavailable))
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
