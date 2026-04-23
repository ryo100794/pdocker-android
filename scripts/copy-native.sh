#!/usr/bin/env bash
# copy-native.sh — stage native binaries and pdockerd python tree from the
# docker-proot-setup submodule into app/src/main/{assets,python,jniLibs}/.
# Run this before `gradle assembleDebug`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUB="$ROOT/docker-proot-setup"
APP="$ROOT/app/src/main"

if [[ ! -d "$SUB" ]]; then
    echo "error: submodule $SUB missing — run 'git submodule update --init'" >&2
    exit 1
fi

# --- python: pdockerd source tree ---
rm -rf "$APP/python/pdockerd"
mkdir -p "$APP/python/pdockerd/bin" "$APP/python/pdockerd/docker-bin"
cp "$SUB/bin/pdockerd" "$APP/python/pdockerd/bin/"
cp "$SUB/bin/pdocker"  "$APP/python/pdockerd/bin/" 2>/dev/null || true

# --- native: crane + proot ---
# Only bundle binaries actually needed at runtime. They ride in assets/native
# and get extracted to filesDir on first launch (so we don't waste APK space
# on duplicate ABI copies — Android doesn't auto-expose assets as x-executable).
mkdir -p "$APP/assets/native"
for b in crane proot; do
    if [[ -f "$SUB/docker-bin/$b" ]]; then
        cp "$SUB/docker-bin/$b" "$APP/assets/native/"
        chmod +x "$APP/assets/native/$b"
    fi
done

# --- jniLibs ---
# libcow.so + libpdockerpty.so are produced by scripts/build-native-termux.sh
# directly into app/src/main/jniLibs/arm64-v8a/, so nothing to copy here.
# Just sanity-check that they exist.
for lib in libcow.so libpdockerpty.so; do
    p="$APP/jniLibs/arm64-v8a/$lib"
    if [[ ! -f "$p" ]]; then
        echo "warn: $p missing — run scripts/build-native-termux.sh first" >&2
    fi
done

echo "copy-native.sh: done"
