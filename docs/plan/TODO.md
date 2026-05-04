# pdocker TODO ledger

Snapshot date: 2026-05-04.

This is the working TODO list for unfinished items and deliberate temporary
accommodations. Keep this file current whenever a workaround is added so it
does not become product behavior by accident.

## Active Task Board

This board is the operating task list. Keep the detailed sections below as the
source of context, but update this board first when work starts, gets blocked,
or closes.

### Runtime / Compose-Up

- [done] Remove upstream Docker CLI/Compose from APK payload. Product UI must
  use Engine API/native orchestration; upstream Docker CLI/Compose are allowed
  only as test-staged compatibility tools.
- [done] Document Docker compatibility scope, non-goals, and discussion points
  for BuildKit, network, volume, cgroup, overlayfs, signals, TTY, and archive
  API in `docs/design/DOCKER_COMPAT_SCOPE.md`.
- [done] Tiny SDK28 compat smoke: `docker build`, `docker compose up`, logs,
  exit code, and `compose down` pass with the scratch direct executor.
- [done] Ubuntu `apt-get update` signature verification works under direct
  runtime path mediation.
- [done] Re-run default dev workspace `compose up --detach --build` on SOG15:
  Ubuntu `apt-get`, NodeSource Node.js, code-server, Codex, Continue/YAML/
  Docker extensions, image tagging, container create, and service start passed.
- [done] Verified default workspace with `docker ps -a`, `docker compose ps -a`,
  `docker logs --tail=120 pdocker-dev`, and `curl -I http://127.0.0.1:18080/`.
  The real code-server endpoint returns HTTP 302 to `./?folder=/workspace`.
- [done] Ensure the compat backend startup path force-enables direct process
  execution when `PDOCKER_RUNTIME_BACKEND=direct`, so the helper probe
  advertises `process-exec=1` and `RUN/docker run/compose up` no longer fail
  at the backend gate.
- [done] Harden llama template env substitution (`LLAMA_MODEL_URL`) to `:-` form
  and add project-library verification coverage so parser incompatibility regressions
  are caught by `scripts/verify-project-library.py`.
- [done] Start llama.cpp GPU workspace on SOG15 through UI/Engine-compatible
  compose path after rebuilding the APK. The 8B Qwen3 GGUF model is present,
  `pdocker-llama-cpp` reaches `Up`, `GET /` returns HTTP 200 after model load,
  `/v1/models` reports the loaded 8.19B GGUF, and `docker logs` streams the
  real llama-server output.
- [done] Record a repeatable llama.cpp CPU fallback baseline with
  `scripts/android-llama-bench.sh`. Latest SOG15 8B Qwen3 Q4_K_M HTTP result:
  3 repetitions of 8 generated tokens averaged about 0.260 tokens/s, with
  the full JSON stored in `docs/test/llama-bench-cpu-repeat3.json` and copied
  to `files/pdocker/bench/llama-bench-cpu-repeat3.json` on device.
- [done] Add and run `scripts/android-llama-tool-bench.sh` for the official
  llama.cpp `llama-bench` binary. Latest CPU fallback tool result with
  `-p 16 -n 8 -r 3 -ngl 0 -t 8`: prompt processing about 2.40 tokens/s,
  generation about 0.228 tokens/s, backend `BLAS`/OpenBLAS CPU.
- [done] Add llama healthcheck support end-to-end. The template now declares a
  Dockerfile `HEALTHCHECK`, pdockerd carries image healthchecks into container
  state, runs a lightweight monitor, and `docker ps` reports `Up (healthy)`.
- [done] Test Vulkan-requested llama mode. `gpus: all` now reaches
  `DeviceRequests`, `PdockerGpu.Modes`, GPU env, and the profile selects
  Vulkan with `-ngl 999`, but measured HTTP generation is slower than CPU
  baseline and `vulkaninfo` fails to load Android's Bionic-dependent
  `libvulkan.so` from the Ubuntu/glibc container. Recorded in
  `docs/test/LLAMA_BENCHMARKS.md`.
- [done] Probe OpenCL after Vulkan. pdocker now records `opencl` mode, injects
  OpenCL ICD metadata, and binds the host OpenCL library when present. The
  library is visible but fails to load because Android/Bionic dependencies
  such as `liblog.so` are not available to the glibc container.
- [next] Add a UI/device health card that checks the real 18080 listener and
  links to the container logs rather than relying on placeholder state.
- [next] Prevent duplicate container names after interrupted compose attempts.
  The current device had an old exited `pdocker-llama-cpp` plus the new running
  one, which makes name-based `docker logs` and `docker ps` display ambiguous.
