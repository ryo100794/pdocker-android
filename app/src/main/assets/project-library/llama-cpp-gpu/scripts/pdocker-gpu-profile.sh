#!/usr/bin/env bash
set -euo pipefail

out="${1:-/profiles/pdocker-gpu.env}"
mkdir -p "$(dirname "$out")"

threads="${LLAMA_ARG_THREADS:-}"
if [[ -z "$threads" ]]; then
  threads="$(nproc 2>/dev/null || echo 4)"
fi

mem_kb="$(awk '/MemTotal/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
ctx="${LLAMA_ARG_CTX:-4096}"
if [[ "$mem_kb" -gt 0 && "$mem_kb" -lt 5000000 ]]; then
  ctx=2048
fi

backend="cpu"
ngl="0"
extra="${LLAMA_EXTRA_ARGS:-}"

if [[ "${PDOCKER_CUDA_COMPAT:-}" = "1" || -e /dev/nvidia0 ]]; then
  backend="cuda-compat"
  ngl="${LLAMA_ARG_N_GPU_LAYERS:-999}"
elif [[ "${PDOCKER_VULKAN_PASSTHROUGH:-}" = "1" || -n "${VK_ICD_FILENAMES:-}" || -e /etc/vulkan/icd.d/pdocker-android.json ]]; then
  backend="vulkan"
  ngl="${LLAMA_ARG_N_GPU_LAYERS:-999}"
elif command -v vulkaninfo >/dev/null 2>&1 && vulkaninfo --summary >/dev/null 2>&1; then
  backend="vulkan"
  ngl="${LLAMA_ARG_N_GPU_LAYERS:-999}"
fi

cat > "$out" <<EOF
LLAMA_GPU_BACKEND=$backend
LLAMA_ARG_THREADS=$threads
LLAMA_ARG_CTX=$ctx
LLAMA_ARG_N_GPU_LAYERS=$ngl
LLAMA_EXTRA_ARGS=$extra
EOF

cat "$out"
