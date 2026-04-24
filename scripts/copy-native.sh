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

# --- python: pdockerd script as an Android asset ---
# We deliberately don't ship pdockerd through Chaquopy's src/main/python
# tree. Chaquopy's AssetFinder only handles .py/.pyc via its custom
# importer, and pdockerd expects to resolve its runtime layout (docker-bin,
# lib) relative to its own __file__. So we stage the raw single-file
# script under assets/pdockerd/ and let Kotlin extract it to
# filesDir/pdocker-runtime/bin/ on first launch.
mkdir -p "$APP/assets/pdockerd"
cp "$SUB/bin/pdockerd" "$APP/assets/pdockerd/pdockerd"

# --- native: crane + proot ---
# Package as jniLibs with lib*.so naming so Android extracts them to
# nativeLibraryDir — the only location an app is allowed to execve
# from on API 29+ (files in /data/data/<pkg>/files/ have exec_no_trans
# SELinux denial). The names must start with "lib" and end with ".so"
# or AGP drops them during packaging. crane is static Go + proot is a
# Termux-built aarch64 ELF, so both run on Android without bionic
# repackaging.
JNI_DIR="$APP/jniLibs/arm64-v8a"
mkdir -p "$JNI_DIR"
cp "$SUB/docker-bin/crane" "$JNI_DIR/libcrane.so"
cp "$SUB/docker-bin/proot" "$JNI_DIR/libproot.so"
cp "$ROOT/vendor/lib/libtalloc.so.2" "$JNI_DIR/libtalloc.so"

# proot (from Termux packaging) has DT_NEEDED=libtalloc.so.2 and the
# bundled libtalloc carries SONAME libtalloc.so.2 — Android's JNI lib
# loader only looks for "lib*.so" filenames in nativeLibraryDir, so we
# patchelf both sides to the simple libtalloc.so name. Without this,
# proot aborts at startup with 'library "libtalloc.so.2" not found'.
command -v patchelf >/dev/null 2>&1 \
    || { echo "ABORT: patchelf required (apt install patchelf)" >&2; exit 1; }
patchelf --replace-needed libtalloc.so.2 libtalloc.so "$JNI_DIR/libproot.so"
patchelf --set-soname     libtalloc.so                 "$JNI_DIR/libtalloc.so"

chmod 0755 "$JNI_DIR/libcrane.so" "$JNI_DIR/libproot.so" "$JNI_DIR/libtalloc.so"
echo "staged crane -> $JNI_DIR/libcrane.so"
echo "staged proot -> $JNI_DIR/libproot.so (libtalloc.so.2 -> libtalloc.so)"
echo "staged libtalloc -> $JNI_DIR/libtalloc.so"

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
