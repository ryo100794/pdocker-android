#!/usr/bin/env bash
# wrap-ndk-box64.sh — wrap every x86_64 ELF in the NDK's prebuilt
# linux-x86_64 toolchain with a /usr/bin/box64 shim so the build system
# can invoke the toolchain transparently on an aarch64 host.
#
# Idempotent: previously wrapped binaries are detected by the ".x86_64"
# sibling file and skipped.
set -euo pipefail

NDK_DIR="${ANDROID_NDK_HOME:-/root/android-ndk-r26d}"
BIN_DIR="$NDK_DIR/toolchains/llvm/prebuilt/linux-x86_64/bin"

if [[ ! -d "$BIN_DIR" ]]; then
    echo "ABORT: $BIN_DIR not found — NDK not installed?" >&2
    exit 1
fi

if ! command -v box64 >/dev/null 2>&1; then
    echo "ABORT: /usr/bin/box64 not found — apt install box64" >&2
    exit 1
fi

wrapped=0 skipped=0 nonelf=0 symlink=0
for f in "$BIN_DIR"/*; do
    # Skip symlinks — they'll be routed to their real target which we
    # wrap separately. Wrapping the symlink would break the indirection.
    if [[ -L "$f" ]]; then
        symlink=$((symlink+1))
        continue
    fi
    [[ -f "$f" ]] || continue

    # Already wrapped? (shim sits at $f, original at $f.x86_64)
    if [[ -f "$f.x86_64" ]]; then
        skipped=$((skipped+1))
        continue
    fi

    # Only wrap x86-64 ELF executables — shell scripts, libs (.so), and
    # other arches skip. NDK binaries are usually "pie executable".
    desc="$(file -b "$f" 2>/dev/null)"
    case "$desc" in
        *"ELF 64-bit"*"x86-64"*executable*) ;;
        *) nonelf=$((nonelf+1)); continue ;;
    esac

    mv "$f" "$f.x86_64"
    cat > "$f" <<WRAP
#!/bin/sh
exec /usr/bin/box64 "$f.x86_64" "\$@"
WRAP
    chmod +x "$f"
    wrapped=$((wrapped+1))
done

echo "wrap-ndk-box64: wrapped=$wrapped skipped=$skipped non-elf=$nonelf symlink=$symlink"
