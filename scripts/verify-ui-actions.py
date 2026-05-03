#!/usr/bin/env python3
"""Offline checks for native UI action wiring.

This is not a substitute for device tapping, but it catches the common
"screen opens, then the useful action is disconnected" regressions:
Docker actions must open persistent PTY tools, image rows must deep-link to
the selected rootfs, and editor tabs must be keyed by file path.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/MainActivity.kt"
IMAGE = ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/ImageFilesActivity.kt"
EDITOR = ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/CodeEditorView.kt"
EDITOR_ACTIVITY = ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/TextEditorActivity.kt"
XTERM = ROOT / "app/src/main/assets/xterm/index.html"
BRIDGE = ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/Bridge.kt"
PDOCKERD_BRIDGE = ROOT / "app/src/main/python/pdockerd_bridge.py"
ANDROID_SMOKE = ROOT / "scripts/android-device-smoke.sh"
STRINGS = [
    ROOT / "app/src/main/res/values/strings.xml",
    ROOT / "app/src/main/res/values-ja/strings.xml",
]


def ok(name: str) -> None:
    print(f"ok: {name}")


def fail(name: str) -> None:
    print(f"FAIL: {name}")
    raise SystemExit(1)


def require(name: str, condition: bool) -> None:
    ok(name) if condition else fail(name)


def main() -> int:
    main_src = MAIN.read_text()
    image_src = IMAGE.read_text()
    editor_src = EDITOR.read_text()
    editor_activity_src = EDITOR_ACTIVITY.read_text()
    xterm_src = XTERM.read_text()
    bridge_src = BRIDGE.read_text()
    pdockerd_bridge_src = PDOCKERD_BRIDGE.read_text()
    android_smoke_src = ANDROID_SMOKE.read_text() if ANDROID_SMOKE.exists() else ""
    manifest_src = (ROOT / "app/src/main/AndroidManifest.xml").read_text()
    debug_receiver_src = (ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/PdockerdDebugReceiver.kt").read_text()
    engine_src = (ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/DockerEngineClient.kt").read_text()
    string_src = "\n".join(path.read_text() for path in STRINGS)

    docker_action_count = main_src.count("openDockerTerminal(") - 1
    require("upstream docker cli is not a normal apk ui path", docker_action_count <= 1)
    require("default apk build targets process-exec compat flavor", 'PDOCKER_ANDROID_FLAVOR:=compat' in (ROOT / "scripts/build-apk.sh").read_text() and 'PDOCKER_ANDROID_FLAVOR:-compat' in android_smoke_src)
    require("runtime flavors have distinct launcher labels", "pdocker compat" in (ROOT / "app/src/compat/res/values/strings.xml").read_text() and "pdocker modern" in (ROOT / "app/src/modern/res/values/strings.xml").read_text())
    build_gradle_src = (ROOT / "app/build.gradle.kts").read_text()
    require("main header surfaces version and build time", "appBuildInfo()" in main_src and "BUILD_TIME_UTC" in main_src and "app_build_info_fmt" in string_src and "buildConfigField(\"String\", \"BUILD_TIME_UTC\"" in build_gradle_src)
    require("test-staged docker terminal helper remains diagnostic only", "private fun openDockerTerminal" in main_src and "startDaemon()" in main_src)
    require("diagnostic docker helper can wait for test-staged cli", "waiting for pdockerd" in main_src and "docker version >/dev/null" in main_src)
    require("docker build uses legacy builder env", "DOCKER_BUILDKIT=0" in main_src and "COMPOSE_DOCKER_CLI_BUILD=0" in main_src)
    require("terminal exports legacy builder env", "DOCKER_BUILDKIT=0" in bridge_src)
    require("compose up uses engine-native path", "runComposeUp" in main_src and "engine compose up" in main_src)
    require("legacy docker-compose normalizer remains test-only", "normalizeDockerCommand" in main_src and "docker-compose" in main_src and "docker compose" in main_src)
    runtime_src = (ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/PdockerdRuntime.kt").read_text()
    require("runtime omits upstream docker cli and compose plugin", "libdocker.so" not in runtime_src and "libdocker-compose.so" not in runtime_src and 'File(dockerBin, "docker")' in runtime_src)
    require("runtime installs direct executor helper", "libpdockerdirect.so" in runtime_src and "pdocker-direct" in runtime_src and "PDOCKER_DIRECT_EXECUTOR" in pdockerd_bridge_src)
    require("runtime removes stale docker-compose shim", "docker-compose" in runtime_src)
    require("runtime selects no-proot when proot absent", 'PDOCKER_RUNTIME_BACKEND", "no-proot"' in pdockerd_bridge_src)
    require("terminal exposes test cli config path without bundling cli", "DOCKER_CONFIG=${runtime.absolutePath}/docker-bin" in bridge_src and "Product APKs do not bundle upstream Docker CLI" in bridge_src)
    require("docker helper keeps absolute test cli config", 'File(filesDir, "pdocker-runtime/docker-bin").absolutePath' in main_src and 'DOCKER_CONFIG=$dockerConfig' in main_src)
    require("one-shot docker commands leave an interactive shell", "status=\\$?" in main_src and "exec sh" in main_src)
    require("docker actions create native job cards", "private data class DockerJob" in main_src and "renderDockerJobs" in main_src)
    require("docker jobs persist state to json", "jobs.json" in main_src and "saveDockerJobs()" in main_src)
    require("docker jobs capture terminal output", "onOutput: ((ByteArray) -> Unit)?" in main_src and "handleDockerJobOutput" in main_src)
    require("docker jobs record exit markers", "__PDOCKER_JOB_EXIT" in main_src and "job_status_failed_fmt" in string_src)
    require("docker jobs parse progress lines", "dockerJobProgressLine" in main_src and "snapshotting layer:" in main_src and "Successfully tagged" in main_src)
    require("docker jobs render logs through xterm", "TerminalLogPane" in main_src and "TerminalLogBridge" in (ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/TerminalLogBridge.kt").read_text() and "terminalLogPane()" in main_src and "ToolKind.Terminal" in main_src)
    require("log panes are readonly and do not summon keyboard", "fun readOnly(): Boolean = true" in (ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/TerminalLogBridge.kt").read_text() and "body.readonly-log #keybar" in xterm_src and "term.options.disableStdin = true" in xterm_src and "inputmode', 'none'" in xterm_src and "if (!readOnly)" in xterm_src)
    require("docker jobs preserve carriage-return progress", "appendLiveJobTerminal(jobId, chunk)" in main_src and "recordJobTerminalOutput" in main_src and "text.endsWith(\"\\r\")" in main_src and "replace(\"\\r\", \"\")" not in main_src)
    require("engine job logs render terminal CRLF", "normalizeTerminalNewlines" in main_src and "\"$text\\r\\n\"" in main_src and "ch == '\\n' && previous != '\\r'" in main_src)
    require("job summaries tick while logs are quiet", "jobTickerTask" in main_src and "tickRunningJobs" in main_src and "job_activity_fmt" in string_src)
    require("job log ui batches expensive refreshes", "scheduleDockerJobsSave" in main_src and "scheduleJobRenderUpdate" in main_src and "postDelayed" in main_src)
    require("engine stream keeps terminal control characters", "decoded.split('\\n')" in engine_src and "decoded.replace(\"\\r\", \"\")" not in engine_src)
    require("engine api jobs are persisted", "runEngineJob" in main_src and "appendEngineJobOutput" in main_src and "finishEngineJob" in main_src)
    require("engine jobs wait through slow daemon warmup", "private fun waitForEngine(timeoutMs: Long = 90_000)" in main_src)
    require("docker jobs expose retry actions", "action_retry_job_fmt" in main_src and "retryDockerJob(job)" in main_src)
    require("engine job retry stays on engine api route", "private fun retryDockerJob" in main_src and "runComposeUp(dir, job.title)" in main_src and "runImageBuild(dir, job.title)" in main_src)
    require("docker jobs expose stop and log actions", "stopDockerJob(job.id)" in main_src and "openJobLog(job)" in main_src)
    require("docker jobs recover after missing terminal", "markInterruptedJob" in main_src and "else openJobLog(job)" in main_src and "job_status_interrupted_fmt" in string_src)
    require("container console avoids host shell fallback", "openDockerInteractiveTerminal" in main_src and "container console exited" in main_src)
    require("host shell is diagnostic-only", "private fun renderDiagnostics" in main_src and "action_host_shell" in main_src and "Tab.Overview -> renderOverview()" in main_src)
    bench_src = (ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/AndroidGpuBench.kt").read_text()
    require("android gpu bench writes cpu and gles reference artifacts", "AndroidGpuBench.run" in main_src and "android-gpu-bench" in bench_src and "vector_add" in bench_src and "saxpy" in bench_src and "matmul_fp32" in bench_src and "cpu_scalar" in bench_src and "gles31_compute" in bench_src and "compile_ms" in bench_src and "action_run_gpu_bench" in string_src)
    require("container cards surface network warnings", "containerWarningSummary" in main_src and "container_warning_ports_metadata" in string_src)
    require("container cards open compose-declared service ports", "containerServiceUrls" in main_src and "containerServiceLabels" in main_src and "PDOCKER_SERVICE_URL_LABEL_PREFIX" in main_src)
    require("container cards expose lifecycle actions", "action_container_start_fmt" in string_src and "/start" in main_src and "/stop" in main_src and "logs(dir.name" in main_src)
    require("container lifecycle avoids docker cli shell", "DockerEngineClient" in main_src and "runContainerAction" in main_src)
    require("image pull uses engine api", "pullImage(\"ubuntu:22.04\")" in main_src and "/images/create?fromImage=" in engine_src)
    require("dockerfile builds use streaming engine api", "runImageBuild" in main_src and "buildImageStreaming(dir" in main_src and "requestJsonStream" in engine_src and "/build?t=" in engine_src)
    require("compose up streams build output into ui jobs", "buildImageStreaming(contextDir, image)" in main_src and "emit(buildLine)" in main_src and "openLiveJobLog(job" in main_src)
    require("compose up links header-declared services after successful start", "Service URL $label $url" in main_src and "openServiceWhenReady" in main_src and "ACTION_VIEW" in main_src and "parseComposeHeaderServiceLinks" in main_src and "pdocker.auto-open:" in main_src)
    require("container create allows slow rootfs materialization", "fun postJson(path: String, json: JSONObject, timeoutMs: Int = 30_000)" in engine_src and "postJson(path, config, timeoutMs = 900_000)" in engine_src)
    require("compose up uses in-app orchestrator", "runComposeUp" in main_src and "parseComposeServices" in main_src and "/containers/create" in engine_src)
    require("compose up records android runtime blocker", "isRuntimeBackendBlocked" in main_src and "Prepared for inspection" in main_src and "dockerfileBaseImage" in main_src)
    require("engine job output is deduplicated", "appendUniqueLines" in main_src and "existing.add" in main_src)
    require("android runtime preflight is enabled", "PDOCKER_RUNTIME_PREFLIGHT" in pdockerd_bridge_src)
    require("debug smoke can start daemon through activity", "ACTION_SMOKE_START" in main_src and "FLAG_DEBUGGABLE" in main_src and ".PdockerdService" not in android_smoke_src)
    require("debug smoke can run gpu bench", "ACTION_SMOKE_GPU_BENCH" in main_src and "PdockerdDebugReceiver" in manifest_src and "AndroidGpuBench.run" in debug_receiver_src and "am broadcast" in android_smoke_src and "android-gpu-bench-*.jsonl" in android_smoke_src and 'android:launchMode="singleTop"' in manifest_src)
    require("terminals label their origin", "terminalSessionCommand" in main_src and "PDOCKER_TERMINAL_TITLE" in main_src)
    require("existing templates migrate service ports", "migrateProjectPorts" in main_src and "8080:8080" in main_src and "18080:18080" in main_src)
    require("template install reports copied and kept files", "copyAssetTreeMissing" in main_src and "TemplateInstallReport" in main_src and "status_library_install_report_fmt" in string_src)
    require("template install records version stamp", ".pdocker-template-id" in main_src and ".pdocker-template-version" in main_src and "obj.optInt(\"version\", libraryVersion)" in main_src)
    require("android device smoke script exists", "docker compose up --detach --build" in android_smoke_src and "run-as" in android_smoke_src and "docker version" in android_smoke_src)

    require("image browser accepts selected image extra", "EXTRA_IMAGE_NAME" in image_src)
    require("image browser accepts selected container extra", "EXTRA_CONTAINER_ID" in image_src)
    require("image rows deep-link to selected image", "openImageFiles(image)" in main_src)
    require("generic image browser action remains available", "openImageFiles()" in main_src)
    require("container rows deep-link to container files", "openContainerFiles(dir)" in main_src and "action_browse_container_files_fmt" in string_src)
    require("image/container files copy into editable projects", "copyFileToProject" in image_src and "TextEditorActivity.EXTRA_PATH" in image_src)
    require("container files expose writable overlay editing", "containerBrowserRoots" in image_src and "copyToOverlayAndOpen" in image_src and "EXTRA_ROOT_PATH" in editor_activity_src)
    require("container files expose merged lower upper view", "mergedEntries" in image_src and ".wh..wh..opq" in image_src and "title_container_merged_files_fmt" in string_src)

    require("editor tabs are keyed by canonical file path", "val key = target.absolutePath" in main_src)
    require("same-name editors get parent-qualified titles", "private fun editorTitle" in main_src)
    require("project files are surfaced as editor shortcuts", "renderProjectFileShortcuts" in main_src and "section_project_files" in string_src)
    require("project dashboard summarizes compose/dockerfile/files", "renderProjectDashboard" in main_src and "private data class ProjectSummary" in main_src and "section_project_dashboard" in string_src)
    require("project dashboard opens primary files and actions", "openProjectPrimaryFile" in main_src and "action_open_project_compose_fmt" in string_src and "action_open_project_dockerfile_fmt" in string_src)
    require("project dashboard tracks services jobs containers", "projectContainerCount" in main_src and "projectJobSummary" in main_src and "parseComposeServices" in main_src)
    require("project dashboard exposes compose-declared service URLs", "projectServiceUrls" in main_src and "composeServiceUrls" in main_src and "composeHostPort" in main_src and "project_dashboard_urls_fmt" in string_src)
    require("project dashboard probes service health", "projectServiceHealthSummary" in main_src and "scheduleServiceHealthProbe" in main_src and "HttpURLConnection" in main_src and "project_dashboard_service_health_fmt" in string_src)
    require("project dashboard surfaces compose dependencies", "dependsOn" in main_src and "projectDependencySummary" in main_src and "project_dashboard_dependencies_fmt" in string_src)
    require("project dashboard surfaces compose healthchecks", "hasHealthcheck" in main_src and "projectHealthSummary" in main_src and "project_dashboard_health_fmt" in string_src)
    require("project dashboard surfaces llama model and gpu diagnostics", "projectModelSummary" in main_src and "projectGpuProfileSummary" in main_src and "pdocker-gpu-diagnostics.json" in main_src and "project_dashboard_models_fmt" in string_src and "action_open_gpu_diagnostics" in string_src)
    require("default dev workspace migrates codex and claude code plugins", "migrateDefaultDevWorkspace" in main_src and "openai.chatgpt" in main_src and "Anthropic.claude-code" in main_src and "CLAUDE_CODE_NPM_PACKAGE" in main_src and "ANTHROPIC_API_KEY" in main_src)

    require("terminal font remains 12pt", "fontSize: 12" in xterm_src)
    require("terminal shortcut key palette is present", 'id="keybar"' in xterm_src and 'data-toggle="ctrl"' in xterm_src)
    require("terminal supports pinch zoom", "setFontSize" in xterm_src and "event.touches.length === 2" in xterm_src)
    require("terminal touch scroll remains available", "term.scrollLines" in xterm_src and "touchScroll" in xterm_src)
    require("terminal shows touch selection markers", "selection-handle" in xterm_src and "term.select(" in xterm_src and "data-toggle=\"select\"" in xterm_src)
    require("terminal select-all is wired", "data-select-all=\"1\"" in xterm_src and "selectAllTerminal" in xterm_src and "term.selectAll()" in xterm_src)
    require("terminal selection menu touch does not steal button clicks", "selectionMenu.contains(event.target)" in xterm_src and "event.stopPropagation()" in xterm_src)
    require("terminal copy uses android clipboard bridge", "copyToClipboard" in bridge_src and "PdockerBridge.copyToClipboard" in xterm_src)
    require("terminal selection handles drag independently", "roleForVisualHandle" in xterm_src and "nearestSelectionHandle" in xterm_src and "pointer-events: auto" in xterm_src)

    require("code editor exposes whitespace", "VisibleWhitespaceTransformation" in editor_src)
    require("code editor keeps indent/outdent actions", "indentSelection()" in editor_src and "outdentSelection()" in editor_src)
    require("code editor supports search and replace", "findNext()" in editor_src and "replaceAllMatches()" in editor_src)
    require("code editor supports pinch zoom", "ScaleGestureDetector" in editor_src and "editorFontSize" in editor_src)

    require("localized terminal title formats exist", "terminal_compose_up_fmt" in string_src and "terminal_docker_build_fmt" in string_src)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
