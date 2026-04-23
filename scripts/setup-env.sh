#!/usr/bin/env bash
# setup-env.sh — one-shot bootstrap for a fresh Termux+PRoot Ubuntu aarch64.
# Installs JDK 17, gradle, adb, downloads Android SDK cmdline-tools and
# the NDK. Idempotent.
set -euo pipefail

SDK="${ANDROID_HOME:-$HOME/android-sdk}"
NDK="${ANDROID_NDK_HOME:-$HOME/android-ndk-r26d}"

if ! command -v javac >/dev/null; then
    echo "==> apt install openjdk-17-jdk-headless gradle adb unzip wget"
    DEBIAN_FRONTEND=noninteractive apt install -y \
        openjdk-17-jdk-headless gradle adb unzip wget
fi

if [[ ! -x "$SDK/cmdline-tools/latest/bin/sdkmanager" ]]; then
    echo "==> downloading Android cmdline-tools"
    mkdir -p "$SDK/cmdline-tools"
    cd "$SDK/cmdline-tools"
    wget -q https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip \
        -O cmdline-tools.zip
    unzip -q cmdline-tools.zip
    rm -f cmdline-tools.zip
    rm -rf latest
    mv cmdline-tools latest
fi

export ANDROID_HOME="$SDK"
export PATH="$SDK/cmdline-tools/latest/bin:$SDK/platform-tools:$PATH"

yes 2>/dev/null | sdkmanager --licenses >/dev/null
sdkmanager \
    "platform-tools" \
    "platforms;android-34" \
    "build-tools;34.0.0"

if [[ ! -d "$NDK" ]]; then
    echo "==> fetching NDK"
    bash "$(dirname "$0")/../docker-proot-setup/scripts/fetch-ndk.sh"
fi

echo
echo "Setup done. Export these in your shell:"
echo "  export ANDROID_HOME=$SDK"
echo "  export ANDROID_NDK_HOME=$NDK"
echo "  export PATH=\$ANDROID_HOME/cmdline-tools/latest/bin:\$ANDROID_HOME/platform-tools:\$PATH"
