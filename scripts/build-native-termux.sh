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

echo "==> Termux clang: $CLANG"
"$CLANG" --version | head -1
echo "==> target: $TARGET, sysroot: $SYSROOT"
echo

# strip_rpath: Termux clang auto-injects -rpath pointing at the host NDK
# sysroot; that path doesn't exist on Android and clutters readelf -d.
strip_rpath() {
    command -v patchelf >/dev/null 2>&1 && patchelf --remove-rpath "$1" || true
}

# ---- libcow.so ----
echo "==> building libcow.so"
"$CLANG" "${COMMON_FLAGS[@]}" \
    -o "$JNI_DIR/libcow.so" \
    "$SUB/src/overlay/libcow.c" \
    -ldl
strip_rpath "$JNI_DIR/libcow.so"
file "$JNI_DIR/libcow.so" | head -1

# ---- libpdockerpty.so ----
echo "==> building libpdockerpty.so"
"$CLANG" "${COMMON_FLAGS[@]}" \
    -o "$JNI_DIR/libpdockerpty.so" \
    "$ROOT/app/src/main/cpp/pty.c" \
    -llog
strip_rpath "$JNI_DIR/libpdockerpty.so"
file "$JNI_DIR/libpdockerpty.so" | head -1

# ---- mirror libcow.so back into the submodule's lib/ for parity with
#      `make android-arm64`, so tests depending on it still find it. ----
mkdir -p "$SUB/src/overlay"
cp "$JNI_DIR/libcow.so" "$SUB/src/overlay/libcow-android-arm64.so"

echo
echo "==> JNI .so ready:"
ls -la "$JNI_DIR"/
