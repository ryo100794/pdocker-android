# pdocker llama.cpp GPU workspace

This template builds a llama.cpp server workspace for pdocker.

It includes:

- `llama-server` built from `ggml-org/llama.cpp`.
- Vulkan-oriented build flags (`GGML_VULKAN=ON`) for Android GPU passthrough.
- CPU fallback with OpenMP/OpenBLAS packages available.
- `scripts/pdocker-gpu-profile.sh`, which writes a local GPU profile based on
  the runtime environment.
- `models/` and `workspace/` bind directories for GGUF models and experiments.

Usage from pdocker:

1. Open the Library tab.
2. Install the `llama.cpp GPU workspace` template.
3. Run the GPU profile action.
4. Put a GGUF model under `models/model.gguf`.
5. Run compose up.

The compose file requests Docker-compatible `gpus: all`. pdockerd maps that to
its Vulkan passthrough / CUDA-compatible negotiation state where available.
The default llama-server port is `18081`, offset from common development ports
to reduce collisions with Android/Termux services.
