# Documentation index

Snapshot date: 2026-05-03.

This tree is grouped by document purpose. When adding or updating docs, put the
content in one category and link to it instead of copying the same status,
command list, or TODO table into another file.

## Manual

User-facing operation notes.

- [`manual/DEFAULT_DEV_WORKSPACE.md`](manual/DEFAULT_DEV_WORKSPACE.md): bundled
  VS Code Server, Continue, Codex, Claude Code, and llama.cpp workspace flow.

## Design

Architecture, compatibility boundary, and feasibility decisions.

- [`design/DOCKER_COMPAT_SCOPE.md`](design/DOCKER_COMPAT_SCOPE.md): Docker
  compatibility scope, non-goals, and replacement strategies.
- [`design/RUNTIME_STRATEGY.md`](design/RUNTIME_STRATEGY.md): direct runtime
  direction and PRoot retirement plan.
- [`design/API29_DIRECT_EXEC_FEASIBILITY.md`](design/API29_DIRECT_EXEC_FEASIBILITY.md):
  API 29+ direct execution feasibility notes.
- [`design/GPU_COMPAT.md`](design/GPU_COMPAT.md): Android GPU/Vulkan/cuVK
  compatibility direction.
- [`../docker-proot-setup/docs/GPU_COMPAT.md`](../docker-proot-setup/docs/GPU_COMPAT.md):
  backend GPU request/env contract.
- [`../docker-proot-setup/docs/NETWORK_COMPAT.md`](../docker-proot-setup/docs/NETWORK_COMPAT.md):
  backend network metadata and port rewrite plan.

## Build

Build, packaging, and install commands.

- [`build/BUILD.md`](build/BUILD.md): local environment setup, APK build, install,
  and common build gates.

## Test

Repeatable checks, audits, and debug workflows.

- [`test/COMPATIBILITY.md`](test/COMPATIBILITY.md): Docker API, file format,
  protocol, and data exchange compatibility coverage.
- [`test/compat-audit-latest.md`](test/compat-audit-latest.md): latest recorded
  compatibility audit report.
- [`test/ANDROID_SELFDEBUG.md`](test/ANDROID_SELFDEBUG.md): Android Wi-Fi ADB and
  self-debug workflow.

## Plan

Current status, active TODOs, and historical steering.

- [`plan/STATUS.md`](plan/STATUS.md): implementation status summary.
- [`plan/TODO.md`](plan/TODO.md): live unfinished-work ledger and temporary
  workaround tracker.
- [`plan/REPLAN_2026-05-01.md`](plan/REPLAN_2026-05-01.md): historical replan
  snapshot after UI/build/GPU steering.

## License Notice

Distribution and license inventory.

- [`../LICENSE`](../LICENSE): license status for pdocker-android original code.
- [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md): maintained source
  license inventory.
- [`license-notice/README.md`](license-notice/README.md): category index for
  license notice files.
- [`../app/src/main/assets/oss-licenses/THIRD_PARTY_NOTICES.md`](../app/src/main/assets/oss-licenses/THIRD_PARTY_NOTICES.md):
  notice file bundled into the APK.
