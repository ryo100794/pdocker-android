# Runtime strategy: retiring PRoot

Snapshot date: 2026-05-03.

For the API 29+ direct-execution feasibility boundary, see
[API29_DIRECT_EXEC_FEASIBILITY.md](API29_DIRECT_EXEC_FEASIBILITY.md). That note
tracks which low-level pieces are proved and which app-domain blockers remain
unproved.

The default APK and integrated backend no longer bundle PRoot, proot-loader, or
talloc. PRoot was the earlier execution backend because it gave an unprivileged
Android app a Linux rootfs view without `chroot`, mount namespaces, overlayfs,
or cgroups, but it is now treated only as a command-supplied diagnostic
comparison.

## Why remove it

- **License pressure**: PRoot is GPL-2.0-or-later. Shipping it inside the APK is
  possible, but every binary release must keep the corresponding source, local
  patches, and build recipe available. Removing it from the default APK lowers
  release and store-distribution risk.
- **Runtime ceiling**: PRoot is ptrace path translation. It is useful for path
  rewrite and fake root, but weak for Docker-level semantics such as cgroups,
  namespaces, TTY attach, signal supervision, network isolation, and exact
  overlayfs behavior.
- **Android 15 blocker**: on the Sony SOG15 test device (Android SDK 36,
  kernel 6.6.92-android15), PRoot can read tracee memory but fails when it
  must rewrite tracee memory for `execve`. Direct `proot -r ROOTFS /bin/sh`
  returns `execve(...): Bad address`; Dockerfile `RUN`/`docker run` may also
  surface `proot warning: signal 11`. Disabling seccomp does not change this.
- **Patch burden**: Extending PRoot with `--cow-bind` moves more project-specific
  syscall rewriting into a GPL binary. That makes the runtime harder to replace
  later and raises the cost of keeping source/binary correspondence exact.

## Android 15 execve test record

These checks are the current repeatable evidence for the P0 runtime blocker:

```sh
scripts/android-device-smoke.sh --quick --no-install
scripts/android-device-smoke.sh --no-install
```

Result on 2026-05-02:

- quick smoke passes: daemon starts, Engine API responds, Docker CLI can talk to
  `pdockerd`, and the staged native `pdocker-direct` helper passes its probe.
- full smoke fails at the first Dockerfile `RUN`, after image layer
  materialization, because the no-PRoot direct helper currently advertises
  `process-exec=0`. This is intentional until filesystem/syscall mediation can
  run rootfs processes without exposing the Android host filesystem.

Focused direct helper probe:

```sh
adb shell "run-as io.github.ryo100794.pdocker sh -c 'files/pdocker-runtime/docker-bin/pdocker-direct --pdocker-direct-probe'"
```

Observed output:

```text
pdocker-direct-executor:1
process-exec=0
```

## Current dependency points

| Area | Current PRoot usage | Removal target |
|---|---|---|
| container start/exec | none in default APK; compat uses `pdocker-direct` | broaden RuntimeBackend coverage |
| bind mounts | metadata/path mapping in pdockerd | backend-owned mount/path mapping |
| overlay experiment | historical patched PRoot path removed | independent lower/upper filesystem engine |
| DNS workaround for crane | no PRoot dependency | already moved to HTTP CONNECT proxy |
| APK packaging | no `libproot.so`, `libproot-loader.so`, or `libtalloc.so` | keep default payload clean |

## Build-time packaging

The default and compat APK staging paths omit PRoot payloads. Optional PRoot or
proot-like comparisons must be supplied as an existing command to benchmark
scripts; this repository must not download, build, or bundle them by default.

## Runtime backend switch

`pdockerd` now has an explicit runtime backend selector:

```sh
PDOCKER_RUNTIME_BACKEND=auto      # default direct/chroot selection
PDOCKER_RUNTIME_BACKEND=proot     # require explicit PDOCKER_RUNNER diagnostic
PDOCKER_RUNTIME_BACKEND=chroot    # Linux host fallback
PDOCKER_RUNTIME_BACKEND=no-proot  # Android direct backend metadata mode
```

The `no-proot` backend is intentionally visible before process execution is
complete on modern API levels. With it selected, `/info` reports
`Driver=pdocker-direct` and runtime operations return a clear diagnostic instead
of silently falling back to PRoot. The SDK28 compat flavor enables the direct
executor process path for real Dockerfile/Compose smoke tests.

Important: `no-proot` must not bind service ports or report a service as
running unless the requested container process is actually executing. Earlier
smoke-test placeholders were useful for UI plumbing, but they are not Docker
compatibility and should stay out of product behavior.

## Replacement plan

1. **Runtime backend boundary**
   - Backend selector and neutral driver reporting: **started**.
   - Add a fuller backend contract around `start`, `exec`, `stop`, `logs`,
     `archive`, and `mounts`.
   - Keep any external PRoot comparison outside the default repository payload.

2. **No-PRoot rootfs executor**
   - Use app-owned process spawning plus explicit filesystem mediation where
     possible.
   - Keep image/layer management in `pdockerd`; only process execution changes.
   - Treat Android app sandbox as the real isolation boundary.
   - First prototype should prove direct `fork/exec` of APK-shipped helper
     binaries, then add a syscall-hook/helper ABI for filesystem, port, and GPU
     mediation instead of depending on PRoot ptrace memory writes.

3. **Independent filesystem layer**
   - Move lower/upper/whiteout semantics out of patched PRoot.
   - Implement path operations and archive exchange in `pdockerd`/native code.
   - Keep `libcow` only as a compatibility fallback until write-path coverage is
     replaced.

4. **Optional external comparison**
   - If PRoot remains useful for profiling on older devices, pass an existing
     command to `scripts/android-runtime-bench.sh --proot-cmd ...`.
   - Do not commit or bundle PRoot/talloc/proot-loader artifacts without an
     explicit license and architecture decision.

## Acceptance criteria for default removal

- `bash scripts/build-apk.sh` produces an APK with no PRoot or talloc payload.
- `docker pull`, image listing, image file browsing, Dockerfile/Compose editing,
  and logs UI still work without PRoot.
- A replacement backend can run at least:
  - `docker create`
  - `docker start`
  - `docker wait`
  - `docker logs`
  - non-interactive `docker exec`
- Third-party notice asset no longer needs a PRoot GPL entry for the default
  APK build.
