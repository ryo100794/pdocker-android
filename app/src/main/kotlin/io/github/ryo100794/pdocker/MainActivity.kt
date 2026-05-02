package io.github.ryo100794.pdocker

import android.Manifest
import android.content.Intent
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
import android.text.TextUtils
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
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.io.File
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
        val features: List<String>,
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

    companion object {
        private const val REQUEST_POST_NOTIFICATIONS = 100
        private const val MAX_INLINE_EDIT_BYTES = 512 * 1024
        private const val MAX_JOB_HISTORY = 20
        private const val MAX_JOB_LINES = 200
        private const val ACTION_SMOKE_START = "io.github.ryo100794.pdocker.action.SMOKE_START"
    }

    private val ui = Handler(Looper.getMainLooper())
    private val tabs = listOf(Tab.Overview, Tab.Library, Tab.Compose, Tab.Dockerfiles, Tab.Images, Tab.Containers, Tab.Sessions)
    private val pollTask = object : Runnable {
        override fun run() {
            refreshStatus()
            ui.postDelayed(this, 3000)
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
    private var upperWeight = 0.56f
    private var lowerWeight = 0.44f
    private var splitDragStartY = 0f
    private var splitDragStartUpper = 0f

    private val pdockerHome: File by lazy { File(filesDir, "pdocker") }
    private val imageRoot: File by lazy { File(pdockerHome, "images") }
    private val containerRoot: File by lazy { File(pdockerHome, "containers") }
    private val projectRoot: File by lazy { File(pdockerHome, "projects") }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestNotificationPermission()
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
        lowerGroupRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        lowerTabRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        lowerHost = FrameLayout(this)

        upperPane.addView(status)
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
        handleAutomationIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleAutomationIntent(intent)
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

    override fun onDestroy() {
        toolTabs.forEach { it.bridge?.close() }
        toolTabs.clear()
        super.onDestroy()
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
                        "cd ${shellQuote(target.absolutePath)} && bash scripts/pdocker-gpu-profile.sh profiles/pdocker-gpu.env; printf '\\n'; cat profiles/pdocker-gpu.env; sh",
                    )
                }
            }
            addAction(getString(R.string.action_compose_up_template_fmt, template.name), getString(R.string.detail_compose_up)) {
                installTemplate(template)
                openDockerTerminal(
                    getString(R.string.terminal_compose_up_fmt, template.projectDir),
                    composeUpCommand(target),
                )
            }
        }
    }

    private fun renderCompose() {
        addSection(getString(R.string.section_compose))
        renderDockerJobs { it.command.contains("docker compose ") }
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
                openDockerTerminal(getString(R.string.terminal_compose_up_fmt, dir.name), composeUpCommand(dir))
            }
        }
    }

    private fun renderDockerfiles() {
        addSection(getString(R.string.section_dockerfile))
        renderDockerJobs { it.command.contains("docker build ") }
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
                openDockerTerminal(getString(R.string.terminal_docker_build_fmt, dir.name), dockerBuildCommand(dir))
            }
        }
    }

    private fun renderImages() {
        addSection(getString(R.string.section_images))
        addAction(getString(R.string.action_pull_image), getString(R.string.detail_pull_image)) {
            openDockerTerminal(getString(R.string.action_pull_image), "docker pull ubuntu:22.04")
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
        addAction(getString(R.string.action_docker_ps), getString(R.string.detail_docker_ps)) {
            openDockerTerminal(getString(R.string.terminal_docker_ps), "docker ps -a")
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
            val statusText = state?.optJSONObject("State")
                ?.optString("Status")
                ?.ifBlank { getString(R.string.unknown_status) }
                ?: getString(R.string.unknown_status)
            addWidget(name, statusText, "$image\n${containerNetworkSummary(state)}\n${containerLogPreview(dir)}") {
                openDockerInteractiveTerminal(
                    getString(R.string.terminal_container_fmt, name),
                    "docker logs --tail 80 ${dir.name}; printf '\\n# attach shell\\n'; docker exec -it ${dir.name} sh",
                    name,
                )
            }
            addAction(getString(R.string.action_container_start_fmt, name), dir.name) {
                openDockerTerminal(
                    getString(R.string.terminal_container_start_fmt, name),
                    "docker start ${shellQuote(dir.name)} && docker ps --filter id=${shellQuote(dir.name)}",
                    name,
                )
            }
            addAction(getString(R.string.action_container_stop_fmt, name), dir.name) {
                openDockerTerminal(
                    getString(R.string.terminal_container_stop_fmt, name),
                    "docker stop ${shellQuote(dir.name)} && docker ps -a --filter id=${shellQuote(dir.name)}",
                    name,
                )
            }
            addAction(getString(R.string.action_container_restart_fmt, name), dir.name) {
                openDockerTerminal(
                    getString(R.string.terminal_container_restart_fmt, name),
                    "docker stop ${shellQuote(dir.name)} || true; docker start ${shellQuote(dir.name)}; docker ps --filter id=${shellQuote(dir.name)}",
                    name,
                )
            }
            addAction(getString(R.string.action_container_logs_fmt, name), dir.name) {
                openDockerTerminal(
                    getString(R.string.terminal_container_logs_fmt, name),
                    "docker logs --tail 200 ${shellQuote(dir.name)}",
                    name,
                )
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

    private fun renderSessions() {
        addSection(getString(R.string.section_sessions))
        addAction(getString(R.string.action_docker_it), getString(R.string.detail_docker_it)) {
            openDockerTerminal(getString(R.string.terminal_docker_interactive), "docker ps -a; printf '\\nUse: docker exec -it <container> sh\\n'")
        }
        addAction(getString(R.string.action_compose_session), getString(R.string.detail_compose_session)) {
            projectRoot.mkdirs()
            openDockerTerminal(getString(R.string.action_compose_session), "cd ${shellQuote(projectRoot.absolutePath)} && docker compose ps")
        }
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
        addAction(getString(R.string.action_start_pdockerd), getString(R.string.detail_start_pdockerd)) { startDaemon() }
        addAction(getString(R.string.action_stop_pdockerd), getString(R.string.detail_stop_pdockerd)) {
            startService(Intent(this, PdockerdService::class.java).setAction(PdockerdService.ACTION_STOP))
            status.text = getString(R.string.status_stopped)
        }
        addAction(getString(R.string.action_keep_resident), getString(R.string.detail_keep_resident)) {
            requestBatteryOptimizationBypass()
        }
        addAction(getString(R.string.action_docker_console), getString(R.string.detail_docker_console)) {
            startDaemon()
            openDockerTerminal(getString(R.string.action_docker_console), "docker ps -a; printf '\\nUse docker commands for diagnostics.\\n'")
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

    private fun handleAutomationIntent(intent: Intent?) {
        if (intent?.action != ACTION_SMOKE_START) return
        val debuggable = (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
        if (!debuggable) return
        startDaemon()
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

    private fun openDockerInteractiveTerminal(title: String, command: String, group: String = workspaceGroup()) {
        startDaemon()
        val wrapped = dockerCommand("$command; status=\$?; printf '\\n[pdocker] container console exited: %s\\n' \"\$status\"; exit \"\$status\"")
        val launchCommand = terminalSessionCommand(title, group, wrapped)
        openTerminal(
            title,
            launchCommand,
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
        return listOf(
            "export DOCKER_CONFIG=\"\$HOME/pdocker-runtime/docker-bin\" DOCKER_BUILDKIT=0 COMPOSE_DOCKER_CLI_BUILD=0 BUILDKIT_PROGRESS=plain COMPOSE_PROGRESS=plain COMPOSE_MENU=false",
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
                    openDockerTerminal(job.title, job.command, job.group)
                }
            }
        }
    }

    private fun jobStatusText(job: DockerJob): String {
        val elapsed = ((job.endedAt ?: System.currentTimeMillis()) - job.startedAt).coerceAtLeast(0) / 1000
        return when {
            job.exitCode == null -> getString(R.string.job_status_running_fmt, elapsed)
            job.exitCode == 0 -> getString(R.string.job_status_done_fmt, elapsed)
            job.exitCode == -129 -> getString(R.string.job_status_stopped_fmt, elapsed)
            job.exitCode == -130 -> getString(R.string.job_status_interrupted_fmt, elapsed)
            else -> getString(R.string.job_status_failed_fmt, job.exitCode ?: -1, elapsed)
        }
    }

    private fun openJobLog(job: DockerJob) {
        val key = "job-log:${job.id}"
        val existing = toolTabs.indexOfFirst { it.key == key }
        if (existing >= 0) {
            switchTool(existing)
            return
        }
        val log = listOf(
            job.title,
            jobStatusText(job),
            job.progress,
            job.command,
            "",
            job.output.joinToString("\n"),
        ).joinToString("\n").trimEnd()
        val view = ScrollView(this).apply {
            addView(TextView(this@MainActivity).apply {
                text = log
                textSize = 12f
                typeface = Typeface.MONOSPACE
                setTextIsSelectable(true)
                setPadding(18, 18, 18, 18)
            })
        }
        toolTabs += ToolTab(
            job.group,
            getString(R.string.terminal_job_log_fmt, job.title),
            ToolKind.Editor,
            view,
            key = key,
        )
        switchTool(toolTabs.lastIndex)
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
        job.output += "[pdocker] job stopped from UI"
        job.progress = getString(R.string.job_stopped)
        while (job.output.size > MAX_JOB_LINES) job.output.removeAt(0)
        saveDockerJobs()
        renderContent()
    }

    private fun handleDockerJobOutput(jobId: String, bytes: ByteArray) {
        val chunk = bytes.toString(Charsets.UTF_8)
        ui.post {
            val job = dockerJobs.firstOrNull { it.id == jobId } ?: return@post
            val text = dockerJobBuffers.getOrDefault(jobId, "") + chunk
            val normalized = text.replace("\r", "")
            val complete = normalized.endsWith("\n")
            val rawLines = normalized.split("\n")
            val lines = if (complete) rawLines else rawLines.dropLast(1)
            dockerJobBuffers[jobId] = if (complete) "" else rawLines.last().takeLast(4096)
            lines.map { it.trim() }
                .filter { it.isNotBlank() }
                .forEach { line ->
                    val marker = Regex("__PDOCKER_JOB_EXIT:${Regex.escape(jobId)}:(\\d+)__").find(line)
                    if (marker != null) {
                        job.exitCode = marker.groupValues[1].toIntOrNull()
                        job.status = if (job.exitCode == 0) getString(R.string.job_done) else getString(R.string.job_failed)
                        job.progress = if (job.exitCode == 0) {
                            getString(R.string.job_done)
                        } else {
                            getString(R.string.job_failed)
                        }
                        job.endedAt = System.currentTimeMillis()
                        dockerJobBuffers.remove(jobId)
                    } else {
                        updateDockerJobProgress(job, line)
                        job.output += line
                        while (job.output.size > MAX_JOB_LINES) job.output.removeAt(0)
                    }
                }
            saveDockerJobs()
            if (currentTab in setOf(Tab.Overview, Tab.Compose, Tab.Dockerfiles)) {
                renderContent()
            }
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
        while (dockerJobs.size > MAX_JOB_HISTORY) dockerJobs.removeAt(dockerJobs.lastIndex)
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
        copyAssetTree(template.assetPath, target)
        migrateProjectPorts(target)
        status.text = getString(R.string.status_library_installed_fmt, template.name)
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
        val labels = mapOf(
            "18080/tcp" to "VS Code",
            "18081/tcp" to "llama.cpp",
        )
        val urls = mutableListOf<Pair<String, String>>()
        val iter = ports.keys()
        while (iter.hasNext()) {
            val key = iter.next()
            val value = ports.opt(key)
            val label = labels[key] ?: key
            if (value is JSONArray && value.length() > 0) {
                for (i in 0 until value.length()) {
                    val binding = value.optJSONObject(i) ?: continue
                    val host = browserHost(binding.optString("HostIp"))
                    val port = binding.optString("HostPort")
                    if (port.isBlank()) continue
                    urls += label to "http://$host:$port/"
                }
            } else {
                _splitPortKey(key)?.let { port ->
                    urls += label to "http://127.0.0.1:$port/"
                }
            }
        }
        return urls.distinctBy { it.second }
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
        stamp.parentFile?.mkdirs()
        stamp.writeText("2\n")
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
                }
                if (migrated != original) file.writeText(migrated)
            }
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
