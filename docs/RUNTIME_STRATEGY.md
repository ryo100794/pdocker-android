# Runtime strategy: retiring PRoot

Snapshot date: 2026-05-02.

PRoot is the current execution backend because it gives an unprivileged Android
app a usable Linux rootfs view without `chroot`, mount namespaces, overlayfs, or
cgroups. It should not remain the long-term core runtime.

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

Focused direct runner probe:

```sh
adb shell "run-as io.github.ryo100794.pdocker sh -c 'cd files; R=\$PWD/pdocker-runtime; ROOT=\$(find \$PWD/pdocker/containers -maxdepth 2 -path \"*/rootfs\" | tail -1); export LD_LIBRARY_PATH=\$R/lib PROOT_LOADER=\$R/docker-bin/proot-loader TMPDIR=\$R/tmp PROOT_TMP_DIR=\$R/tmp PROOT_NO_SECCOMP=1; \$R/docker-bin/proot -v 9 -0 -r \$ROOT -b /proc -b /sys -b /dev -w / /bin/sh -c \"echo direct-ok\"; echo rc=\$?'"
```

Observed failure:

- `translate_execve_enter()` reaches the first `execve`.
- PRoot returns `-EFAULT` before loader handoff.
- Experiments that moved the cached stack pointer first, and experiments that
  attempted shorter in-place loader path rewriting, still failed. This points
  at tracee memory write restrictions rather than only path length, tmpdir, or
  seccomp.

## Current dependency points

| Area | Current PRoot usage | Removal target |
|---|---|---|
| container start/exec | `pdockerd` builds `proot -0 -r ROOTFS ...` argv | RuntimeBackend interface |
| bind mounts | `proot -b host:guest` | backend-owned mount/path mapping |
| overlay experiment | patched `proot --cow-bind upper:lower:/` | independent lower/upper filesystem engine |
| DNS workaround for crane | formerly considered PRoot bind of `resolv.conf` | already moved to HTTP CONNECT proxy |
| APK packaging | `libproot.so`, `libproot-loader.so`, `libtalloc.so` | optional payload, then removed default |

## Build-time switch

The APK can now be staged without PRoot payloads:

```sh
PDOCKER_WITH_PROOT=0 bash scripts/build-apk.sh  # default no-PRoot payload
PDOCKER_WITH_PROOT=1 bash scripts/build-apk.sh  # legacy diagnostic payload
```

This is an experimental packaging mode. Without a replacement runtime,
container start/exec will not be feature-complete on Android. It exists so the
APK distribution shape can be tested while the backend replacement lands.

## Runtime backend switch

`pdockerd` now has an explicit runtime backend selector:

```sh
PDOCKER_RUNTIME_BACKEND=auto      # default: PRoot when available, else chroot
PDOCKER_RUNTIME_BACKEND=proot     # require PRoot
PDOCKER_RUNTIME_BACKEND=chroot    # Linux host fallback
PDOCKER_RUNTIME_BACKEND=no-proot  # Android direct backend metadata mode
```

The `no-proot` backend is intentionally visible before process execution is
complete. With it selected, `/info` reports `Driver=pdocker-direct` and runtime
operations return a clear "not implemented yet" diagnostic instead of silently
falling back to PRoot. This lets the APK and tests exercise the PRoot-free
packaging shape while image pull, image browsing, Compose/Dockerfile editing,
and metadata workflows continue to use the Engine API.

Important: `no-proot` must not bind service ports or report a service as
running unless the requested container process is actually executing. Earlier
smoke-test placeholders were useful for UI plumbing, but they are not Docker
compatibility and should stay out of product behavior.

## Replacement plan

1. **Runtime backend boundary**
   - Backend selector and neutral driver reporting: **started**.
   - Add a fuller backend contract around `start`, `exec`, `stop`, `logs`,
     `archive`, and `mounts`.
   - Keep `proot` as one backend, not the daemon's identity.

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

4. **Optional compatibility plugin**
   - If PRoot remains useful for older devices or dev builds, ship it as an
     opt-in build flavor or external add-on with clear GPL source links.
   - Default APK target should not contain `libproot.so`, `libproot-loader.so`,
     or `libtalloc.so`.

## Acceptance criteria for default removal

- `PDOCKER_WITH_PROOT=0 bash scripts/build-apk.sh` produces an APK with no PRoot
  or talloc payload.
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
