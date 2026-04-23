#!/usr/bin/env bash
# fetch-xterm.sh — vendor xterm.js + FitAddon into assets so the
# WebView can load them as file:///android_asset/xterm/*.
# Idempotent: skips downloads when files already exist.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/app/src/main/assets/xterm"

XTERM_VER="${XTERM_VER:-5.3.0}"
FIT_VER="${FIT_VER:-0.8.0}"

declare -A urls=(
    ["xterm.js"]="https://cdn.jsdelivr.net/npm/xterm@${XTERM_VER}/lib/xterm.js"
    ["xterm.css"]="https://cdn.jsdelivr.net/npm/xterm@${XTERM_VER}/css/xterm.css"
    ["xterm-addon-fit.js"]="https://cdn.jsdelivr.net/npm/xterm-addon-fit@${FIT_VER}/lib/xterm-addon-fit.js"
)

mkdir -p "$DEST"
for f in "${!urls[@]}"; do
    if [[ -s "$DEST/$f" ]]; then
        echo "have $DEST/$f"
        continue
    fi
    echo "fetching $f"
    curl -fsSL --retry 3 "${urls[$f]}" -o "$DEST/$f"
done

ls -la "$DEST"
