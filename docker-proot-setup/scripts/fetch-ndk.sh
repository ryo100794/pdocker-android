#!/usr/bin/env bash
#
# fetch-ndk.sh — Download and extract the Android NDK for cross-compiling
# libcow.so against bionic libc (target = standalone APK on Android).
#
# Usage:
#   ./scripts/fetch-ndk.sh                       # default install: $HOME/android-ndk-r26d
#   ./scripts/fetch-ndk.sh /opt/ndk              # custom install path
#   NDK_VERSION=r27 ./scripts/fetch-ndk.sh       # different revision
#
# After completion, point the build at it:
#   export NDK="$HOME/android-ndk-r26d"
#   make -C src/overlay android
#
# The script is idempotent — if the toolchain already exists at the target
# path, it skips the download. A working clang from the toolchain is required;
# the script verifies it before declaring success.
#
set -euo pipefail

NDK_VERSION="${NDK_VERSION:-r26d}"
INSTALL_BASE="${1:-$HOME}"
NDK_DIR="$INSTALL_BASE/android-ndk-$NDK_VERSION"

ZIP_URL="https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux.zip"
ZIP_PATH="/tmp/android-ndk-$NDK_VERSION-linux.zip"

# ---- 1. Already installed? ----
TOOLCHAIN="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"
if [[ -x "$TOOLCHAIN/aarch64-linux-android24-clang" ]]; then
    echo "NDK already installed at: $NDK_DIR"
    echo "Verifying clang..."
    "$TOOLCHAIN/aarch64-linux-android24-clang" --version | head -1
    echo
    echo "To use:"
    echo "  export NDK=\"$NDK_DIR\""
    echo "  make -C src/overlay android-arm64"
    exit 0
fi

# ---- 2. Preflight ----
mkdir -p "$INSTALL_BASE"
for tool in wget unzip; do
    command -v "$tool" >/dev/null 2>&1 \
        || { echo "ABORT: $tool required (apt install $tool)"; exit 1; }
done

# Sanity: NDK is ~1.5GB extracted; check space
need_kb=$((1700 * 1024))
have_kb=$(df -k "$INSTALL_BASE" | awk 'NR==2 {print $4}')
if [[ "$have_kb" -lt "$need_kb" ]]; then
    echo "ABORT: need ~1.7GB free at $INSTALL_BASE, have $((have_kb/1024))MB"
    exit 1
fi

# ---- 3. Download (resumable) ----
if [[ ! -f "$ZIP_PATH" ]]; then
    echo "Downloading $ZIP_URL"
    echo "  → $ZIP_PATH (~1.5GB, this will take a while)"
    wget -c -O "$ZIP_PATH.partial" "$ZIP_URL"
    mv "$ZIP_PATH.partial" "$ZIP_PATH"
else
    echo "Using cached download: $ZIP_PATH"
fi

# ---- 4. Extract ----
echo "Extracting to $INSTALL_BASE..."
unzip -q -o "$ZIP_PATH" -d "$INSTALL_BASE"

# ---- 5. Verify ----
if [[ ! -x "$TOOLCHAIN/aarch64-linux-android24-clang" ]]; then
    echo "ABORT: extraction succeeded but clang missing at expected path"
    echo "  expected: $TOOLCHAIN/aarch64-linux-android24-clang"
    exit 1
fi

echo
echo "==== NDK $NDK_VERSION installed ===="
echo "Path: $NDK_DIR"
"$TOOLCHAIN/aarch64-linux-android24-clang" --version | head -1
echo
echo "Next step:"
echo "  export NDK=\"$NDK_DIR\""
echo "  make -C src/overlay android"
echo "  ls src/overlay/libcow-android-{arm64,x86_64}.so"
echo
echo "Run scripts/verify_all.sh — step 8 will exercise the cross-build."
