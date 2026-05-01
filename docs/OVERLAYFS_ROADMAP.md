# Real overlayfs roadmap

Status of the "replace LD_PRELOAD libcow with kernel-equivalent
overlayfs semantics" effort. As of v0.5.2 + Phase 1 complete (commit
`1b0bb98`).

**Phase status:**
- Phase 1 (own proot build) — **DONE**
- Phase 2a (pdockerd cow_bind client gate) — **DONE in docker-proot-setup**
- Phase 2b (proot cow_bind extension) — design fixed, implementation next
- Phase 3 (retire libcow) — pending Phase 2 verification

## Why the current approach is incomplete

`libcow.so` is `LD_PRELOAD`'d into every dynamically-linked binary
inside the container rootfs. It hooks `open(O_WRONLY|O_RDWR|O_TRUNC)`,
`truncate`, `chmod`, `chown`, `setxattr`, `unlink`, `rename` etc., and
on first write to a hardlinked file it does `mkstemp` + content copy +
`rename` to break the hardlink — leaving the lower (image) layer
inode untouched.

**The hole**: `LD_PRELOAD` only kicks in when the dynamic linker
loads the shim. **Statically-linked binaries skip it entirely** and
write straight through the hardlink chain into the shared image
rootfs. That includes:

- Most Go binaries (CGO_ENABLED=0)
- musl-distro pre-built tools
- Any single-binary CLI built with `gcc -static`

When such a binary writes inside one container, every other container
(and the image itself) sees the change. That's not overlayfs.

## What "real overlayfs" looks like on this stack

We can't get the kernel to do `mount -t overlay` on an Android app
sandbox (CAP_SYS_ADMIN missing, `/dev/fuse` denied, user namespaces
denied — already verified). The only general-purpose mechanism we
have is **proot's syscall interception**, which already runs around
every container process.

Plan: **add a `cow_bind` extension to proot itself**, so the same
mechanism that does path translation also performs copy-up + write
redirection — the static-binary case included automatically.

```
container process              proot tracee                    host fs
    │                              │                              │
    │ open("/etc/hosts", O_WRONLY) │                              │
    ├─────── PTRACE_SYSCALL ──────►│ resolve via overlay state    │
    │                              │   lower:  image_rootfs/etc/  │
    │                              │   upper:  container_upper/etc│
    │                              │                              │
    │                              │ if upper missing AND write:  │
    │                              │   copy lower → upper         │
    │                              │ rewrite path → upper         │
    │                              │ ─── continue syscall ───────►│
    │                              │                              ▼
    │                              │                  upper/etc/hosts
```

Key insight: proot already does step 2 (path translation). We just
need step 2.5 (copy-up before write-class syscalls).

## Phase 1: own the proot build — DONE (commit `1b0bb98`)

Replaced the bundled Termux proot binary with a self-built one from
upstream `https://github.com/proot-me/proot` @ `5f780cb`, using the
same Termux clang + NDK r26d sysroot toolchain that already builds
`libpdockerpty.so`. No box64, no separate cross-gcc.

**Reproducible recipe** lives at [scripts/build-proot.sh](../scripts/build-proot.sh).
Key choices baked in:

- Source: `git clone --depth=1 https://github.com/proot-me/proot` into `/tmp/proot-src`
- Toolchain: Termux native `clang-21` with `--target=aarch64-linux-android24 --sysroot=$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot`
- talloc: only `talloc.h` is copied into an isolated include dir
  (`$WORKDIR/.proot-include/`); the full Termux include tree leaks
  `stdio.h`/`signal.h`/`limits.h` with conflicting nullability annotations
- Wrapper script (`$WORKDIR/.android-clang.sh`) flattens the multi-arg
  clang invocation into a single argv slot; without it, `make`'s `CC=`
  cmdline override is split apart on parallel build
