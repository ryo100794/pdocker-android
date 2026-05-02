#!/usr/bin/env bash
# build-apk.sh — end-to-end build from PRoot Ubuntu aarch64.
# Expects: JDK 17, gradle, Android cmdline-tools, NDK r26d.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

: "${ANDROID_HOME:=$HOME/android-sdk}"
: "${ANDROID_NDK_HOME:=$HOME/android-ndk-r26d}"
export ANDROID_HOME ANDROID_NDK_HOME
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"

# Ensure submodule is populated.
git submodule update --init --recursive

# Build libcow.so + libpdockerpty.so natively with Termux aarch64 clang.
# Bypasses the x86_64-only NDK toolchain (which would need box64 emulation
# on aarch64 hosts). Output goes directly to app/src/main/jniLibs/arm64-v8a/.
bash scripts/build-native-termux.sh

# Build the self-contained Android proot binary, including pdocker's
# minimal --cow-bind extension patch. PRoot is now opt-in because it is
# blocked on current Android devices and carries GPL/talloc payload. Set
# PDOCKER_WITH_PROOT=1 to produce a legacy diagnostic APK while the
# replacement runtime is being developed.
if [[ "${PDOCKER_WITH_PROOT:-0}" != "0" ]]; then
    bash scripts/build-proot.sh
else
    echo "==> skipping proot build (PDOCKER_WITH_PROOT=0)"
fi

# Stage submodule assets (crane, optional proot, pdockerd python tree).
bash scripts/copy-native.sh

# Gradle build. Use the checked-in wrapper so the included :app project and
# Android Gradle Plugin versions are resolved consistently.
./gradlew :app:assembleDebug --no-daemon

APK="$ROOT/app/build/outputs/apk/debug/app-debug.apk"
if [[ -f "$APK" ]]; then
    echo
    echo "APK: $APK"
    ls -lh "$APK"
else
    echo "APK missing — build failed" >&2
    exit 1
fi