- [next] If default workspace regresses, capture the first failing syscall or
  package-manager operation and add a focused direct-runtime smoke before
  retrying the full template. Latest focused blocker: npm self-update
  (`npm install -g npm@latest`) retires its own tree then loses
  `promise-retry` during Arborist rebuild. `@openai/codex` install with the
  NodeSource-bundled npm 10.9.7 works, so the template temporarily avoids the
  self-update while the runtime rename/reify parity bug remains open.
- [next] Add `docker run --rm ubuntu:22.04 echo hi` as an Android smoke gate.

### Performance

- [done] Add repeatable Android runtime benchmark split:
  `scripts/android-runtime-bench.sh`, optional `--apt-update`, optional
  existing `--proot-cmd`.
- [done] Establish baseline: all-syscall ptrace `apt-cache policy nodejs`
  took about 22.5s / 15,839 stops.
- [done] Add scratch seccomp-BPF selective tracing and switch default direct
  trace mode to `seccomp`.
- [done] Establish improved baseline: selective tracing `apt-cache policy
  nodejs` took about 4.3s / 1,783 stops.
- [done] Keep default workspace build on `apt-get`; performance fixes belong
  in syscall mediation, not Dockerfile shortcuts.
- [done] Add benchmark output capture to a stable artifact file under
  `files/pdocker/bench` so device runs can be compared over time.
- [done] Add a regression threshold for stop count and wall-clock deltas in the
  lightweight bench, with generous device variance.
- [done] Make the benchmark fail when no traced rootfs/stats are available;
  after build-prune removed transient roots, the old script could report a
  false PASS on `rootfs dynamic loader not found`.
- [done] Add Docker-compatible build/system prune paths for interrupted build
  cleanup. Test-staged upstream Docker CLI can still exercise these paths, but
  product APK UI should use Engine API/native actions.
- [done] Stop repeated Android UI rebuilds from growing the shared layer pool
  with stale copies. Successful tag replacement now prunes unreferenced layer
  store entries by default in the APK, and Dockerfile `RUN` snapshots have a
  parent-layer/build-state cache so an unchanged dev-workspace build can reuse
  the prior apt/npm layer instead of generating another multi-GB layer.
- [done] Add a whole-image rebuild cache for unchanged Dockerfile/context/tag
  pairs. On SOG15, the default VS Code workspace rebuild path measured 129s
  before this pass, then 62s after RUN cache reuse exposed a remaining rootfs
  re-merge bottleneck, and finally 0s wall-clock at shell-second resolution
  once the existing tagged image was reused directly. Simple metadata-only
  `RUN chmod ...` also uses touched-path snapshotting when the full image cache
  is invalidated.
- [done] Expose daemon-owned active operations through
  `GET /system/operations` and render them in the Overview. Builds triggered
  from ADB, tests, or the UI are now visible from the app because the state is
  recorded in pdockerd rather than only in UI job memory.
- [done] Add tracer process cleanup: `PTRACE_O_EXITKILL` where available,
  separate child process group, and SIGINT/SIGTERM/SIGHUP/SIGQUIT handling so
  aborted direct runs do not leave tracee process leftovers.
- [done] Remove `/proc/<pid>/status` ownership validation from the syscall-stop
  hot path. It is now opt-in with `PDOCKER_DIRECT_VALIDATE_TRACEES=1` for
  diagnostics, so normal seccomp/ptrace runs avoid one procfs open/read per
  trapped syscall.
- [next] Run optional PRoot/proot-like comparison only when an existing command
  is supplied; do not download or bundle external PRoot/fakechroot.
- [doing] Profile remaining hot trapped syscalls after `newfstatat/openat` and
  decide which can be safely handled with fewer ptrace stops. Current tuning
  adds seccomp errno returns for probe syscalls and uses a blocking
  `waitpid(__WALL)` path with the old 1ms polling wait removed. On SOG15, the
  filesystem-heavy `npm install -g @openai/codex --dry-run` profile improved
  from about 19.4s with the old polling wait loop to about 1.8-2.4s with the
  blocking wait loop at roughly the same 9.9k traced stops.
  A clean lightweight run after pruning uses
  `docker.io/library/ubuntu:22.04` and reports about 0.210s / 1,069 stops,
  with `newfstatat` and `openat` still the top trapped syscalls. After removing
  default per-stop `/proc/<pid>/status` validation, the same lightweight run on
  2026-05-03 reported about 0.141s / 1,069 stops.
- [done] Optimize Python layer diff/snapshot by comparing against a compact
  prior-layer path index and re-hardlinking committed snapshot files. Tiny
  Android `RUN` layer snapshots dropped from about 3.0s to about 1.5-1.9s.
- [done] Tune `libcow` copy-up hot paths. Read-only fd tracking is now opt-in
  with `PDOCKER_COW_TRACK_READONLY_FDS=1`, xattr copy-up is opt-in with
  `PDOCKER_COW_COPY_XATTRS=1`, `O_CREAT|O_EXCL` skips pre-open copy checks,
  `O_TRUNC`/`creat` copy up metadata without copying discarded file content,
  and copy-up uses `copy_file_range` when available. Reusable microbench:
  `docker-proot-setup/src/overlay/bench_cow.sh`.
