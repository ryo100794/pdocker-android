# pdocker-android

**Docker-compatible containers for Android, packaged as a native APK.**

pdocker-android is an experimental Docker Engine-compatible runtime and
workspace app for Android. It aims to make Compose projects, Dockerfiles,
container filesystems, logs, and interactive shells usable from a normal app
sandbox, without root and without Termux as the user-facing shell.

The project is deliberately transparent about its limits: it is not upstream
Docker, it cannot rely on Linux namespaces/cgroups/overlayfs inside a regular
Android app, and the direct Android executor is still being hardened. The goal
is a practical Docker-compatible surface for mobile development, inspection,
self-build, and self-debug workflows.

## Why this is interesting

- **One APK, Android-first workflow**: pdockerd runs inside a foreground Android
  service, with a native UI for Compose, Dockerfile, images, containers,
  persistent jobs, logs, editors, and terminals.
- **Docker-compatible protocol surface**: pdockerd speaks Docker Engine API
  over a Unix socket and keeps compatibility records for CLI/API behavior.
- **No bundled upstream Docker CLI in the product APK**: the shipped UI uses
  Engine API/native orchestration. Upstream Docker CLI/Compose may be staged by
  tests, but they are not normal app payload.
- **SDK28 direct-exec compatibility route**: the compat APK uses the scratch
  `pdocker-direct` executor path for real process execution experiments while
  keeping the API29+ route switchable.
- **Real workspace UX**: VS Code Server, Continue, Codex, Claude Code,
  llama.cpp GPU workspace templates, image/container file browsing, code
  editing, and `-it`-style grouped terminal tabs are built into the app.
- **GPU direction**: experimental Vulkan passthrough and a future
  CUDA-compatible API shim are documented and benchmarked as first-class
  pdocker extensions.

## Current status

| Area | Status |
|---|---|
| APK shell | Native Android UI, foreground daemon, boot/package-replaced restart, notification resident mode |
| Engine API | Docker Engine API-compatible metadata, image, container, build, logs, and lifecycle endpoints |
| Compose up | In-app orchestrator path, persistent job UI, streaming logs, build progress, retry/stop actions |
| Direct execution | SDK28 compat executor under active development; syscall mediation and performance profiling are tracked |
| Filesystems | Image rootfs browser, container lower/upper merged view, editable writable layers, build prune |
| TTY/editor UX | xterm.js terminal tabs, compact readonly log terminals, Japanese-friendly input, in-app editor |
| Networking | Host-port style metadata and browser actions; bridge/IP parity is intentionally scoped as limited |
| Licensing | External payloads are audited; PRoot/talloc/proot-loader are not part of the default product APK |

See [`docs/plan/STATUS.md`](docs/plan/STATUS.md) for the detailed
implementation snapshot and [`docs/plan/TODO.md`](docs/plan/TODO.md) for the
live task board.

## Screens and workflows

The main UI is split into an upper control pane and a lower tool pane:

- Upper pane: overview, Compose, Dockerfile, project health, images,
  containers, storage metrics, job cards, and lifecycle controls.
- Lower pane: grouped terminal/log/editor tabs. A container can keep its own
  console and editor tools together without losing the session when the user
  navigates away.

The app exposes Docker-like workflows through widgets first. Terminals are
available when needed, but the normal path is not "drop to host shell and type
commands".

## Runtime model

Android apps do not get the kernel primitives that upstream Docker expects:
namespaces, cgroups, overlayfs, netlink, privileged mounts, and bridge
networking are unavailable or heavily constrained. pdocker replaces those with
userspace components:

| Upstream Docker piece | pdocker approach |
|---|---|
| dockerd | `pdockerd`, a Python Engine API daemon hosted through Chaquopy |
| containerd image pull | `crane export` to tarball, then controlled extraction |
| overlayfs snapshotter | content-addressed layer pool plus per-container upper data |
| runc namespaces/cgroups | Android direct userspace executor and syscall mediation |
| BuildKit | legacy-compatible builder path in pdockerd |
| Docker CLI UX | native app actions, persistent job cards, and test-staged CLI only |

The runtime strategy and Android feasibility notes live in
[`docs/design/RUNTIME_STRATEGY.md`](docs/design/RUNTIME_STRATEGY.md) and
[`docs/design/API29_DIRECT_EXEC_FEASIBILITY.md`](docs/design/API29_DIRECT_EXEC_FEASIBILITY.md).

## Build

Build and install commands live in [`docs/build/README.md`](docs/build/README.md).

Short form for the SDK28 direct-exec compatibility APK:

```sh
bash scripts/setup-env.sh
PDOCKER_ANDROID_FLAVOR=compat bash scripts/build-apk.sh
```

For a fixed-signature release APK, keep signing material outside Git and pass
it through environment variables:

```sh
export PDOCKER_SIGNING_STORE_FILE=$HOME/.pdocker/release.jks
export PDOCKER_SIGNING_STORE_PASSWORD=...
export PDOCKER_SIGNING_KEY_ALIAS=pdocker
export PDOCKER_SIGNING_KEY_PASSWORD=...
PDOCKER_ANDROID_FLAVOR=compat PDOCKER_ANDROID_BUILD_TYPE=release bash scripts/build-apk.sh
```

Signing keys and certificates are intentionally ignored by Git.

## Test gates

Fast checks used during normal development:

```sh
bash scripts/verify-fast.sh
python3 scripts/verify-ui-actions.py
python3 scripts/verify-project-library.py
```

Slower/device checks:

```sh
bash scripts/verify-heavy.sh --backend-quick
ANDROID_SERIAL=<host:port> bash scripts/android-device-smoke.sh --no-install
ANDROID_SERIAL=<host:port> bash scripts/android-runtime-bench.sh
```

Compatibility and compliance records are maintained under
[`docs/test/`](docs/test/).

## Documentation map

- [`docs/README.md`](docs/README.md): documentation index and maintenance rules
- [`docs/manual/`](docs/manual/): user-facing workflows and promotion assets
- [`docs/design/`](docs/design/): architecture, scope, feasibility, GPU design
- [`docs/build/`](docs/build/): local build, signing, install, and APK gates
- [`docs/test/`](docs/test/): repeatable test scenarios and audit outputs
- [`docs/plan/`](docs/plan/): live status, TODOs, and steering records

Root-level standards:

- [`LICENSE`](LICENSE): repository license status
- [`SECURITY.md`](SECURITY.md): vulnerability reporting and secret handling
- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md): third-party inventory

## Suggested GitHub topics

`android`, `docker`, `containers`, `compose`, `docker-engine-api`,
`android-apk`, `vscode-server`, `llama-cpp`, `vulkan`, `syscall`,
`ptrace`, `mobile-development`

## Project posture

pdocker-android is a research-heavy product build, not a Docker trademark
replacement. The compatibility target is Docker-like behavior where Android
allows it, explicit pdocker extensions where Android needs a different shape,
and honest UI feedback when a feature is metadata-only or still incomplete.
