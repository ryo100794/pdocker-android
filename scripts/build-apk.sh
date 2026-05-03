#!/usr/bin/env bash
# build-apk.sh — end-to-end Android APK build from an aarch64 shell.
# Expects: JDK 17, gradle, Android cmdline-tools, NDK r26d.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

: "${ANDROID_HOME:=$HOME/android-sdk}"
: "${ANDROID_NDK_HOME:=$HOME/android-ndk-r26d}"
: "${PDOCKER_ANDROID_FLAVOR:=modern}"
export ANDROID_HOME ANDROID_NDK_HOME
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"

case "$PDOCKER_ANDROID_FLAVOR" in
    modern)
        GRADLE_TASK=":app:assembleModernDebug"
        APK="$ROOT/app/build/outputs/apk/modern/debug/app-modern-debug.apk"
        ;;
    compat)
        GRADLE_TASK=":app:assembleCompatDebug"
        APK="$ROOT/app/build/outputs/apk/compat/debug/app-compat-debug.apk"
        ;;
    *)
        echo "ABORT: PDOCKER_ANDROID_FLAVOR must be 'modern' or 'compat' (got '$PDOCKER_ANDROID_FLAVOR')" >&2
        exit 2
        ;;
esac

# Build libcow.so + libpdockerpty.so natively with Termux aarch64 clang.
# Bypasses the x86_64-only NDK toolchain (which would need box64 emulation
# on aarch64 hosts). Output goes directly to app/src/main/jniLibs/arm64-v8a/.
bash scripts/build-native-termux.sh

# External PRoot is not part of the default or compat runtime. The SDK28
# compatibility APK uses the scratch pdocker-direct executor path so the
# RuntimeBackend switch remains usable without adding a GPL/PRoot payload.
echo "==> skipping external proot build (pdocker-direct compat runtime)"

# Stage integrated backend assets (crane/docker, pdockerd python tree).
bash scripts/copy-native.sh

# Gradle build. Use the checked-in wrapper so the included :app project and
# Android Gradle Plugin versions are resolved consistently.
./gradlew "$GRADLE_TASK" --no-daemon

if [[ -f "$APK" ]]; then
    echo
    echo "APK: $APK"
    ls -lh "$APK"
else
    echo "APK missing — build failed" >&2
    exit 1
fi