- [done] Enable direct `cow_bind` container create/start in packaged APKs and
  sync the backend asset during Gradle builds so stale pdockerd code cannot be
  shipped accidentally. On SOG15, dev-workspace container create dropped from
  about 77.35s with full rootfs materialization to about 1.10s with lower/upper
  sharing; a fresh `pdocker-dev` create/start measured about 0.382s/0.389s.
- [done] Make container start idempotent when a live runtime PID already exists.
  This prevents a fast repeated start from launching a second process, having
  the second process fail on an already-bound port, and overwriting state as
  `Exited` while the first service is still serving.
- [done] Keep release builds from exposing debug-only daemon entry points.
  The product still starts the internal pdockerd Engine API for UI-driven
  compose/build/container management, but release APKs do not export the smoke
  broadcast receiver and the normal UI hides host shell/manual daemon/debug
  benchmark actions.
- [done] Keep COW terminology independent from PRoot. `libcow` is an
  LD_PRELOAD libc hook shim; it does not use ptrace or waitpid. PRoot-era COW
  comments and the diagnostic `proot-cow` driver label were renamed.
- [doing] Profile large apt/npm template layers separately from ptrace. Build
  logs now include `build-profile` timings for base materialization, RUN exec,
  COPY work, and snapshot subphases (`prev-index`, `walk`, `stage`, `tar`,
  `digest`, `extract`, `relink`). COPY/ADD snapshots now use touched-path mode
  instead of scanning the whole rootfs, cutting the reusable microbench from
  about 2.1-2.3s per COPY snapshot to about 0.2s. Remaining non-cache large
  RUN layers still need a direct-runtime changed-path manifest so snapshot can
  avoid a full rootfs walk after apt/npm.
- [next] Revisit rootfs-fd path rewriting as an opt-in optimization only after
  fd lifetime handling is proven. A trial that rewrote absolute `*at` paths to
  `openat(rootfs_fd, relative)` made apt resolver cleanup hit
  `getaddrinfo (9: Bad file descriptor)`, so the optimization is currently
  gated behind `PDOCKER_DIRECT_ROOTFD_REWRITE` and off by default.
- [next] Revisit `statx -> ENOSYS` as an optional Node/npm optimization only
  after apt Acquire DNS remains stable; a trial removed `statx` stops but made
  `apt-get update` report `Could not resolve 'ports.ubuntu.com'` despite
  `getent hosts` working.
- [done] Measure whether `newfstatat/statx` can simply bypass ptrace. It cannot:
  `PDOCKER_DIRECT_UNTRACED_STAT_PATHS=1` reduced an apt-cache probe to 73 stops
  but broke PATH resolution (`apt-cache: not found`, `apt-get: not found`).
  Keep it as a benchmark-only negative control, not a runtime default.

### Filesystem / Syscall Semantics

- [done] Add xattr path mediation so `ls`, `find`, and apt-key do not inspect
  Android host paths after `statx`.
- [done] Fix seccomp event emulation so user-space return values survive via a
  one-shot syscall-exit stop.
- [done] `dpkg --configure -a` focused reproduction passes after the previous
  `ca-certificates`/debconf failure; a device run completed in about 145s with
  `newfstatat` and `openat` as the dominant trapped syscalls.
- [doing] Replace permissive syscall answers with Docker/Linux-compatible errno
  behavior where package managers depend on it.
- [done] Treat Android-blocked NUMA policy syscalls (`mbind`,
  `get_mempolicy`, `set_mempolicy`, `migrate_pages`, `move_pages`, and
  `set_mempolicy_home_node`) as unavailable with `ENOSYS`. This unblocks
  llama.cpp/OpenBLAS-style startup on Android app seccomp without granting fake
  NUMA behavior.
- [next] Fix npm self-update rename/reify compatibility so
  `npm install -g npm@latest` works without temporarily relying on the
  NodeSource-bundled npm.
- [next] Replace `linkat` copy fallback with an inode/hardlink/CoW storage
  model.
- [next] Replace `/proc/self/exe` rootfs temporary symlink mediation with direct
  readlink emulation that does not mutate image state.
- [next] Remove normal stderr diagnostics from direct runtime logs once default
  workspace start is stable.

### UI / Workflow

- [done] Keep host shell diagnostic-only and keep normal Compose/Dockerfile
  flows on widgets or Engine actions.
- [doing] Keep job/task cards useful for build/compose failures, retries, and
  logs.
- [next] Surface default workspace service health only from the real container
  listener, never from a placeholder process.
- [next] Validate terminal text selection and copy on-device after the runtime
  smoke is stable.

### GPU / Models

- [done] Keep llama.cpp and dev workspace templates standard Dockerfile/
  Compose definitions.
