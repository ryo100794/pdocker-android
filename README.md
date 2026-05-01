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
  Japanese-capable monospace font stack.
- **Workspace UI**: top-level tabs for Compose, Dockerfile, images,
  containers, and PTY-backed sessions, so normal UI use does not require
  typing the bundled `docker` command directly.
- **Localization**: Android string resources cover the main UI in English and
  Japanese (`values/` and `values-ja/`), following the device language.
- **Widgets first**: image/container/project tabs show state, counts, paths,
  and log previews in-app; terminal actions are explicit tools, not the main
  display surface.
- **Terminal tabs**: one terminal screen can keep multiple PTY sessions alive
  and switch between them with tabs.
- **Editor**: in-app text editor for Compose files and Dockerfiles under
  `filesDir/pdocker/projects`.
- **Default dev workspace**: first launch seeds a Dockerfile/Compose project
  for code-server, Continue, OpenAI Codex CLI, and common development tools.
- **GPU extensions**: experimental Docker-compatible `--gpus` handling for
  Vulkan passthrough and a future CUDA-compatible API layer.
- **Image files**: read-only in-app browser for pulled image rootfs trees
  under `filesDir/pdocker/images/*/rootfs`, without invoking the docker CLI.
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
- [`docs/THIRD_PARTY_LICENSES.md`](docs/THIRD_PARTY_LICENSES.md)
- `python3 scripts/compat-audit.py --output docs/compat-audit-latest.md`

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
