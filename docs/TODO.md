# pdocker TODO ledger

Snapshot date: 2026-05-02.

This is the working TODO list for unfinished items and deliberate temporary
accommodations. Keep this file current whenever a workaround is added so it
does not become product behavior by accident.

## P0: Real Android Container Execution

Status: **not implemented**. This is the blocker for `docker run`,
Dockerfile `RUN`, `docker exec`, Compose services, VS Code server, and
llama-server.

Temporary behavior:

- `PDOCKER_RUNTIME_BACKEND=no-proot` is metadata/edit/browse mode only.
- The APK stages a native `pdocker-direct` helper and sets
  `PDOCKER_DIRECT_EXECUTOR`, but its probe advertises `process-exec=0`.
- Experimental process execution probes must stay gated. The 2026-05-02
  `scripts/android-api29-direct-feasibility.sh --no-install` run on SOG15
  (Android 16 / SDK 36, app targetSdk 34, `untrusted_app`) still failed the real
  app-domain Dockerfile `RUN` path with `exit code -31` even though `run-as`
  controls could execute the helper and rootfs shell.
- Direct backend start/exec fails with an explicit error instead of starting a
  fake listener.
- Dockerfile `RUN` fails in direct mode instead of recording a fake layer.
- UI `compose up` may create inspection metadata or show runtime-blocked state,
  but must not report a service as running unless a real process is running.

Real implementation needed:

1. Add a direct executor boundary for `start`, `exec`, `wait`, `stop`, `logs`,
   attach, PTY, environment, workdir, and signal handling: **started, process
   execution still blocked**.
   - `PDOCKER_DIRECT_EXECUTOR` is now the explicit helper entry point.
   - The helper must pass `--pdocker-direct-probe` by printing
     `pdocker-direct-executor:1`.
   - The helper must also print `process-exec=1` before pdockerd will route
     `RUN`, `docker run`, `docker exec`, or Compose services to it.
   - Without a passing helper and capability, pdockerd refuses process
     execution instead of falling back to `/system/bin/sh`.
2. Prototype APK-owned native `fork/exec` helper with stdout/stderr capture.
3. Add rootfs path mediation so process paths resolve inside the image rootfs,
   not the Android host filesystem.
4. Add bind mount/path rewrite support for project volumes and named volumes.
5. Add Engine-level TTY plumbing for `docker run -t` and `docker exec -it`.
6. Add process supervision that survives UI navigation and reports honest exit
   codes.

Acceptance:

- `docker run --rm ubuntu:22.04 echo hi` prints `hi`.
- `docker build` with a tiny `RUN echo ok > /marker` creates the marker in the
  image.
- `docker compose up -d` starts a service process, `compose logs` shows its
  stdout, and `compose down` stops it.
- Opening a container terminal runs inside the container rootfs; `ls /` lists
  container root, not Android host root.

## P0: Dockerfile Semantics Stay Upstream-Compatible

Status: **guarded by tests**.

Temporary behavior:

- Legacy builder supports only a Docker-compatible subset.
- Unsupported standard Docker/BuildKit features must fail clearly.
- pdocker-specific Dockerfile instructions are forbidden.

Real implementation needed:

1. Keep bundled Dockerfiles standard-only.
2. Do not add `PDOCKER_*` Dockerfile instructions or custom frontend syntax.
3. Expand standard Docker support in priority order:
   - multi-stage `FROM ... AS` plus `COPY --from`;
   - `COPY --chown` and `COPY --chmod` metadata;
   - `SHELL`-aware `RUN`;
   - `.dockerignore` parity;
   - BuildKit syntax only after a real BuildKit-compatible path exists.
4. Keep unsupported syntax as explicit failures, not silent skips.

Acceptance:

- `scripts/verify-dockerfile-standard.py` passes.
- Unknown Dockerfile instructions fail the build.
- direct runtime never creates fake `RUN` layers.

## P0: No Fake Service Ports

Status: **guarded by tests**.

Temporary behavior removed:

- The previous direct-runtime placeholder HTTP listener on `127.0.0.1:18080`
  was removed because it was not a container process.

Real implementation needed:

1. Service URLs should become healthy only when the container process actually
   binds the port.
2. UI health checks must distinguish:
   - configured/published port metadata;
   - listener exists but not from container;
   - real container listener.
3. `docker ps` should continue to show requested port mappings as metadata, but
   UI must label them as inactive until runtime port rewrite/listen support is
   implemented.

Acceptance:

- `127.0.0.1:18080` is refused until code-server really runs.
- `compose up` cannot succeed by launching an out-of-container placeholder.

## P1: Port Rewrite and Networking

Status: **metadata only**.

Temporary behavior:

- Synthetic IPs and `PdockerNetwork.PortRewrite` are recorded.
- Network mode is treated as a Compose-compatible host-network stub.
- Port publishing warnings are surfaced.

Real implementation needed:

1. Implement bind/connect syscall mediation or a container-aware socket proxy.
2. Support multiple containers wanting the same internal port.
3. Provide container DNS/alias resolution beyond `/etc/hosts` injection.
4. Add UI state for active/inactive/blocked port mappings.

Acceptance:

- A service listening on container port 80 can be mapped to host `18080`.
- Two services can both listen on internal port 80 with different host ports.
- Compose service names resolve consistently inside containers.

