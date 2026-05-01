# pdocker-android: implementation status

Snapshot at v0.5.2. This document covers (1) what pdockerd actually
implements today, (2) what works on the Android APK end-to-end, and
(3) the known gaps vs upstream Docker Engine.

## At a glance

| layer | size | status |
|---|---|---|
| **pdockerd** (Python single-file daemon, docker-proot-setup/bin) | 3500 LOC | Engine API 1.43-compatible, ~30 endpoints |
| **APK** (pdocker-android) | 31 MB | install + foreground service + ubuntu image pull + container run end-to-end on Android 15 |
| **Workspace UI** | native widgets + xterm.js 5.3 + JNI pty tabs + editor | Compose, Dockerfile, images, containers, and `-it`-style sessions share one console surface |

## Implementation overview

### 1. The "no kernel features" trick

Kernel namespaces, mount, cgroups, netlink, fuse — none of them are
available in either Termux+PRoot Ubuntu or Android app sandboxes. So
the entire stack is built on user-space replacements:

| upstream Docker bits | what we use instead |
|---|---|
| containerd snapshotter (overlayfs) | content-addressable layer pool + per-container hardlink-clone tree |
| overlayfs CoW | `libcow.so` LD_PRELOAD shim — break_hardlink on `open(O_WRONLY/RDWR/TRUNC)`, `truncate`, `chmod`, etc. |
| runc (kernel ns + cgroups) | nested `proot -0 -r` (ptrace-based path translation) |
| dockerd | pdockerd — Python HTTP server speaking Docker Engine API 1.43 over unix socket |
| BuildKit | legacy builder path (FROM/RUN/COPY/ADD/WORKDIR/ENV/ARG/CMD/ENTRYPOINT in pdockerd) |
| containerd image pull | `crane export` → tarball → Python tarfile extract |

### 2. Endpoint coverage in pdockerd

| Docker API path | status |
|---|---|
| `/_ping`, `/version`, `/info`, `/events` | ✓ |
| `/images/json`, `/images/{name}/json`, `/images/{name}/history` | ✓ |
| `/images/create` (pull) | ✓ via crane (works on Android via HTTP CONNECT proxy in pdockerd_bridge) |
| `/images/{name}` DELETE | ✓ |
| `/images/get`, `/images/load` (save/load) | ✓ |
| `/containers/json`, `/containers/{id}/json` | ✓ |
| `/containers/create`, `/containers/{id}/start`, `/stop`, `/kill`, `/wait` | ✓ |
| `/containers/{id}/wait?condition=removed` | ✓ (since 0.5.1) |
| `/containers/{id}/logs` | ✓ (file tail) |
| `/containers/{id}/attach` | ✓ HTTP/1.1 hijack — verified on Linux host; on Android the `docker run` (no `-d`) path that needs synchronous attach still flaky |
| `/containers/{id}/exec`, `/exec/{id}/start` | ✓ live multiplexed streaming |
| `/containers/{id}/archive` HEAD/GET/PUT | ✓ docker cp both directions |
| `/containers/{id}/stats` | ✓ /proc walking approximation (no cgroups) |
| `/networks/*`, `/volumes/*` | ✓ stubs that satisfy compose's create/list/connect flow |
| `HostConfig.DeviceRequests` / `--gpus` | experimental pdocker extension: Vulkan passthrough + CUDA-compatible API negotiation |
| `/build` | ✓ legacy builder; BuildKit not supported |
| `/auth` (registry login) | ✗ |
| `/swarm/*`, `/services/*`, `/secrets/*`, `/configs/*` | ✗ (Swarm-only) |
| `/system/df` | partial via `/info` |

### 3. End-to-end Android verification

What's been confirmed working on a physical Android 15 device (Pixel-class):

- `adb install pdocker-android.apk` → MainActivity launches → "Start pdockerd" → unix socket binds at `filesDir/pdocker/pdockerd.sock`
- `curl --unix-socket .../pdockerd.sock http://d/_ping` → `OK`
- `docker version` (CLI 29.4 client → pdockerd 0.1 server) → both sides report API 1.43
- `docker pull ubuntu:latest` → 132 MB image landed under `filesDir/pdocker/{images,layers}/` in 52s
- `docker create + start + wait + logs` → `hi-from-container` printed via /bin/echo inside ubuntu rootfs
- xterm.js WebView terminal → spawns sh with `PATH=runtime/docker-bin:...` and `DOCKER_HOST=unix://...` so user can type `docker ps` directly
- Terminal UTF-8 output is decoded through `TextDecoder`, uses an Android/CJK
  monospace font stack, disables IME autocorrect/capitalization, and reports
  resize changes from both window and visual viewport.
