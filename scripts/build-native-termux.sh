#!/usr/bin/env bash
# build-native-termux.sh — build libcow.so + libpdockerpty.so for
# arm64-v8a using Termux's native aarch64 clang + the NDK sysroot.
#
# Rationale: the host is aarch64 but the NDK toolchain binaries are
# linux-x86_64 ELFs. Routing every compiler invocation through box64
# emulation is slow and memory-hungry. Termux ships a native aarch64
# clang with an android24 target, so point it at the NDK sysroot
# (bionic headers + libs) to get NDK-equivalent cross-build output
# without emulation.
#
# Produces:
#   app/src/main/jniLibs/arm64-v8a/libcow.so
#   app/src/main/jniLibs/arm64-v8a/libpdockerpty.so
#   app/src/main/jniLibs/arm64-v8a/libpdockerdirect.so
#   app/src/main/jniLibs/arm64-v8a/libpdockergpuexecutor.so
#
# These are packaged as-is by the gradle build — the APK's
# externalNativeBuild (CMake) block has been removed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUB="$ROOT/docker-proot-setup"

# Termux native clang (aarch64 ELF, interpreter /system/bin/linker64).
# This is the same binary Termux users invoke from their shell.
CLANG="${TERMUX_CLANG:-/data/data/com.termux/files/usr/bin/clang-21}"
if [[ ! -x "$CLANG" ]]; then
    # Fall back to the versioned driver script if clang-21 is named
    # differently on a future Termux.
    CLANG="$(ls /data/data/com.termux/files/usr/bin/clang-[0-9]* 2>/dev/null \
             | sort -V | tail -1)"
    [[ -x "$CLANG" ]] || { echo "ABORT: Termux native clang not found" >&2; exit 1; }
fi

NDK="${ANDROID_NDK_HOME:-/root/android-ndk-r26d}"
SYSROOT="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
if [[ ! -d "$SYSROOT/usr/include" ]]; then
    echo "ABORT: NDK sysroot missing ($SYSROOT)" >&2
    exit 1
fi

ABI="arm64-v8a"
TARGET="aarch64-linux-android24"
JNI_DIR="$ROOT/app/src/main/jniLibs/$ABI"
mkdir -p "$JNI_DIR"

# --sysroot points Termux clang at bionic headers/libs from the NDK.
# --target pins ABI to android24 so produced .so loads on Android 8+.
# -U_FORTIFY_SOURCE mirrors the submodule Makefile (avoid __*_chk refs).
# -fuse-ld=lld uses Termux's lld which is also aarch64 native.
# --unwindlib=none skips Termux's default `-l:libunwind.a` link flag —
# Termux's clang-rt for aarch64-android doesn't ship libunwind.a, and
# C sources don't need C++ exception unwinding anyway.
COMMON_FLAGS=(
    --target="$TARGET"
    --sysroot="$SYSROOT"
    -fPIC -O2
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -U_FORTIFY_SOURCE
    -fuse-ld=lld
    --unwindlib=none
    -shared
)

EXEC_FLAGS=(
    --target="$TARGET"
    --sysroot="$SYSROOT"
    -fPIE -pie -O2
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
    -U_FORTIFY_SOURCE
    -fuse-ld=lld
    --unwindlib=none
)

echo "==> Termux clang: $CLANG"
"$CLANG" --version | head -1
echo "==> target: $TARGET, sysroot: $SYSROOT"
echo

# strip_rpath: Termux clang auto-injects -rpath pointing at the host NDK
# sysroot; that path doesn't exist on Android and clutters readelf -d.
strip_rpath() {
    command -v patchelf >/dev/null 2>&1 && patchelf --remove-rpath "$1" || true
}

# NOTE: libcow.so is intentionally NOT built here. It's LD_PRELOAD'd
# inside the container's rootfs (typically glibc or musl), where a
# bionic-targeted shim fails to load (libdl.so vs libdl.so.2 NEEDED
# mismatch + ld-linux-* vs ld-android-* interpreter). copy-native.sh
# stages docker-proot-setup/lib/libcow.so (host glibc build) into
# jniLibs, which loads cleanly inside ubuntu/debian containers.

# ---- libpdockerpty.so ----
echo "==> building libpdockerpty.so"
"$CLANG" "${COMMON_FLAGS[@]}" \
    -o "$JNI_DIR/libpdockerpty.so" \
    "$ROOT/app/src/main/cpp/pty.c" \
    -llog
strip_rpath "$JNI_DIR/libpdockerpty.so"
file "$JNI_DIR/libpdockerpty.so" | head -1

# ---- libpdockerdirect.so ----
# Android packaging only extracts native files named lib*.so, but this
# one is intentionally an executable PIE renamed to .so. Kotlin symlinks
# it as docker-bin/pdocker-direct and pdockerd probes it via execve.
echo "==> building libpdockerdirect.so"
"$CLANG" "${EXEC_FLAGS[@]}" \
    -o "$JNI_DIR/libpdockerdirect.so" \
    "$ROOT/app/src/main/cpp/pdocker_direct_exec.c"
strip_rpath "$JNI_DIR/libpdockerdirect.so"
file "$JNI_DIR/libpdockerdirect.so" | head -1

# ---- libpdockergpuexecutor.so ----
# Android packaging only extracts native files named lib*.so. This is an
# executable PIE renamed to .so, then symlinked as gpu/pdocker-gpu-executor.
echo "==> building libpdockergpuexecutor.so"
"$CLANG" "${EXEC_FLAGS[@]}" \
    -o "$JNI_DIR/libpdockergpuexecutor.so" \
    "$ROOT/app/src/main/cpp/pdocker_gpu_executor.c" \
    -lEGL -lGLESv3 -llog -ldl -lm
strip_rpath "$JNI_DIR/libpdockergpuexecutor.so"
file "$JNI_DIR/libpdockergpuexecutor.so" | head -1

echo
echo "==> JNI .so ready:"
ls -la "$JNI_DIR"/
