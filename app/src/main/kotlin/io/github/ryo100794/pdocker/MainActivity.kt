package io.github.ryo100794.pdocker

import android.Manifest
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.ApplicationInfo
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
import android.system.Os
import android.text.TextUtils
import android.util.Base64
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlin.concurrent.thread
import org.json.JSONArray
import org.json.JSONObject

class MainActivity : AppCompatActivity() {
    private enum class Tab { Overview, Library, Compose, Dockerfiles, Images, Containers, Sessions }
    private enum class ToolKind { Terminal, Editor, Split }

    private data class ToolTab(
        val group: String,
        val title: String,
        val kind: ToolKind,
        val view: View,
        val bridge: Bridge? = null,
        val key: String = title,
    )

    private data class ProjectTemplate(
        val id: String,
        val name: String,
        val category: String,
        val description: String,
        val assetPath: String,
        val projectDir: String,
        val compose: String,
        val dockerfile: String,
        val gpu: String,
        val version: Int,
        val features: List<String>,
    )

    private data class TemplateInstallReport(
        var copied: Int = 0,
        var kept: Int = 0,
    )

    private data class StorageMetrics(
        val fsTotalBytes: Long,
        val fsFreeBytes: Long,
        val pdockerBytes: Long,
        val layersBytes: Long,
        val imageViewBytes: Long,
        val containerPrivateBytes: Long,
    )

    private data class DaemonOperation(
        val id: String,
        val kind: String,
        val title: String,
        val detail: String,
        val status: String,
        val startedAtMs: Long,
        val updatedAtMs: Long,
    )

    private data class DiskUsage(
        val bytes: Long,
        val inodeKeys: Set<String>,
    )

    private data class DockerJob(
        val id: String,
        val title: String,
        val detail: String,
        val command: String,
        val group: String,
        val toolKey: String,
        var status: String,
        var exitCode: Int? = null,
        var startedAt: Long = System.currentTimeMillis(),
        var endedAt: Long? = null,
        var progress: String = "",
        var output: MutableList<String> = mutableListOf(),
    )

    private data class LiveJobView(
        val header: TextView,
        val progress: TextView,
        val services: LinearLayout,
        val terminal: TerminalLogPane,
    )

    private class TerminalLogPane(val view: WebView) {
        private val pending = StringBuilder()
        private var ready = false
        private var flushScheduled = false

        fun markReady() {
            ready = true
            scheduleFlush(0L)
        }

        fun write(text: String) {
            if (text.isEmpty()) return
            pending.append(text)
            if (!ready) return
            scheduleFlush(8L)
        }

        private fun scheduleFlush(delayMs: Long) {
            if (flushScheduled) return
            flushScheduled = true
            view.postDelayed({
                flushScheduled = false
                if (!ready || pending.isEmpty()) return@postDelayed
                val text = pending.toString()
                pending.clear()
                flush(text)
                if (pending.isNotEmpty()) scheduleFlush(8L)
            }, delayMs)
        }

        private fun flush(text: String) {
            val b64 = Base64.encodeToString(text.toByteArray(Charsets.UTF_8), Base64.NO_WRAP)
            view.evaluateJavascript("window.pdockerRecv('$b64')", null)
        }
    }

    private data class ComposeService(
        val name: String,
        var image: String = "",
        var containerName: String = "",
        var buildContext: String? = null,
        var workingDir: String = "",
        var command: List<String> = emptyList(),
        val environment: MutableMap<String, String> = mutableMapOf(),
        val ports: MutableList<String> = mutableListOf(),
        val volumes: MutableList<String> = mutableListOf(),
        val dependsOn: MutableList<String> = mutableListOf(),
        var gpus: String = "",
        var hasHealthcheck: Boolean = false,
        val serviceLinks: MutableList<ComposeServiceLink> = mutableListOf(),
    )

    private data class ComposeServiceLink(
        val port: Int?,
        val label: String,
        val url: String?,
        val autoOpen: Boolean = false,
    )

    private data class ProjectSummary(
        val dir: File,
        val compose: List<File>,
        val dockerfiles: List<File>,
        val editable: List<File>,
        val services: List<ComposeService>,
        val serviceUrls: List<Pair<String, String>>,
        val serviceHealth: String,
        val modelSummary: String,
        val gpuProfileSummary: String,
        val gpuDiagnostics: File?,
        val containerCount: Int,
        val jobSummary: String,
    )

    companion object {
        private const val REQUEST_POST_NOTIFICATIONS = 100
        private const val MAX_INLINE_EDIT_BYTES = 512 * 1024
        private const val MAX_JOB_HISTORY = 20
        private const val MAX_JOB_LINES = 200
        private const val MAX_JOB_LOG_VIEW_BYTES = 256 * 1024
        private const val PDOCKER_SERVICE_URL_LABEL_PREFIX = "io.github.ryo100794.pdocker.service-url."
        private const val ACTION_SMOKE_START = "io.github.ryo100794.pdocker.action.SMOKE_START"
        private const val ACTION_SMOKE_GPU_BENCH = "io.github.ryo100794.pdocker.action.SMOKE_GPU_BENCH"
        private const val ACTION_SMOKE_COMPOSE_UP = "io.github.ryo100794.pdocker.action.SMOKE_COMPOSE_UP"
    }