- Main UI → tabbed workspace for Overview, Compose, Dockerfile, Images,
  Containers, and Sessions. Tabs show widget-style state, counts, paths, and
  log previews in the native UI instead of immediately dropping into a console.
- Terminal UI → one screen can host multiple PTY-backed sessions and switch
  between them with tabs. `DOCKER_HOST` is prewired, which is the app-side
  equivalent of `docker run -it` / `docker exec -it` until Engine attach TTY is
  complete. Back navigation returns to the workspace without closing the
  terminal Activity, so live PTY tabs remain available when reopened.
- Main UI → Compose and Dockerfile tabs can create/edit project files through
  the in-app text editor under `filesDir/pdocker/projects`.
- First launch seeds `filesDir/pdocker/projects/default` from
  `assets/default-project/`, providing a Dockerfile/Compose workspace with
  code-server, Continue, OpenAI Codex CLI, and common dev tools.
- Main UI → "Browse image files" opens a read-only browser for pulled image
  rootfs trees under `filesDir/pdocker/images/*/rootfs`, so users can inspect
  image contents without starting a container or invoking the docker CLI

### 4. Android-specific workarounds (how we got here)

| problem | fix |
|---|---|
| crane (pure-Go) reads `/etc/resolv.conf` — Android app sandbox doesn't have it | in-process Python HTTP CONNECT proxy in pdockerd_bridge → `HTTPS_PROXY` env (uses bionic getaddrinfo) |
| Go x509 default certFiles list misses Android | `SSL_CERT_DIR=/system/etc/security/cacerts` |
| pdockerd's `/tmp` blob staging EACCES | `PDOCKER_TMP_DIR=runtime/tmp` |
| proot expects Termux tmpdir → noisy warnings | `TMPDIR=runtime/tmp` (since 0.5.2) |
| SELinux denies `link()` on app_data_file | `os.link` probe at startup → `PDOCKER_LINK_MODE=symlink` for tar extraction + clone |
| SELinux denies `setxattr` in security.* | `_copy_no_xattr()` substitute that drops xattr/flag copy |
| Files in app data have `exec_no_trans` SELinux deny | crane/proot/libcow shipped via jniLibs/arm64-v8a/lib*.so so they extract to `nativeLibraryDir` (the only exec-allowed location in app sandbox) |
| proot `libtalloc.so.2` SONAME doesn't fit `lib*.so` jniLibs filter | patchelf `--replace-needed` on proot + `--set-soname` on libtalloc to use the bare `libtalloc.so` form |
| proot fails execve "No such file or directory" without its loader | bundle Termux's `proot/loader` (18 KB static aarch64) and set `PROOT_LOADER` |
| bionic-built libcow.so won't load inside ubuntu (libdl.so vs libdl.so.2) | ship the host-glibc libcow build instead — pdockerd just `shutil.copy`s it into the container, container's own ld.so does the loading |
| proot cow_bind rollout | bundled self-built proot now advertises `--cow-bind`; `PDOCKER_USE_COW_BIND=1` switches pdockerd to lower/upper storage, with current syscall coverage limited to write-open copy-up |

### 5. Gaps vs upstream Docker

What mainline docker has that pdockerd doesn't:

#### Networking
- ✗ **Bridge networks with real IPs**: each container's IP = host's 127.0.0.1 (host network always). Means you can't simulate multi-container topologies where each container wants its own port 80.
- ✗ **iptables port forwarding** (`-p 8080:80`): no kernel hooks. `docker run -p` is silently ignored — container processes bind on the host (uid permitting).
- ✗ **DNS aliases via libnetwork**: we synthesize `/etc/hosts` for compose-network members, but no proper DNS server.
- ✗ **macvlan / ipvlan / overlay / Swarm networking**.

#### Storage / drivers
- ✗ **Real overlayfs**: container CoW is via libcow.so LD_PRELOAD, which only intercepts dynamically-linked binaries. Statically-linked binaries inside the container write through to the shared image rootfs.
- ◐ **proot cow_bind path**: pdockerd has the opt-in lower/upper plumbing (`PDOCKER_USE_COW_BIND=1`) and proot has minimal `open`/`openat`/`creat` copy-up. Whiteouts, rename, and metadata syscalls are still pending.
- ✗ **Volumes with native mount semantics**: bind mounts work via `proot -b`, but `docker volume` is a thin shim (per-name dir under `$PDOCKER_HOME/volumes/`).
- ✗ **tmpfs / devpts / proc / sys mounts**: proot binds `/proc`, `/sys`, `/dev` from host; no isolation.

#### Process
- ✗ **PID namespace**: `ps` inside container sees host processes. `docker stop $CID` sends SIGTERM only to the proot tracee tree, not via cgroup freezer.
- ✗ **User namespace remapping**: container processes run as the host app uid (no fake uid 0 unless you `proot -0`).
- ✗ **Capabilities, seccomp, AppArmor**: can't drop or grant — process inherits the app's sandbox profile.
- ✗ **cgroup limits** (`-m`, `--cpus`): no v1/v2 cgroup interface exposed.