- [done] Add first-pass CPU/GLES GPU benchmark artifacts.
- [next] Add Vulkan benchmark backend and device/thermal metadata.
- [next] Verify llama.cpp compose after runtime service start works, including
  model download/resume and docker logs.

### Packaging / License

- [done] Default no-PRoot packaging path exists.
- [done] Fold `docker-proot-setup` into this repository as a normal tracked
  directory instead of a submodule.
- [done] Remove unused bundled `proot`/`proot-runtime` payloads from the
  integrated backend tree; optional proot comparison remains command-supplied
  only.
- [next] Re-run third-party notice audit after packaging changes.

## P0: Real Android Container Execution

Status: **SDK28 compat smoke works for tiny build/compose through scratch
`pdocker-direct`; the default dev workspace now reaches real Ubuntu `apt-get`
execution with signature verification fixed under the selective syscall broker.
The default `compose up --build` command returned successfully on 2026-05-03
and `pdocker-dev` was verified running with real code-server logs and
HTTP 302 from `127.0.0.1:18080`**. This closes the first VS Code server/Codex/
Continue usability gate; llama-server,
PTY attach, port publishing, and broader Docker compatibility.

Temporary behavior:

- `PDOCKER_RUNTIME_BACKEND=no-proot` is metadata/edit/browse mode only.
- The APK stages a native `pdocker-direct` helper and sets
  `PDOCKER_DIRECT_EXECUTOR`, but its probe advertises `process-exec=0`.
- Experimental process execution probes must stay gated. The 2026-05-02
  `scripts/android-api29-direct-feasibility.sh --no-install` run on SOG15
  (Android 16 / SDK 36, app targetSdk 34, `untrusted_app`) still failed the real
  app-domain Dockerfile `RUN` path with `exit code -31` even though `run-as`
  controls could execute the helper and rootfs shell.
- The SDK28 compat flavor is now a separate runtime switch point and does not
  include PRoot/talloc/proot-loader. On SOG15 it can run the tiny
  `ubuntu:22.04` build/compose smoke through scratch `pdocker-direct`.
- The syscall-fetch foundation is now proven in scratch `pdocker-direct`:
  ptrace can fetch syscall registers in the app domain, trace fork/vfork/clone
  children, route child `execve()` through the rootfs loader, rewrite common
  absolute path syscalls into the image rootfs, and emulate/suppress known
  Android-blocked startup syscalls.
- `PDOCKER_DIRECT_TRACE_SYSCALLS=0` now disables verbose syscall logging only;
  the syscall broker remains enabled by default. This is required so UI builds
  are not drowned in trace logs while path mediation still happens.
- `io_uring_setup`/`io_uring_enter`/`io_uring_register` currently return
  `ENOSYS` in the direct runtime so Node/libuv falls back to portable polling.
  Replace this with an explicit compatibility policy and tests for runtimes
  that probe io_uring.
- `/proc/self/exe` readlink is mediated with a rootfs-local temporary symlink
  so Node sees `/usr/bin/node` instead of the Android host loader path. Replace
  this with a cleaner readlink emulation path that does not create helper
  symlinks in rootfs state.
- `faccessat2` is now handled in user-space mediation for apt-key. Replace the
  current minimal path probing with full flags/errno parity.
- `linkat` currently uses a file-copy fallback, including a dpkg
  `/var/lib/dpkg/status-old` replace case, because Android app data rejects the
  hardlink behavior dpkg expects. Replace this with a real inode/hardlink/CoW
  storage model.
- apt archive staging currently has a narrow `/var/cache/apt/archives/*.deb` to
  `/tmp/apt-dpkg-install-*` symlink mediation path. The earlier
  `DPkg::Go (14: Bad address)` blocker is past the current test point, but the
  special case still needs to be replaced with general rootfs symlink handling.
- Absolute symlink normalization inside rootfs is a temporary compatibility
  measure for direct execution without chroot. Replace it with runtime path
  mediation that does not mutate image data.
- `ldconfig`/`ldconfig.real` can be skipped in direct mode to avoid blocking
  Android ptrace execution. Replace this with deterministic ld cache handling
  or a correct executable path.
- Direct tracee cleanup now prunes vanished or detached tracees and includes
  temporary idle diagnostics. Remove normal stderr diagnostics once the default
  dev workspace compose build/start is stable.
- The direct runtime now defaults to a scratch seccomp-BPF selective trace mode
  instead of stopping every syscall. A focused device bench on 2026-05-03
  improved `apt-cache policy nodejs` from about 22.5s / 15,839 stops to about
  4.3s / 1,783 stops. Keep `PDOCKER_DIRECT_TRACE_MODE=syscall` available as a
  diagnostic fallback and continue tuning the selective syscall set.
