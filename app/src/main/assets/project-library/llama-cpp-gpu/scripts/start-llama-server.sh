#!/usr/bin/env bash
set -euo pipefail

profile="${LLAMA_GPU_PROFILE:-/profiles/pdocker-gpu.env}"
if [[ ! -f "$profile" ]]; then
  pdocker-gpu-profile "$profile" >/dev/null || true
fi
if [[ -f "$profile" ]]; then
  # shellcheck disable=SC1090
  source "$profile"
fi

model="${LLAMA_ARG_MODEL:-/models/model.gguf}"
port="${LLAMA_ARG_PORT:-8081}"
ctx="${LLAMA_ARG_CTX:-4096}"
threads="${LLAMA_ARG_THREADS:-$(nproc 2>/dev/null || echo 4)}"
ngl="${LLAMA_ARG_N_GPU_LAYERS:-0}"
server="/opt/llama.cpp/build/bin/llama-server"

if [[ ! -f "$model" ]]; then
  cat >&2 <<EOF
Missing model: $model
Place a GGUF model at ./models/model.gguf or set LLAMA_ARG_MODEL.
Current GPU profile:
$(cat "$profile" 2>/dev/null || true)
EOF
  sleep infinity
fi

echo "llama.cpp backend=${LLAMA_GPU_BACKEND:-unknown} ngl=$ngl threads=$threads ctx=$ctx port=$port"
exec "$server" \
  --host 0.0.0.0 \
  --port "$port" \
  --model "$model" \
  --ctx-size "$ctx" \
  --threads "$threads" \
  --n-gpu-layers "$ngl" \
  ${LLAMA_EXTRA_ARGS:-}
