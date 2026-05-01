#!/usr/bin/env bash
# build-proot.sh — cross-build the proot tracer for Android bionic so we
# can drop the Termux-package-derived blob and (next phase) layer our own
# cow_bind extension on top.
#
# Idempotent: clones into $WORKDIR, leaves the binaries at
# vendor/lib/{proot,proot-loader} ready for copy-native.sh to stage.
#
# Toolchain rationale: Termux native clang-21 is already aarch64-native
# on the host, with the NDK r26d sysroot we already keep around for
# build-native-termux.sh. No box64 emulation, no separate cross-gcc.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKDIR="${WORKDIR:-/tmp/proot-src}"
PROOT_REPO="${PROOT_REPO:-https://github.com/proot-me/proot}"
PROOT_REF="${PROOT_REF:-master}"

CLANG="${TERMUX_CLANG:-/data/data/com.termux/files/usr/bin/clang-21}"
NDK="${ANDROID_NDK_HOME:-/root/android-ndk-r26d}"
SYSROOT="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
TERMUX_USR="${TERMUX_USR:-/data/data/com.termux/files/usr}"

[[ -x "$CLANG" ]]      || { echo "missing clang: $CLANG" >&2; exit 1; }
[[ -d "$SYSROOT" ]]    || { echo "missing NDK sysroot: $SYSROOT" >&2; exit 1; }
[[ -f "$TERMUX_USR/include/talloc.h" ]] \
                       || { echo "missing talloc.h in Termux ($TERMUX_USR)" >&2; exit 1; }

mkdir -p "$WORKDIR"
if [[ ! -d "$WORKDIR/.git" ]]; then
    git clone --depth=1 --branch "$PROOT_REF" "$PROOT_REPO" "$WORKDIR"
fi

# Isolate just talloc.h. The full $TERMUX_USR/include tree leaks Termux
# stdio.h / signal.h / limits.h into the NDK sysroot and clang 21 trips
# on the conflicting nullability annotations + macro redefinitions.
INC="$WORKDIR/.proot-include"
mkdir -p "$INC"
cp "$TERMUX_USR/include/talloc.h" "$INC/talloc.h"

# Multi-word CC. make's `CC=` cmdline override doesn't survive parallel
# build with a quoted multi-arg compiler invocation; a wrapper flattens
# it into a single argv slot.
WRAPPER="$WORKDIR/.android-clang.sh"
cat > "$WRAPPER" <<EOF
#!/bin/sh
exec "$CLANG" \\
    --target=aarch64-linux-android24 \\
    --sysroot="$SYSROOT" \\
    "\$@"
EOF
chmod +x "$WRAPPER"

# tracee.c on master @5f780cb misses an explicit #include "tracee/mem.h"
# (clang 21 enforces -Werror=implicit-function-declaration). Patch in
# place, idempotent.
TRACEE_C="$WORKDIR/src/tracee/tracee.c"
if ! grep -q '"tracee/mem.h"' "$TRACEE_C"; then
    sed -i '/^#include "tracee\/reg.h"$/a #include "tracee/mem.h"' "$TRACEE_C"
fi

cd "$WORKDIR/src"

# Loader-only LDFLAGS. Default `-Ttext=$(LOADER_ADDRESS)` keeps the PHDR
# at 0x200000 and pushes .text to 0x2000000000 inside the same LOAD
# segment, which makes lld emit a 137 GB sparse executable. lld doesn't
# support -Ttext-segment (gnu-ld-only); use --image-base for the same
# effect (move the entire load segment up).
make -j"$(nproc)" \
  CC="$WRAPPER" \
  STRIP="$TERMUX_USR/bin/llvm-strip" \
  CFLAGS="-O2 -fPIC -I$INC -fuse-ld=lld --unwindlib=none -Wno-macro-redefined -Wno-error" \
  LDFLAGS="-L$TERMUX_USR/lib -ltalloc -fuse-ld=lld --unwindlib=none -Wl,-rpath-link,$TERMUX_USR/lib" \
  LOADER_LDFLAGS="-static -nostdlib -Wl,--image-base=0x2000000000,-z,noexecstack" \
  proot

# Stage. patchelf the SONAME of libtalloc.so.2 in proot's NEEDED list so
# the bundled jniLibs/arm64-v8a/libtalloc.so satisfies the link at load
# time (Android JNI native lib loader only ships files matching lib*.so
# without further suffix). Also drop the NDK sysroot RUNPATH leak.
cp "$WORKDIR/src/proot"          "$ROOT/vendor/lib/proot"
cp "$WORKDIR/src/loader/loader"  "$ROOT/vendor/lib/proot-loader"
chmod +x "$ROOT/vendor/lib/proot" "$ROOT/vendor/lib/proot-loader"
patchelf --replace-needed libtalloc.so.2 libtalloc.so "$ROOT/vendor/lib/proot"
patchelf --remove-rpath                                "$ROOT/vendor/lib/proot"

echo
echo "==> built proot $(ls -la "$ROOT/vendor/lib/proot" | awk '{print $5}') bytes"
echo "==> built proot-loader $(ls -la "$ROOT/vendor/lib/proot-loader" | awk '{print $5}') bytes"
readelf -d "$ROOT/vendor/lib/proot" | grep -E 'NEEDED|SONAME|RUNPATH'