- Seccomp event emulation must take a one-shot syscall-exit stop before writing
  the final return value. Returning from the seccomp event directly caused
  emulated `faccessat2` to appear as `ENOSYS`, which made apt-key treat Ubuntu
  keyrings as unreadable.
- `setxattr`/`getxattr`/`listxattr` path syscalls are now included in rootfs
  path mediation. Missing xattr path rewrite caused tools like `ls`, `find`,
  and apt-key to partially evaluate `/etc/...` against the Android host
  filesystem.
- Temporary `/bin` -> `/usr/bin` and related `/sbin`/`/lib` path fallback was
  added in `pdocker-direct` only to prove the syscall broker path, then removed.
  Correct behavior is to preserve Docker/OCI rootfs symlinks during layer
  materialization. Any future path fallback of this kind is forbidden unless it
  is explicitly modeled as real symlink resolution from the image rootfs.
- Direct backend start/exec fails with an explicit error instead of starting a
  fake listener.
- Dockerfile `RUN` fails in direct mode instead of recording a fake layer.
- UI `compose up` may create inspection metadata or show runtime-blocked state,
  but must not report a service as running unless a real process is running.

Real implementation needed:

1. Add a direct executor boundary for `start`, `exec`, `wait`, `stop`, `logs`,
   attach, PTY, environment, workdir, and signal handling: **tiny start/logs
   smoke works; attach/PTY/signals still incomplete**.
   - `PDOCKER_DIRECT_EXECUTOR` is now the explicit helper entry point.
   - The helper must pass `--pdocker-direct-probe` by printing
     `pdocker-direct-executor:1`.
   - The helper must also print `process-exec=1` before pdockerd will route
     `RUN`, `docker run`, `docker exec`, or Compose services to it.
   - Without a passing helper and capability, pdockerd refuses process
     execution instead of falling back to `/system/bin/sh`.
2. Harden APK-owned native `fork/exec` helper stdout/stderr capture and remove
   remaining noisy diagnostics from normal container logs.
3. Extend syscall coverage beyond the tiny smoke and replace permissive
   compatibility answers with accurate return values.
4. Complete rootfs path mediation so process paths resolve inside the image
   rootfs, not the Android host filesystem, including symlink and errno
   behavior.
5. Keep merged-usr symlinks (`/bin`, `/sbin`, `/lib`, `/lib64`) as image data.
   Do not flatten them into directories and do not paper over a broken rootfs
   by redirecting hard-coded paths to `/usr/...`.
6. Add bind mount/path rewrite support for project volumes and named volumes.
7. Add Engine-level TTY plumbing for `docker run -t` and `docker exec -it`.
8. Add process supervision that survives UI navigation and reports honest exit
   codes.
9. Reduce direct-runtime overhead for apt/npm-heavy Dockerfiles. Current
   selective seccomp/ptrace mediation is correct enough for tiny compose and
   `apt-get update`, but still needs full default workspace confirmation.
   - `build-profile` log lines are the canonical build bottleneck record; keep
     them in UI logs and test artifacts when measuring default workspace
     builds.
   - COPY/ADD now use path-scoped layer snapshots. Extend the same idea to RUN
     by having `pdocker-direct` record mutated guest paths from traced
     filesystem syscalls.
   - Continue tuning the child seccomp-BPF trace filter so path/credential/
     process syscalls trap, while hot syscalls such as `read`, `write`,
     `mmap`, `mprotect`, and `brk` run without ptrace stops.
   - Do not simply untrace `newfstatat/statx`; apt and shell PATH lookup need
     those path checks mediated. Optimize the handler path instead: reduce
     register writes, cache read-only path classifications, and add a RUN
     changed-path manifest so snapshot does not need a full post-RUN scan.
   - Keep path mediation on `openat`, `newfstatat`, `statx`, `execve`,
     `readlinkat`, `linkat`, `symlinkat`, `renameat`, `unlinkat`, and related
     filesystem syscalls.
   - Keep a comparison bench path for existing proot/proot-like commands when
     the user supplies one, but do not download or bundle PRoot/fakechroot.
10. Keep the fast native verification loop documented and working:
   `scripts/build-native-termux.sh`, `adb push libpdockerdirect.so`, then
   replace `files/pdocker-runtime/docker-bin/pdocker-direct` via `run-as`.
   APK rebuilds are for final packaging checks, not every runtime iteration.

Acceptance:

- `docker run --rm ubuntu:22.04 echo hi` prints `hi`.
- `docker build` with a tiny `RUN echo ok > /marker` creates the marker in the
  image. **Passing in SDK28 compat smoke.**
- `docker compose up -d` starts a service process, `compose logs` shows its
  stdout, and `compose down` stops it. **Passing for the tiny SDK28 compat
  smoke.**
