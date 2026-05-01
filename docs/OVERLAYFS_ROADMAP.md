# Real overlayfs roadmap

Status of the "replace LD_PRELOAD libcow with kernel-equivalent
overlayfs semantics" effort. As of v0.5.2.

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

## Phase 1: own the proot build (in progress)

Replace the bundled Termux proot binary with one we build ourselves
from upstream source (https://github.com/proot-me/proot), using the
same Termux clang + NDK sysroot toolchain that already builds
`libpdockerpty.so`.

**Build environment hand-rolled at /tmp/proot-src/:**
- Source: `git clone --depth=1 https://github.com/proot-me/proot`
- Toolchain: Termux native `clang-21` with `--target=aarch64-linux-android24 --sysroot=$NDK_SYSROOT`
- talloc: header copied from Termux into `/tmp/proot-include/talloc.h`,
  link against the same `vendor/lib/libtalloc.so.2` we already ship
- Wrapper script `/tmp/android-clang.sh` so make's `CC=` cmdline
  override survives the multi-word toolchain invocation

**Hurdles cleared so far:**
- Termux include leakage when using `-I/data/data/com.termux/files/usr/include` (talloc.h pulled in stdio.h etc with conflicting nullability annotations) — fixed by isolating just `talloc.h`.
- `make -j4` not propagating CC properly with multi-word values — fixed via wrapper script + cmdline `CC=` override.
- Strict implicit-function-declaration error in `tracee/tracee.c` (clang 21 vs proot's expected glibc) — fixed by adding `#include "tracee/mem.h"`.

**Hurdle in progress:**
- The `loader.elf` strip step (`OBJIFY` macro) fails with a corrupt buffer-size header — the loader binary is supposed to be a tiny static aarch64 stub, but the link with our LDFLAGS includes too much. Need to scope `loader/loader` link to only its two .o (no libtalloc, no NDK CRT). Probably split LDFLAGS so the loader uses a leaner set.

## Phase 2 (pending Phase 1): cow_bind extension

Once we build proot ourselves:

1. Add a new extension under `src/extension/cow_bind/cow_bind.c`,
   modeled on `extension/link2symlink/link2symlink.c` (which
   intercepts `link()`/`symlink()` to convert hardlinks into
   symlinks at syscall layer — already a great precedent).
2. CLI surface: `proot --cow-bind upper:lower:guest_path` where
   `lower` is read-only, `upper` is the writable layer, `guest_path`
   is what the tracee sees.
3. Hooks needed on syscall entry:
   - `SYS_open`/`SYS_openat` with `O_WRONLY|O_RDWR|O_TRUNC|O_CREAT`
   - `SYS_truncate`/`SYS_truncate64`
   - `SYS_chmod`/`SYS_fchmodat`/`SYS_chown`/`SYS_lchown`/`SYS_fchownat`
   - `SYS_setxattr`/`SYS_lsetxattr`/`SYS_removexattr`
   - `SYS_unlink`/`SYS_unlinkat`/`SYS_rename`/`SYS_renameat`
4. For each: resolve target via overlay state. If target is in lower
   only and op is write: copy lower → upper (preserving mode/owner/
   xattr — same handling as our current `_copy_no_xattr` scope), then
   rewrite the path syscall arg to upper. If target already in upper:
   forward as-is. Whiteouts: keep a `.wh.<name>` marker in upper for
   `unlink`, treat `lower/<name>` as gone when reading.

5. pdockerd: stop calling `_install_libcow` and `materialize_container_rootfs` for hardlink cloning. Instead,
   pass `--cow-bind` args to proot pointing at `images/<tag>/rootfs`
   (lower) and `containers/<id>/upper` (upper). Containers no longer
   own a full clone of the rootfs — they own only their upper diff.
   Image storage drops by N× for N containers per image.

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
