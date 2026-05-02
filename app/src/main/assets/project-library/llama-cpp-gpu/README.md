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
4. Run compose up and let the default gpt-oss GGUF download complete.
5. Open the service on port `18081`.

By default, first compose up downloads the smaller OpenAI gpt-oss model in
GGUF form:

`https://huggingface.co/ggml-org/gpt-oss-20b-GGUF/resolve/main/gpt-oss-20b-mxfp4.gguf`

The file is about 12 GB and is stored as `models/model.gguf`. The download uses
`models/model.gguf.part` while in progress so it can resume after interruption.
Set `LLAMA_MODEL_URL` to another direct GGUF URL, or set it to an empty value
and place a GGUF manually. If no model is available after the download attempt,
the container still opens a small status page on port `18081` so the workspace
has a visible running state.

The entrypoint adds `--jinja` by default because the gpt-oss GGUF uses a chat
template. Override `LLAMA_EXTRA_ARGS` if you need different llama-server
options.

OpenAI gpt-oss weights are available under the Apache 2.0 license. This
template downloads the model at runtime; it is not bundled into the APK.

The compose file requests Docker-compatible `gpus: all`. pdockerd maps that to
its Vulkan passthrough / CUDA-compatible negotiation state where available.
The default llama-server port is `18081`, offset from common development ports
to reduce collisions with Android/Termux services.