- `apt-get update` inside an Ubuntu 22.04 direct rootfs verifies Ubuntu archive
  signatures without apt-key keyring readability warnings. **Passing in the
  2026-05-03 device run after xattr mediation and seccomp return fixes.**
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
- Network mode is treated as a Compose-compatible host-network stub with stable
  network IDs, endpoint IDs, service aliases, and `/networks` metadata.
- Port publishing warnings are surfaced.
- API warnings explicitly state the Android runtime has no TUN, namespace,
  bridge, iptables, or embedded DNS yet.

Real implementation needed:

1. Implement bind/connect syscall mediation or a container-aware socket proxy.
2. Support multiple containers wanting the same internal port.
3. Provide container DNS/alias resolution beyond `/etc/hosts` injection.
4. Teach running containers to refresh peer aliases after network connect and
   disconnect without requiring a restart.
5. Add UI state for active/inactive/blocked port mappings.

Acceptance:

- A service listening on container port 80 can be mapped to host `18080`.
- Two services can both listen on internal port 80 with different host ports.
- Compose service names resolve consistently inside containers.

## P1: Filesystem and Overlay Semantics

Status: **partial**.

Temporary behavior:

- Image/container browsing works.
- cow_bind merged browsing is basic.
- `libcow` remains the compatibility CoW shim; PRoot payloads are no longer
  part of the default APK or integrated backend tree.

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

Status: **quick-start template exists; runtime is now the blocker for first
successful service start**.

Temporary behavior:

- The default Dockerfile is standard-only and intentionally trimmed to the
  first useful path: code-server, Continue, Codex, Docker/YAML editing support,
  Python 3, git, ripgrep, curl, and Vulkan library presence.
- The earlier all-tools default (`pip`, `venv`, vim/nano, Jupyter/Python/ESLint/
  Prettier/GitLens extensions, `vulkan-tools`, etc.) made first on-device
  `compose up` too slow under ptrace. Reintroduce it as a separate full dev
  workspace template or optional install layer after quick-start compose is
  stable.

Real implementation needed:

1. Use only standard Dockerfile/Compose semantics for the template.
2. Once executor lands, start real code-server and expose the actual service
   URL.
3. Add first-run credential/password handling.
4. Add a full dev workspace template with the heavier editor extensions and
   CLI tools.
5. Add test that `docker compose up -d` makes the VS Code HTTP endpoint respond.

Acceptance:

- `docker compose logs` shows real code-server logs.
- UI service health reaches healthy from the real container listener.

## P1: llama.cpp and Model Workflow

Status: **llama.cpp server runs on Android direct runtime with CPU fallback**.

Temporary behavior:

- llama.cpp GPU template, model volume, optional model download, and logs script
  exist.
- Current SOG15 run starts the real `llama-server` and serves HTTP on
  `127.0.0.1:18081`, but GPU diagnostics select CPU fallback because Vulkan
  and CUDA-compatible container signals are not visible yet.

Real implementation needed:

1. Keep Dockerfile standard-only.
2. Add reliable model download/resume and model selection UI.
3. Validate the 8B default model path on-device with storage checks.
4. Keep `docker compose up -d` llama-server start in the Android smoke/manual
   regression loop.
5. Stream real llama logs through `docker logs`.
6. Keep Compose env defaults compatible with the strict parser in runtime-backed
   builds (`${LLAMA_MODEL_URL:-...}`), and add a project-library verify assertion
   so this regression is caught early.

Acceptance:

- A selected GGUF model appears in UI status.
- llama-server responds on the configured port from inside the container.
- GPU mode shows Vulkan/CUDA-compatible evidence before claiming acceleration;
  otherwise the UI must label the run as CPU fallback.

## P2: GPU, Vulkan, and CUDA-Compatible API

Status: **contract and benchmark first pass only**.

### llama.cpp Container GPU 10x Task List

Status: **in progress; llama.cpp source must remain unmodified**.

Goal:

- Load Qwen3 8B Q4_K_M into the llama.cpp container with GPU layers enabled
  through standard Vulkan/OpenCL loader APIs.
- Record CPU and GPU benchmarks from the same container image and model.
- Reach at least `10.0x` GPU generation throughput over the CPU baseline on
  the current Android device, without moving the llama.cpp engine to a host RPC
  process and without patching llama.cpp.

Reusable scenario:

- `scripts/android-llama-gpu-compare.sh` restarts the llama project container
  in CPU mode, records an HTTP benchmark, restarts it in forced Vulkan mode,
  records either a GPU HTTP benchmark or a structured model-load failure, writes
  `docs/test/llama-gpu-compare-latest.json`, copies it to
  `files/pdocker/bench`, and restores the CPU server.

Tasks:

1. **[done] CPU baseline is repeatable.**
   `scripts/android-llama-bench.sh` and `scripts/android-llama-gpu-compare.sh`
   record the current HTTP throughput for Qwen3 8B Q4_K_M.
