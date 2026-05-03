# pdocker-android

Standalone Android APK wrapping the integrated `docker-proot-setup` backend.

- **Daemon**: `pdockerd` (Python, from the integrated backend) running inside a
  Chaquopy-hosted ForegroundService. Unix socket at
  `filesDir/pdocker/pdockerd.sock`.
- **Resident mode**: pdockerd stays in the notification area, can be reopened
  from the notification, restarts after task removal, and starts again after
  device boot or APK replacement unless explicitly stopped.
- **Console**: WebView + xterm.js (CJK IME compatible) backed by a
  pty child via JNI (`app/src/main/cpp/pty.c`), with UTF-8 decoding and a
  Japanese-capable monospace font stack. The terminal includes a keyboard-side
  shortcut palette for Esc/Ctrl/Alt, arrows, Tab, Enter, Backspace, Delete,
  paging, common Ctrl chords, and shell punctuation.
- **Workspace UI**: top-level tabs for Compose, Dockerfile, images,
  containers, and PTY-backed sessions. Normal UI use goes through pdockerd's
  Engine API/native orchestrator; the product APK does not bundle upstream
  Docker CLI or Docker Compose binaries. Sessions also surface recent editable
  project/imported files as editor shortcuts.
- **Resizable split workspace**: the main screen has a draggable upper/lower
  split. The upper pane stays on Compose/Dockerfile/container status and
  control; the lower pane keeps grouped multi-tabs for consoles and editors.
- **Grouped tool tabs**: lower tabs are grouped by workspace or container, so a
  container can keep its console/editor sessions together. Split tabs can stack
  a console above an editor inside the lower pane.
- **Project Library**: bundled Compose/Dockerfile templates can be installed
  into `filesDir/pdocker/projects` from the UI. The library includes the
  existing VS Code Server + Continue + Codex workspace and a llama.cpp GPU
  workspace with auto GPU profile generation.
- **Localization**: Android string resources cover the main UI in English and
  Japanese (`values/` and `values-ja/`), following the device language.
- **Widgets first**: image/container/project tabs show state, counts, paths,
  and log previews in-app; terminal actions are explicit tools, not the main
  display surface.
- **Container controls**: container cards expose start, stop, restart, logs,
  file browser, known service URLs, and grouped interactive console actions.
- **Docker job widgets**: Docker-backed UI actions create persistent job cards
  in the upper pane with running/done/failed status, elapsed time, command
  context, parsed build/compose/pull progress, a small log tail, log tabs, stop
  controls for running jobs, retry actions, and restart-safe log fallback when
  a persisted job no longer has a live PTY tab.
- **Persistent job actions**: UI actions such as image pull, Dockerfile build,
  and Compose up start pdockerd and use Engine API/native orchestration. Test
  scripts may stage upstream Docker CLI/Compose separately, but those binaries
  are not part of the shipped APK.
- **Network visibility**: container cards show the synthetic container IP,
  exposed/published ports, metadata-only port-publishing warnings, and planned
  port-hook rewrite count from pdockerd, so unsupported networking is visible
  in the app instead of being hidden behind Docker-looking metadata. Known
  services such as VS Code (`18080`) and llama.cpp (`18081`) get local browser
  actions.
- **Terminal tabs**: one terminal screen can keep multiple PTY sessions alive
  and switch between them with tabs.
- **Editor**: in-app code editor for Compose files and Dockerfiles under
  `filesDir/pdocker/projects`, with line numbers, visible spaces/tabs/full-width
  spaces, lightweight syntax highlighting, tab-width controls, space/tab
  conversion, selected-line indent/outdent, and search/replace.
- **Default dev workspace**: first launch seeds a Dockerfile/Compose project
  for code-server, Continue, OpenAI Codex CLI, and common development tools.
- **GPU extensions**: experimental Docker-compatible `--gpus` handling for
  Vulkan passthrough and a future CUDA-compatible API layer.
- **Image/container files**: in-app browser for pulled image rootfs trees and
  created container rootfs/upperdir trees, without invoking the docker CLI.
  cow_bind containers get a merged lower/upper view with upper-layer whiteout
  handling. Files can be copied into `projects/imports/`; container writable
  layers can be edited directly, and read-only lower-layer files can be copied
  into the writable overlay before editing.
- **CoW**: current default runtime uses bundled glibc `libcow.so` inside
  containers while snapshot and overlay semantics are moved into pdockerd and
  the direct executor.
- **Runtime direction**: the default APK and integrated backend do not bundle
  PRoot/talloc/proot-loader. See
  [`docs/design/RUNTIME_STRATEGY.md`](docs/design/RUNTIME_STRATEGY.md).

Current implementation status: [`docs/plan/STATUS.md`](docs/plan/STATUS.md)

Compatibility and compliance records:

- [`docs/README.md`](docs/README.md) is the documentation map and canonical
  ownership table.
- [`LICENSE`](LICENSE) records the repository license status.
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) is the maintained
  third-party license inventory.
- [`docs/plan/TODO.md`](docs/plan/TODO.md) is the live unfinished-work ledger.
- `python3 scripts/compat-audit.py --output docs/test/compat-audit-latest.md`
- `bash scripts/verify-fast.sh` for the build-time fast gate
- `bash scripts/verify-heavy.sh --backend-quick` or `--backend-full` for slower backend regression
- `python3 scripts/verify-project-library.py`
- `python3 scripts/verify-ui-actions.py`

## Build

Build and install commands live in [`docs/build/README.md`](docs/build/README.md).

Short form:

```sh
bash scripts/setup-env.sh
./gradlew assembleModernDebug
```
