# pdocker-android

Standalone Android APK wrapping `docker-proot-setup` — no Termux required.

- **Daemon**: `pdockerd` (Python, from the submodule) running inside a
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
  containers, and PTY-backed sessions, so normal UI use does not require
  typing the bundled `docker` command directly. Sessions also surface recent
  editable project/imported files as editor shortcuts.
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
- **Docker job widgets**: Docker-backed UI actions create persistent job cards
  in the upper pane with running/done/failed status, elapsed time, command
  context, a small log tail, log tabs, stop controls for running jobs, and
  retry actions, while the PTY tab remains available below.
- **Persistent command actions**: UI actions such as `docker ps`, `docker pull`,
  `docker build`, and `docker compose up` start pdockerd, run in PTY-backed
  lower tabs, export the legacy builder/Compose environment expected by
  pdockerd, and leave an interactive shell open when the command exits.
  Compose actions use `docker compose up -d --build` so the UI does not stay
  pinned to foreground logs.
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
  containers; experimental `PDOCKER_USE_COW_BIND=1` uses the self-built
  proot `--cow-bind` backend for write-open copy-up.
- **Runtime direction**: PRoot is treated as a temporary backend. See
  [`docs/RUNTIME_STRATEGY.md`](docs/RUNTIME_STRATEGY.md); experimental no-PRoot
  payloads can be staged with `PDOCKER_WITH_PROOT=0`.

Plan document: [`docker-proot-setup/APK_PLAN.md`](docker-proot-setup/APK_PLAN.md)

Compatibility and compliance records:

- [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md)
- [`docs/DEFAULT_DEV_WORKSPACE.md`](docs/DEFAULT_DEV_WORKSPACE.md)
- [`docs/GPU_COMPAT.md`](docs/GPU_COMPAT.md)
- [`docs/REPLAN_2026-05-01.md`](docs/REPLAN_2026-05-01.md)
- [`docs/THIRD_PARTY_LICENSES.md`](docs/THIRD_PARTY_LICENSES.md)
- `python3 scripts/compat-audit.py --output docs/compat-audit-latest.md`
- `python3 scripts/verify-project-library.py`
- `python3 scripts/verify-ui-actions.py`

## Build (from Termux+PRoot Ubuntu aarch64)

```sh
git clone <this repo>
cd pdocker-android
bash scripts/setup-env.sh         # JDK / cmdline-tools / NDK (first run only)
bash scripts/build-apk.sh         # -> app/build/outputs/apk/debug/app-debug.apk
```

## Install (ADB over Wi-Fi to the same phone)

```sh
adb connect 127.0.0.1:5555        # pair once via Android's Wireless debugging
adb install app/build/outputs/apk/debug/app-debug.apk
```