- patchelf post-link: `--replace-needed libtalloc.so.2 libtalloc.so` +
  `--remove-rpath` so the bundled `vendor/lib/libtalloc.so` (renamed
  for the Android JNI `lib*.so` filter) satisfies the link

**Hurdles cleared:**
- Termux include leakage (nullability/macro conflict with NDK sysroot) → isolate `talloc.h` only
- `make CC=...` losing multi-word args under `-j` → wrapper script
- clang 21 strict mode rejects `tracee/tracee.c`'s undeclared `peek_word()` → idempotent `sed` patch adds `#include "tracee/mem.h"`
- Loader linked into a 137 GB sparse file when `LOADER_LDFLAGS=-Ttext=0x2000000000` (lld places PHDR at 0x200000, `.text` at 0x2000000000 in the same LOAD segment, NUL-fills the gap; lld doesn't support gnu-ld's `-Ttext-segment`) → use `-Wl,--image-base=0x2000000000` to move the entire LOAD segment up

**Output:**
- [vendor/lib/proot](../vendor/lib/proot) — 173 KB self-built (replaces 214 KB Termux blob)
- [vendor/lib/proot-loader](../vendor/lib/proot-loader) — 2032 byte static aarch64 stub
- `NEEDED libtalloc.so + libdl.so + libc.so`, no RUNPATH leak

## Phase 2: cow_bind extension (next)

Now that proot is ours, add a new extension under
`src/extension/cow_bind/cow_bind.c`, modeled on
`extension/link2symlink/link2symlink.c` (which intercepts
`link()`/`symlink()` and rewrites them at the syscall layer — exactly
the same shape we need).

### CLI surface

```
proot --cow-bind upper:lower:guest_path ...
```

- `lower` (host abs path): read-only layer, e.g. `images/<tag>/rootfs`
- `upper` (host abs path): writable layer, e.g. `containers/<id>/upper`
- `guest_path` (guest abs path): mount point inside the tracee view

INITIALIZATION parses the triple, calls `bind_path(lower, guest_path)`
to expose the read-only layer at the guest mount point, and stashes
`(upper, lower)` in `extension->config` for the syscall hooks.

### Extension API plumbing (proot codebase touch points)

- `src/extension/cow_bind/cow_bind.c` — new file
- `src/extension/extension.h` — add `extern int cow_bind_callback(...)`
- `src/cli/proot.c` — add `handle_option_cow_bind` that calls
  `initialize_extension(tracee, cow_bind_callback, value)`
- `src/cli/proot.h` — add a `--cow-bind` option entry
- `src/GNUmakefile` — add `extension/cow_bind/cow_bind.o` to `OBJECTS`

### Syscalls hooked (FILTER_SYSEXIT mask in INITIALIZATION)

Write-class (need copy-up + path rewrite):
- `PR_open`, `PR_openat` (when flags include `O_WRONLY | O_RDWR | O_TRUNC | O_CREAT`)
- `PR_creat`
- `PR_truncate`, `PR_truncate64` (`PR_ftruncate*` use fd, no copy-up — covered by the open-time redirect)
- `PR_chmod`, `PR_fchmodat`
- `PR_chown`, `PR_lchown`, `PR_fchownat`, `PR_chown32`, `PR_lchown32`
- `PR_setxattr`, `PR_lsetxattr` (skip on Android — SELinux denies anyway; only copy-up + ENOENT-translate the deny)
- `PR_removexattr`, `PR_lremovexattr`
- `PR_unlink`, `PR_unlinkat` (whiteout in upper)
- `PR_rename`, `PR_renameat`, `PR_renameat2` (copy-up source if in lower, target always in upper)
- `PR_mkdir`, `PR_mkdirat`, `PR_symlink`, `PR_symlinkat`, `PR_link`, `PR_linkat` (always create in upper)

Read-class (need read redirect from upper if upper has a copy):
- `PR_open`/`PR_openat` (read-only flags) — TRANSLATED_PATH callback

### Per-syscall logic

