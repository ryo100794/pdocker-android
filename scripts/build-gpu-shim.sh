#!/usr/bin/env bash
# Build the Linux/glibc container-facing GPU shim.
#
# This is not an Android/Bionic binary. It is packaged as a native payload only
# so the APK can extract it, then pdockerd bind-mounts it into Linux/glibc
# containers as /usr/local/bin/pdocker-gpu-shim.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="$ROOT/docker-proot-setup/src/gpu"
OUT_DIR="$ROOT/app/src/main/jniLibs/arm64-v8a"
OUT="$OUT_DIR/libpdockergpushim.so"
CC="${CC:-gcc}"

mkdir -p "$OUT_DIR" "$ROOT/docker-proot-setup/lib"
"$CC" -O2 -fPIE -pie -Wall -Wextra \
    -o "$OUT" \
    "$SRC_DIR/pdocker_gpu_shim.c"
chmod 0755 "$OUT"
cp "$OUT" "$ROOT/docker-proot-setup/lib/pdocker-gpu-shim"
chmod 0755 "$ROOT/docker-proot-setup/lib/pdocker-gpu-shim"
file "$OUT" | head -1
