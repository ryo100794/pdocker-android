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
# or AGP drops them during packaging. crane is static Go; proot is the
# self-built bionic binary staged by scripts/build-proot.sh so it can carry
# pdocker-specific extensions such as --cow-bind.
JNI_DIR="$APP/jniLibs/arm64-v8a"
mkdir -p "$JNI_DIR"
cp "$SUB/docker-bin/crane" "$JNI_DIR/libcrane.so"
if [[ "${PDOCKER_WITH_PROOT:-1}" != "0" ]]; then
    cp "$ROOT/vendor/lib/proot" "$JNI_DIR/libproot.so"
    cp "$ROOT/vendor/lib/libtalloc.so.2" "$JNI_DIR/libtalloc.so"
    # proot bootstraps its tracee via a separate loader binary that
    # Termux ships at /data/data/com.termux/files/usr/libexec/proot/loader.
    # Without it, every execve under proot dies with "No such file or
    # directory" (it's the loader that's missing, not the target binary).
    cp "$ROOT/vendor/lib/proot-loader" "$JNI_DIR/libproot-loader.so"
else
    rm -f "$JNI_DIR/libproot.so" "$JNI_DIR/libproot-loader.so" "$JNI_DIR/libtalloc.so"
fi
# libcow.so is LD_PRELOAD'd inside the *container* rootfs (typically
# glibc — ubuntu/debian — or musl — alpine). A bionic-targeted shim
# fails to load there ("libdl.so" vs "libdl.so.2", ld-android-* vs
# ld-linux-*). Stage the host-glibc build pdockerd ships in lib/
# (built on Termux+PRoot Ubuntu = same glibc as ubuntu containers).
cp "$SUB/lib/libcow.so" "$JNI_DIR/libcow.so"
# docker CLI and Compose plugin (Go static) so the WebView terminal can
# drive pdockerd via DOCKER_HOST=unix://... pdocker/pdockerd.sock without
# us building a custom client.
cp "$ROOT/vendor/lib/docker" "$JNI_DIR/libdocker.so"
cp "$ROOT/vendor/lib/docker-compose" "$JNI_DIR/libdocker-compose.so"

# proot (from Termux packaging) has DT_NEEDED=libtalloc.so.2 and the
# bundled libtalloc carries SONAME libtalloc.so.2 — Android's JNI lib
# loader only looks for "lib*.so" filenames in nativeLibraryDir, so we
# patchelf both sides to the simple libtalloc.so name. Without this,
# proot aborts at startup with 'library "libtalloc.so.2" not found'.
if [[ "${PDOCKER_WITH_PROOT:-1}" != "0" ]]; then
    command -v patchelf >/dev/null 2>&1 \
        || { echo "ABORT: patchelf required (apt install patchelf)" >&2; exit 1; }
    patchelf --replace-needed libtalloc.so.2 libtalloc.so "$JNI_DIR/libproot.so"
    patchelf --set-soname     libtalloc.so                 "$JNI_DIR/libtalloc.so"
fi

chmod 0755 "$JNI_DIR/libcrane.so" "$JNI_DIR/libcow.so" "$JNI_DIR/libdocker.so" \
           "$JNI_DIR/libdocker-compose.so"
if [[ "${PDOCKER_WITH_PROOT:-1}" != "0" ]]; then
    chmod 0755 "$JNI_DIR/libproot.so" "$JNI_DIR/libtalloc.so" \
               "$JNI_DIR/libproot-loader.so"
fi
echo "staged crane -> $JNI_DIR/libcrane.so"
if [[ "${PDOCKER_WITH_PROOT:-1}" != "0" ]]; then
    echo "staged proot -> $JNI_DIR/libproot.so (libtalloc.so.2 -> libtalloc.so)"
    echo "staged libtalloc -> $JNI_DIR/libtalloc.so"
    echo "staged proot-loader -> $JNI_DIR/libproot-loader.so"
else
    echo "skipped proot/talloc/proot-loader (PDOCKER_WITH_PROOT=0)"
fi
echo "staged libcow (glibc) -> $JNI_DIR/libcow.so"
echo "staged docker CLI -> $JNI_DIR/libdocker.so"
echo "staged docker compose plugin -> $JNI_DIR/libdocker-compose.so"

# --- jniLibs sanity ---
# libpdockerpty.so is built natively by scripts/build-native-termux.sh
# (the Kotlin/JNI pty bridge for the WebView terminal — no glibc/bionic
# concern because it runs in the Android process).
for lib in libpdockerpty.so; do
    p="$APP/jniLibs/arm64-v8a/$lib"
    if [[ ! -f "$p" ]]; then
        echo "warn: $p missing — run scripts/build-native-termux.sh first" >&2
    fi
done

echo "copy-native.sh: done"