For each hooked syscall, in `SYSCALL_ENTER_END`:

1. `read_string(tracee, path, peek_reg(tracee, CURRENT, SYSARG_x), PATH_MAX)`
   gives the host path PRoot already canonicalized.
2. If `path` does not start with `lower/`, return 0 (passthrough).
3. Compute `upper_path = upper + (path - lower)`.
4. If write-class:
   - Ensure `dirname(upper_path)` exists (mkdir -p, mode from lower
     parent if visible)
   - If `upper_path` doesn't exist, copy `path` (the lower file) →
     `upper_path` (preserve mode + mtime; **drop xattr** — same
     `_copy_no_xattr` semantics we already use in pdockerd, because
     SELinux on Android denies `security.*` setxattr)
   - `set_sysarg_path(tracee, upper_path, SYSARG_x)` so the kernel
     sees the rewritten path
5. If read-class via TRANSLATED_PATH:
   - If `upper_path` exists, rewrite `(char *) data1` to `upper_path`

### Whiteouts

`unlink`/`unlinkat` of a path that lives only in lower: create a
`.wh.<basename>` marker file in `upper_path`'s parent dir. Treat the
target as ENOENT for any subsequent translate (TRANSLATED_PATH check).

OCI/overlayfs uses `char-dev 0/0` for whiteouts; we use a regular
file marker because `mknod(S_IFCHR)` fails in app sandbox.

### pdockerd integration (Phase 2 client side) — DONE

- New env var: `PDOCKER_USE_COW_BIND=1` now switches container create/start
  into cow_bind mode **only if** the selected runner advertises
  `--cow-bind` in `proot --help`; otherwise pdockerd logs a warning and
  falls back to the current hardlink-clone-then-libcow path.
- cow_bind mode records `Storage.Mode=cow_bind`,
  `Storage.LowerDir=images/<tag>/rootfs`, and
  `Storage.UpperDir=containers/<id>/upper` in `state.json`.
- Runtime files (`etc/hosts`, `etc/resolv.conf`, network host injection)
  are pre-seeded into the upperdir so the pristine image rootfs stays
  read-only.
- `build_run_argv` passes `proot --cow-bind upper:lower:/` and suppresses
  `LD_PRELOAD=/.libcow.so` for cow_bind containers.
- Per-container disk = `containers/<id>/upper` only; image rootfs
  shared read-only across containers

Remaining Phase 2 work is the proot extension itself (`src/extension/cow_bind/`).

## Phase 3 (after Phase 2 lands): retire libcow

- Drop `libcow.so` from the APK.
- Remove `_install_libcow` from pdockerd.
- Remove `_clone_tree` / `_hardlink_clone_tree` (image rootfs is now
  a pristine read-only layer; no per-container materialization).
- The `PDOCKER_LINK_MODE=symlink` workaround for SELinux link()
  becomes irrelevant — proot does the cloning at syscall level using
  open+write loops, which always succeed regardless of SELinux link()
  policy.

## Files of interest right now

- `/tmp/proot-src/` — upstream proot @ `5f780cb` (cloned in this session)
- `/tmp/android-clang.sh` — multi-word CC wrapper for make
- `/tmp/proot-include/talloc.h` — copied from Termux to avoid include leakage
- `/root/tl/pdocker-android/vendor/lib/{libtalloc.so.2, proot, proot-loader}` — the Termux-derived blobs we currently bundle, will be replaced once Phase 1 lands
- `/root/tl/docker-proot-setup/src/overlay/libcow.c` — what we're aiming to retire after Phase 3

## Why this is worth the effort

Static-binary CoW correctness is the difference between "container
isolation that mostly works" and "container isolation that actually
matches Docker's contract". Without it, the moment a user runs a Go
or musl-static container that mutates a path, every other container
sharing the same image gets that mutation — silent data corruption
across containers. Catching this at the proot syscall layer gets us
to Docker-equivalent semantics on a phone with no kernel support.
