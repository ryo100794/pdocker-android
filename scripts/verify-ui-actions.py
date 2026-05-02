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
XTERM = ROOT / "app/src/main/assets/xterm/index.html"
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
    xterm_src = XTERM.read_text()
    string_src = "\n".join(path.read_text() for path in STRINGS)

    docker_action_count = main_src.count("openDockerTerminal(") - 1
    require("docker actions use persistent terminal helper", docker_action_count >= 7)
    require("docker terminal starts pdockerd first", "private fun openDockerTerminal" in main_src and "startDaemon()" in main_src)
    require("docker actions wait for daemon readiness", "waiting for pdockerd" in main_src and "docker version >/dev/null" in main_src)
    require("docker build uses legacy builder env", "DOCKER_BUILDKIT=0" in main_src and "COMPOSE_DOCKER_CLI_BUILD=0" in main_src)
    require("terminal exports legacy builder env", "DOCKER_BUILDKIT=0" in (ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/Bridge.kt").read_text())
    require("compose up action is detached and builds first", "docker compose up --detach --build" in main_src)
    require("legacy docker-compose commands are normalized", "normalizeDockerCommand" in main_src and "docker-compose" in main_src and "docker compose" in main_src)
    require("runtime removes stale docker-compose shim", "docker-compose" in (ROOT / "app/src/main/kotlin/io/github/ryo100794/pdocker/PdockerdRuntime.kt").read_text())
    require("one-shot docker commands leave an interactive shell", "status=\\$?" in main_src and "exec sh" in main_src)
    require("docker actions create native job cards", "private data class DockerJob" in main_src and "renderDockerJobs" in main_src)
    require("docker jobs persist state to json", "jobs.json" in main_src and "saveDockerJobs()" in main_src)
    require("docker jobs capture terminal output", "onOutput: ((ByteArray) -> Unit)?" in main_src and "handleDockerJobOutput" in main_src)
    require("docker jobs record exit markers", "__PDOCKER_JOB_EXIT" in main_src and "job_status_failed_fmt" in string_src)
    require("docker jobs expose retry actions", "action_retry_job_fmt" in main_src and "openDockerTerminal(job.title, job.command, job.group)" in main_src)
    require("docker jobs expose stop and log actions", "stopDockerJob(job.id)" in main_src and "openJobLog(job)" in main_src)
    require("container console avoids host shell fallback", "openDockerInteractiveTerminal" in main_src and "container console exited" in main_src)
    require("host shell is diagnostic-only", "private fun renderDiagnostics" in main_src and "action_host_shell" in main_src and "Tab.Overview -> renderOverview()" in main_src)
    require("container cards surface network warnings", "containerWarningSummary" in main_src and "container_warning_ports_metadata" in string_src)
    require("container cards open known service ports", "containerServiceUrls" in main_src and "18080/tcp" in main_src and "18081/tcp" in main_src)
    require("terminals label their origin", "terminalSessionCommand" in main_src and "PDOCKER_TERMINAL_TITLE" in main_src)
    require("existing templates migrate service ports", "migrateProjectPorts" in main_src and "8080:8080" in main_src and "18080:18080" in main_src)

    require("image browser accepts selected image extra", "EXTRA_IMAGE_NAME" in image_src)
    require("image browser accepts selected container extra", "EXTRA_CONTAINER_ID" in image_src)
    require("image rows deep-link to selected image", "openImageFiles(image)" in main_src)
    require("generic image browser action remains available", "openImageFiles()" in main_src)
    require("container rows deep-link to container files", "openContainerFiles(dir)" in main_src and "action_browse_container_files_fmt" in string_src)
    require("image/container files copy into editable projects", "copyFileToProject" in image_src and "TextEditorActivity.EXTRA_PATH" in image_src)

    require("editor tabs are keyed by canonical file path", "val key = target.absolutePath" in main_src)
    require("same-name editors get parent-qualified titles", "private fun editorTitle" in main_src)
    require("project files are surfaced as editor shortcuts", "renderProjectFileShortcuts" in main_src and "section_project_files" in string_src)

    require("terminal font remains 12pt", "fontSize: 12" in xterm_src)
    require("terminal shortcut key palette is present", 'id="keybar"' in xterm_src and 'data-toggle="ctrl"' in xterm_src)
    require("terminal supports pinch zoom", "setFontSize" in xterm_src and "event.touches.length === 2" in xterm_src)
    require("terminal touch scroll remains available", "term.scrollLines" in xterm_src and "touchScroll" in xterm_src)
    require("terminal shows touch selection markers", "selection-handle" in xterm_src and "term.select(" in xterm_src and "data-toggle=\"select\"" in xterm_src)

    require("code editor exposes whitespace", "VisibleWhitespaceTransformation" in editor_src)
    require("code editor keeps indent/outdent actions", "indentSelection()" in editor_src and "outdentSelection()" in editor_src)
    require("code editor supports search and replace", "findNext()" in editor_src and "replaceAllMatches()" in editor_src)
    require("code editor supports pinch zoom", "ScaleGestureDetector" in editor_src and "editorFontSize" in editor_src)

    require("localized terminal title formats exist", "terminal_compose_up_fmt" in string_src and "terminal_docker_build_fmt" in string_src)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
