Put GGUF models here.

Default path used by the compose template:

`models/model.gguf`

The default compose template downloads Qwen3-8B Q4_K_M GGUF here on first
startup:

`https://huggingface.co/Qwen/Qwen3-8B-GGUF/resolve/main/Qwen3-8B-Q4_K_M.gguf`

The partial download path is `models/model.gguf.part`; keep it to resume an
interrupted download. Override `LLAMA_MODEL_URL` before compose up to use
another GGUF model.
