# pdocker-android

**A Docker-compatible Android workbench in one native APK.**

pdocker-android turns an Android device into a portable container workspace:
pull images, inspect root filesystems, edit Dockerfiles and Compose projects,
watch build logs, open `-it`-style terminals, browse container files, and run
development templates such as VS Code Server and llama.cpp from a normal app UI.

This is not "Docker Desktop for Android" and it does not pretend Android gives
apps Linux host privileges. Instead, pdocker builds a practical Docker-like
surface inside the app sandbox: a Docker Engine-compatible daemon, native
Compose/Dockerfile controls, a direct Android executor, userspace storage, and
explicit Android extensions for GPU, media, networking, and self-debugging.

If you are interested in mobile development workstations, container runtime
internals, Android sandbox limits, or running real developer environments from
a phone or tablet, this repository is the experiment.

## What You Can Do

- **Manage projects visually**: Compose files, Dockerfiles, images, containers,
  ports, jobs, logs, storage, terminals, and editor tabs live in one Android UI.
- **Use Docker-shaped workflows**: `pdockerd` speaks the Docker Engine API over
  a Unix socket, so compatibility can be tested against real Docker clients
  while the product UI uses native Engine API calls.
- **Inspect without starting a shell**: browse image rootfs trees and container
  lower/upper views, copy files into editable projects, and edit writable
  container layers directly.
- **Keep sessions alive**: grouped terminal/log/editor tabs are designed for
  long-running builds and container consoles instead of disposable shell views.
- **Start real dev templates**: bundled project-library templates cover VS Code
  Server with Continue/Codex/Claude Code, llama.cpp, ROS2/RViz/noVNC, and
  Blender/noVNC experiments.
- **Measure the hard parts**: GPU bridge, syscall mediation, storage reuse,
  runtime overhead, and Docker API parity are tracked with repeatable tests.

## Why It Is Different

Android apps do not get Docker's normal toolbox: no privileged namespaces, no
cgroups, no overlayfs mounts, no bridge network, and no raw host device access.
pdocker treats that as the design challenge rather than hiding it.

- The product APK does **not** bundle upstream Docker CLI or Compose binaries.
- PRoot/talloc/proot-loader are **not** part of the default product APK.
- The UI tells the truth when a feature is metadata-only, blocked, or still
  experimental.
- Compatibility decisions are documented under `docs/design/` and verified by
  reusable tests under `docs/test/`.
- Android-specific features, such as Vulkan/OpenCL GPU bridging and media
  proxying through Camera2/AudioRecord/AudioTrack, are explicit pdocker
  extensions rather than disguised raw `/dev` passthrough.

## Current status

| Area | Status |
|---|---|
| APK shell | Native Android UI, foreground daemon, boot/package-replaced restart, notification resident mode |
| Engine API | Docker Engine API-compatible metadata, image, container, build, logs, and lifecycle endpoints |
| Compose up | In-app orchestrator path, persistent job UI, streaming logs, build progress, retry/stop actions; no product Docker CLI dependency |
| Direct execution | SDK28 compat executor under active development; syscall mediation and performance profiling are tracked |
| Filesystems | Image rootfs browser, container lower/upper merged view, editable writable layers, build prune |
| TTY/editor UX | xterm.js terminal tabs, compact readonly log terminals, Japanese-friendly input, selection/copy controls, in-app editor |
| GPU/media | Vulkan/OpenCL bridge experiments, llama.cpp GPU comparison workflow, Camera2/AudioRecord/AudioTrack media proxy scaffold |
| Networking | Host-port style metadata and browser actions; bridge/IP parity is intentionally scoped as limited |
| Licensing | External payloads are audited; PRoot/talloc/proot-loader are not part of the default product APK |

See [`docs/plan/STATUS.md`](docs/plan/STATUS.md) for the detailed
implementation snapshot and [`docs/plan/TODO.md`](docs/plan/TODO.md) for the
live task board.

For a GitHub-friendly view of the current demo surface, template library,
compatibility counters, and TODO-linked timeline, see the generated
[`Showcase Dashboard`](docs/showcase/PROJECT_DASHBOARD.md) and
[`Roadmap Timeline`](docs/showcase/ROADMAP_TIMELINE.md).

## What To Expect Today

The most useful current workflows are:

1. Install the compat APK on an SDK28-capable test route.
2. Start `pdockerd` from the app.
3. Pull or build an image from the native UI.
4. Open image/container files directly from the app.
5. Run Compose-style project actions and watch logs in persistent job cards.
6. Use project templates for VS Code Server or llama.cpp experiments.

The most important current limits are also explicit:

- Android direct execution is still being hardened for broader image coverage.
- Docker bridge networking is represented as metadata plus host-port behavior,
  not a real Linux bridge namespace.
- GPU acceleration is under active bridge work; llama.cpp reaches Vulkan
  offload paths, but generic SPIR-V dispatch is still the current blocker.
- Media devices are exposed through an Android API proxy contract, not raw
  `/dev/video*` or `/dev/snd/*` passthrough.

## Screens and Workflows

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
- [`CONTRIBUTING.md`](CONTRIBUTING.md): issue, PR, testing, and scope guidance
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
