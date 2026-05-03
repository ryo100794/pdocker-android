# GitHub Promotion Kit

Snapshot date: 2026-05-03.

This document keeps the public-facing message consistent across the GitHub
repository, releases, demos, and social posts.

## Repository tagline

Docker-compatible containers for Android, packaged as a native APK.

## Short description

pdocker-android is an experimental Android app that embeds a Docker
Engine-compatible daemon, Compose/Dockerfile workspace UI, image/container file
browser, persistent build logs, and interactive terminal/editor tools inside a
normal APK.

## One-minute pitch

Docker was built for Linux hosts with namespaces, cgroups, overlayfs, and
bridge networking. Android apps do not get those primitives. pdocker-android
explores how far a Docker-compatible workflow can go inside the Android app
sandbox: Engine API metadata, image pull/extraction, Compose orchestration,
container files, build logs, `-it`-style terminals, VS Code Server templates,
and a direct syscall-mediated executor path for SDK28 compatibility.

The project is useful as a mobile development workbench, Android container
runtime experiment, and compatibility testbed. It is intentionally clear about
the unsupported parts so users can see what is real, what is emulated, and what
is a pdocker-specific extension.

## README hero bullets

- Native APK, no root, no Termux-first shell.
- Docker Engine API-compatible daemon over a Unix socket.
- Compose, Dockerfile, images, containers, jobs, logs, editor, and terminals in
  one Android UI.
- VS Code Server, Continue, Codex, Claude Code, and llama.cpp GPU workspace
  templates.
- Product APK does not bundle upstream Docker CLI/Compose; tests can stage
  them separately for compatibility checks.
- Direct Android executor and syscall mediation are developed in-tree, with
  repeatable performance benchmarks.

## GitHub topics

Use these repository topics:

```text
android
docker
containers
compose
docker-engine-api
android-apk
vscode-server
llama-cpp
vulkan
syscall
ptrace
mobile-development
```

## Suggested repository description

Docker-compatible Android APK with pdockerd, Compose/Dockerfile UI,
container files, persistent logs, VS Code Server templates, and direct Android
executor experiments.

## Pinned issue ideas

1. **Roadmap: Compose up parity on Android**
   Track direct executor maturity, TTY attach, signals, networking, volumes,
   archive APIs, and storage cleanup.

2. **Compatibility report: what works vs Docker**
   Link to `docs/test/COMPATIBILITY.md`, `docs/plan/STATUS.md`, and the latest
   Android smoke logs.

3. **Call for testers: Android device matrix**
   Ask users to report model, Android version, ABI, SDK route, image pull,
   build, compose up, VS Code port, and runtime benchmark output.

## Release note template

```markdown
## pdocker-android vX.Y.Z

### Highlights

- ...

### Compatibility

- Engine API:
- Compose:
- Direct executor:
- TTY/logs:
- Storage:

### Device testing

- Device:
- Android:
- APK flavor:
- Smoke:
- Runtime benchmark:

### Known limits

- ...
```

## Social post drafts

### Technical post

I am building pdocker-android: an experimental Docker-compatible runtime and
workspace app packaged as a normal Android APK. It embeds pdockerd, speaks a
Docker Engine-like API, manages Compose/Dockerfile projects, streams build
logs into the UI, and experiments with direct syscall-mediated container
execution on Android.

### Demo post

Docker-like workflows on Android are strange in exactly the interesting way:
no namespaces, no cgroups, no overlayfs, no bridge network. pdocker-android
turns that constraint into an APK with Compose controls, image/container file
browsing, persistent logs, editor tabs, terminals, and VS Code Server
templates.

### Tester call

Looking for Android testers for pdocker-android. The useful reports are:
device model, Android version, APK flavor, image pull result, Compose up log,
VS Code port check, `docker ps` view, and runtime benchmark output.

## Demo checklist

- Record the upper/lower split UI.
- Show Compose up from the UI, not from adb.
- Show live log updates with elapsed time and progress.
- Show container card ports and service URL.
- Open VS Code Server at `127.0.0.1:18080` when available.
- Open image/container file browser.
- Open an interactive container terminal tab.
- Show storage metrics and prune action.

## Boundaries to state publicly

- This is Docker-compatible work, not upstream Docker.
- Android kernel restrictions mean cgroup, namespace, overlayfs, and bridge
  parity are intentionally scoped.
- Some GPU behavior is pdocker-specific extension design, not NVIDIA Docker.
- Upstream Docker CLI/Compose are test tools only, not APK payload.
- Signing keys, certificates, and local debug secrets must never be committed.
