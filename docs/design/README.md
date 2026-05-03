# Design Documents

Snapshot date: 2026-05-03.

## Purpose

This category records architectural choices, compatibility boundaries, and
technical feasibility decisions. Design documents should describe constraints,
tradeoffs, accepted behavior, and non-goals.

## Contents

| Document | Scope |
|---|---|
| [`DOCKER_COMPAT_SCOPE.md`](DOCKER_COMPAT_SCOPE.md) | Docker compatibility scope, non-goals, and replacement strategies |
| [`RUNTIME_STRATEGY.md`](RUNTIME_STRATEGY.md) | Direct runtime direction and PRoot retirement plan |
| [`API29_DIRECT_EXEC_FEASIBILITY.md`](API29_DIRECT_EXEC_FEASIBILITY.md) | API 29+ direct execution feasibility notes |
| [`GPU_COMPAT.md`](GPU_COMPAT.md) | Android GPU, Vulkan, cuVK, and benchmark design direction |
| [`../../docker-proot-setup/docs/GPU_COMPAT.md`](../../docker-proot-setup/docs/GPU_COMPAT.md) | Backend GPU request/env contract |
| [`../../docker-proot-setup/docs/NETWORK_COMPAT.md`](../../docker-proot-setup/docs/NETWORK_COMPAT.md) | Backend network metadata and port rewrite plan |

## Maintenance

- Keep product boundaries here, not in test result files.
- Link to [`../test/COMPATIBILITY.md`](../test/COMPATIBILITY.md) for measured
  compatibility status.
- Link to [`../plan/TODO.md`](../plan/TODO.md) for unfinished implementation
  tasks.