2. **[done] Vulkan device discovery reaches llama.cpp.**
   Forced Vulkan mode now reaches `Vulkan0 (pdocker Vulkan bridge (queue))`
   instead of `ggml_vulkan: No devices found`.
3. **[done] Make the first GPU model-buffer allocation pass.**
   The forced `--n-gpu-layers 1` path now allocates the offloaded output-layer
   Vulkan model buffer through `pdocker-vulkan-icd.so`. The key fix was to
   advertise non-zero storage-buffer alignment from the ICD; llama.cpp remains
   unchanged.
4. **[next] Lower the first real llama.cpp SPIR-V dispatches.**
   Keep unknown shaders rejected, but add a trace classifier and implement the
   minimal operations needed for one offloaded Qwen3 output/repeating layer.
5. **[active] Fix Vulkan buffer base/range accounting during scheduler warmup.**
   Transfer-only submits now complete and llama.cpp reaches context
   construction, compute-buffer allocation, and warmup. The current failure is
   a `ggml_backend_buffer_get_alloc_size` range assertion, so the ICD must make
   mapped buffer base/size accounting match what ggml's scheduler expects.
   The 2026-05-04 trace confirmed that the two model copy-buffer regions are
   in-bounds; the remaining failure occurs before the first descriptor-backed
   compute dispatch. A `--no-warmup` diagnostic still reaches the same range
   assertion during slot initialization, so this is a buffer-type/accounting
   issue rather than only the warmup call path. The latest trace exposes
   separate Vulkan memory types: device-local model/compute buffers and
   host-visible staging/output buffers. It also shows allocation pNext records
   (`sType=1000060000`) on llama.cpp buffer allocations, so the next slice is
   exact dedicated-allocation/memory-requirements accounting instead of broad
   device discovery.
6. **[next] Add persistent GPU command-ring transport.**
   Replace per-dispatch socket commands with shared ring descriptors, reusable
   buffer handles, fences, and error records under `/run/pdocker-gpu`.
7. **[next] Establish small-model GPU green path.**
   Use the same unmodified llama.cpp container with a small GGUF model to prove
   model load, first token, and `llama-bench -ngl 1` before returning to 8B.
8. **[next] Optimize to 10x.**
   Measure CPU vs GPU after every dispatch slice; target is GPU
   `tokens/s >= CPU tokens/s * 10`. Prioritize persistent buffers, batched
   command submission, and resident compute over transfer-heavy paths.
9. **[next] UI reporting.**
   Surface `target_met`, speedup, current blocker, GPU layer count, and latest
   compare artifact in the project dashboard.
10. **[done] Distinguish daemon operations from containers in the UI.**
    Long-running compare/build cards are pdockerd operations and intentionally
    do not appear in `docker ps`; container cards are reconciled only from
    Engine API `/containers/json?all=1`.

11. **[doing] Rework project/container identity.**
    Stop using project-name prefixes as the primary relationship key. Compose
    launches now label containers with a stable pdocker project ID, project
    directory, project name, and compose service name; UI cards must prefer
    those labels and Engine container IDs over name guesses. Name matching is
    only a legacy fallback for containers created before labels existed.
12. **[next] Add a local SQLite project index.**
    Add an app-owned database for `projects`, `compose_services`,
    `containers`, `images`, and `jobs`. The database is an index and
    relationship layer, not a replacement for Docker-compatible Engine state:
    container truth remains Engine ID/state, image truth remains image ID and
    layer digests, and project truth can later attach git remote/branch/status.
    Do not store file contents in SQLite. For overlay/COW, store metadata only:
    path, lower layer digest, upper path, whiteout state, size, mtime, and the
    owning project/container IDs. File payloads remain in content-addressed
    layers and upperdirs.
    The database must be disposable: store `schema_version`, run consistency
    checks on startup, and rebuild the index from `projects/*/compose.yaml`,
    `containers/*/state.json`, image configs, layer manifests, and upperdirs
    whenever the DB is missing, corrupt, or has dangling references.
    Use SQLite WAL for normal operation, periodically checkpoint to a replica
    such as `metadata.snapshot.sqlite`, and write a small manifest containing
    source hashes/counts so startup can decide whether to trust the primary DB,
    fall back to the replica, or rebuild from the filesystem.

Current 2026-05-04 blocker:

- Qwen3 8B Q4_K_M forced Vulkan can discover the pdocker GPU bridge, allocate
  the first offloaded Vulkan model buffer, complete transfer-only queue submits,
  and reach context warmup. It still cannot serve tokens because ggml currently
  trips a `ggml_backend_buffer_get_alloc_size` range assertion during warmup.
  CPU mode is restored as the usable path after GPU experiments.

Temporary behavior:

- `--gpus all`, Vulkan env, CUDA-compatible env, and GPU diagnostics are
  negotiation signals, not a complete runtime.