## P1: Filesystem and Overlay Semantics

Status: **partial**.

Temporary behavior:

- Image/container browsing works.
- cow_bind merged browsing is basic.
- `libcow`/PRoot paths are still compatibility pieces.

Real implementation needed:

1. Move lower/upper/whiteout semantics out of patched PRoot.
2. Implement rename, deletion, chmod/chown/xattr, hardlink, symlink, and merged
   directory semantics.
3. Make `docker cp` and UI edits share one storage contract.
4. Add tests for lower read, upper write, whiteout delete, and copy-back edit.

Acceptance:

- Editing a copied-up lower file affects only that container.
- Deletes create correct whiteout behavior in image/container browse and export.
- `docker cp` preserves expected Docker archive behavior.

## P1: VS Code Server and Dev Workspace

Status: **template exists; runtime blocked**.

Temporary behavior:

- Dockerfile and Compose templates exist for code-server, Continue, Codex, and
  common dev tools.
- The image can be inspected, but the server cannot run until the executor
  exists.

Real implementation needed:

1. Use only standard Dockerfile/Compose semantics for the template.
2. Once executor lands, start real code-server and expose the actual service
   URL.
3. Add first-run credential/password handling.
4. Add test that `docker compose up -d` makes the VS Code HTTP endpoint respond.

Acceptance:

- `docker compose logs` shows real code-server logs.
- UI service health reaches healthy from the real container listener.

## P1: llama.cpp and Model Workflow

Status: **template exists; runtime blocked**.

Temporary behavior:

- llama.cpp GPU template, model volume, optional model download, and logs script
  exist.
- No real llama-server runs on Android direct mode yet.

Real implementation needed:

1. Keep Dockerfile standard-only.
2. Add reliable model download/resume and model selection UI.
3. Validate the 8B default model path on-device with storage checks.
4. Once executor lands, verify `docker compose up -d` starts llama-server.
5. Stream real llama logs through `docker logs`.

Acceptance:

- A selected GGUF model appears in UI status.
- llama-server responds on the configured port from inside the container.

## P2: GPU, Vulkan, and CUDA-Compatible API

Status: **contract and benchmark first pass only**.

Temporary behavior:

- `--gpus all`, Vulkan env, CUDA-compatible env, and GPU diagnostics are
  negotiation signals, not a complete runtime.
- Current benchmark has CPU/GLES first-pass coverage; Vulkan/cuVK remains
  pending.

Real implementation needed:

1. Add Vulkan backend to `android-gpu-bench`.
2. Add device/thermal/driver metadata to benchmark artifacts.
3. Implement minimal container-facing Vulkan passthrough validation.
4. Implement CUDA-compatible shim API only as a real library/runtime, not just
   env variables.
5. Add UI recommendation based on measured CPU/GPU crossover size.

Acceptance:

- Benchmark report shows when GPU beats CPU on the current Android device.
- Container GPU diagnostics prove Vulkan loader access from inside the runtime.

## P2: UI and Editor Polish

Status: **partial**.

Temporary behavior:

- Some workflows are widgets; some still fall back to terminal/log tabs.
- Terminal/editor pinch zoom and selection are implemented but need device
  edge-case validation.

Real implementation needed:

1. Add feature-status coverage for every menu item so dead-end actions are
   visible in tests.
2. Keep host shell diagnostic-only, outside the normal user path.
3. Finish terminal selection handle behavior across wide ranges and IME cases.
4. Add editor encoding/newline controls and undo-safe whitespace transforms.
5. Add migration UI for project templates.

Acceptance:

- Every visible action either works, is clearly blocked with reason, or is
  hidden from normal flow.
- Terminal sessions survive back navigation and process recreation where
  feasible.

## P2: License and PRoot Retirement

Status: **default no-PRoot packaging exists; compatibility payload still
documented**.

Temporary behavior:

- PRoot/talloc are optional/diagnostic payloads.
- `/root/tl/docker-proot-setup/docker-bin/libtalloc.so.2` may appear as a local
  untracked artifact and should not be committed accidentally.

Real implementation needed:

1. Remove PRoot/talloc from default APK once direct executor works.
2. Keep legacy PRoot as opt-in diagnostic flavor only if useful.
3. Keep third-party notices aligned with actual packaged payloads.

Acceptance:

- Default release APK contains no PRoot/talloc binaries.
- License notice asset matches packaged binaries and template-sourced code.

## Required Test Split

Fast tests, run on most builds:

- `bash scripts/verify-fast.sh`
- `python3 scripts/verify-dockerfile-standard.py`
- `python3 docker-proot-setup/scripts/verify_runtime_contract.py`

Heavy tests, run before major runtime changes:

- `bash scripts/verify-heavy.sh --backend-quick`
- `bash scripts/verify-heavy.sh --backend-full`
- `bash scripts/android-device-smoke.sh --quick --no-install` for current
  device Engine/helper smoke.
- `bash scripts/android-device-smoke.sh --no-install` as the full Android
  runtime smoke. It is expected to fail at Dockerfile `RUN` until
  `pdocker-direct` advertises `process-exec=1`.
- GPU benchmark scenarios after GPU/runtime changes

Never mark a temporary workaround as complete unless the acceptance check for
the real behavior passes.
