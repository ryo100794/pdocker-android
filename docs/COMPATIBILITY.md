# Docker compatibility audit

Snapshot date: 2026-05-01.

This document is the repeatable compatibility record for pdocker-android and
the `docker-proot-setup` backend. Compatibility here means three layers:

- Surface behavior: Docker CLI commands and Engine API endpoints.
- Definition/data exchange: Dockerfile, image config, save/load tar archives,
  container archive copy, and APK payload shape.
- Protocol: HTTP over Unix domain socket, API version negotiation, hijacked
  raw streams, tar content types, and Docker-specific headers.

## How to run the audit

Fast offline audit:

```sh
python3 scripts/compat-audit.py --output docs/compat-audit-latest.md
```

Native UI action wiring only:

```sh
python3 scripts/verify-ui-actions.py
```

Full backend regression, including public image pulls and container runs:

```sh
python3 scripts/compat-audit.py --full --output docs/compat-audit-latest.md
```

For iterative work, cap the long regression and record timeout as a test result:

```sh
python3 scripts/compat-audit.py --full --full-timeout 90 --output docs/compat-audit-latest.md
```

APK packaging verification:

```sh
bash scripts/build-apk.sh
python3 scripts/compat-audit.py --output docs/compat-audit-latest.md
```

Backend-only regression remains available in the submodule:

```sh
bash docker-proot-setup/scripts/verify_all.sh
```

While a full audit is running, the backend regression daemon usually listens at
`/tmp/pdockerd-verify.sock`. You can inspect it with the bundled Docker CLI:

```sh
DOCKER_HOST=unix:///tmp/pdockerd-verify.sock docker-proot-setup/docker-bin/docker ps -a
DOCKER_HOST=unix:///tmp/pdockerd-verify.sock docker-proot-setup/docker-bin/docker logs <container-id>
tail -f /tmp/pdockerd-verify.log
```

Most `docker run --rm` test containers are auto-removed quickly, so `docker logs`
is most useful for named or long-running containers created by the compose,
exec, stats, and network parts of the regression.

Latest recorded fast result: [compat-audit-latest.md](compat-audit-latest.md)
has 55 PASS, 0 FAIL, and 0 SKIP. The reusable offline/API/APK/license/UI/GPU
design checks pass, including native Docker job-card wiring, and the APK
payload is present.

Recent focused backend smoke checks also passed for a small Dockerfile build, a
multi-step RUN/COPY/RUN Dockerfile build, and `docker compose up -d --build` /
`compose ps` / `compose down`. The full backend regression remains the slow
suite and should be recorded separately when it is run to completion.

## Current compatibility matrix

| Area | Current status | Notes |
|---|---:|---|
| Engine API negotiation | Good | `/_ping`, `/version`, `/info`, API prefix stripping, and `Api-Version` response headers are implemented. |
| Image pull/list/inspect/delete | Good | Pull uses `crane export`; public registries work, private registry auth is not complete. |
| Image save/load | Partial | Docker-style tar exchange works for the implemented flattened image format. Multi-platform indexes, zstd layers, and all OCI edge cases are not complete. |
| Container create/start/stop/kill/wait/rm | Good | Implemented through PRoot runner and state files. No cgroups or namespaces. |
| Logs/attach/exec | Partial | Raw stream and hijack paths exist. Non-TTY exec works; `docker run -t` and `docker exec -it` still need PTY integration into the container side. |
| `docker cp` archive API | Partial | HEAD/GET/PUT support Docker tar and `X-Docker-Container-Path-Stat`. cow_bind reads prefer upper then lower, writes target upper. Directory merge of lower+upper entries is still incomplete. |
| Stats | Partial | CPU/memory are approximated from `/proc`; network, blkio, and cgroup-limit counters are absent. |
| Networks | Compose-compatible stub | List/create/connect/disconnect/inspect/delete satisfy common Compose flows. Synthetic IPs, Docker-visible ports, and explicit port-publishing warnings are recorded, but no bridge IPs, DNS server, iptables, or active port forwarding. |
| Volumes/binds | Partial | Named volumes map to host directories; bind mounts use PRoot. No kernel mount propagation or tmpfs semantics. |
| Dockerfile build | Partial | Legacy builder supports common instructions. BuildKit, buildx, multi-stage edge cases, cache mounts, and advanced frontend syntax are not implemented. |
| Compose | Partial | Basic up/down flows work when they stay inside the supported network/build/container subset. |
| Events | Partial | `/events` now persists Docker-style JSONL lifecycle events and live-streams new events with basic `since`, `until`, and filter handling. It covers container/image/network/volume/build events, but does not yet reproduce every daemon-internal event emitted by Moby. |
| APK data exchange | Good | APK includes pdockerd, Docker CLI, crane, proot, proot-loader, libcow, talloc, xterm assets, and license notice asset. |