    private val ui = Handler(Looper.getMainLooper())
    private val logIo: ExecutorService = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "pdocker-job-log-writer").apply { isDaemon = true }
    }
    private val tabs = listOf(Tab.Overview, Tab.Library, Tab.Compose, Tab.Dockerfiles, Tab.Images, Tab.Containers, Tab.Sessions)
    private val pollTask = object : Runnable {
        override fun run() {
            refreshStatus()
            refreshDaemonOperationsAsync()
            if (currentTab in setOf(Tab.Overview, Tab.Containers, Tab.Compose)) {
                refreshContainerSnapshotAsync()
            }
            ui.postDelayed(this, 3000)
        }
    }
    private val jobTickerTask = object : Runnable {
        override fun run() {
            tickRunningJobs()
            ui.postDelayed(this, 1000)
        }
    }

    private lateinit var status: TextView
    private lateinit var content: LinearLayout
    private lateinit var tabRow: LinearLayout
    private lateinit var upperPane: LinearLayout
    private lateinit var lowerPane: LinearLayout
    private lateinit var lowerGroupRow: LinearLayout
    private lateinit var lowerTabRow: LinearLayout
    private lateinit var lowerHost: FrameLayout
    private var currentTab = Tab.Overview
    private val toolTabs = mutableListOf<ToolTab>()
    private var currentTool = -1
    private var currentToolGroup: String? = null
    private val dockerJobs = mutableListOf<DockerJob>()
    private val dockerJobBuffers = mutableMapOf<String, String>()
    private val dockerJobPendingCarriageReturn = mutableSetOf<String>()
    private val liveJobViews = mutableMapOf<String, LiveJobView>()
    private var dockerJobsSaveScheduled = false
    private var dockerJobsDirty = false
    private var jobRenderScheduled = false
    private val ansiControlRegex = Regex("\u001B\\[[0-?]*[ -/]*[@-~]")
    private val serviceHealth = mutableMapOf<String, String>()
    private val serviceHealthCheckedAt = mutableMapOf<String, Long>()
    private val serviceHealthInFlight = mutableSetOf<String>()
    private var upperWeight = 0.56f
    private var lowerWeight = 0.44f
    private var splitDragStartY = 0f
    private var splitDragStartUpper = 0f
    private var lastDaemonStartAttemptAt = 0L
    private var storageMetrics: StorageMetrics? = null
    private var storageMetricsScanning = false
    private var lastStorageMetricsAt = 0L
    private var daemonOperations: List<DaemonOperation> = emptyList()
    private var daemonOperationsRefreshing = false
    private var containerSnapshot: List<JSONObject> = emptyList()
    private var containerSnapshotFingerprint = ""
    private var containerSnapshotRefreshing = false
    private var lastContainerSnapshotAt = 0L
    private var hostEnvironment: JSONObject? = null
    private var hostEnvironmentRefreshing = false
    private var lastHostEnvironmentAt = 0L

    private val pdockerHome: File by lazy { File(filesDir, "pdocker") }
    private val imageRoot: File by lazy { File(pdockerHome, "images") }
    private val layerRoot: File by lazy { File(pdockerHome, "layers") }
    private val containerRoot: File by lazy { File(pdockerHome, "containers") }
    private val projectRoot: File by lazy { File(pdockerHome, "projects") }
    private val engine: DockerEngineClient by lazy { DockerEngineClient(File(pdockerHome, "pdockerd.sock")) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_NOSENSOR
        seedDefaultProject()
        loadDockerJobs()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(28, 28, 28, 28)
        }
        upperPane = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        lowerPane = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }

        val headerRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        status = TextView(this).apply {
            text = getString(R.string.status_unknown)
            textSize = 14f
            setSingleLine(true)
            ellipsize = TextUtils.TruncateAt.END
        }
        val buildInfo = TextView(this).apply {
            text = appBuildInfo()
            textSize = 11f
            gravity = Gravity.END
            setSingleLine(true)
            ellipsize = TextUtils.TruncateAt.START
            typeface = Typeface.MONOSPACE
        }
        tabRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
        }
        lowerGroupRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        lowerTabRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        lowerHost = FrameLayout(this)

        headerRow.addView(status, LinearLayout.LayoutParams(
            0,
            LinearLayout.LayoutParams.WRAP_CONTENT,
            1f,
        ))
        headerRow.addView(buildInfo, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT,
        ))
        upperPane.addView(headerRow)
        upperPane.addView(HorizontalScrollView(this).apply { addView(tabRow) })
        upperPane.addView(ScrollView(this).apply { addView(content) }, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0,
            1f,
        ))
        lowerPane.addView(HorizontalScrollView(this).apply { addView(lowerGroupRow) })
        lowerPane.addView(HorizontalScrollView(this).apply { addView(lowerTabRow) })
        lowerPane.addView(lowerHost, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0,
            1f,
        ))
        root.addView(upperPane, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0,
            upperWeight,
        ))
        root.addView(splitterView())
        root.addView(lowerPane, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT,
            0,
            lowerWeight,
        ))
        setContentView(root)

        renderTabs()
        renderContent()
        renderToolChrome()
        ensureDaemonStarted()
        handleAutomationIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleAutomationIntent(intent)
    }

    override fun onResume() {
        super.onResume()
        ensureDaemonStarted()
        renderContent()
        ui.post(pollTask)
        ui.post(jobTickerTask)
    }

    override fun onPause() {
        super.onPause()
        ui.removeCallbacks(pollTask)
        ui.removeCallbacks(jobTickerTask)
        flushDockerJobsSave()
    }

    override fun onDestroy() {
        toolTabs.forEach { it.bridge?.close() }
        toolTabs.clear()
        liveJobViews.clear()
        ui.removeCallbacks(jobTickerTask)
        logIo.shutdown()
        super.onDestroy()
    }

    private fun appBuildInfo(): String {
        val info = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            packageManager.getPackageInfo(packageName, PackageManager.PackageInfoFlags.of(0))
        } else {
            @Suppress("DEPRECATION")
            packageManager.getPackageInfo(packageName, 0)
        }
        val versionName = info.versionName ?: "dev"
        val versionCode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            info.longVersionCode
        } else {
            @Suppress("DEPRECATION")
            info.versionCode.toLong()
        }
        val rawBuildTime = BuildConfig.BUILD_TIME_UTC
            .replace('T', ' ')
            .removeSuffix("Z")
        val buildTime = rawBuildTime.substringBeforeLast('.', rawBuildTime)
        return getString(R.string.app_build_info_fmt, versionName, versionCode, buildTime)
    }

    private fun splitterView(): View =
        TextView(this).apply {
            text = "━━"
            gravity = Gravity.CENTER
            textSize = 12f
            alpha = 0.62f
            setPadding(0, 3, 0, 3)
            setBackgroundColor(0x11888888)
            setOnTouchListener { _, event ->
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        splitDragStartY = event.rawY
                        splitDragStartUpper = upperWeight
                        true
                    }
                    MotionEvent.ACTION_MOVE -> {
                        val total = (upperPane.height + lowerPane.height).coerceAtLeast(1)
                        val delta = (event.rawY - splitDragStartY) / total
                        upperWeight = (splitDragStartUpper + delta).coerceIn(0.24f, 0.78f)
                        lowerWeight = 1f - upperWeight
                        upperPane.layoutParams = LinearLayout.LayoutParams(
                            LinearLayout.LayoutParams.MATCH_PARENT,
                            0,
                            upperWeight,
                        )
                        lowerPane.layoutParams = LinearLayout.LayoutParams(
                            LinearLayout.LayoutParams.MATCH_PARENT,
                            0,
                            lowerWeight,
                        )
                        true
                    }
                    else -> true
                }
            }
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
            Tab.Library -> renderLibrary()
            Tab.Compose -> renderCompose()
            Tab.Dockerfiles -> renderDockerfiles()
            Tab.Images -> renderImages()
            Tab.Containers -> renderContainers()
            Tab.Sessions -> renderSessions()
        }
    }

    private fun tabLabel(tab: Tab): String = getString(when (tab) {
        Tab.Overview -> R.string.tab_overview
        Tab.Library -> R.string.tab_library
        Tab.Compose -> R.string.tab_compose
        Tab.Dockerfiles -> R.string.tab_dockerfile
        Tab.Images -> R.string.tab_images
        Tab.Containers -> R.string.tab_containers
        Tab.Sessions -> R.string.tab_sessions
    })

    private fun renderOverview() {
        addSection(getString(R.string.section_workspace))
        addAction(getString(R.string.action_default_dev_workspace), getString(R.string.detail_default_dev_workspace)) {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }

        addSection(getString(R.string.section_inventory))
        addWidget(getString(R.string.widget_images), imageDirs().size.toString(), getString(R.string.detail_images_inventory))
        addWidget(getString(R.string.widget_containers), containerDirs().size.toString(), getString(R.string.detail_containers_inventory))
        addWidget(getString(R.string.widget_compose_projects), composeFiles().size.toString(), projectRoot.absolutePath)
        addWidget(getString(R.string.widget_dockerfiles), dockerfiles().size.toString(), getString(R.string.detail_dockerfiles_inventory))
        renderStorageMetrics()
        renderHostEnvironment()
        renderDaemonOperations()
        renderProjectDashboard()
        renderDockerJobs()
    }

    private fun renderLibrary() {
        addSection(getString(R.string.section_project_library))
        val templates = projectTemplates()
        if (templates.isEmpty()) {
            addMessage(getString(R.string.message_library_empty))
            return
        }
        templates.forEach { template ->
            val target = File(projectRoot, template.projectDir)
            val installed = File(target, template.compose).isFile || File(target, template.dockerfile).isFile
            val detail = listOf(
                template.description,
                getString(R.string.library_features_fmt, template.features.joinToString(", ")),
                getString(R.string.library_gpu_fmt, template.gpu),
                getString(R.string.library_target_fmt, target.absolutePath),
            ).joinToString("\n")
            addWidget(
                template.name,
                if (installed) getString(R.string.library_installed) else template.category,
                detail,
            ) {
                installTemplate(template)
                openEditor(File(target, template.compose))
            }
            addAction(getString(R.string.action_install_template_fmt, template.name), getString(R.string.detail_install_template)) {
                installTemplate(template)
                renderContent()
            }
            addAction(getString(R.string.action_open_template_compose_fmt, template.name), template.compose) {
                installTemplate(template)
                openEditor(File(target, template.compose))
            }
            addAction(getString(R.string.action_open_template_dockerfile_fmt, template.name), template.dockerfile) {
                installTemplate(template)
                openEditor(File(target, template.dockerfile))
            }
            if (template.gpu == "auto") {
                addAction(getString(R.string.action_gpu_profile_fmt, template.name), getString(R.string.detail_gpu_profile)) {
                    installTemplate(template)
                    openTerminal(
                        getString(R.string.terminal_gpu_profile),
                        "cd ${shellQuote(target.absolutePath)} && LLAMA_GPU_DIAGNOSTICS=profiles/pdocker-gpu-diagnostics.json bash scripts/pdocker-gpu-profile.sh profiles/pdocker-gpu.env; printf '\\n'; cat profiles/pdocker-gpu.env; printf '\\n'; cat profiles/pdocker-gpu-diagnostics.json; sh",
                    )
                }
            }
            addAction(getString(R.string.action_compose_up_template_fmt, template.name), getString(R.string.detail_compose_up)) {
                installTemplate(template)
                runComposeUp(target, getString(R.string.terminal_compose_up_fmt, template.projectDir))
            }
        }
    }

    private fun renderCompose() {
        addSection(getString(R.string.section_compose))
        renderDockerJobs { it.command.contains("compose up") }
        addAction(getString(R.string.action_new_compose), getString(R.string.detail_new_compose)) {
            openEditor(File(projectRoot, "default/compose.yaml"))
        }
        addAction(getString(R.string.action_default_dev_compose), getString(R.string.detail_default_dev_compose)) {
            openEditor(File(projectRoot, "default/compose.yaml"))
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
                runComposeUp(dir, getString(R.string.terminal_compose_up_fmt, dir.name))
            }
        }
    }

    private fun renderDockerfiles() {
        addSection(getString(R.string.section_dockerfile))
        renderDockerJobs { it.command.contains("docker build") }
        addAction(getString(R.string.action_new_dockerfile), getString(R.string.detail_new_dockerfile)) {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }
        addAction(getString(R.string.action_default_dev_image), getString(R.string.detail_default_dev_image)) {
            openEditor(File(projectRoot, "default/Dockerfile"))
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
                runImageBuild(dir, getString(R.string.terminal_docker_build_fmt, dir.name))
            }
        }
    }

    private fun renderImages() {
        addSection(getString(R.string.section_images))
        addAction(getString(R.string.action_pull_image), getString(R.string.detail_pull_image)) {
            runEngineJob(
                getString(R.string.action_pull_image),
                workspaceGroup(),
                "engine pull ubuntu:22.04",
            ) { emit ->
                emit("Image ubuntu:22.04 Pulling")
                pullImage("ubuntu:22.04")
            }
        }
        addAction(getString(R.string.action_browse_image_files), getString(R.string.detail_browse_image_files)) {
            openImageFiles()
        }
        val images = imageDirs()
        if (images.isEmpty()) {
            addMessage(getString(R.string.message_no_pulled_images))
            return
        }
        images.forEach { image ->
            addWidget(image.name, getString(R.string.detail_image_rootfs), summarizeRootfs(File(image, "rootfs"))) {
                openImageFiles(image)
            }
        }
    }

    private fun renderContainers() {
        addSection(getString(R.string.section_containers))
        refreshContainerSnapshotAsync()
        addAction(getString(R.string.action_docker_ps), getString(R.string.detail_docker_ps)) {
            runEngineAction(getString(R.string.terminal_docker_ps), workspaceGroup()) {
                formatContainers(getArray("/containers/json?all=1"))
            }
        }
        val containers = containerDirs()
        if (containers.isEmpty()) {
            addMessage(getString(R.string.message_no_containers))
            return
        }
        val snapshotByKey = containerSnapshotLookup()
        containers.forEach { dir ->
            val state = readState(dir)
            val snapshot = snapshotByKey[dir.name]
                ?: snapshotByKey[state?.optString("Name").orEmpty().trim('/')]
            val name = containerDisplayName(snapshot, state, dir)
            val image = state?.optString("Image")?.ifBlank { getString(R.string.unknown_image) } ?: getString(R.string.unknown_image)
            val statusText = snapshot?.optString("Status")?.takeIf { it.isNotBlank() }
                ?: state?.optJSONObject("State")
                ?.optString("Status")
                ?.ifBlank { getString(R.string.unknown_status) }
                ?: getString(R.string.unknown_status)
            addWidget(name, statusText, "$image\n${containerNetworkSummary(state)}\n${containerLogPreview(dir)}") {
                openDockerInteractiveTerminal(
                    getString(R.string.terminal_container_fmt, name),
                    dir.name,
                    name,
                )
            }
            addAction(getString(R.string.action_container_terminal_fmt, name), getString(R.string.detail_container_terminal)) {
                openDockerInteractiveTerminal(
                    getString(R.string.terminal_container_fmt, name),
                    name,
                )
            }
            addAction(getString(R.string.action_container_start_fmt, name), dir.name) {
                runContainerAction(name, getString(R.string.terminal_container_start_fmt, name)) {
                    post("/containers/${DockerEngineClient.encodePath(dir.name)}/start")
                    formatContainers(getArray("/containers/json?all=1"))
                }
            }
            addAction(getString(R.string.action_container_stop_fmt, name), dir.name) {
                runContainerAction(name, getString(R.string.terminal_container_stop_fmt, name)) {
                    post("/containers/${DockerEngineClient.encodePath(dir.name)}/stop?t=10")
                    formatContainers(getArray("/containers/json?all=1"))
                }
            }
            addAction(getString(R.string.action_container_restart_fmt, name), dir.name) {
                runContainerAction(name, getString(R.string.terminal_container_restart_fmt, name)) {
                    runCatching { post("/containers/${DockerEngineClient.encodePath(dir.name)}/stop?t=10") }
                    post("/containers/${DockerEngineClient.encodePath(dir.name)}/start")
                    formatContainers(getArray("/containers/json?all=1"))
                }
            }
            addAction(getString(R.string.action_container_logs_fmt, name), dir.name) {
                runContainerAction(name, getString(R.string.terminal_container_logs_fmt, name)) {
                    logs(dir.name, 200).ifBlank { "(no logs)" }
                }
            }
            addAction(getString(R.string.action_browse_container_files_fmt, name), dir.name) {
                openContainerFiles(dir)
            }
            containerServiceUrls(state).forEach { (label, url) ->
                addAction(getString(R.string.action_open_service_fmt, label), url) {
                    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                }
            }
        }
    }

    private fun containerDisplayName(snapshot: JSONObject?, state: JSONObject?, dir: File): String {
        val names = snapshot?.optJSONArray("Names")
        val fromSnapshot = names?.optString(0).orEmpty().trim('/').takeIf { it.isNotBlank() }
        return fromSnapshot
            ?: state?.optString("Name")?.trim('/')?.takeIf { it.isNotBlank() }
            ?: dir.name
    }

    private fun containerSnapshotLookup(): Map<String, JSONObject> {
        val out = mutableMapOf<String, JSONObject>()
        containerSnapshot.forEach { obj ->
            val id = obj.optString("Id")
            if (id.isNotBlank()) {
                out[id] = obj
                out[id.take(12)] = obj
            }
            val names = obj.optJSONArray("Names")
            if (names != null) {
                for (i in 0 until names.length()) {
                    val name = names.optString(i).trim('/')
                    if (name.isNotBlank()) out[name] = obj
                }
            }
        }
        return out
    }

    private fun refreshContainerSnapshotAsync(force: Boolean = false) {
        val now = System.currentTimeMillis()
        if (containerSnapshotRefreshing) return
        if (!force && now - lastContainerSnapshotAt < 2500L) return
        containerSnapshotRefreshing = true
        thread(isDaemon = true, name = "pdocker-container-snapshot") {
            val arr = runCatching { engine.getArray("/containers/json?all=1") }.getOrNull()
            val list = if (arr == null) emptyList() else (0 until arr.length()).mapNotNull { arr.optJSONObject(it) }
            val fingerprint = list.joinToString("\n") { obj ->
                listOf(
                    obj.optString("Id"),
                    obj.optString("Status"),
                    obj.optString("State"),
                    obj.optJSONArray("Names")?.toString().orEmpty(),
                ).joinToString("|")
            }
            ui.post {
                val changed = fingerprint != containerSnapshotFingerprint
                containerSnapshot = list
                containerSnapshotFingerprint = fingerprint
                containerSnapshotRefreshing = false
                lastContainerSnapshotAt = System.currentTimeMillis()
                if (changed && currentTab in setOf(Tab.Overview, Tab.Containers, Tab.Compose)) {
                    renderContent()
                }
            }
        }
    }

    private fun renderSessions() {
        addSection(getString(R.string.section_sessions))
        addAction(getString(R.string.action_text_editor), getString(R.string.detail_text_editor)) {
            openEditor(File(projectRoot, "default/Dockerfile"))
        }
        addAction(getString(R.string.action_console_editor_split), getString(R.string.detail_console_editor_split)) {
            openConsoleEditorSplit(
                getString(R.string.action_console_editor_split),
                "cd ${shellQuote(projectRoot.absolutePath)} && sh",
                File(projectRoot, "default/Dockerfile"),
            )
        }
        renderProjectFileShortcuts()
        renderDiagnostics()
    }

    private fun renderDiagnostics() {
        addSection(getString(R.string.section_diagnostics))
        renderHostEnvironment()
        addAction(getString(R.string.action_start_pdockerd), getString(R.string.detail_start_pdockerd)) { startDaemon() }
        addAction(getString(R.string.action_stop_pdockerd), getString(R.string.detail_stop_pdockerd)) {
            startService(Intent(this, PdockerdService::class.java).setAction(PdockerdService.ACTION_STOP))
            status.text = getString(R.string.status_stopped)
        }
        addAction(getString(R.string.action_keep_resident), getString(R.string.detail_keep_resident)) {
            requestBatteryOptimizationBypass()
        }
        addAction(getString(R.string.action_enable_notifications), getString(R.string.detail_enable_notifications)) {
            requestNotificationPermission()
        }
        addAction(getString(R.string.action_run_gpu_bench), getString(R.string.detail_run_gpu_bench)) {
            runAndroidGpuBench()
        }
        addAction(getString(R.string.action_prune_build_cache), getString(R.string.detail_prune_build_cache)) {
            runEngineAction(getString(R.string.action_prune_build_cache), getString(R.string.section_diagnostics)) {
                post("/build/prune").text
            }
        }
        addAction(getString(R.string.action_docker_console), getString(R.string.detail_docker_console)) {
            startDaemon()
            openTerminal(
                getString(R.string.action_docker_console),
                "printf '[pdocker] upstream Docker CLI is not packaged in this APK.\\n[pdocker] Use UI Engine actions; test suites may stage Docker CLI separately.\\n'; sh",
            )
        }
        addAction(getString(R.string.action_host_shell), getString(R.string.detail_host_shell)) {
            openTerminal(getString(R.string.terminal_host_shell), "sh")
        }
        addAction(getString(R.string.action_library_shell), getString(R.string.detail_library_shell)) {
            projectRoot.mkdirs()
            openTerminal(getString(R.string.action_library_shell), "cd ${shellQuote(projectRoot.absolutePath)} && find . -maxdepth 2 -name compose.yaml -o -name Dockerfile; sh")
        }
        addAction(getString(R.string.action_compose_shell), getString(R.string.detail_open_at_fmt, projectRoot.absolutePath)) {
            projectRoot.mkdirs()
            openTerminal(getString(R.string.section_compose), "cd ${shellQuote(projectRoot.absolutePath)} && sh")
        }
        addAction(getString(R.string.action_build_shell), getString(R.string.detail_build_shell)) {
            projectRoot.mkdirs()
            openTerminal(getString(R.string.terminal_docker_build), "cd ${shellQuote(projectRoot.absolutePath)} && sh")
        }
    }

    private fun renderStorageMetrics() {
        refreshStorageMetricsAsync()
        val metrics = storageMetrics
        if (metrics == null) {
            addWidget(
                getString(R.string.widget_storage),
                getString(R.string.storage_scanning),
                getString(R.string.detail_storage_scanning),
                detailLines = 4,
            )
            return
        }
        addWidget(
            getString(R.string.widget_storage),
            getString(R.string.storage_total_fmt, formatBytes(metrics.pdockerBytes), formatBytes(metrics.fsTotalBytes)),
            getString(
                R.string.storage_detail_fmt,
                formatBytes(metrics.layersBytes),
                formatBytes(metrics.imageViewBytes),
                formatBytes(metrics.containerPrivateBytes),
                formatBytes(metrics.fsFreeBytes),
            ),
            detailLines = 4,
        ) {
            refreshStorageMetricsAsync(force = true)
        }
    }

    private fun refreshStorageMetricsAsync(force: Boolean = false) {
        val now = System.currentTimeMillis()
        if (storageMetricsScanning) return
        if (!force && storageMetrics != null && now - lastStorageMetricsAt < 30_000L) return
        storageMetricsScanning = true
        thread(isDaemon = true, name = "pdocker-storage-metrics") {
            val layerUsage = diskUsage(layerRoot)
            val imageUsage = diskUsage(imageRoot, excludeInodes = layerUsage.inodeKeys)
            val containerUsage = diskUsage(
                containerRoot,
                excludeInodes = layerUsage.inodeKeys + imageUsage.inodeKeys,
            )
            val pdockerUsage = diskUsage(pdockerHome)
            val metrics = StorageMetrics(
                fsTotalBytes = pdockerHome.totalSpace,
                fsFreeBytes = pdockerHome.freeSpace,
                pdockerBytes = pdockerUsage.bytes,
                layersBytes = layerUsage.bytes,
                imageViewBytes = imageUsage.bytes,
                containerPrivateBytes = containerUsage.bytes,
            )
            ui.post {
                storageMetrics = metrics
                lastStorageMetricsAt = System.currentTimeMillis()
                storageMetricsScanning = false
                if (currentTab == Tab.Overview) renderContent()
            }
        }
    }

    private fun renderDaemonOperations() {
        if (daemonOperations.isEmpty()) return
        addSection(getString(R.string.section_daemon_operations))
        val now = System.currentTimeMillis()
        daemonOperations.take(5).forEach { op ->
            val elapsed = ((now - op.startedAtMs).coerceAtLeast(0L) / 1000L)
            val idle = ((now - op.updatedAtMs).coerceAtLeast(0L) / 1000L)
            val value = getString(R.string.daemon_operation_status_fmt, op.status, elapsed, jobActivityFrame())
            val job = daemonOperationJob(op)
            val detail = listOf(
                getString(R.string.daemon_operation_kind_fmt, op.kind),
                op.detail.ifBlank { "-" },
                getString(R.string.daemon_operation_idle_fmt, idle),
                job?.let { getString(R.string.action_open_job_log_fmt, it.title) }.orEmpty(),
            ).filter { it.isNotBlank() }.joinToString("\n")
            addWidget(op.title, value, detail, detailLines = 5, onClick = job?.let { activeJob ->
                { openJobLog(activeJob) }
            })
        }
    }

    private fun daemonOperationJob(op: DaemonOperation): DockerJob? {
        val running = dockerJobs.filter { it.exitCode == null }
        val detail = op.detail.lowercase()
        return running.firstOrNull { job ->
            val haystack = "${job.title} ${job.detail} ${job.command} ${job.progress}".lowercase()
            haystack.contains(op.kind.lowercase()) ||
                detail.isNotBlank() && haystack.contains(detail.take(48))
        } ?: running.firstOrNull {
            it.command.startsWith("engine compose up:") || it.command.startsWith("engine docker build:")
        } ?: dockerJobs.firstOrNull {
            it.command.startsWith("engine compose up:") || it.command.startsWith("engine docker build:")
        }
    }

    private fun refreshDaemonOperationsAsync() {
        if (daemonOperationsRefreshing) return
        daemonOperationsRefreshing = true
        thread(isDaemon = true, name = "pdocker-daemon-ops") {
            val ops = runCatching {
                val arr = engine.getArray("/system/operations")
                (0 until arr.length()).mapNotNull { index ->
                    val obj = arr.optJSONObject(index) ?: return@mapNotNull null
                    DaemonOperation(
                        id = obj.optString("Id"),
                        kind = obj.optString("Kind", "operation"),
                        title = obj.optString("Title", "operation"),
                        detail = obj.optString("Detail"),
                        status = obj.optString("Status", "running"),
                        startedAtMs = (obj.optDouble("StartedAt", 0.0) * 1000.0).toLong(),
                        updatedAtMs = (obj.optDouble("UpdatedAt", 0.0) * 1000.0).toLong(),
                    )
                }
            }.getOrDefault(emptyList())
            ui.post {
                val changed = ops != daemonOperations
                daemonOperations = ops
                daemonOperationsRefreshing = false
                if (changed && currentTab == Tab.Overview) renderContent()
            }
        }
    }

    private fun renderHostEnvironment() {
        refreshHostEnvironmentAsync()
        val env = hostEnvironment
        if (env == null) {
            addWidget(
                getString(R.string.widget_host_environment),
                getString(R.string.host_environment_loading),
                getString(R.string.detail_host_environment_loading),
                detailLines = 5,
            )
            return
        }
        addWidget(
            getString(R.string.widget_host_environment),
            hostEnvironmentSummary(env),
            hostEnvironmentDetails(env),
            detailLines = 8,
        ) {
            refreshHostEnvironmentAsync(force = true)
        }
    }

    private fun refreshHostEnvironmentAsync(force: Boolean = false) {
        val now = System.currentTimeMillis()
        if (hostEnvironmentRefreshing) return
        if (!force && hostEnvironment != null && now - lastHostEnvironmentAt < 30_000L) return
        hostEnvironmentRefreshing = true
        thread(isDaemon = true, name = "pdocker-host-environment") {
            val env = runCatching { engine.getObject("/system/host") }.getOrNull()
            ui.post {
                hostEnvironment = env ?: hostEnvironment
                lastHostEnvironmentAt = System.currentTimeMillis()
                hostEnvironmentRefreshing = false
                if (currentTab == Tab.Overview || currentTab == Tab.Sessions) renderContent()
            }
        }
    }

    private fun hostEnvironmentSummary(env: JSONObject): String {
        val host = env.optJSONObject("Host")
        val runtime = env.optJSONObject("Runtime")
        val gpu = env.optJSONObject("Gpu")
        val machine = host?.optString("Machine").orEmpty().ifBlank { "-" }
        val backend = runtime?.optString("Backend").orEmpty().ifBlank { "-" }
        val vulkan = gpu?.optString("VulkanIcdKind").orEmpty().ifBlank { "vulkan-icd" }
        val ready = if (gpu?.optBoolean("VulkanIcdReady", false) == true) "ready" else "probe"
        return getString(R.string.host_environment_summary_fmt, machine, backend, vulkan, ready)
    }

    private fun hostEnvironmentDetails(env: JSONObject): String {
        val host = env.optJSONObject("Host")
        val hardware = env.optJSONObject("Hardware")
        val software = env.optJSONObject("Software")
        val runtime = env.optJSONObject("Runtime")
        val gpu = env.optJSONObject("Gpu")
        val frameworks = env.optJSONObject("Frameworks")
        val vulkan = frameworks?.optJSONObject("Vulkan")
        val eglGles = frameworks?.optJSONObject("EglOpenGles")
        val opencl = frameworks?.optJSONObject("OpenCL")
        val nnapi = frameworks?.optJSONObject("NnApi")
        val ahb = frameworks?.optJSONObject("AndroidHardwareBuffer")
        val mediaCodec = frameworks?.optJSONObject("MediaCodec")
        val paths = env.optJSONObject("Paths")
        val helperStatus = listOf("DirectExecutor", "GpuExecutor", "GpuShim", "VulkanIcd")
            .map { key ->
                val obj = paths?.optJSONObject(key)
                "$key=${if (obj?.optBoolean("Exists", false) == true) "ok" else "missing"}"
            }
            .joinToString(" ")
        return listOf(
            getString(R.string.host_environment_kernel_fmt, host?.optString("Release").orEmpty().ifBlank { "-" }),
            getString(
                R.string.host_environment_hardware_fmt,
                hardware?.optInt("ProcessorCount", 0) ?: 0,
                formatBytes(hardware?.optLong("MemTotal", 0L) ?: 0L),
                formatBytes(hardware?.optLong("MemAvailable", 0L) ?: 0L),
            ),
            getString(R.string.host_environment_python_fmt, software?.optString("Python").orEmpty().ifBlank { "-" }),
            getString(
                R.string.host_environment_runtime_fmt,
                runtime?.optString("Driver").orEmpty().ifBlank { "-" },
                runtime?.optString("Platform").orEmpty().ifBlank { "-" },
                runtime?.optString("DockerApiVersion").orEmpty().ifBlank { "-" },
            ),
            getString(
                R.string.host_environment_gpu_fmt,
                gpu?.optString("CommandApi").orEmpty().ifBlank { "-" },
                if (gpu?.optBoolean("ExecutorAvailable", false) == true) "yes" else "no",
            ),
            getString(
                R.string.host_environment_vulkan_fmt,
                vulkan?.optString("ApiVersion").orEmpty().ifBlank { gpu?.optString("VulkanApiVersion").orEmpty().ifBlank { "-" } },
                vulkan?.optString("IcdKind").orEmpty().ifBlank { gpu?.optString("VulkanIcdKind").orEmpty().ifBlank { "-" } },
                if (vulkan?.optBoolean("IcdReady", false) == true) "ready" else "probe",
            ),
            getString(
                R.string.host_environment_gles_fmt,
                eglGles?.optString("ComputeApi").orEmpty().ifBlank { "OpenGL ES" },
                if (eglGles?.optBoolean("EglAvailable", false) == true) "yes" else "no",
                if (eglGles?.optBoolean("GlesAvailable", false) == true) "yes" else "no",
            ),
            getString(
                R.string.host_environment_opencl_fmt,
                openclStatus(opencl),
                if (opencl?.optBoolean("IcdReady", false) == true) "ready" else "probe",
            ),
            getString(
                R.string.host_environment_accel_fmt,
                if (nnapi?.optBoolean("RuntimeAvailable", false) == true) "yes" else "no",
                if (ahb?.optJSONObject("Library")?.optBoolean("Available", false) == true) "yes" else "no",
                if (mediaCodec?.optJSONObject("Library")?.optBoolean("Available", false) == true) "yes" else "no",
            ),
            helperStatus,
        ).joinToString("\n")
    }

    private fun openclStatus(opencl: JSONObject?): String {
        if (opencl == null) return "missing"
        val api = opencl.optString("ApiVersion").ifBlank { "api unknown" }
        val loader = opencl.optJSONObject("Loader")
        val loaderText = if (loader?.optBoolean("Available", false) == true) {
            loader.optString("Path").ifBlank { "loader available" }
        } else {
            "loader missing"
        }
        return "$api, $loaderText"
    }

    private fun diskUsage(root: File, excludeInodes: Set<String> = emptySet()): DiskUsage {
        if (!root.exists()) return DiskUsage(0L, emptySet())
        var bytes = 0L
        val seen = HashSet<String>()
        runCatching {
            root.walkTopDown().forEach { file ->
                val stat = runCatching { Os.lstat(file.absolutePath) }.getOrNull() ?: return@forEach
                val key = "${stat.st_dev}:${stat.st_ino}"
                if (key in excludeInodes || !seen.add(key)) return@forEach
                bytes += stat.st_blocks * 512L
            }
        }
        return DiskUsage(bytes, seen)
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

    private fun ensureDaemonStarted() {
        if (File(pdockerHome, "pdockerd.sock").exists()) return
        val now = System.currentTimeMillis()
        if (now - lastDaemonStartAttemptAt < 5000L) return
        lastDaemonStartAttemptAt = now
        startDaemon()
    }

    private fun runAndroidGpuBench() {
        val title = getString(R.string.action_run_gpu_bench)
        val group = getString(R.string.section_diagnostics)
        status.text = getString(R.string.status_gpu_bench_running)
        thread(isDaemon = true, name = "android-gpu-bench") {
            val output = runCatching { AndroidGpuBench.run(this) }
                .getOrElse { getString(R.string.engine_operation_failed_fmt, it.message.orEmpty()) }
            ui.post {
                status.text = getString(R.string.status_gpu_bench_done)
                openTextTool(group, title, output)
            }
        }
    }

    private fun waitForEngine(timeoutMs: Long = 90_000): Boolean {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            runCatching {
                val resp = engine.request("GET", "/_ping")
                if (resp.status == 200 && resp.text.trim() == "OK") return true
            }
            Thread.sleep(300)
        }
        return false
    }

    private fun runContainerAction(group: String, title: String, block: DockerEngineClient.() -> String) {
        runEngineAction(title, group, block)
    }

    private fun runEngineAction(title: String, group: String, block: DockerEngineClient.() -> String) {
        runEngineJob(title, group, "engine action: $title") { _ -> block() }
    }

    private fun runEngineJob(
        title: String,
        group: String,
        command: String,
        block: DockerEngineClient.((String) -> Unit) -> String,
    ) {
        startDaemon()
        status.text = getString(R.string.status_starting)
        val key = "engine-job:$group:$title:$command"
        val job = DockerJob(
            id = "job-" + System.currentTimeMillis().toString(36),
            title = title,
            detail = group,
            command = command,
            group = group,
            toolKey = key,
            status = getString(R.string.job_running),
        )
        dockerJobs.add(0, job)
        trimDockerJobs()
        appendPersistentJobLog(job.id, jobTerminalPrelude(job))
        saveDockerJobs()
        renderContent()
        openLiveJobLog(job, switchTo = true)
        thread(isDaemon = true, name = "engine-action") {
            val output = StringBuilder()
            val result = runCatching {
                if (!waitForEngine()) error(getString(R.string.status_socket_absent))
                engine.block { line ->
                    output.appendLine(line)
                    appendEngineJobOutput(job.id, line)
                }
            }
            val exitCode = if (result.isSuccess) 0 else 1
            val finalOutput = result.getOrElse {
                getString(R.string.engine_operation_failed_fmt, summarizeEngineFailure(it.message.orEmpty()))
            }
            val finishOutput = if (command.startsWith("engine compose up:") || command.startsWith("engine docker build:")) {
                if (exitCode == 0) "" else finalOutput
            } else {
                appendUniqueLines(output, finalOutput)
                output.toString()
            }
            ui.post {
                finishEngineJob(job.id, exitCode, finishOutput)
                val finished = dockerJobs.firstOrNull { it.id == job.id }
                if (finished != null) handleEngineJobFinished(finished)
                refreshStorageMetricsAsync(force = true)
                renderContent()
                refreshStatus()
            }
        }
    }

    private fun summarizeEngineFailure(message: String): String {
        val lines = message.lineSequence()
            .map { cleanTerminalLine(it) }
            .filter { it.isNotBlank() }
            .toList()
        val important = lines.lastOrNull {
            it.contains("ERROR:", ignoreCase = true) ||
                it.contains("No space left", ignoreCase = true) ||
                it.contains("failed", ignoreCase = true)
        }
        return important ?: lines.lastOrNull().orEmpty().ifBlank { message.take(500) }
    }

    private fun runImageBuild(dir: File, title: String) {
        runEngineJob(title, workspaceGroup(), "engine docker build: ${dir.absolutePath}") { emit ->
            emit("Service ${dir.name} Building")
            buildImageStreaming(dir, "local/${dir.name}:latest") { line -> emit(line) }
        }
    }

    private fun runComposeUp(dir: File, title: String) {
        runEngineJob(title, dir.name, "engine compose up: ${dir.absolutePath}") { emit ->
            val services = parseComposeServices(dir)
            if (services.isEmpty()) error("compose file has no services")
            val out = StringBuilder()
            services.forEach { service ->
                var image = service.image.ifBlank { "local/${dir.name}-${service.name}:latest" }
                var runtimeBlocked = false
                val context = service.buildContext
                if (!context.isNullOrBlank()) {
                    val contextDir = File(dir, context).canonicalFile
                    val line = "Service ${service.name} Building"
                    emit(line)
                    out.appendLine(line)
                    val buildOutput = StringBuilder()
                    val built = runCatching {
                        buildImageStreaming(contextDir, image) { buildLine ->
                            emit(buildLine)
                            buildOutput.appendLine(buildLine)
                        }
                    }
                    if (built.isSuccess) {
                        out.append(buildOutput)
                    } else {
                        val message = built.exceptionOrNull()?.message.orEmpty()
                        if (!isRuntimeBackendBlocked(message)) throw built.exceptionOrNull() ?: IllegalStateException(message)
                        runtimeBlocked = true
                        val fallbackImage = dockerfileBaseImage(contextDir) ?: "ubuntu:22.04"
                        image = fallbackImage
                        val blocked = "Service ${service.name} Build blocked by current container runtime; using materialized base image $fallbackImage for inspection"
                        emit(blocked)
                        out.appendLine(blocked)
                    }
                }
                val containerName = service.containerName.ifBlank { "${dir.name}-${service.name}-1" }
                runCatching {
                    request("DELETE", "/containers/${DockerEngineClient.encodePath(containerName)}?force=1")
                }
                emit("Container $containerName Creating")
                out.appendLine("Container $containerName Creating")
                val id = createContainer(containerName, service.toContainerConfig(image, dir))
                if (runtimeBlocked) {
                    val prepared = "Container $containerName Prepared for inspection (container runtime unavailable)"
                    emit(prepared)
                    out.appendLine(prepared)
                    return@forEach
                }
                emit("Container $containerName Starting")
                out.appendLine("Container $containerName Starting")
                val started = runCatching { post("/containers/${DockerEngineClient.encodePath(id)}/start") }
                if (started.isSuccess) {
                    emit("Container $containerName Started")
                    out.appendLine("Container $containerName Started")
                    composeServiceUrls(service).forEach { (label, url) ->
                        val line = "Service URL $label $url"
                        emit(line)
                        out.appendLine(line)
                    }
                } else {
                    val message = started.exceptionOrNull()?.message.orEmpty()
                    if (!isRuntimeBackendBlocked(message)) {
                        throw started.exceptionOrNull() ?: IllegalStateException(message)
                    }
                    emit("Container $containerName Prepared (runtime blocked)")
                    out.appendLine("Container $containerName Prepared (runtime blocked)")
                }
            }
            out.append('\n').append(formatContainers(getArray("/containers/json?all=1")))
            out.toString()
        }
    }

    private fun appendUniqueLines(output: StringBuilder, text: String) {
        val existing = output.lineSequence().map { it.trim() }.filter { it.isNotBlank() }.toMutableSet()
        text.lineSequence()
            .map { it.trimEnd() }
            .filter { it.isNotBlank() }
            .forEach { line ->
                if (existing.add(line.trim())) output.appendLine(line)
            }
    }

    private fun isRuntimeBackendBlocked(message: String): Boolean {
        val lower = message.lowercase()
        return listOf(
            "android execution backend is unavailable",
            "bundled proot backend crashed",
            "no-proot/direct android execution backend",
            "cannot execute container processes yet",
            "will not start a fake listener",
            "runtime preflight failed before running",
            "run skipped because the android execution backend is unavailable",
        ).any { it in lower }
    }

    private fun dockerfileBaseImage(contextDir: File): String? {
        val dockerfile = File(contextDir, "Dockerfile")
        if (!dockerfile.isFile) return null
        return dockerfile.readLines()
            .map { it.trim() }
            .mapNotNull { Regex("""(?i)^FROM\s+([^\s]+)""").find(it)?.groupValues?.getOrNull(1) }
            .firstOrNull()
    }

    private fun parseComposeServices(dir: File): List<ComposeService> {
        val file = listOf("compose.yaml", "compose.yml", "docker-compose.yaml", "docker-compose.yml")
            .map { File(dir, it) }
            .firstOrNull { it.isFile }
            ?: return emptyList()
        val lines = file.readLines()
        val serviceLinks = parseComposeHeaderServiceLinks(lines)
        val services = mutableListOf<ComposeService>()
        var inServices = false
        var current: ComposeService? = null
        var blockKey: String? = null
        lines.forEach { raw ->
            val line = raw.substringBefore('#').trimEnd()
            if (line.isBlank()) return@forEach
            val indent = raw.takeWhile { it == ' ' }.length
            val trimmed = line.trim()
            if (indent == 0) {
                inServices = trimmed == "services:"
                current = null
                blockKey = null
                return@forEach
            }
            if (!inServices) return@forEach
            if (indent == 2 && trimmed.endsWith(":")) {
                current = ComposeService(
                    name = trimmed.removeSuffix(":"),
                    serviceLinks = serviceLinks.toMutableList(),
                ).also { services += it }
                blockKey = null
                return@forEach
            }
            val svc = current ?: return@forEach
            if (indent == 4) {
                val key = trimmed.substringBefore(':')
                val value = trimmed.substringAfter(':', "").trim()
                blockKey = if (value.isBlank()) key else null
                when (key) {
                    "image" -> svc.image = composeValue(value)
                    "container_name" -> svc.containerName = composeValue(value)
                    "working_dir" -> svc.workingDir = composeValue(value)
                    "command" -> svc.command = composeCommand(value)
                    "gpus" -> svc.gpus = composeValue(value)
                    "build" -> if (value.isNotBlank()) svc.buildContext = composeValue(value)
                    "depends_on" -> if (value.isNotBlank()) svc.dependsOn += composeStringList(value)
                    "healthcheck" -> svc.hasHealthcheck = true
                }
                return@forEach
            }
            if (indent >= 6) {
                when (blockKey) {
                    "build" -> {
                        val key = trimmed.substringBefore(':')
                        val value = trimmed.substringAfter(':', "").trim()
                        if (key == "context") svc.buildContext = composeValue(value)
                    }
                    "environment" -> parseComposeMapOrList(trimmed)?.let { (k, v) -> svc.environment[k] = v }
                    "ports" -> parseComposeListValue(trimmed)?.let { svc.ports += it }
                    "volumes" -> parseComposeListValue(trimmed)?.let { svc.volumes += it }
                    "depends_on" -> {
                        parseComposeListValue(trimmed)?.let { svc.dependsOn += it }
                        parseComposeMapOrList(trimmed)?.first?.let { svc.dependsOn += it }
                    }
                }
            }
        }
        return services
    }

    private fun parseComposeHeaderServiceLinks(lines: List<String>): List<ComposeServiceLink> {
        val links = mutableListOf<ComposeServiceLink>()
        val autoOpenLabels = mutableSetOf<String>()
        for (raw in lines) {
            val trimmed = raw.trim()
            if (trimmed.isBlank()) continue
            if (!trimmed.startsWith("#")) break
            val comment = trimmed.removePrefix("#").trim()
            comment.removePrefix("pdocker.auto-open:")
                .takeIf { it != comment }
                ?.trim()
                ?.takeIf { it.isNotBlank() }
                ?.let { autoOpenLabels += it }
            val rest = comment
                .removePrefix("pdocker.service-url:")
                .takeIf { it != comment }
                ?.trim()
                ?: continue
            parseComposeServiceLink(rest)?.let { links += it }
        }
        return links.distinct().map { link ->
            if (link.label in autoOpenLabels) link.copy(autoOpen = true) else link
        }
    }

    private fun parseComposeServiceLink(value: String): ComposeServiceLink? {
        val left = value.substringBefore('=', "").trim()
        val right = value.substringAfter('=', "").trim()
        if (left.isBlank() || right.isBlank()) return null
        val port = left.toIntOrNull()
        return if (port != null) {
            if (port !in 1..65535) return null
            ComposeServiceLink(port, right, null)
        } else {
            if (!right.startsWith("http://") && !right.startsWith("https://")) return null
            ComposeServiceLink(null, left, right)
        }
    }

    private fun ComposeService.toContainerConfig(imageName: String, projectDir: File): JSONObject {
        val exposedPorts = JSONObject()
        val portBindings = JSONObject()
        ports.forEach { spec ->
            val parts = spec.split(":")
            val container = (parts.getOrNull(1) ?: parts.firstOrNull()).orEmpty()
            if (container.isNotBlank()) {
                val key = if (container.contains("/")) container else "$container/tcp"
                exposedPorts.put(key, JSONObject())
                if (parts.size >= 2) {
                    portBindings.put(key, JSONArray().put(JSONObject().put("HostPort", parts[0])))
                }
            }
        }
        val binds = JSONArray()
        volumes.forEach { spec ->
            val parts = spec.split(":")
            if (parts.size >= 2) {
                val host = File(projectDir, parts[0]).absolutePath
                val guest = parts[1]
                val mode = parts.getOrNull(2)?.let { ":$it" }.orEmpty()
                binds.put("$host:$guest$mode")
            }
        }
        val hostConfig = JSONObject()
            .put("Binds", binds)
            .put("PortBindings", portBindings)
        if (gpus.isNotBlank() && gpus != "null") {
            hostConfig.put(
                "DeviceRequests",
                JSONArray().put(
                    JSONObject()
                        .put("Driver", "pdocker-gpu")
                        .put("Count", -1)
                        .put("Capabilities", JSONArray().put(JSONArray().put("gpu")))
                        .put(
                            "Options",
                            JSONObject()
                                .put("pdocker.gpu", "vulkan")
                                .put("pdocker.cuda", "compat")
                                .put("pdocker.opencl", "opencl"),
                        ),
                ),
            )
        }
        val labels = JSONObject()
        serviceLinks.forEachIndexed { index, link ->
            if (link.port != null) {
                labels.put("$PDOCKER_SERVICE_URL_LABEL_PREFIX${link.port}", link.label)
            } else if (!link.url.isNullOrBlank()) {
                labels.put("${PDOCKER_SERVICE_URL_LABEL_PREFIX}url.$index", "${link.label}=${link.url}")
            }
        }
        return JSONObject()
            .put("Image", imageName)
            .put("Cmd", JSONArray(command))
            .put("WorkingDir", workingDir)
            .put("Env", JSONArray(environment.map { (k, v) -> "$k=$v" }))
            .put("ExposedPorts", exposedPorts)
            .put("Labels", labels)
            .put("HostConfig", hostConfig)
    }

    private fun parseComposeMapOrList(line: String): Pair<String, String>? {
        val item = line.removePrefix("-").trim()
        if ("=" in item) {
            val k = item.substringBefore('=')
            return k to composeValue(item.substringAfter('='))
        }
        if (":" in item) {
            val k = item.substringBefore(':').trim()
            return k to composeValue(item.substringAfter(':').trim())
        }
        return null
    }

    private fun parseComposeListValue(line: String): String? =
        line.takeIf { it.trimStart().startsWith("-") }?.trim()?.removePrefix("-")?.trim()?.let(::composeValue)

    private fun composeStringList(value: String): List<String> {
        val cleaned = composeValue(value)
        if (cleaned.startsWith("[") && cleaned.endsWith("]")) {
            return runCatching {
                val arr = JSONArray(cleaned)
                (0 until arr.length()).mapNotNull { arr.optString(it).takeIf { item -> item.isNotBlank() } }
            }.getOrElse {
                cleaned.trim('[', ']').split(',').map { composeValue(it) }.filter { it.isNotBlank() }
            }
        }
        return cleaned.split(',').map { composeValue(it) }.filter { it.isNotBlank() }
    }

    private fun composeCommand(value: String): List<String> {
        val cleaned = composeValue(value)
        if (cleaned.startsWith("[") && cleaned.endsWith("]")) {
            return runCatching {
                val arr = JSONArray(cleaned)
                (0 until arr.length()).map { arr.getString(it) }
            }.getOrElse { emptyList() }
        }
        return if (cleaned.isBlank()) emptyList() else listOf("/bin/sh", "-lc", cleaned)
    }

    private fun composeValue(value: String): String {
        var out = value.trim().trim('"', '\'')
        out = Regex("""\$\{[A-Za-z_][A-Za-z0-9_]*:-([^}]*)\}""").replace(out) { it.groupValues[1] }
        out = Regex("""\$\{[A-Za-z_][A-Za-z0-9_]*-([^}]*)\}""").replace(out) { it.groupValues[1] }
        out = Regex("""\$\{[A-Za-z_][A-Za-z0-9_]*\}""").replace(out, "")
        return out
    }

    private fun formatContainers(containers: JSONArray): String {
        val columns = listOf(
            "CONTAINER ID" to 12,
            "IMAGE" to 25,
            "STATUS" to 13,
            "PORTS" to 30,
        )
        fun cell(value: String, width: Int): String {
            val compact = value.replace('\n', ' ').replace('\r', ' ')
            val clipped = if (compact.length > width) compact.take(width - 3) + "..." else compact
            return clipped.padEnd(width)
        }
        val separator = "  "
        val header = columns.joinToString(separator) { (title, width) -> cell(title, width) } + separator + "NAMES"
        if (containers.length() == 0) return "$header\n"
        val lines = mutableListOf(header)
        for (i in 0 until containers.length()) {
            val obj = containers.optJSONObject(i) ?: continue
            val names = obj.optJSONArray("Names")
            val name = names?.optString(0).orEmpty().trim('/').ifBlank { obj.optString("Id").take(12) }
            lines += listOf(
                cell(obj.optString("Id").take(12), columns[0].second),
                cell(obj.optString("Image"), columns[1].second),
                cell(obj.optString("Status"), columns[2].second),
                cell(formatPortsForPs(obj.optJSONArray("Ports")), columns[3].second),
                name,
            ).joinToString(separator)
        }
        return lines.joinToString("\n")
    }

    private fun formatPortsForPs(ports: JSONArray?): String {
        if (ports == null || ports.length() == 0) return ""
        return (0 until ports.length()).mapNotNull { i ->
            val port = ports.optJSONObject(i) ?: return@mapNotNull null
            val privatePort = port.optInt("PrivatePort", -1).takeIf { it > 0 } ?: return@mapNotNull null
            val type = port.optString("Type").ifBlank { "tcp" }
            val publicPort = port.optInt("PublicPort", -1)
            val ip = port.optString("IP").ifBlank { "127.0.0.1" }
            if (publicPort > 0) "$ip:$publicPort->$privatePort/$type" else "$privatePort/$type"
        }.joinToString(", ")
    }

    private fun openTextTool(group: String, title: String, text: String) {
        val key = "engine:$group:$title"
        val existing = toolTabs.indexOfFirst { it.key == key }
        if (existing >= 0) {
            val tab = toolTabs[existing]
            ((tab.view as? ScrollView)?.getChildAt(0) as? TextView)?.text = text
            switchTool(existing)
            return
        }
        val view = ScrollView(this).apply {
            addView(TextView(this@MainActivity).apply {
                this.text = text
                textSize = 12f
                typeface = Typeface.MONOSPACE
                setTextIsSelectable(true)
                setPadding(18, 18, 18, 18)
            })
        }
        toolTabs += ToolTab(group, title, ToolKind.Editor, view, key = key)
        switchTool(toolTabs.lastIndex)
    }

    private fun handleAutomationIntent(intent: Intent?) {
        val action = intent?.action ?: return
        val debuggable = (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
        if (!debuggable) return
        when (action) {
            ACTION_SMOKE_START -> startDaemon()
            ACTION_SMOKE_GPU_BENCH -> runAndroidGpuBench()
            ACTION_SMOKE_COMPOSE_UP -> {
                val project = intent.getStringExtra("project").orEmpty().ifBlank { "default" }
                val dir = File(projectRoot, project)
                ui.post { runComposeUp(dir, getString(R.string.terminal_compose_up_fmt, project)) }
            }
        }
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

    private fun openTerminal(
        title: String,
        command: String,
        group: String = workspaceGroup(),
        onOutput: ((ByteArray) -> Unit)? = null,
        contextualize: Boolean = true,
    ) {
        val launchCommand = if (contextualize) terminalSessionCommand(title, group, command) else command
        val key = "$title\n$launchCommand"
        val existing = toolTabs.indexOfFirst {
            it.kind == ToolKind.Terminal && it.group == group && it.key == key
        }
        if (existing >= 0) {
            switchTool(existing)
            return
        }
        val view = terminalView(launchCommand, onOutput)
        val bridge = view.getTag(R.id.pdocker_bridge_tag) as Bridge
        toolTabs += ToolTab(group, title, ToolKind.Terminal, view, bridge, key)
        switchTool(toolTabs.lastIndex)
    }

    private fun openDockerTerminal(title: String, command: String, group: String = workspaceGroup()) {
        startDaemon()
        val normalizedCommand = normalizeDockerCommand(command)
        val id = "job-" + System.currentTimeMillis().toString(36)
        val wrapped = stayAfterCommand(dockerCommand(normalizedCommand), id)
        val launchCommand = terminalSessionCommand(title, group, wrapped)
        val key = "$title\n$launchCommand"
        val job = DockerJob(
            id = id,
            title = title,
            detail = group,
            command = normalizedCommand,
            group = group,
            toolKey = key,
            status = getString(R.string.job_running),
        )
        dockerJobs.add(0, job)
        trimDockerJobs()
        saveDockerJobs()
        openTerminal(
            title,
            launchCommand,
            group,
            onOutput = { bytes -> handleDockerJobOutput(job.id, bytes) },
            contextualize = false,
        )
        renderContent()
    }

    private fun openDockerInteractiveTerminal(title: String, containerId: String, group: String = workspaceGroup()) {
        startDaemon()
        openTerminal(
            title,
            "${Bridge.ENGINE_EXEC_PREFIX}$containerId",
            group,
            contextualize = false,
        )
    }

    private fun openEditor(file: File, group: String = workspaceGroup()) {
        val target = resolveProjectFile(file)
        val title = editorTitle(target)
        val key = target.absolutePath
        val existing = toolTabs.indexOfFirst {
            it.kind == ToolKind.Editor && it.group == group && it.key == key
        }
        if (existing >= 0) {
            switchTool(existing)
            return
        }
        toolTabs += ToolTab(group, title, ToolKind.Editor, editorView(target), key = key)
        switchTool(toolTabs.lastIndex)
    }

    private fun openConsoleEditorSplit(title: String, command: String, file: File, group: String = workspaceGroup()) {
        val target = resolveProjectFile(file)
        val view = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val terminal = terminalView(command)
            addView(terminal, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                0,
                0.56f,
            ))
            addView(editorView(target), LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                0,
                0.44f,
            ))
        }
        val bridge = findBridge(view)
        toolTabs += ToolTab(group, title, ToolKind.Split, view, bridge, "$title\n$command\n${target.absolutePath}")
        switchTool(toolTabs.lastIndex)
    }

    private fun openImageFiles(image: File? = null) {
        startActivity(Intent(this, ImageFilesActivity::class.java).apply {
            image?.let { putExtra(ImageFilesActivity.EXTRA_IMAGE_NAME, it.name) }
        })
    }

    private fun openContainerFiles(container: File) {
        startActivity(Intent(this, ImageFilesActivity::class.java).apply {
            putExtra(ImageFilesActivity.EXTRA_CONTAINER_ID, container.name)
        })
    }

    private fun terminalView(command: String, onOutput: ((ByteArray) -> Unit)? = null): View {
        val webView = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
        }
        val bridge = Bridge(this, webView, command, onOutput)
        webView.addJavascriptInterface(bridge, "PdockerBridge")
        webView.loadUrl("file:///android_asset/xterm/index.html")
        webView.setTag(R.id.pdocker_bridge_tag, bridge)
        return webView
    }

    private fun terminalLogPane(): TerminalLogPane {
        lateinit var pane: TerminalLogPane
        val webView = WebView(this).apply {
            settings.javaScriptEnabled = true
            settings.domStorageEnabled = true
            addJavascriptInterface(TerminalLogBridge(this@MainActivity), "PdockerBridge")
            webViewClient = object : WebViewClient() {
                override fun onPageFinished(view: WebView?, url: String?) {
                    pane.markReady()
                }
            }
        }
        pane = TerminalLogPane(webView)
        webView.loadUrl("file:///android_asset/xterm/index.html")
        return pane
    }

    private fun editorView(file: File): View {
        return CodeEditorView(this, file, MAX_INLINE_EDIT_BYTES, ::defaultEditorContent)
    }

    private fun terminalSessionCommand(title: String, group: String, command: String): String {
        val label = "$group / $title"
        val prompt = "[pdocker:$label] \\w $ "
        return listOf(
            "export PDOCKER_TERMINAL_TITLE=${shellQuote(title)}",
            "export PDOCKER_TERMINAL_GROUP=${shellQuote(group)}",
            "export PS1=${shellQuote(prompt)}",
            "printf '\\n[pdocker terminal] %s\\n[pdocker group] %s\\n\\n' ${shellQuote(title)} ${shellQuote(group)}",
            command,
        ).joinToString("; ")
    }

    private fun switchTool(index: Int) {
        if (index !in toolTabs.indices) return
        currentTool = index
        currentToolGroup = toolTabs[index].group
        lowerHost.removeAllViews()
        val view = toolTabs[index].view
        if (view.parent != null) {
            (view.parent as? FrameLayout)?.removeView(view)
        }
        lowerHost.addView(view, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT,
            FrameLayout.LayoutParams.MATCH_PARENT,
        ))
        renderToolChrome()
    }

    private fun renderToolChrome() {
        if (!::lowerGroupRow.isInitialized || !::lowerTabRow.isInitialized || !::lowerHost.isInitialized) return
        lowerGroupRow.removeAllViews()
        lowerTabRow.removeAllViews()
        if (toolTabs.isEmpty()) {
            lowerHost.removeAllViews()
            lowerHost.addView(TextView(this).apply {
                text = getString(R.string.tool_empty)
                textSize = 14f
                alpha = 0.72f
                gravity = Gravity.CENTER
            }, FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ))
            return
        }
        val groups = toolTabs.map { it.group }.distinct()
        if (currentTool !in toolTabs.indices) currentTool = 0
        if (currentToolGroup == null || currentToolGroup !in groups) {
            currentToolGroup = toolTabs[currentTool].group
        }
        groups.forEach { group ->
            lowerGroupRow.addView(Button(this).apply {
                text = group
                isAllCaps = false
                alpha = if (group == currentToolGroup) 1f else 0.66f
                setOnClickListener {
                    currentToolGroup = group
                    switchTool(toolTabs.indexOfFirst { it.group == group })
                }
            })
        }
        toolTabs.forEachIndexed { index, tab ->
            if (tab.group == currentToolGroup) {
                lowerTabRow.addView(Button(this).apply {
                    text = tab.title
                    isAllCaps = false
                    alpha = if (index == currentTool) 1f else 0.72f
                    setOnClickListener { switchTool(index) }
                })
            }
        }
        lowerTabRow.addView(Button(this).apply {
            text = "+"
            isAllCaps = false
            setOnClickListener {
                openTerminal(
                    getString(R.string.terminal_shell_numbered, toolTabs.count { it.group == currentToolGroup } + 1),
                    "sh",
                    currentToolGroup ?: workspaceGroup(),
                )
            }
        })
    }

    private fun findBridge(view: View): Bridge? {
        (view.getTag(R.id.pdocker_bridge_tag) as? Bridge)?.let { return it }
        if (view is LinearLayout) {
            for (i in 0 until view.childCount) {
                findBridge(view.getChildAt(i))?.let { return it }
            }
        }
        return null
    }

    private fun workspaceGroup(): String = getString(R.string.tool_group_workspace)

    private fun resolveProjectFile(file: File): File {
        val projects = projectRoot.apply { mkdirs() }.canonicalFile
        val canonical = file.canonicalFile
        return if (canonical.toPath().startsWith(projects.toPath())) {
            canonical
        } else {
            File(projects, "default/${file.name.ifBlank { "Dockerfile" }}").canonicalFile
        }
    }

    private fun defaultEditorContent(name: String): String =
        when (name) {
            "compose.yaml", "compose.yml", "docker-compose.yaml", "docker-compose.yml" ->
                "services:\n  app:\n    image: ubuntu:22.04\n    command: [\"/bin/bash\", \"-lc\", \"echo hello from compose\"]\n"
            "Dockerfile" ->
                "FROM ubuntu:22.04\nCMD [\"/bin/bash\", \"-lc\", \"echo hello from Dockerfile\"]\n"
            else -> ""
        }

    private fun editorTitle(file: File): String {
        val parent = file.parentFile?.name.orEmpty()
        return if (parent.isBlank() || parent == "default") file.name else "$parent/${file.name}"
    }

    private fun stayAfterCommand(command: String, jobId: String? = null): String {
        val marker = jobId?.let {
            "; printf '\\n__PDOCKER_JOB_EXIT:${it}:%s__\\n' \"\$status\""
        }.orEmpty()
        return "$command; status=\$?; printf '\\n[pdocker] command exited: %s\\n' \"\$status\"$marker; exec sh"
    }

    private fun dockerCommand(command: String): String {
        val normalized = normalizeDockerCommand(command)
        val quoted = shellQuote(normalized)
        val dockerConfig = shellQuote(File(filesDir, "pdocker-runtime/docker-bin").absolutePath)
        return listOf(
            "export DOCKER_CONFIG=$dockerConfig DOCKER_BUILDKIT=0 COMPOSE_DOCKER_CLI_BUILD=0 BUILDKIT_PROGRESS=plain COMPOSE_PROGRESS=plain COMPOSE_MENU=false",
            "i=0; until docker version >/dev/null 2>&1; do i=\$((i+1)); if [ \"\$i\" -ge 30 ]; then echo '[pdocker] pdockerd did not become ready within 30s'; break; fi; printf '[pdocker] waiting for pdockerd... %s/30\\n' \"\$i\"; sleep 1; done",
            "if printf '%s\\n' $quoted | grep -q 'docker compose' && ! docker compose version >/dev/null 2>&1; then echo '[pdocker] docker compose is unavailable in the bundled docker CLI'; false; else $normalized; fi",
        ).joinToString("; ")
    }

    private fun dockerBuildCommand(dir: File): String =
        "cd ${shellQuote(dir.absolutePath)} && docker build -t local/${dir.name}:latest ."

    private fun composeUpCommand(dir: File): String =
        "cd ${shellQuote(dir.absolutePath)} && docker compose up --detach --build && docker compose ps && docker compose logs --tail=80"

    private fun normalizeDockerCommand(command: String): String =
        command.replace(Regex("(^|[;&|]\\s*)docker-compose(?=\\s)")) {
            "${it.groupValues[1]}docker compose"
        }

    private fun renderDockerJobs(filter: ((DockerJob) -> Boolean)? = null) {
        val jobs = dockerJobs.filter { filter?.invoke(it) ?: true }
        if (jobs.isEmpty()) return
        addSection(getString(R.string.section_jobs))
        jobs.take(5).forEach { job ->
            val statusText = jobStatusText(job)
            val detail = listOf(
                job.detail,
                job.progress,
                job.command,
                job.output.takeLast(3).joinToString("\n"),
            ).filter { it.isNotBlank() }.joinToString("\n")
            addWidget(job.title, statusText, detail) {
                val index = toolTabs.indexOfFirst { it.key == job.toolKey }
                if (index >= 0) switchTool(index) else openJobLog(job)
            }
            addAction(getString(R.string.action_open_job_log_fmt, job.title), job.command) {
                openJobLog(job)
            }
            if (job.exitCode == null) {
                addAction(getString(R.string.action_stop_job_fmt, job.title), job.command) {
                    stopDockerJob(job.id)
                }
            } else {
                addAction(getString(R.string.action_retry_job_fmt, job.title), job.command) {
                    retryDockerJob(job)
                }
            }
        }
    }

    private fun retryDockerJob(job: DockerJob) {
        when {
            job.command.startsWith("engine compose up:") -> {
                val dir = File(job.command.removePrefix("engine compose up:").trim())
                runComposeUp(dir, job.title)
            }
            job.command.startsWith("engine docker build:") -> {
                val dir = File(job.command.removePrefix("engine docker build:").trim())
                runImageBuild(dir, job.title)
            }
            job.command.startsWith("engine action:") -> {
                openJobLog(job)
                appendEngineJobOutput(job.id, "Retry for this Engine API action is not wired yet; use the visible action button instead.")
            }
            else -> openDockerTerminal(job.title, job.command, job.group)
        }
    }

    private fun jobStatusText(job: DockerJob): String {
        val elapsed = ((job.endedAt ?: System.currentTimeMillis()) - job.startedAt).coerceAtLeast(0) / 1000
        val activity = if (job.exitCode == null) " ${jobActivityFrame()}" else ""
        return when {
            job.exitCode == null -> getString(R.string.job_status_running_fmt, elapsed) + activity
            job.exitCode == 0 -> getString(R.string.job_status_done_fmt, elapsed)
            job.exitCode == -129 -> getString(R.string.job_status_stopped_fmt, elapsed)
            job.exitCode == -130 -> getString(R.string.job_status_interrupted_fmt, elapsed)
            else -> getString(R.string.job_status_failed_fmt, job.exitCode ?: -1, elapsed)
        }
    }

    private fun jobProgressText(job: DockerJob): String {
        val progress = job.progress.takeIf { it.isNotBlank() }
        if (job.exitCode != null) return progress.orEmpty()
        return progress ?: getString(R.string.job_activity_fmt, jobActivityFrame())
    }

    private fun jobActivityFrame(): String {
        val frames = charArrayOf('|', '/', '-', '\\')
        return frames[((System.currentTimeMillis() / 250L) % frames.size).toInt()].toString()
    }

    private fun openJobLog(job: DockerJob) {
        val existing = toolTabs.indexOfFirst { it.key == job.toolKey }
        if (existing >= 0) {
            switchTool(existing)
            return
        }
        if (job.exitCode == null) {
            openLiveJobLog(job, switchTo = true)
            return
        }
        val log = listOf(
            job.title,
            jobStatusText(job),
            job.progress,
            job.command,
            "",
            readJobLogText(job).ifBlank { job.output.joinToString("\n") },
        ).joinToString("\n").trimEnd()
        val terminal = terminalLogPane()
        toolTabs += ToolTab(
            job.group,
            getString(R.string.terminal_job_log_fmt, job.title),
            ToolKind.Terminal,
            terminal.view,
            key = job.toolKey,
        )
        terminal.write(log + "\r\n")
        switchTool(toolTabs.lastIndex)
    }

    private fun openLiveJobLog(job: DockerJob, switchTo: Boolean) {
        val existing = toolTabs.indexOfFirst { it.key == job.toolKey }
        if (existing >= 0) {
            updateLiveJobView(job)
            if (switchTo) switchTool(existing)
            return
        }
        val header = TextView(this).apply {
            textSize = 13f
            typeface = Typeface.DEFAULT_BOLD
            setPadding(18, 14, 18, 4)
        }
        val progress = TextView(this).apply {
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setPadding(18, 0, 18, 6)
        }
        val services = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(14, 2, 14, 8)
        }
        val terminal = terminalLogPane()
        val view = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(header)
            addView(progress)
            addView(services)
            addView(terminal.view, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                0,
                1f,
            ))
        }
        liveJobViews[job.id] = LiveJobView(header, progress, services, terminal)
        toolTabs += ToolTab(
            job.group,
            getString(R.string.terminal_job_log_fmt, job.title),
            ToolKind.Terminal,
            view,
            key = job.toolKey,
        )
        val persistedLog = readJobLogText(job)
        if (persistedLog.isNotBlank()) {
            terminal.write(ensureJobTerminalPrelude(job, persistedLog))
        } else if (job.output.isNotEmpty()) {
            terminal.write(jobTerminalPrelude(job) + job.output.joinToString("\r\n") + "\r\n")
        } else {
            terminal.write(jobTerminalPrelude(job))
        }
        updateLiveJobView(job)
        if (switchTo) switchTool(toolTabs.lastIndex) else renderToolChrome()
    }

    private fun updateLiveJobView(job: DockerJob) {
        val live = liveJobViews[job.id] ?: return
        live.header.text = listOf(job.title, jobStatusText(job)).joinToString("  ")
        live.progress.text = listOf(jobProgressText(job), job.command)
            .filter { it.isNotBlank() }
            .joinToString("\n")
        live.services.removeAllViews()
        liveJobServiceLinks(job).forEach { (label, url) ->
            live.services.addView(Button(this).apply {
                text = label
                isAllCaps = false
                setOnClickListener { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
            })
        }
    }

    private fun updateLiveJobViews() {
        dockerJobs.forEach { updateLiveJobView(it) }
    }

    private fun tickRunningJobs() {
        if (dockerJobs.none { it.exitCode == null } && daemonOperations.isEmpty()) return
        updateLiveJobViews()
        if (currentTab in setOf(Tab.Overview, Tab.Compose, Tab.Dockerfiles, Tab.Images, Tab.Containers)) {
            scheduleJobRenderUpdate(0L)
        }
    }

    private fun liveJobServiceLinks(job: DockerJob): List<Pair<String, String>> {
        val composeDir = job.command
            .takeIf { it.startsWith("engine compose up:") }
            ?.removePrefix("engine compose up:")
            ?.trim()
            ?.takeIf { it.isNotBlank() }
            ?.let { File(it) }
            ?: return emptyList()
        return projectServiceUrls(parseComposeServices(composeDir))
    }

    private fun stopDockerJob(jobId: String) {
        val job = dockerJobs.firstOrNull { it.id == jobId } ?: return
        if (job.exitCode != null) return
        val index = toolTabs.indexOfFirst { it.key == job.toolKey }
        if (index >= 0) {
            toolTabs[index].bridge?.close()
        }
        job.exitCode = -129
        job.status = getString(R.string.job_stopped)
        job.endedAt = System.currentTimeMillis()
        dockerJobBuffers.remove(jobId)
        dockerJobPendingCarriageReturn.remove(jobId)
        job.output += "[pdocker] job stopped from UI"
        appendPersistentJobLog(job.id, "[pdocker] job stopped from UI\n")
        job.progress = getString(R.string.job_stopped)
        while (job.output.size > MAX_JOB_LINES) job.output.removeAt(0)
        saveDockerJobs()
        updateLiveJobView(job)
        renderContent()
    }

    private fun handleDockerJobOutput(jobId: String, bytes: ByteArray) {
        val chunk = bytes.toString(Charsets.UTF_8)
        ui.post {
            val job = dockerJobs.firstOrNull { it.id == jobId } ?: return@post
            val text = terminalDisplayText(chunk)
            appendLiveJobTerminal(jobId, text)
            appendPersistentJobLog(jobId, text)
            recordJobTerminalOutput(job, jobId, text)
            scheduleDockerJobsSave()
            scheduleJobRenderUpdate()
        }
    }

    private fun scheduleJobRenderUpdate(delayMs: Long = 500L) {
        if (jobRenderScheduled) return
        jobRenderScheduled = true
        ui.postDelayed({
            jobRenderScheduled = false
            updateLiveJobViews()
            if (currentTab in setOf(Tab.Overview, Tab.Compose, Tab.Dockerfiles, Tab.Images, Tab.Containers)) {
                renderContent()
            }
        }, delayMs)
    }

    private fun appendEngineJobOutput(jobId: String, line: String) {
        ui.post {
            val job = dockerJobs.firstOrNull { it.id == jobId } ?: return@post
            val text = terminalRecordText(line)
            val displayText = terminalDisplayText(text)
            appendLiveJobTerminal(jobId, displayText)
            appendPersistentJobLog(jobId, displayText)
            recordJobTerminalOutput(job, jobId, displayText)
            scheduleDockerJobsSave()
            scheduleJobRenderUpdate()
        }
    }

    private fun terminalRecordText(text: String): String =
        normalizeTerminalNewlines(if (text.endsWith("\n") || text.endsWith("\r")) text else "$text\r\n")

    private fun jobTerminalPrelude(job: DockerJob): String =
        terminalRecordText(
            listOf(
                "[pdocker] job=${job.title} group=${job.group}",
                "[pdocker] command=${job.command}",
                "",
            ).joinToString("\n")
        )

    private fun ensureJobTerminalPrelude(job: DockerJob, text: String): String =
        if ("[pdocker] command=" in text.take(2048) || "[pdocker command]" in text.take(2048)) text else jobTerminalPrelude(job) + text

    private fun normalizeTerminalNewlines(text: String): String {
        val out = StringBuilder(text.length + 8)
        var previous = '\u0000'
        text.forEach { ch ->
            if (ch == '\n' && previous != '\r') out.append('\r')
            out.append(ch)
            previous = ch
        }
        return out.toString()
    }

    private fun terminalDisplayText(text: String): String {
        val out = StringBuilder(text.length + 16)
        text.forEachIndexed { index, ch ->
            if (ch == '\r' && text.getOrNull(index + 1) != '\n') {
                out.append('\r').append("\u001B[2K")
            } else {
                out.append(ch)
            }
        }
        return out.toString()
    }

    private fun appendLiveJobTerminal(jobId: String, text: String) {
        liveJobViews[jobId]?.terminal?.write(text)
    }

    private fun recordJobTerminalOutput(job: DockerJob, jobId: String, text: String) {
        var current = dockerJobBuffers.getOrDefault(jobId, "")
        var index = 0
        if (dockerJobPendingCarriageReturn.remove(jobId) && text.firstOrNull() != '\n') {
            updateCurrentJobProgress(job, current)
            current = ""
        }
        while (index < text.length) {
            when (val ch = text[index]) {
                '\r' -> {
                    when {
                        index + 1 >= text.length -> dockerJobPendingCarriageReturn += jobId
                        text[index + 1] == '\n' -> Unit
                        else -> {
                            updateCurrentJobProgress(job, current)
                            current = ""
                        }
                    }
                }
                '\n' -> {
                    commitJobOutputLine(job, jobId, current)
                    current = ""
                }
                '\u0000' -> Unit
                else -> current += ch
            }
            index += 1
        }
        if (current.length > 4096) current = current.takeLast(4096)
        dockerJobBuffers[jobId] = current
        updateCurrentJobProgress(job, current)
    }

    private fun updateCurrentJobProgress(job: DockerJob, rawLine: String) {
        val line = cleanTerminalLine(rawLine)
        if (line.isNotBlank()) updateDockerJobProgress(job, line)
    }

    private fun commitJobOutputLine(job: DockerJob, jobId: String, rawLine: String) {
        val line = cleanTerminalLine(rawLine)
        if (line.isBlank()) return
        val marker = Regex("__PDOCKER_JOB_EXIT:${Regex.escape(jobId)}:(\\d+)__").find(line)
        if (marker != null) {
            job.exitCode = marker.groupValues[1].toIntOrNull()
            job.status = if (job.exitCode == 0) getString(R.string.job_done) else getString(R.string.job_failed)
            job.progress = if (job.exitCode == 0) getString(R.string.job_done) else getString(R.string.job_failed)
            job.endedAt = System.currentTimeMillis()
            dockerJobBuffers.remove(jobId)
            dockerJobPendingCarriageReturn.remove(jobId)
            return
        }
        updateDockerJobProgress(job, line)
        if (job.output.lastOrNull() != line) job.output += line
        while (job.output.size > MAX_JOB_LINES) job.output.removeAt(0)
    }

    private fun cleanTerminalLine(rawLine: String): String =
        ansiControlRegex.replace(rawLine, "")
            .filter { it >= ' ' || it == '\t' }
            .trim()

    private fun finishEngineJob(jobId: String, exitCode: Int, output: String) {
        val job = dockerJobs.firstOrNull { it.id == jobId } ?: return
        job.exitCode = exitCode
        job.status = if (exitCode == 0) getString(R.string.job_done) else getString(R.string.job_failed)
        job.progress = if (exitCode == 0) getString(R.string.job_done) else getString(R.string.job_failed)
        job.endedAt = System.currentTimeMillis()
        val existing = job.output.toMutableSet()
        val terminalBackfill = mutableListOf<String>()
        output.lineSequence()
            .map { cleanTerminalLine(it) }
            .filter { it.isNotBlank() }
            .forEach { line ->
                if (job.output.lastOrNull() != line) {
                    job.output += line
                    if (existing.add(line)) terminalBackfill += line
                }
                while (job.output.size > MAX_JOB_LINES) job.output.removeAt(0)
            }
        if (terminalBackfill.isNotEmpty()) {
            val text = terminalRecordText(terminalBackfill.joinToString("\n"))
            appendLiveJobTerminal(jobId, text)
            appendPersistentJobLog(jobId, text)
        }
        saveDockerJobs()
        updateLiveJobView(job)
    }

    private fun handleEngineJobFinished(job: DockerJob) {
        updateLiveJobView(job)
        if (job.exitCode != 0 || !job.command.startsWith("engine compose up:")) return
        val urls = liveJobServiceLinks(job)
        if (urls.isEmpty()) return
        appendEngineJobOutput(job.id, urls.joinToString("\n") { (label, url) -> "Open $label $url" })
        val autoOpen = liveJobAutoOpenService(job) ?: return
        openServiceWhenReady(job.id, autoOpen.first, autoOpen.second)
    }

    private fun liveJobAutoOpenService(job: DockerJob): Pair<String, String>? {
        val composeDir = job.command
            .takeIf { it.startsWith("engine compose up:") }
            ?.removePrefix("engine compose up:")
            ?.trim()
            ?.takeIf { it.isNotBlank() }
            ?.let { File(it) }
            ?: return null
        return parseComposeServices(composeDir)
            .asSequence()
            .mapNotNull { composeServiceAutoOpenUrl(it) }
            .firstOrNull()
    }

    private fun openServiceWhenReady(jobId: String, label: String, url: String) {
        thread(isDaemon = true, name = "pdocker-service-open") {
            repeat(45) { attempt ->
                val result = probeServiceUrl(url)
                if (result.startsWith("HTTP ")) {
                    ui.post {
                        serviceHealth[url] = result
                        serviceHealthCheckedAt[url] = System.currentTimeMillis()
                        appendEngineJobOutput(jobId, "$label ready: $url ($result)")
                        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                    }
                    return@thread
                }
                if (attempt == 0 || attempt % 5 == 4) {
                    ui.post {
                        serviceHealth[url] = result
                        serviceHealthCheckedAt[url] = System.currentTimeMillis()
                        appendEngineJobOutput(jobId, "$label waiting: $url ($result)")
                    }
                }
                Thread.sleep(1000)
            }
            ui.post { appendEngineJobOutput(jobId, "$label not reachable yet: $url") }
        }
    }

    private fun updateDockerJobProgress(job: DockerJob, line: String) {
        val progress = dockerJobProgressLine(line) ?: return
        job.progress = progress.take(180)
    }

    private fun dockerJobProgressLine(line: String): String? {
        val cleaned = line
            .removePrefix("[+] ")
            .replace(Regex("\\s+"), " ")
            .trim()
        if (cleaned.isBlank()) return null
        val buildPrefixes = listOf(
            "Step:",
            "snapshotting layer:",
            "Successfully built",
            "Successfully tagged",
            "ERROR:",
        )
        if (buildPrefixes.any { cleaned.startsWith(it) }) return cleaned
        val pullPrefixes = listOf("Pulling ", "Status: Downloaded", "Downloaded newer image", "Image is up to date")
        if (pullPrefixes.any { cleaned.startsWith(it) }) return cleaned
        val composeWords = listOf(
            "Building",
            "Pulling",
            "Creating",
            "Created",
            "Starting",
            "Started",
            "Running",
            "Recreating",
            "Removing",
            "Removed",
        )
        if (composeWords.any { Regex("(^|\\s)$it($|\\s)").containsMatchIn(cleaned) }) return cleaned
        return null
    }

    private fun trimDockerJobs() {
        while (dockerJobs.size > MAX_JOB_HISTORY) {
            val removed = dockerJobs.removeAt(dockerJobs.lastIndex)
            if (removed.exitCode != null) runCatching { jobLogFile(removed.id).delete() }
        }
    }

    private fun jobLogFile(jobId: String): File =
        File(pdockerHome, "logs/jobs/$jobId.log")

    private fun appendPersistentJobLog(jobId: String, text: String) {
        if (text.isEmpty()) return
        val bytes = text.toByteArray(Charsets.UTF_8)
        logIo.execute {
            runCatching {
                val file = jobLogFile(jobId)
                file.parentFile?.mkdirs()
                FileOutputStream(file, true).use { it.write(bytes) }
            }
        }
    }

    private fun readJobLogText(job: DockerJob): String {
        val file = jobLogFile(job.id)
        if (!file.isFile) return ""
        return runCatching {
            val bytes = file.readBytes()
            val start = (bytes.size - MAX_JOB_LOG_VIEW_BYTES).coerceAtLeast(0)
            val prefix = if (start > 0) "[pdocker] log truncated to last ${MAX_JOB_LOG_VIEW_BYTES / 1024} KiB\n" else ""
            prefix + bytes.copyOfRange(start, bytes.size).toString(Charsets.UTF_8)
        }.getOrDefault("")
    }

    private fun scheduleDockerJobsSave() {
        dockerJobsDirty = true
        if (dockerJobsSaveScheduled) return
        dockerJobsSaveScheduled = true
        ui.postDelayed({
            dockerJobsSaveScheduled = false
            flushDockerJobsSave()
        }, 1500L)
    }

    private fun flushDockerJobsSave() {
        if (!dockerJobsDirty) return
        dockerJobsDirty = false
        saveDockerJobs()
    }

    private fun markInterruptedJob(job: DockerJob) {
        job.exitCode = -130
        job.status = getString(R.string.job_interrupted)
        job.endedAt = System.currentTimeMillis()
        job.progress = getString(R.string.job_interrupted)
        job.output += "[pdocker] job terminal was not restored after app restart; open logs or retry"
        while (job.output.size > MAX_JOB_LINES) job.output.removeAt(0)
    }

    private fun loadDockerJobs() {
        val file = File(pdockerHome, "jobs.json")
        val arr = runCatching { JSONArray(file.readText()) }.getOrNull() ?: return
        dockerJobs.clear()
        var migrated = false
        for (i in 0 until arr.length()) {
            val obj = arr.optJSONObject(i) ?: continue
            val lines = obj.optJSONArray("output") ?: JSONArray()
            val command = normalizeDockerCommand(obj.optString("command"))
            val job = DockerJob(
                id = obj.optString("id"),
                title = obj.optString("title"),
                detail = obj.optString("detail"),
                command = command,
                group = obj.optString("group"),
                toolKey = obj.optString("toolKey"),
                status = obj.optString("status"),
                exitCode = if (obj.has("exitCode") && !obj.isNull("exitCode")) obj.optInt("exitCode") else null,
                startedAt = obj.optLong("startedAt", System.currentTimeMillis()),
                endedAt = if (obj.has("endedAt") && !obj.isNull("endedAt")) obj.optLong("endedAt") else null,
                progress = obj.optString("progress"),
                output = (0 until lines.length()).mapNotNull { j ->
                    lines.optString(j).takeIf { it.isNotBlank() }
                }.toMutableList(),
            )
            if (job.exitCode == null) {
                markInterruptedJob(job)
                migrated = true
            }
            dockerJobs += job
        }
        trimDockerJobs()
        if (migrated) saveDockerJobs()
    }

    private fun saveDockerJobs() {
        val arr = JSONArray()
        dockerJobs.forEach { job ->
            arr.put(JSONObject().apply {
                put("id", job.id)
                put("title", job.title)
                put("detail", job.detail)
                put("command", job.command)
                put("group", job.group)
                put("toolKey", job.toolKey)
                put("status", job.status)
                if (job.exitCode == null) {
                    put("exitCode", JSONObject.NULL)
                } else {
                    put("exitCode", job.exitCode)
                }
                put("startedAt", job.startedAt)
                if (job.endedAt == null) {
                    put("endedAt", JSONObject.NULL)
                } else {
                    put("endedAt", job.endedAt)
                }
                put("progress", job.progress)
                put("output", JSONArray().apply { job.output.forEach { put(it) } })
            })
        }
        File(pdockerHome, "jobs.json").apply {
            parentFile?.mkdirs()
            writeText(arr.toString(2))
        }
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

    private fun addWidget(title: String, value: String, detail: String, detailLines: Int = 3, onClick: (() -> Unit)? = null) {
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
                maxLines = detailLines
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

    private fun renderProjectDashboard() {
        val projects = projectSummaries()
        if (projects.isEmpty()) return
        addSection(getString(R.string.section_project_dashboard))
        projects.take(8).forEach { project ->
            val serviceText = project.services
                .map { it.name }
                .distinct()
                .take(4)
                .joinToString(", ")
                .ifBlank { "-" }
            val detail = listOf(
                getString(
                    R.string.project_dashboard_counts_fmt,
                    project.compose.size,
                    project.dockerfiles.size,
                    project.editable.size,
                    project.containerCount,
                ),
                getString(R.string.project_dashboard_services_fmt, serviceText),
                getString(R.string.project_dashboard_dependencies_fmt, projectDependencySummary(project.services)),
                getString(R.string.project_dashboard_health_fmt, projectHealthSummary(project.services)),
                getString(R.string.project_dashboard_urls_fmt, project.serviceUrls.joinToString(", ") { it.first }.ifBlank { "-" }),
                getString(R.string.project_dashboard_service_health_fmt, project.serviceHealth),
                getString(R.string.project_dashboard_models_fmt, project.modelSummary),
                getString(R.string.project_dashboard_gpu_fmt, project.gpuProfileSummary),
                getString(R.string.project_dashboard_jobs_fmt, project.jobSummary),
            ).joinToString("\n")
            addWidget(project.dir.name, getString(R.string.section_project_dashboard), detail, detailLines = 9) {
                openProjectPrimaryFile(project)
            }
            project.compose.take(2).forEach { file ->
                addAction(
                    getString(R.string.action_open_project_compose_fmt, project.dir.name),
                    relativeProjectPath(project.dir, file),
                ) { openEditor(file) }
            }
            project.dockerfiles.take(2).forEach { file ->
                addAction(
                    getString(R.string.action_open_project_dockerfile_fmt, project.dir.name),
                    relativeProjectPath(project.dir, file),
                ) { openEditor(file) }
            }
            if (project.compose.isNotEmpty()) {
                addAction(getString(R.string.action_up_fmt, project.dir.name), getString(R.string.detail_compose_up)) {
                    runComposeUp(project.dir, getString(R.string.terminal_compose_up_fmt, project.dir.name))
                }
            }
            if (project.dockerfiles.isNotEmpty()) {
                addAction(getString(R.string.action_build_fmt, project.dir.name), project.dir.absolutePath) {
                    runImageBuild(project.dir, getString(R.string.terminal_docker_build_fmt, project.dir.name))
                }
            }
            project.serviceUrls.forEach { (label, url) ->
                addAction(getString(R.string.action_open_service_fmt, label), url) {
                    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
                }
            }
            project.gpuDiagnostics?.takeIf { it.isFile }?.let { file ->
                addAction(getString(R.string.action_open_gpu_diagnostics), relativeProjectPath(project.dir, file)) {
                    openEditor(file)
                }
            }
            project.editable
                .filterNot { it in project.compose || it in project.dockerfiles }
                .take(4)
                .forEach { file ->
                    addAction(
                        getString(R.string.action_open_project_file_fmt, relativeProjectPath(project.dir, file)),
                        getString(R.string.detail_project_file),
                    ) { openEditor(file) }
                }
        }
    }

    private fun projectSummaries(): List<ProjectSummary> =
        projectDirs().map { dir ->
            val files = dir.walkSafe().filter { it.isFile }
            val compose = files
                .filter { it.name in setOf("compose.yaml", "compose.yml", "docker-compose.yaml", "docker-compose.yml") }
                .sortedBy { it.absolutePath }
            val dockerfiles = files
                .filter { it.name == "Dockerfile" }
                .sortedBy { it.absolutePath }
            val editable = files
                .filter { it.length() <= MAX_INLINE_EDIT_BYTES && isProjectTextFile(it) }
                .sortedWith(compareBy<File> { projectFileRank(it) }.thenBy { it.absolutePath })
            val services = compose
                .mapNotNull { it.parentFile }
                .distinctBy { it.absolutePath }
                .flatMap { parseComposeServices(it) }
            val serviceUrls = projectServiceUrls(services)
            ProjectSummary(
                dir = dir,
                compose = compose,
                dockerfiles = dockerfiles,
                editable = editable,
                services = services,
                serviceUrls = serviceUrls,
                serviceHealth = projectServiceHealthSummary(serviceUrls),
                modelSummary = projectModelSummary(dir),
                gpuProfileSummary = projectGpuProfileSummary(dir),
                gpuDiagnostics = File(dir, "profiles/pdocker-gpu-diagnostics.json").takeIf { it.isFile },
                containerCount = projectContainerCount(dir.name),
                jobSummary = projectJobSummary(dir.name),
            )
        }.sortedWith(compareBy<ProjectSummary> {
            if (it.compose.isNotEmpty() || it.dockerfiles.isNotEmpty()) 0 else 1
        }.thenBy { it.dir.name })

    private fun projectDirs(): List<File> =
        projectRoot.listFiles()
            ?.filter { it.isDirectory && it.name !in setOf(".git", "node_modules") }
            ?.sortedBy { it.name }
            .orEmpty()

    private fun openProjectPrimaryFile(project: ProjectSummary) {
        val target = project.compose.firstOrNull()
            ?: project.dockerfiles.firstOrNull()
            ?: project.editable.firstOrNull()
        if (target != null) {
            openEditor(target)
        }
    }

    private fun relativeProjectPath(project: File, file: File): String =
        runCatching { project.toPath().relativize(file.toPath()).toString() }.getOrDefault(file.name)

    private fun isProjectTextFile(file: File): Boolean {
        val name = file.name
        val ext = name.substringAfterLast('.', "")
        return name in setOf("Dockerfile", "compose.yaml", "compose.yml", "docker-compose.yaml", "docker-compose.yml", "README.md") ||
            ext in setOf("yaml", "yml", "json", "sh", "env", "md", "txt", "toml", "conf", "properties", "gradle", "kt", "py", "js", "ts", "css", "html")
    }

    private fun projectFileRank(file: File): Int = when (file.name) {
        "compose.yaml", "compose.yml", "docker-compose.yaml", "docker-compose.yml" -> 0
        "Dockerfile" -> 1
        "README.md" -> 2
        else -> 3
    }

    private fun projectContainerCount(projectName: String): Int =
        containerDirs().count { dir ->
            val state = readState(dir)
            val name = state?.optString("Name")?.trim('/')?.ifBlank { dir.name } ?: dir.name
            name == projectName || name.startsWith("$projectName-")
        }

    private fun projectJobSummary(projectName: String): String {
        val jobs = dockerJobs.filter { it.group == projectName || projectName in it.command }
        if (jobs.isEmpty()) return "-"
        return jobs.groupingBy { it.status }.eachCount()
            .entries
            .joinToString(", ") { "${it.key}:${it.value}" }
    }

    private fun projectDependencySummary(services: List<ComposeService>): String {
        val edges = services.flatMap { service ->
            service.dependsOn.distinct().map { dep -> "${service.name} -> $dep" }
        }
        return edges.take(4).joinToString(", ").ifBlank { "-" }
    }

    private fun projectHealthSummary(services: List<ComposeService>): String {
        val health = services.filter { it.hasHealthcheck }.map { it.name }.distinct()
        return health.take(4).joinToString(", ").ifBlank { "-" }
    }

    private fun projectServiceUrls(services: List<ComposeService>): List<Pair<String, String>> =
        services.flatMap(::composeServiceUrls).distinctBy { it.second }

    private fun composeServiceUrls(service: ComposeService): List<Pair<String, String>> {
        val urls = mutableListOf<Pair<String, String>>()
        service.ports
            .mapNotNull { port -> composeHostPort(port) }
            .distinct()
            .forEach { hostPort ->
                val link = service.serviceLinks.firstOrNull { it.port == hostPort }
                val label = link?.label?.takeIf { it.isNotBlank() } ?: "${service.name}:$hostPort"
                val url = link?.url?.takeIf { it.isNotBlank() } ?: "http://127.0.0.1:$hostPort/"
                urls += label to url
            }
        service.serviceLinks
            .filter { it.port == null && !it.url.isNullOrBlank() }
            .forEach { link -> urls += link.label to link.url.orEmpty() }
        return urls.distinctBy { it.second }
    }

    private fun composeServiceAutoOpenUrl(service: ComposeService): Pair<String, String>? {
        service.ports
            .mapNotNull { port -> composeHostPort(port) }
            .distinct()
            .forEach { hostPort ->
                val link = service.serviceLinks.firstOrNull { it.port == hostPort && it.autoOpen }
                if (link != null) {
                    val url = link.url?.takeIf { it.isNotBlank() } ?: "http://127.0.0.1:$hostPort/"
                    return link.label to url
                }
            }
        return service.serviceLinks
            .firstOrNull { it.autoOpen && it.port == null && !it.url.isNullOrBlank() }
            ?.let { it.label to it.url.orEmpty() }
    }

    private fun projectServiceHealthSummary(urls: List<Pair<String, String>>): String {
        if (urls.isEmpty()) return "-"
        urls.forEach { (_, url) -> scheduleServiceHealthProbe(url) }
        return urls.take(4).joinToString(", ") { (label, url) ->
            "$label:${serviceHealth[url] ?: getString(R.string.service_health_checking)}"
        }
    }

    private fun scheduleServiceHealthProbe(url: String) {
        if (url in serviceHealthInFlight) return
        val checkedAt = serviceHealthCheckedAt[url] ?: 0L
        if (url in serviceHealth && System.currentTimeMillis() - checkedAt < 15_000L) return
        serviceHealthInFlight += url
        thread(isDaemon = true, name = "pdocker-service-probe") {
            val result = probeServiceUrl(url)
            ui.post {
                serviceHealth[url] = result
                serviceHealthCheckedAt[url] = System.currentTimeMillis()
                serviceHealthInFlight -= url
                if (currentTab == Tab.Overview) renderContent()
            }
        }
    }

    private fun probeServiceUrl(url: String): String =
        runCatching {
            val conn = (URL(url).openConnection() as HttpURLConnection).apply {
                connectTimeout = 900
                readTimeout = 900
                requestMethod = "GET"
            }
            try {
                val code = conn.responseCode
                if (code in 200..399) "HTTP $code" else "HTTP $code"
            } finally {
                conn.disconnect()
            }
        }.getOrElse { err ->
            val reason = err.message?.take(32)?.ifBlank { err::class.java.simpleName } ?: err::class.java.simpleName
            "down $reason"
        }

    private fun composeHostPort(port: String): Int? {
        val cleaned = port.trim().trim('"', '\'')
        val withoutProtocol = cleaned.substringBefore('/')
        val parts = withoutProtocol.split(':').filter { it.isNotBlank() }
        return parts.firstOrNull { it.toIntOrNull() in 1..65535 }?.toIntOrNull()
    }

    private fun projectModelSummary(project: File): String {
        val modelDir = File(project, "models")
        val models = modelDir.walkSafe()
            .filter { it.isFile && it.name.endsWith(".gguf", ignoreCase = true) }
        val partials = modelDir.walkSafe()
            .filter { it.isFile && it.name.endsWith(".gguf.part", ignoreCase = true) }
        return when {
            models.isNotEmpty() -> "${models.size} GGUF / ${formatBytes(models.sumOf { it.length() })}"
            partials.isNotEmpty() -> "partial ${partials.size} / ${formatBytes(partials.sumOf { it.length() })}"
            else -> "-"
        }
    }

    private fun projectGpuProfileSummary(project: File): String {
        val diagnostics = File(project, "profiles/pdocker-gpu-diagnostics.json")
        if (diagnostics.isFile) {
            val obj = runCatching { JSONObject(diagnostics.readText()) }.getOrNull()
            if (obj != null) {
                val backend = obj.optString("backend", "unknown").ifBlank { "unknown" }
                val reason = obj.optString("reason", "").ifBlank { "diagnostics ready" }
                return "$backend: $reason"
            }
            return getString(R.string.project_dashboard_gpu_invalid)
        }
        val env = File(project, "profiles/pdocker-gpu.env")
        if (env.isFile) {
            val backend = env.readLines()
                .firstOrNull { it.startsWith("LLAMA_GPU_BACKEND=") }
                ?.substringAfter("=")
                ?.ifBlank { "unknown" }
                ?: "unknown"
            return "$backend: env profile"
        }
        return "-"
    }

    private fun formatBytes(bytes: Long): String {
        val units = arrayOf("B", "KiB", "MiB", "GiB")
        var value = bytes.toDouble()
        var unit = 0
        while (value >= 1024.0 && unit < units.lastIndex) {
            value /= 1024.0
            unit += 1
        }
        return if (unit == 0) "${bytes} ${units[unit]}" else String.format("%.1f %s", value, units[unit])
    }

    private fun renderProjectFileShortcuts() {
        val files = projectRoot.walkSafe()
            .filter { it.isFile && it.length() <= MAX_INLINE_EDIT_BYTES }
            .sortedByDescending { it.lastModified() }
            .take(8)
        if (files.isEmpty()) return
        addSection(getString(R.string.section_project_files))
        files.forEach { file ->
            addWidget(editorTitle(file), getString(R.string.detail_project_file), file.absolutePath) {
                openEditor(file)
            }
        }
    }

    private fun projectTemplates(): List<ProjectTemplate> =
        runCatching {
            val root = JSONObject(assets.open("project-library/library.json").bufferedReader().use { it.readText() })
            val arr = root.optJSONArray("templates") ?: JSONArray()
            val libraryVersion = root.optInt("version", 1)
            (0 until arr.length()).mapNotNull { i ->
                arr.optJSONObject(i)?.let { obj ->
                    val features = obj.optJSONArray("features") ?: JSONArray()
                    ProjectTemplate(
                        id = obj.optString("id"),
                        name = obj.optString("name"),
                        category = obj.optString("category"),
                        description = obj.optString("description"),
                        assetPath = obj.optString("assetPath"),
                        projectDir = obj.optString("projectDir"),
                        compose = obj.optString("compose", "compose.yaml"),
                        dockerfile = obj.optString("dockerfile", "Dockerfile"),
                        gpu = obj.optString("gpu", "none"),
                        version = obj.optInt("version", libraryVersion),
                        features = (0 until features.length()).mapNotNull { j -> features.optString(j).takeIf { it.isNotBlank() } },
                    )
                }
            }.filter { it.id.isNotBlank() && it.assetPath.isNotBlank() && it.projectDir.isNotBlank() }
        }.getOrElse {
            status.text = getString(R.string.status_library_failed, it.message.orEmpty())
            emptyList()
        }

    private fun installTemplate(template: ProjectTemplate) {
        val target = File(projectRoot, template.projectDir)
        val report = copyAssetTreeMissing(template.assetPath, target)
        File(target, ".pdocker-template-id").writeText(template.id + "\n")
        File(target, ".pdocker-template-version").writeText(template.version.toString() + "\n")
        migrateProjectPorts(target)
        if (template.id == "dev-workspace") migrateDefaultDevWorkspace(target)
        status.text = getString(
            R.string.status_library_install_report_fmt,
            template.name,
            report.copied,
            report.kept,
            template.version,
        )
    }

    private fun copyAssetTreeMissing(assetPath: String, dest: File): TemplateInstallReport {
        val report = TemplateInstallReport()
        fun copyNode(src: String, target: File) {
            val children = assets.list(src).orEmpty()
            if (children.isEmpty()) {
                if (target.exists()) {
                    report.kept += 1
                    return
                }
                target.parentFile?.mkdirs()
                assets.open(src).use { input ->
                    target.outputStream().use { output -> input.copyTo(output) }
                }
                report.copied += 1
                return
            }
            target.mkdirs()
            children.forEach { child -> copyNode("$src/$child", File(target, child)) }
        }
        copyNode(assetPath.trim('/'), dest)
        return report
    }

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
        containerWarningSummary(state, pdockerNetwork).takeIf { it.isNotBlank() }?.let {
            lines += it
        }
        return lines.joinToString("\n")
    }

    private fun containerWarningSummary(state: JSONObject?, pdockerNetwork: JSONObject?): String {
        val warnings = mutableListOf<String>()
        fun appendWarnings(arr: JSONArray?) {
            if (arr == null) return
            for (i in 0 until arr.length()) {
                arr.optString(i).takeIf { it.isNotBlank() }?.let { warnings += it }
            }
        }
        appendWarnings(state?.optJSONArray("Warnings"))
        appendWarnings(pdockerNetwork?.optJSONArray("Warnings"))
        val unique = warnings.distinct()
        if (unique.isEmpty()) return ""
        val text = unique.joinToString(" / ") { warning ->
            when {
                "not active yet" in warning -> getString(R.string.container_warning_ports_metadata)
                "host-network stub" in warning -> getString(R.string.container_warning_network_stub)
                else -> warning
            }
        }
        return getString(R.string.container_warnings_fmt, text)
    }

    private fun containerServiceUrls(state: JSONObject?): List<Pair<String, String>> {
        val dockerNetwork = state?.optJSONObject("NetworkSettings")
        val pdockerNetwork = state?.optJSONObject("PdockerNetwork")
        val ports = pdockerNetwork?.optJSONObject("Ports")
            ?: dockerNetwork?.optJSONObject("Ports")
            ?: return emptyList()
        val labels = containerServiceLabels(state)
        val urls = mutableListOf<Pair<String, String>>()
        val iter = ports.keys()
        while (iter.hasNext()) {
            val key = iter.next()
            val value = ports.opt(key)
            val port = _splitPortKey(key)
            val label = port?.let { labels[it] } ?: key
            if (value is JSONArray && value.length() > 0) {
                for (i in 0 until value.length()) {
                    val binding = value.optJSONObject(i) ?: continue
                    val host = browserHost(binding.optString("HostIp"))
                    val hostPort = binding.optString("HostPort")
                    if (hostPort.isBlank()) continue
                    urls += label to "http://$host:$hostPort/"
                }
            } else {
                _splitPortKey(key)?.let { exposedPort ->
                    urls += label to "http://127.0.0.1:$exposedPort/"
                }
            }
        }
        containerExplicitServiceUrls(state).forEach { urls += it }
        return urls.distinctBy { it.second }
    }

    private fun containerServiceLabels(state: JSONObject?): Map<Int, String> {
        val labels = state?.optJSONObject("Labels") ?: return emptyMap()
        val out = mutableMapOf<Int, String>()
        val iter = labels.keys()
        while (iter.hasNext()) {
            val key = iter.next()
            if (!key.startsWith(PDOCKER_SERVICE_URL_LABEL_PREFIX)) continue
            val suffix = key.removePrefix(PDOCKER_SERVICE_URL_LABEL_PREFIX)
            val port = suffix.toIntOrNull() ?: continue
            val label = labels.optString(key).takeIf { it.isNotBlank() } ?: continue
            out[port] = label
        }
        return out
    }

    private fun containerExplicitServiceUrls(state: JSONObject?): List<Pair<String, String>> {
        val labels = state?.optJSONObject("Labels") ?: return emptyList()
        val urls = mutableListOf<Pair<String, String>>()
        val iter = labels.keys()
        while (iter.hasNext()) {
            val key = iter.next()
            if (!key.startsWith(PDOCKER_SERVICE_URL_LABEL_PREFIX)) continue
            if (key.removePrefix(PDOCKER_SERVICE_URL_LABEL_PREFIX).toIntOrNull() != null) continue
            val raw = labels.optString(key)
            val label = raw.substringBefore('=', "").trim()
            val url = raw.substringAfter('=', "").trim()
            if (label.isNotBlank() && (url.startsWith("http://") || url.startsWith("https://"))) {
                urls += label to url
            }
        }
        return urls
    }

    private fun browserHost(host: String): String =
        if (host.isBlank() || host == "0.0.0.0" || host == "::") "127.0.0.1" else host

    private fun _splitPortKey(key: String): Int? =
        key.substringBefore('/').toIntOrNull()?.takeIf { it in 1..65535 }

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
        val target = File(projectRoot, "default")
        if (!stamp.exists()) {
            copyAssetTree("default-project", target)
        }
        migrateProjectPorts(target)
        migrateDefaultDevWorkspace(target)
        stamp.parentFile?.mkdirs()
        stamp.writeText("3\n")
    }

    private fun migrateDefaultDevWorkspace(project: File) {
        val dockerfile = File(project, "Dockerfile")
        if (dockerfile.isFile) {
            var text = dockerfile.readText()
            if (!text.contains("CLAUDE_CODE_NPM_PACKAGE")) {
                text = text.replace(
                    "ARG CODEX_NPM_PACKAGE=@openai/codex\n",
                    "ARG CODEX_NPM_PACKAGE=@openai/codex\nARG CLAUDE_CODE_NPM_PACKAGE=@anthropic-ai/claude-code\n",
                )
                text = text.replace(
                    "RUN npm install -g \"\$CODEX_NPM_PACKAGE\"",
                    "RUN npm install -g \"\$CODEX_NPM_PACKAGE\" \"\$CLAUDE_CODE_NPM_PACKAGE\"",
                )
            }
            if (!text.contains("Anthropic.claude-code")) {
                text = text.replace(
                    "RUN mkdir -p /workspace \"\$CODE_SERVER_USER_DATA_DIR/User\" \"\$CODE_SERVER_EXTENSIONS_DIR\" \\\n" +
                        "    && code-server --extensions-dir \"\$CODE_SERVER_EXTENSIONS_DIR\" --install-extension Continue.continue || true \\\n" +
                        "    && code-server --extensions-dir \"\$CODE_SERVER_EXTENSIONS_DIR\" --install-extension redhat.vscode-yaml || true \\\n" +
                        "    && code-server --extensions-dir \"\$CODE_SERVER_EXTENSIONS_DIR\" --install-extension ms-azuretools.vscode-docker || true",
                    "RUN mkdir -p /workspace \"\$CODE_SERVER_USER_DATA_DIR/User\" \"\$CODE_SERVER_EXTENSIONS_DIR\" \\\n" +
                        "    && for ext in Continue.continue OpenAI.chatgpt Anthropic.claude-code; do \\\n" +
                        "         code-server --extensions-dir \"\$CODE_SERVER_EXTENSIONS_DIR\" --install-extension \"\$ext\"; \\\n" +
                        "       done \\\n" +
                        "    && for ext in redhat.vscode-yaml ms-azuretools.vscode-docker; do \\\n" +
                        "         code-server --extensions-dir \"\$CODE_SERVER_EXTENSIONS_DIR\" --install-extension \"\$ext\" || true; \\\n" +
                        "       done",
                )
            }
            text = text.replace("openai.chatgpt", "OpenAI.chatgpt")
            dockerfile.writeText(text)
        }

        val compose = File(project, "compose.yaml")
        if (compose.isFile) {
            var text = compose.readText()
            if (!text.contains("CLAUDE_CODE_NPM_PACKAGE")) {
                text = text.replace(
                    "        CODEX_NPM_PACKAGE: \"@openai/codex\"\n",
                    "        CODEX_NPM_PACKAGE: \"@openai/codex\"\n        CLAUDE_CODE_NPM_PACKAGE: \"@anthropic-ai/claude-code\"\n",
                )
            }
            if (!text.contains("ANTHROPIC_API_KEY")) {
                text = text.replace(
                    "      OPENAI_API_KEY: \"\${OPENAI_API_KEY:-}\"\n",
                    "      OPENAI_API_KEY: \"\${OPENAI_API_KEY:-}\"\n      ANTHROPIC_API_KEY: \"\${ANTHROPIC_API_KEY:-}\"\n",
                )
            }
            compose.writeText(text)
        }

        val extensions = File(project, "workspace/.vscode/extensions.json")
        if (!extensions.exists()) {
            extensions.parentFile?.mkdirs()
            extensions.writeText(
                """
                {
                  "recommendations": [
                    "Continue.continue",
                    "OpenAI.chatgpt",
                    "Anthropic.claude-code",
                    "redhat.vscode-yaml",
                    "ms-azuretools.vscode-docker"
                  ]
                }
                """.trimIndent() + "\n",
            )
        } else {
            val original = extensions.readText()
            val migrated = original.replace("openai.chatgpt", "OpenAI.chatgpt")
            if (migrated != original) extensions.writeText(migrated)
        }

        val tasks = File(project, "workspace/.vscode/tasks.json")
        if (!tasks.exists()) {
            tasks.parentFile?.mkdirs()
            tasks.writeText(
                """
                {
                  "version": "2.0.0",
                  "tasks": [
                    {
                      "label": "Codex: start",
                      "type": "shell",
                      "command": "codex",
                      "options": {
                        "cwd": "/workspace"
                      },
                      "problemMatcher": [],
                      "presentation": {
                        "reveal": "always",
                        "panel": "new",
                        "focus": true
                      }
                    },
                    {
                      "label": "Codex: version",
                      "type": "shell",
                      "command": "codex --version",
                      "options": {
                        "cwd": "/workspace"
                      },
                      "problemMatcher": [],
                      "presentation": {
                        "reveal": "always",
                        "panel": "shared"
                      }
                    }
                  ]
                }
                """.trimIndent() + "\n",
            )
        }
    }

    private fun migrateProjectPorts(project: File) {
        val replacements = mapOf(
            "0.0.0.0:8080" to "0.0.0.0:18080",
            "8080:8080" to "18080:18080",
            "CODE_SERVER_PORT:-8080" to "CODE_SERVER_PORT:-18080",
            "0.0.0.0:8081" to "0.0.0.0:18081",
            "8081:8081" to "18081:18081",
            "LLAMA_ARG_PORT:-8081" to "LLAMA_ARG_PORT:-18081",
        )
        project.walkSafe()
            .filter { it.isFile && it.length() <= 512 * 1024 }
            .forEach { file ->
                val original = runCatching { file.readText() }.getOrNull() ?: return@forEach
                val migrated = replacements.entries.fold(original) { text, (from, to) ->
                    text.replace(from, to)
                }.let { text -> migrateComposeHeaderServiceLinks(file, text) }
                if (migrated != original) file.writeText(migrated)
            }
    }

    private fun migrateComposeHeaderServiceLinks(file: File, text: String): String {
        if (file.name !in setOf("compose.yaml", "compose.yml", "docker-compose.yaml", "docker-compose.yml")) {
            return text
        }
        val additions = mutableListOf<String>()
        if ("18080:18080" in text && "pdocker.service-url: 18080=" !in text) {
            additions += "# pdocker.service-url: 18080=VS Code"
        }
        if ("18080:18080" in text && "pdocker.auto-open: VS Code" !in text) {
            additions += "# pdocker.auto-open: VS Code"
        }
        if ("18081:18081" in text && "pdocker.service-url: 18081=" !in text) {
            additions += "# pdocker.service-url: 18081=llama.cpp"
        }
        if (additions.isEmpty()) return text
        return additions.joinToString("\n", postfix = "\n") + text
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