- Current benchmark has CPU/GLES first-pass coverage. Vulkan/OpenCL request
  plumbing exists, but direct Android library exposure is now classified as
  diagnostic-only because it crosses from glibc into Bionic-only dependencies.
  cuVK remains pending.

Real implementation needed:

1. Add Vulkan backend to `android-gpu-bench`.
2. Add device/thermal/driver metadata to benchmark artifacts.
3. Replace raw host-library exposure with a glibc-facing GPU bridge: container
   shim/device ABI, shared-memory command queue, Bionic GPU-executor process,
   fences, error propagation, and lifecycle management. The executor may run
   GPU commands only; llama.cpp and other app engines must stay inside the
   container. The ABI exposed to containers must be device-independent; device
   and backend variation is absorbed by executor capability probing and command
   lowering.
4. Implement minimal container-facing Vulkan/OpenCL validation against that
   bridge, not against directly exposed Android libraries.
5. Implement CUDA-compatible shim API only as a real library/runtime, not just
   env variables.
6. Add UI recommendation based on measured CPU/GPU crossover size.

Scaffold completed:

- APK-side `pdocker-gpu-executor` capability/vector-add probe.
- Container-side Linux/glibc `pdocker-gpu-shim` capability probe injected into
  GPU-requesting containers.
- Container-side Linux/glibc `pdocker-vulkan-icd.so` minimal Vulkan ICD surface
  injected through `/etc/vulkan/icd.d/pdocker-android.json`; this lets
  unmodified apps use the standard Vulkan loader path, but it is still marked
  `PDOCKER_VULKAN_ICD_READY=0`.
- First shared-buffer transport probe: the glibc shim creates a mapped vector
  buffer and passes its FD to the Android/Bionic executor with `SCM_RIGHTS`.
  This proves the bridge can move data without exposing Android GPU libraries
  to the glibc container, but it is still a benchmark scaffold.
- First registered-buffer transport probe: the executor can map a shared
  vector buffer once for a connection and run repeated commands against that
  registered buffer.

Next implementation slice:

- Replace the temporary socket command transport with a persistent shared-memory
  command ring, multi-buffer table, and fence/error protocol. The socket path
  and single registered vector buffer are now useful for measuring and
  debugging, but only as scaffolds.
- Keep persistent transport semantics. Benchmarks show one-connection-per-GPU
  command adds measurable overhead and is the wrong shape for LLM workloads.
- Keep container-visible paths under `/run/pdocker-gpu`; do not expose Android
  app-data absolute paths to container code.
- Add queue lifecycle under pdockerd so container processes never call Android
  vendor libraries directly.
- Add a real reusable buffer/fence protocol and then wire a minimal ggml/llama
  GPU backend path to the bridge.
- Lower minimal Vulkan compute calls from `pdocker-vulkan-icd.so` into the
  bridge before enabling llama.cpp GPU layers; llama.cpp itself must remain
  unmodified.
- Implement the next llama Vulkan bridge blockers found on 2026-05-04:
  split or otherwise support 4 GiB+ model buffers, handle pinned host-buffer
  paths without crashing, and lower real llama.cpp SPIR-V compute dispatches.
  Current forced-GPU status: llama.cpp discovers `Vulkan0 (pdocker Vulkan
  bridge (queue))` and reaches model loading, but Qwen3 8B Q4_K_M cannot serve
  tokens on GPU yet.
- Keep CPU fallback healthy while GPU work is incomplete. CPU mode must hide
  Vulkan devices with `GGML_VK_VISIBLE_DEVICES=""` so llama.cpp does not enter
  Vulkan buffer scheduling with `--n-gpu-layers 0`.

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

Status: **default no-PRoot packaging exists; integrated backend no longer
tracks bundled PRoot/proot-runtime payloads**.

Temporary behavior:

- Optional proot comparisons are command-supplied diagnostics only.
- PRoot/talloc artifacts may appear locally while experimenting and should not
  be committed accidentally.

Real implementation needed:

1. Keep legacy PRoot out of the default APK and integrated backend tree.
2. Keep third-party notices aligned with actual packaged payloads.
3. Remove stale PRoot-era documentation as direct runtime coverage grows.

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
  runtime smoke. It should pass the tiny direct build/compose path on the SDK28
  compat flavor; failures here are release blockers.
- Default dev workspace `docker compose up --detach --build --remove-orphans`
  on device. This is intentionally heavier than the smoke test and is the gate
  for VS Code Server/Codex/Continue usability.
- Runtime performance bench:
  `bash scripts/android-runtime-bench.sh` for short direct syscall stats, and
  `bash scripts/android-runtime-bench.sh --apt-update` for the slow apt wall
  clock path. Optional existing proot comparison:
  `bash scripts/android-runtime-bench.sh --proot-cmd '<command>'`.
- GPU benchmark scenarios after GPU/runtime changes

Never mark a temporary workaround as complete unless the acceptance check for
the real behavior passes.
