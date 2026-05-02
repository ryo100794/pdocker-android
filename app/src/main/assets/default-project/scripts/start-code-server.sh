#!/usr/bin/env bash
set -euo pipefail

mkdir -p /workspace \
  "${CODE_SERVER_USER_DATA_DIR:-/workspace/.vscode-server/data}/User" \
  "${CODE_SERVER_EXTENSIONS_DIR:-/workspace/.vscode-server/extensions}" \
  /workspace/.continue

if [[ -n "${CODE_SERVER_PASSWORD:-}" ]]; then
  export PASSWORD="$CODE_SERVER_PASSWORD"
  AUTH_MODE=password
else
  AUTH_MODE=none
fi
port="${CODE_SERVER_PORT:-18080}"

echo "code-server: http://0.0.0.0:$port"
echo "codex: $(command -v codex || true)"
echo "continue config: /workspace/.continue/config.yaml"

exec code-server \
  --bind-addr "0.0.0.0:$port" \
  --auth "$AUTH_MODE" \
  --user-data-dir "${CODE_SERVER_USER_DATA_DIR:-/workspace/.vscode-server/data}" \
  --extensions-dir "${CODE_SERVER_EXTENSIONS_DIR:-/workspace/.vscode-server/extensions}" \
  /workspace
