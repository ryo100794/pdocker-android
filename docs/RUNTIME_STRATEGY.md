# Runtime strategy: retiring PRoot

Snapshot date: 2026-05-01.

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
- **Patch burden**: Extending PRoot with `--cow-bind` moves more project-specific
  syscall rewriting into a GPL binary. That makes the runtime harder to replace
  later and raises the cost of keeping source/binary correspondence exact.

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
PDOCKER_WITH_PROOT=0 bash scripts/build-apk.sh
```

This is an experimental packaging mode. Without a replacement runtime,
container start/exec will not be feature-complete on Android. It exists so the
APK distribution shape can be tested while the backend replacement lands.

## Replacement plan

1. **Runtime backend boundary**
   - Add a backend contract around `start`, `exec`, `stop`, `logs`, `archive`,
     and `mounts`.
   - Keep `proot` as one backend, not the daemon's identity.
   - Rename public reporting from `proot-cow` to a neutral runtime/driver once
     a second backend exists.

2. **No-PRoot rootfs executor**
   - Use app-owned process spawning plus explicit filesystem mediation where
     possible.
   - Keep image/layer management in `pdockerd`; only process execution changes.
   - Treat Android app sandbox as the real isolation boundary.

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

