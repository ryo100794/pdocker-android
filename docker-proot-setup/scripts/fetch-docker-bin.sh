#!/usr/bin/env bash
#
# fetch-docker-bin.sh — Populate docker-bin/ with the third-party binaries
# pdocker actually uses. Idempotent; skips anything already present.
#
# Required:
#   - docker CLI (v29.x)         → Docker Engine client for pdockerd
#   - crane                      → image pull / export
#
# Not fetched (PRoot can't run them — kept out of the repo):
#   dockerd / containerd / runc / ctr / nerdctl / containerd-shim-runc-v2
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$HERE/docker-bin"
mkdir -p "$BIN"

ARCH="$(uname -m)"
case "$ARCH" in
    aarch64|arm64) CRANE_ARCH=arm64; DOCKER_ARCH=aarch64 ;;
    x86_64)        CRANE_ARCH=x86_64; DOCKER_ARCH=x86_64 ;;
    *) echo "ABORT: unsupported arch: $ARCH"; exit 1 ;;
esac

# ---- crane (go-containerregistry) ----
if [[ ! -x "$BIN/crane" ]]; then
    CRANE_VER="${CRANE_VER:-v0.19.1}"
    URL="https://github.com/google/go-containerregistry/releases/download/${CRANE_VER}/go-containerregistry_Linux_${CRANE_ARCH}.tar.gz"
    echo "Downloading crane $CRANE_VER ($CRANE_ARCH)..."
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    wget -qO "$tmp/crane.tgz" "$URL"
    tar -xf "$tmp/crane.tgz" -C "$tmp" crane
    install -m 0755 "$tmp/crane" "$BIN/crane"
    rm -rf "$tmp"
    trap - EXIT
else
    echo "crane already present: $BIN/crane"
fi

# ---- docker CLI (static build from docker.com) ----
if [[ ! -x "$BIN/docker" ]]; then
    DOCKER_VER="${DOCKER_VER:-29.4.0}"
    URL="https://download.docker.com/linux/static/stable/${DOCKER_ARCH}/docker-${DOCKER_VER}.tgz"
    echo "Downloading docker CLI $DOCKER_VER ($DOCKER_ARCH)..."
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    wget -qO "$tmp/docker.tgz" "$URL"
    tar -xf "$tmp/docker.tgz" -C "$tmp" docker/docker
    install -m 0755 "$tmp/docker/docker" "$BIN/docker"
    rm -rf "$tmp"
    trap - EXIT
else
    echo "docker CLI already present: $BIN/docker"
fi

echo
echo "==== docker-bin ready ===="
for f in docker crane; do
    if [[ -x "$BIN/$f" ]]; then
        size=$(stat -c '%s' "$BIN/$f" 2>/dev/null || echo "?")
        echo "  [x] $f ($size bytes)"
    else
        echo "  [ ] $f MISSING"
    fi
done