#### Image / build
- ✗ **BuildKit, Buildx**: only legacy builder. No multi-stage caching, no `--platform` for cross-build, no `RUN --mount`.
- ✗ **Image signing / Notary / sigstore**: not implemented.
- ✗ **Login / private registries**: no `/auth`. Public registries only.
- ✗ **Layer compression formats**: gzip + plain tar yes, **zstd no** (raises explicitly).

#### Engine
- ✗ **Swarm mode**: no `/services`, `/secrets`, `/configs`, `/nodes`.
- ✗ **Plugins**: no plugin loader.
- ✗ **Live restore on pdockerd restart**: container procs are killed when pdockerd dies.
- ✗ **`docker stats` block I/O / network I/O numbers**: cpu% / mem only (no cgroup → no blkio counter).

#### CLI affordances
- ✗ `docker run --rm` with synchronous stdout streaming on Android (works via `create + start + wait + logs` workaround). Investigation in 0.5.2 — `wait?condition=removed` honored, but the attach hijack interaction with docker CLI 29.4 on the Android device path needs more work.
- ✗ `docker run -t` (TTY allocation): no PTY plumbing into the container side yet.
- ✗ `docker exec -it`: same.

### 6. What it can do that mainline docker can't

- Run as a normal Android app, no root, no Termux — single APK install on Android 8+ (API 26).
- Run on Termux+PRoot Ubuntu without dockerd (no namespace/mount/netlink kernel features).
- Pull and execute aarch64 Linux containers on a phone, with TLS DNS via the system resolver (HTTP CONNECT proxy in the daemon process).

## File map (Android APK)

```
pdocker-android/
├── app/src/main/
│   ├── AndroidManifest.xml           — INTERNET, ACCESS_NETWORK_STATE,
│   │                                   FOREGROUND_SERVICE_DATA_SYNC,
│   │                                   POST_NOTIFICATIONS, WAKE_LOCK,
│   │                                   REQUEST_IGNORE_BATTERY_OPTIMIZATIONS
│   ├── kotlin/io/github/ryo100794/pdocker/
│   │   ├── MainActivity.kt           — tabbed workspace + LocalSocket /_ping poll
│   │   ├── ImageFilesActivity.kt     — read-only browser for pulled image rootfs files
│   │   ├── TextEditorActivity.kt     — Compose/Dockerfile text editor
│   │   ├── PdockerdService.kt        — ForegroundService (dataSync), Chaquopy host
│   │   ├── PdockerdRuntime.kt        — extracts assets/pdockerd, symlinks
│   │   │                              nativeLibraryDir lib*.so into runtime/
│   │   ├── TerminalActivity.kt       — WebView host
│   │   ├── Bridge.kt                 — JS↔ pty bridge with DOCKER_HOST env
│   │   └── PtyNative.kt              — JNI wrapper around libpdockerpty.so
│   ├── cpp/
│   │   ├── pty.c                     — forkpty + TIOCSWINSZ + fd table
│   │   └── CMakeLists.txt            — unused (we build via build-native-termux.sh)
│   ├── jniLibs/arm64-v8a/             — auto-generated, .gitignored
│   │   ├── libcow.so                 — host-glibc CoW shim (loaded inside container)
│   │   ├── libcrane.so               — crane 0.20.3 (static Go)
│   │   ├── libdocker.so              — docker CLI 29.4 (stripped, 26 MB)
│   │   ├── libpdockerpty.so          — Termux-native build (Android JNI lib)
│   │   ├── libproot.so               — self-built proot + minimal --cow-bind, NEEDED patched -> libtalloc.so
│   │   ├── libproot-loader.so        — proot's static bootstrap loader
│   │   └── libtalloc.so              — Termux libtalloc 2.4.3, SONAME patched
│   ├── python/pdockerd_bridge.py     — Chaquopy entry: env setup + CONNECT proxy + runpy
│   └── assets/
│       ├── pdockerd/pdockerd         — 132 KB Python script (extracted on first launch)
│       └── xterm/{index.html,xterm.js,xterm.css,xterm-addon-fit.js}
├── docker-proot-setup/                — git submodule
│   └── bin/pdockerd                   — the actual daemon (3500 LOC)
└── scripts/
    ├── build-native-termux.sh         — Termux clang → libpdockerpty.so
    ├── copy-native.sh                 — submodule + vendor/ → jniLibs
    ├── fetch-xterm.sh                 — pull xterm.js + FitAddon CDN
    └── build-apk.sh                   — orchestrator
```