## Protocol coverage

The audit checks these protocol details directly or statically:

- HTTP/1.1 over Unix domain socket.
- Docker API version prefix handling such as `/v1.43/version`.
- `Api-Version` response header.
- `application/vnd.docker.raw-stream` for logs/attach/exec.
- `application/x-tar` for image save/load and container archive exchange.
- `X-Docker-Container-Path-Stat` for `docker cp` stat behavior.
- Docker CLI `docker version` negotiation when the bundled CLI is executable
  on the current host.
- Docker event JSON objects over `/events`, including `Type`, `Action`,
  `Actor`, `time`, `timeNano`, `since`/`until`, and common filters.

## Definition and data exchange coverage

Covered today:

- Image references normalized into the local pdocker store.
- Image config fields used by `docker image inspect` and container create.
- Dockerfile legacy build context upload through `/build`.
- Docker save/load through `/images/get` and `/images/load`.
- Container archive copy through `/containers/{id}/archive`.
- APK asset/native payload expected by the Android runtime.

Known gaps:

- Complete OCI image layout/index fidelity, multi-platform manifest lists, zstd
  layers, and private registry credential flow.
- Full Dockerfile frontend behavior, BuildKit features, `.dockerignore` parity,
  and multi-stage/cross-platform build behavior.
- Full overlayfs semantics for deletions, rename, metadata operations, and
  merged directory listings in cow_bind mode.

## Additional implementation plan

1. Expand `cow_bind` to overlayfs-like semantics:
   - Implement whiteouts for `unlink` and `unlinkat`.
   - Copy-up and rewrite `rename`, `renameat`, `renameat2`.
   - Copy-up metadata syscalls: `chmod`, `chown`, `setxattr`, `removexattr`,
     and truncate variants.
   - Implement merged directory view for host-side archive reads so `docker cp`
     sees lower-only, upper-only, and upper-overridden entries together.

2. Improve protocol fidelity:
   - Add regression tests for chunked upload bodies and hijacked attach/exec
     across Docker CLI versions.
   - Add PTY plumbing for `docker run -t` and `docker exec -it`.

3. Improve data exchange:
   - Add OCI manifest-list/index import/export tests.
   - Add zstd layer rejection/handling tests and, later, decoder support.
   - Preserve more image config/history metadata through save/load/build.

4. Improve registry support:
   - Implement `/auth` enough for `docker login`.
   - Wire Docker config credentials into `crane`.
   - Add private registry smoke tests against a local test registry.

5. Improve networking and Compose:
   - Make unsupported port publishing explicit instead of silent.
   - Expand `/etc/hosts` alias tests for Compose service names.
   - Document and test the host-network-only model in Compose examples.

6. Improve stats and resource flags:
   - Return explicit unsupported/zeroed fields for cgroup-only counters.
   - Add tests for `--memory`, `--cpus`, and unsupported resource flags so
     behavior stays predictable.

## Refactoring status

Completed in backend commit `d1906d3`:

- Shared container runtime path resolution through `_container_runtime`.
- Shared environment construction through `_container_env`.
- Added `_join_under` and `_container_host_path` to prevent archive path
  traversal and to route cow_bind reads/writes through the right lower/upper
  side.
- Removed duplicated env/rootfs/cow_bind setup from start, exec, and spawn
  paths.

Next cleanup candidates:

- Factor archive tar creation/extraction away from the HTTP handler.
- Split Dockerfile build execution from the daemon request handler.
- Centralize Docker API error response shapes and headers.
